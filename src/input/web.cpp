#include <anakin_env/web_input.hpp>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace anakin::env {
WebInput::WebInput(InputConfig config, std::chrono::milliseconds timeout, std::string environment)
    : config_(std::move(config)), timeout_(timeout), environment_(std::move(environment)) {}

std::optional<InputSample> WebInput::expire(InputTime now) {
  if (owner_.empty() || now < acquired_ || now - acquired_ <= timeout_) return std::nullopt;
  owner_.clear();
  InputSample sample;
  sample.source = InputSource::Web;
  sample.acquired = now;
  sample.connection = connection_;
  return sample;
}

InputSample WebInput::receive(const std::string& packet, InputTime now, int64_t unix_ms) {
  if (packet.size() > 4096) throw std::invalid_argument("Web input packet too large");
  if (std::find(config_.enabled_sources.begin(), config_.enabled_sources.end(), InputSource::Web) == config_.enabled_sources.end())
    throw std::invalid_argument("Web input is disabled by configuration");
  if (config_.primary && config_.primary != InputSource::Web)
    throw std::invalid_argument("Web input is excluded by fixed primary");
  const auto root = YAML::Load(packet);
  if (!root.IsMap()) throw std::invalid_argument("Expected web input object");
  const std::set<std::string> fields{"v", "environment", "session", "sequence", "sent_ms", "operation", "axes", "buttons", "shoulders", "action"};
  std::set<std::string> seen;
  for (const auto& entry : root) {
    const auto key = entry.first.as<std::string>();
    if (!fields.count(key) || !seen.insert(key).second) throw std::invalid_argument("Unknown or duplicate web field");
  }
  if (root["v"].as<int>() != 1 || root["environment"].as<std::string>() != environment_)
    throw std::invalid_argument("Web input version or ENV session mismatch");
  const auto session = root["session"].as<std::string>();
  if (session.empty() || session.size() > 64 || session.find_first_not_of("0123456789abcdef-") != std::string::npos)
    throw std::invalid_argument("Invalid web session");
  const auto sequence = root["sequence"].as<uint64_t>();
  const auto sent = root["sent_ms"].as<int64_t>();
  if (sequence == 0 || sent > unix_ms + 100 || sent < unix_ms - timeout_.count())
    throw std::invalid_argument("Expired web input");
  const auto operation = root["operation"].as<std::string>();
  if (operation != "activate" && operation != "release" && operation != "input")
    throw std::invalid_argument("Unknown web operation");
  InputSample sample;
  sample.source = InputSource::Web;
  sample.acquired = now;
  sample.connected = operation != "release";
  const auto axes = root["axes"], buttons = root["buttons"];
  if (!axes.IsSequence() || axes.size() != 4 || !buttons.IsSequence() || buttons.size() != input_button_count)
    throw std::invalid_argument("Expected four axes and ten buttons");
  for (size_t i = 0; i < 4; ++i) {
    sample.axes[i] = axes[i].as<float>();
    if (!std::isfinite(sample.axes[i]) || std::abs(sample.axes[i]) > 1.f) throw std::invalid_argument("Invalid web axis");
  }
  for (size_t i = 0; i < input_button_count; ++i) sample.buttons.set(i, buttons[i].as<bool>());
  const auto shoulders = root["shoulders"].as<int>();
  if (shoulders < 0 || shoulders > 3) throw std::invalid_argument("Invalid web shoulders");
  sample.shoulders = static_cast<uint8_t>(shoulders);
  if (root["action"]) {
    sample.action = input_action(root["action"].as<std::string>());
    if (operation != "input" || std::find(config_.web_actions.begin(), config_.web_actions.end(), *sample.action) == config_.web_actions.end())
      throw std::invalid_argument("Web action is not configured");
  }
  const bool alive = !owner_.empty() && now >= acquired_ && now - acquired_ <= timeout_;
  if (operation == "activate") {
    if (alive) throw std::invalid_argument("Web control is already leased");
    if (sample.buttons.any() || sample.shoulders || std::any_of(sample.axes.begin(), sample.axes.end(), [](float axis) { return axis != 0.f; }))
      throw std::invalid_argument("Web activation must be neutral");
    owner_ = session;
    ++connection_;
    sample.activate = true;
  } else {
    if (!alive || owner_ != session || sequence <= sequence_) throw std::invalid_argument("Web lease expired, not owned, or input replayed");
    if (operation == "release") owner_.clear();
  }
  sequence_ = sequence;
  acquired_ = now;
  sample.connection = connection_;
  return sample;
}
}  // namespace anakin::env
