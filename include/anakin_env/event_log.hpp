#pragma once
#include <anakin_env/input.hpp>

namespace anakin::env {
struct SwitchEvent {
  uint64_t sequence = 0;
  int64_t time_ms = 0;
  std::string source, from, target, current, result, detail;
};

// Observation only: the runtime's decision is authoritative. Preparing is never
// reported as a completed policy switch. Caller serializes observe/snapshot.
class SwitchEventLog {
 public:
  void observe(const InputFrame& input, const InputConfig& config,
               const ModeConfig& mode, const ActionPlan& plan, const std::string& diagnostic);
  const std::vector<SwitchEvent>& events() const { return events_; }
 private:
  void append(SwitchEvent event);
  std::vector<SwitchEvent> events_;
  std::optional<SwitchEvent> pending_;
  uint64_t sequence_ = 0;
  std::string last_request_;
};
}  // namespace anakin::env
