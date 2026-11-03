#include <anakin_env/event_log.hpp>

namespace anakin::env {
void SwitchEventLog::append(SwitchEvent event) {
  event.sequence = ++sequence_;
  event.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if (events_.size() == 32) events_.erase(events_.begin());
  events_.push_back(std::move(event));
}

void SwitchEventLog::observe(const InputFrame& input, const InputConfig& config,
                            const ModeConfig& mode, const ActionPlan& plan, const std::string& diagnostic) {
  const auto current = to_string(plan.after.state);
  bool settled = false;
  if (pending_ && (current == pending_->target || plan.after.state != State::Preparing)) {
    auto event = *pending_;
    event.current = current;
    event.result = current == event.target ? "success" : plan.after.state == State::Fault ? "failed" : "cancelled";
    event.detail = event.result == "success" ? "Runtime confirmed target state" :
                   diagnostic.empty() ? "Switch interrupted before completion" : diagnostic;
    append(std::move(event));
    pending_.reset();
    settled = true;
  }
  bool requested = false;
  for (const auto& request : input.events) {
    if (request.kind != EventKind::ManualRequest && request.kind != EventKind::EmergencyStop &&
        request.kind != EventKind::ClearFault) continue;
    auto target = request.requested_state;
    if (!target) {
      for (const auto& route : mode.system_transitions)
        if (route.event == request.kind && route.from == plan.before.state && route.to) {
          target = route.to;
          break;
        }
      if (!target)
        for (const auto& route : mode.system_transitions)
          if (route.event == request.kind && route.to) { target = route.to; break; }
    }
    if (!target) continue;
    requested = true;
    const auto active = input.snapshot.active_source ? input.snapshot.active_source : config.primary;
    std::string source = active ? to_string(*active) : "unknown";
    if (request.kind == EventKind::EmergencyStop) {
      source.clear();
      for (size_t i = 0; i < input.snapshot.emergency_sources.size(); ++i)
        if (input.snapshot.emergency_sources[i]) {
          if (!source.empty()) source += "+";
          source += to_string(static_cast<InputSource>(i));
        }
      if (source.empty()) source = "unknown";
    }
    const bool accepted = plan.cause == request.kind &&
        (plan.decision == Decision::Accepted || plan.decision == Decision::Idempotent);
    const bool preparing = accepted && plan.after.state == State::Preparing && plan.after.requested_state == *target;
    const auto result = preparing ? "pending" : accepted && plan.after.state == *target ? "success" : "failed";
    const auto signature = source + ":" + to_string(*target) + ":" + result;
    if (signature == last_request_) continue;  // Held stop is one request, not 500 logs/sec.
    last_request_ = signature;
    SwitchEvent event{0, 0, source, to_string(plan.before.state), to_string(*target), current, result,
        preparing ? "Waiting for runtime policy activation" : result == std::string("success") ?
        "Runtime confirmed target state" : plan.after.state == State::Fault && !diagnostic.empty() ?
        diagnostic : "Request rejected by entry guards, transition rules, or a higher-priority event"};
    if (pending_ && pending_->target != event.target && accepted) {
      auto cancelled = *pending_;
      cancelled.current = current;
      cancelled.result = "cancelled";
      cancelled.detail = "Superseded by another input request";
      append(std::move(cancelled));
      pending_.reset();
    }
    if (preparing) pending_ = event;
    append(std::move(event));
  }
  if (!requested) last_request_.clear();
  if (!requested && !settled && plan.decision == Decision::Accepted && plan.before.state != plan.after.state) {
    SwitchEvent event{0, 0, plan.cause == EventKind::SourceLost && input.lost_source ? to_string(*input.lost_source) : "system", to_string(plan.before.state),
        to_string(plan.after.state == State::Preparing ? plan.after.requested_state : plan.after.state), current,
        plan.after.state == State::Preparing ? "pending" : plan.after.state == State::Fault ? "failed" : "success",
        plan.after.state == State::Fault && !diagnostic.empty() ? diagnostic :
        plan.cause ? to_string(*plan.cause) : "Runtime state transition"};
    if (event.result == "pending") pending_ = event;
    append(std::move(event));
  }
}
}  // namespace anakin::env
