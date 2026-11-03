#include <anakin_env/state_machine.hpp>

#include <algorithm>

namespace anakin::env {
namespace {
int priority(EventKind kind) {
  switch (kind) {
    case EventKind::EmergencyStop: return 0;
    case EventKind::Fault: return 1;
    case EventKind::SourceLost: return 2;
    case EventKind::ManualRequest:
    case EventKind::ClearFault: return 3;
    default: return 4;
  }
}
bool nonzero(const std::array<uint8_t, 16>& value) {
  return std::any_of(value.begin(), value.end(), [](uint8_t byte) { return byte != 0; });
}
}  // namespace

bool TransactionIdentity::valid() const {
  return nonzero(session) && nonzero(instance) && nonzero(request) && epoch != 0;
}
bool TransactionIdentity::operator==(const TransactionIdentity& other) const {
  return session == other.session && instance == other.instance && request == other.request &&
         epoch == other.epoch;
}
Event Event::manual(State state) {
  return {EventKind::ManualRequest, state, 0, std::nullopt, std::nullopt};
}
Event Event::system(EventKind kind) { return {kind, std::nullopt, 0, std::nullopt, std::nullopt}; }
Event Event::completion(EventKind kind, uint64_t generation) {
  return {kind, std::nullopt, generation, std::nullopt, std::nullopt};
}
Event Event::policy(EventKind kind, uint64_t generation, const TransactionIdentity& transaction,
                    PolicyModel producing_model) {
  return {kind, std::nullopt, generation, transaction, producing_model};
}

StateMachine::StateMachine(ModeConfig config) : config_(std::move(config)) {
  if (config_.initial_state != State::Disarmed || config_.states.size() != 8 ||
      config_.states.count(State::Disarmed) != 1)
    throw ConfigError("StateMachine requires a validated mode configuration");
  snapshot_.state = snapshot_.requested_state = snapshot_.effective_state = config_.initial_state;
}

ActionPlan StateMachine::plan(Decision decision, std::optional<EventKind> cause) const {
  ActionPlan result;
  result.decision = decision;
  result.cause = cause;
  result.before = result.after = snapshot_;
  const auto& state = config_.states.at(snapshot_.state);
  result.outputs = state.outputs;
  result.command = state.command;
  result.model = snapshot_.state == State::Preparing
                     ? config_.states.at(snapshot_.requested_state).model
                     : state.model;
  result.inference_allowed = state.outputs.ros_state && state.outputs.lowcmd &&
                             state.command == CommandSource::Policy;
  return result;
}

ActionPlan StateMachine::startup_plan() const {
  auto result = plan(Decision::Accepted);
  for (const auto action : config_.states.at(snapshot_.state).on_enter)
    result.actions.push_back({action, snapshot_.state, result.model});
  return result;
}

bool StateMachine::bind_transaction(uint64_t generation, const TransactionIdentity& transaction) {
  if (snapshot_.state != State::Preparing || snapshot_.generation != generation ||
      !transaction.valid() || snapshot_.transaction)
    return false;
  snapshot_.transaction = transaction;
  return true;
}

const Transition* StateMachine::route(const std::vector<Transition>& routes, EventKind event,
                                     std::optional<State> target) const {
  for (const auto& candidate : routes)
    if (candidate.from == snapshot_.state && candidate.event == event &&
        (!target || candidate.to == target))
      return &candidate;
  return nullptr;
}

bool StateMachine::current(const Event& event) const {
  if (event.generation == 0 || event.generation != snapshot_.generation) return false;
  if (event.kind != EventKind::MotionDone && event.kind != EventKind::PolicyReady) return true;
  if (!snapshot_.transaction || !event.transaction || !event.transaction->valid() ||
      !(*snapshot_.transaction == *event.transaction))
    return false;
  const auto expected = event.kind == EventKind::PolicyReady
                            ? config_.states.at(snapshot_.requested_state).model
                            : snapshot_.producing_model;
  return expected && event.producing_model == expected;
}

ActionPlan StateMachine::transition(const Transition& selected, const Event& event) {
  const auto target = selected.to.value_or(snapshot_.requested_state);
  const auto next = selected.via.value_or(target);
  if (next == snapshot_.state && (target == snapshot_.requested_state || next == State::Fault ||
      event.kind == EventKind::EmergencyStop || event.kind == EventKind::Fault))
    return plan(Decision::Idempotent, event.kind);

  auto result = plan(Decision::Accepted, event.kind);
  for (const auto action : config_.states.at(snapshot_.state).on_exit)
    result.actions.push_back({action, snapshot_.state, result.model});

  snapshot_.state = next;
  if (target != State::Fault) snapshot_.requested_state = target;
  ++snapshot_.generation;
  if (next == State::Preparing) {
    snapshot_.inference_phase = InferencePhase::Preparing;
    snapshot_.transaction.reset();
  } else {
    snapshot_.effective_state = next;
    if (config_.states.at(next).model) {
      snapshot_.producing_model = event.producing_model;
      snapshot_.inference_phase = InferencePhase::Active;
    } else {
      snapshot_.producing_model.reset();
      snapshot_.transaction.reset();
      snapshot_.inference_phase = InferencePhase::Idle;
    }
  }
  auto final = plan(Decision::Accepted, event.kind);
  final.before = result.before;
  final.actions = std::move(result.actions);
  for (const auto action : config_.states.at(next).on_enter)
    final.actions.push_back({action, next, final.model});
  return final;
}

ActionPlan StateMachine::dispatch(const std::vector<Event>& events) {
  std::vector<Event> ordered = events;
  // Immediate rules are evaluated by the same scheduler, after all user events.
  ordered.push_back(Event::completion(EventKind::Immediate, snapshot_.generation));
  std::stable_sort(ordered.begin(), ordered.end(), [](const Event& left, const Event& right) {
    return priority(left.kind) < priority(right.kind);
  });
  auto fallback = plan(Decision::NoEvent);
  for (const auto& event : ordered) {
    if (event.kind == EventKind::SourceLost) {
      if (config_.guards.source_loss == SourceLossPolicy::ZeroMotionKeepMode) {
        auto result = plan(Decision::Accepted, event.kind);
        result.actions.push_back({Action::ZeroMotion, snapshot_.state, result.model});
        return result;
      }
      const auto* selected = route(config_.system_transitions, EventKind::Fault);
      if (selected) return transition(*selected, event);
    } else if (event.kind == EventKind::ManualRequest) {
      if (snapshot_.state != State::Fault && event.requested_state &&
          is_business_state(*event.requested_state)) {
        if (*event.requested_state == snapshot_.requested_state)
          return plan(Decision::Idempotent, event.kind);
        if (const auto* selected = route(config_.manual_transitions, event.kind,
                                         event.requested_state))
          return transition(*selected, event);
      }
    } else if (priority(event.kind) < 4) {
      // Emergency stop never clears a latched fault, and suppresses clear_fault.
      if (event.kind == EventKind::EmergencyStop && snapshot_.state == State::Fault)
        return plan(Decision::Idempotent, event.kind);
      if (const auto* selected = route(config_.system_transitions, event.kind))
        return transition(*selected, event);
    } else {
      const auto* selected = route(config_.automatic_transitions, event.kind);
      if (!selected) {
        if (event.kind == EventKind::Immediate) continue;
      } else if (current(event)) {
        return transition(*selected, event);
      } else {
        fallback = plan(Decision::IgnoredStale, event.kind);
        continue;
      }
    }
    if (fallback.decision == Decision::NoEvent)
      fallback = plan(Decision::Rejected, event.kind);
  }
  return fallback;
}

}  // namespace anakin::env
