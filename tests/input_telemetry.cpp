#include <anakin_env/input_telemetry.hpp>
#include <yaml-cpp/yaml.h>

#include <iostream>

using namespace anakin::env;
using namespace std::chrono_literals;
namespace {
void check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
std::string field(const diagnostic_msgs::msg::DiagnosticStatus& status, const std::string& key) {
  for (const auto& value : status.values) if (value.key == key) return value.value;
  throw std::runtime_error("Missing diagnostic field: " + key);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Expected config directory");
    auto config = load_input_config(std::filesystem::path(argv[1]) / "inputs.yaml");
    config.primary = InputSource::Remote;  // The same effective override used at startup.
    config.gamepad_device_index = 7;
    config.controller_bindings[0] = {ButtonBinding{Button::Y, shoulder_r1, false}, Trigger::Released, InputAction::Hold};
    const auto yaml = YAML::Load(input_config_yaml(config));
    check(yaml["schema_version"].as<int>() == 2 && yaml["web"]["actions"].size() == 11,
          "Effective configuration exports web actions without terminal bindings");
    check(yaml["primary"].as<std::string>() == "remote" && yaml["gamepad"]["device_index"].as<int>() == 7,
          "Metadata comes from effective config, not a default file");
    const auto binding = yaml["controller_bindings"][0];
    check(binding["button"].as<std::string>() == "Y" && binding["shoulders"][0].as<std::string>() == "R1" &&
          binding["trigger"].as<std::string>() == "released" && binding["action"].as<std::string>() == "hold",
          "Runtime YAML preserves remapped combinations and triggers");
    check(yaml["controller_bindings"][1]["shoulders"].as<std::string>() == "any" &&
          yaml["controller_bindings"][8]["shoulders"].IsSequence() &&
          yaml["controller_bindings"][8]["shoulders"].size() == 0,
          "Any shoulder and exact empty shoulder sets remain distinct");
    const auto now = InputTime{} + 10s;
    InputHub hub(config, 250ms);
    InputSample gamepad;
    gamepad.source = InputSource::Gamepad;
    gamepad.connected = true;
    gamepad.acquired = now;
    gamepad.axes = {.25f, .75f, 0.f, 0.f};
    gamepad.triggers = std::array<float, 2>{.25f, 1.f};
    gamepad.buttons.set(static_cast<size_t>(Button::A));
    hub.submit(gamepad);
    hub.submit(UnitreeRemoteAdapter::wireless(0x0001, {-.5f, .4f, 0.f, 0.f}, now + 5ms));
    auto report = input_diagnostics(hub.snapshot(now + 10ms), config, now + 10ms);
    check(report.status.size() == 3 && field(report.status[0], "online") == "true" &&
          field(report.status[1], "online") == "true", "Both controllers report without robot feedback");
    check(field(report.status[0], "axis.left_x") == "0.250000" &&
          field(report.status[1], "axis.left_x") == "-0.500000" &&
          field(report.status[0], "button.A") == "true" && field(report.status[1], "button.A") == "false",
          "Independent normalized readings do not overwrite each other");
    check(field(report.status[0], "trigger.LT") == "0.250000" && field(report.status[0], "trigger.RT") == "1.000000",
          "Analog triggers have separate telemetry fields");
    for (const auto& value : report.status[1].values)
      check(value.key.find("trigger.") != 0, "Remote does not fabricate unavailable analog triggers");
    check(yaml["gamepad"]["mappings"][0].as<std::string>() == config.gamepad_mappings.at(0),
          "Running YAML retains device-specific SDL mappings");
    check(field(report.status[1], "primary") == "true" && field(report.status[0], "primary") == "false",
          "Actual primary source is reported");
    hub.submit(UnitreeRemoteAdapter::wireless(0, {}, now + 290ms));
    report = input_diagnostics(hub.snapshot(now + 300ms), config, now + 300ms);
    check(report.status[0].message == "STALE" && report.status[1].message == "LIVE" &&
          std::stod(field(report.status[0], "age_ms")) == 300.,
          "Publishing a heartbeat never renews a stale acquisition time");
    config.enabled_sources = {InputSource::Remote};
    InputHub disabled(config, 250ms);
    report = input_diagnostics(disabled.snapshot(now), config, now);
    check(report.status[0].message == "DISABLED" && field(report.status[0], "age_ms").empty(),
          "Disabled and never acquired sources remain explicit");
    std::cout << "Input telemetry and runtime YAML contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
