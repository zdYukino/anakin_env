#pragma once

#include <anakin_env/input.hpp>
#include <anakin_env/event_log.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>

namespace anakin::env {
// Independent of robot feedback and actuation. Uses the hub's normalized samples
// and online flags; publishing a heartbeat never renews a device acquisition time.
diagnostic_msgs::msg::DiagnosticArray input_diagnostics(
    const InputSnapshot& input, const InputConfig& config, InputTime now,
    const std::vector<SwitchEvent>& events = {}, const std::string& session = {});
}  // namespace anakin::env
