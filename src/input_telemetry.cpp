#include <anakin_env/input_telemetry.hpp>

#include <algorithm>

namespace anakin::env {
diagnostic_msgs::msg::DiagnosticArray input_diagnostics(
    const InputSnapshot& input, const InputConfig& config, InputTime now,
    const std::vector<SwitchEvent>& events, const std::string& session) {
  diagnostic_msgs::msg::DiagnosticArray report;
  for (const auto source : {InputSource::Gamepad, InputSource::Remote, InputSource::Web}) {
    const auto i = static_cast<size_t>(source);
    const auto& sample = input.sources[i];
    const bool enabled = std::find(config.enabled_sources.begin(), config.enabled_sources.end(), source) !=
                         config.enabled_sources.end();
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "anakin_env/input/" + to_string(source);
    status.hardware_id = to_string(source);
    status.level = input.online[i] ? status.OK : status.STALE;
    status.message = !enabled ? "DISABLED" : input.online[i] ? "LIVE" :
                     sample.connected ? "STALE" : "DISCONNECTED";
    const auto add = [&](const std::string& key, const std::string& value) {
      diagnostic_msgs::msg::KeyValue entry;
      entry.key = key;
      entry.value = value;
      status.values.push_back(std::move(entry));
    };
    add("schema_version", "1");
    add("enabled", enabled ? "true" : "false");
    add("primary", input.active_source == source ? "true" : "false");
    add("connected", sample.connected ? "true" : "false");
    add("online", input.online[i] ? "true" : "false");
    add("age_ms", sample.acquired != InputTime{} && now >= sample.acquired ?
        std::to_string(std::chrono::duration<double, std::milli>(now - sample.acquired).count()) : "");
    size_t index = 0;
    for (const auto* axis : {"left_x", "left_y", "right_x", "right_y"})
      add(std::string("axis.") + axis, std::to_string(sample.axes[index++]));
    index = 0;
    for (const auto* button : {"A", "B", "X", "Y", "Up", "Down", "Left", "Right", "Start", "Select"})
      add(std::string("button.") + button, sample.buttons.test(index++) ? "true" : "false");
    add("button.L1", sample.shoulders & shoulder_l1 ? "true" : "false");
    add("button.R1", sample.shoulders & shoulder_r1 ? "true" : "false");
    if (sample.triggers) {
      add("trigger.LT", std::to_string((*sample.triggers)[0]));
      add("trigger.RT", std::to_string((*sample.triggers)[1]));
    }
    if (sample.device) {
      add("device_name", sample.device->name);
      add("device_guid", sample.device->guid);
      add("sdl_mapping", sample.device->mapping);
    }
    report.status.push_back(std::move(status));
  }
  for (const auto& event : events) {
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "anakin_env/switch_event";
    status.hardware_id = session;
    status.level = event.result == "failed" ? status.ERROR : event.result == "success" ? status.OK : status.WARN;
    status.message = event.result;
    const std::vector<std::pair<std::string, std::string>> values{
        {"schema_version", "1"}, {"sequence", std::to_string(event.sequence)},
        {"time_ms", std::to_string(event.time_ms)}, {"source", event.source}, {"from", event.from},
        {"target", event.target}, {"current", event.current}, {"result", event.result}, {"detail", event.detail}};
    for (const auto& value : values) {
      diagnostic_msgs::msg::KeyValue item;
      item.key = value.first;
      item.value = value.second;
      status.values.push_back(std::move(item));
    }
    report.status.push_back(std::move(status));
  }
  return report;
}
}  // namespace anakin::env
