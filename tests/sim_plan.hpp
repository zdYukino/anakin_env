#pragma once
#include <anakin_env/config.hpp>
#include <yaml-cpp/yaml.h>
#include <fstream>

namespace anakin::env::sim_test {
struct LaunchPlan {
  std::filesystem::path mode_config, input_config;
  std::vector<std::string> env_arguments;
};
// Explicit test-only copies; never rewrite the selected installed defaults.
inline LaunchPlan make_plan(const std::filesystem::path& prefix,
                           const std::filesystem::path& run,
                           const std::filesystem::path& launcher,
                           int sdk_domain, int ros_domain, double alignment_seconds) {
  auto mode = YAML::LoadFile((prefix / "share/anakin_env/config/sim.yaml").string());
  auto inputs = YAML::LoadFile((prefix / "share/anakin_env/config/inputs.yaml").string());
  mode["transport"]["interface"] = "lo";
  mode["transport"]["sdk_domain"] = sdk_domain;
  mode["transport"]["ros_domain"] = ros_domain;
  mode["timing"]["alignment_seconds"] = alignment_seconds;
  inputs["primary"] = "remote";
  inputs["remote"]["channel"] = "wireless_topic";
  LaunchPlan plan{run / "sim.yaml", run / "inputs.yaml", {}};
  for (const auto& entry : {std::make_pair(plan.mode_config, mode), std::make_pair(plan.input_config, inputs)}) {
    std::ofstream output(entry.first);
    if (!output) throw ConfigError("Cannot write explicit test configuration: " + entry.first.string());
    output << entry.second << '\n';
    if (!output) throw ConfigError("Cannot finish explicit test configuration: " + entry.first.string());
  }
  const auto checked_mode = load_mode_config(plan.mode_config, RuntimeMode::Sim);
  const auto checked_inputs = load_input_config(plan.input_config);
  if (checked_mode.guards.source_loss != SourceLossPolicy::ZeroMotionKeepMode ||
      checked_inputs.primary != InputSource::Remote || checked_inputs.remote_channel != RemoteChannel::WirelessTopic)
    throw ConfigError("Explicit probe requires default sim source-loss and wireless remote primary");
  plan.env_arguments = {launcher.string(), "--sim", "--input", "remote", "--config", plan.mode_config.string(),
                        "--input-config", plan.input_config.string()};
  return plan;
}
}  // namespace anakin::env::sim_test
