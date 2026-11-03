#pragma once

#include <anakin_env/config.hpp>

namespace anakin::env {

struct TransactionIdentity {
  std::array<uint8_t, 16> session{}, instance{}, request{};
  uint64_t epoch = 0;
  bool valid() const;
  bool operator==(const TransactionIdentity& other) const;
};

enum class InferencePhase { Idle, Preparing, Active };
struct StateSnapshot {
  State state = State::Disarmed;
  State requested_state = State::Disarmed;
  // Completion is separate from request acceptance; during Preparing this remains
  // the previous completed business state. It grants no permission to reuse targets.
  State effective_state = State::Disarmed;
  std::optional<PolicyModel> producing_model;
  InferencePhase inference_phase = InferencePhase::Idle;
  uint64_t generation = 1;
  std::optional<TransactionIdentity> transaction;
};

struct Event {
  EventKind kind;
  std::optional<State> requested_state;
  uint64_t generation = 0;
  std::optional<TransactionIdentity> transaction;
  std::optional<PolicyModel> producing_model;

  static Event manual(State state);
  static Event system(EventKind kind);
  static Event completion(EventKind kind, uint64_t generation);
  static Event policy(EventKind kind, uint64_t generation, const TransactionIdentity& transaction,
                      PolicyModel producing_model);
};

struct PlannedAction {
  Action action;
  State state;
  std::optional<PolicyModel> model;
};
enum class Decision { NoEvent, Accepted, Idempotent, Rejected, IgnoredStale };
struct ActionPlan {
  Decision decision = Decision::NoEvent;
  std::optional<EventKind> cause;
  StateSnapshot before, after;
  std::vector<PlannedAction> actions;
  OutputPolicy outputs;
  CommandSource command = CommandSource::None;
  std::optional<PolicyModel> model;
  // Runtime executes actions in order before applying outputs; policy output also
  // requires a fresh target matching the bound transaction, even while Preparing.
  // This permits MC inference/warmup in Preparing, not authority or a LowCmd write.
  // The runtime must publish the Preparing phase and apply its target/send guards.
  bool inference_allowed = false;
};

// No ROS, SDK, device, clock or thread is constructed by this engine. The caller
// supplies all events for one scheduling cycle; at most one transition is committed.
class StateMachine {
 public:
  explicit StateMachine(ModeConfig config);
  const StateSnapshot& snapshot() const { return snapshot_; }
  const ModeConfig& config() const { return config_; }
  ActionPlan startup_plan() const;
  ActionPlan dispatch(const std::vector<Event>& events);
  // Runtime binds the new MC transaction after executing RequestPolicy. Late
  // bindings from an interrupted plan are rejected without changing the machine.
  bool bind_transaction(uint64_t generation, const TransactionIdentity& transaction);

 private:
  ActionPlan plan(Decision decision, std::optional<EventKind> cause = std::nullopt) const;
  ActionPlan transition(const Transition& route, const Event& event);
  const Transition* route(const std::vector<Transition>& routes, EventKind event,
                          std::optional<State> target = std::nullopt) const;
  bool current(const Event& event) const;
  ModeConfig config_;
  StateSnapshot snapshot_;
};

}  // namespace anakin::env
