#include <anakin_env/startup.hpp>
#include <set>

namespace anakin::env {
StartupOptions parse_startup(const std::vector<std::string>& arguments) {
  if (arguments.empty()) throw ConfigError("Missing executable argument");
  StartupOptions result;
  result.ros_arguments.push_back(arguments[0]);
  std::set<std::string> seen;
  bool selected_mode = false;
  for (size_t i = 1; i < arguments.size(); ++i) {
    const auto& flag = arguments[i];
    if (flag == "--ros-args") {
      result.ros_arguments.insert(result.ros_arguments.end(), arguments.begin() + i, arguments.end());
      break;
    }
    if (!seen.insert(flag).second) throw ConfigError(flag + ": repeated option");
    if (flag == "--sim" || flag == "--real") {
      if (selected_mode) throw ConfigError(flag + ": conflicting mode selection");
      result.mode = flag == "--sim" ? RuntimeMode::Sim : RuntimeMode::Real;
      selected_mode = true;
    } else if (flag == "--config" || flag == "--input-config" || flag == "--input") {
      if (++i >= arguments.size() || arguments[i].empty() || arguments[i].front() == '-')
        throw ConfigError(flag + ": missing value");
      if (flag == "--config") result.resources.config = arguments[i];
      else if (flag == "--input-config") result.resources.input_config = arguments[i];
      else {
        const auto& source = arguments[i];
        if (source == "keyboard") result.resources.primary_input = InputSource::Keyboard;
        else if (source == "gamepad") result.resources.primary_input = InputSource::Gamepad;
        else if (source == "remote") result.resources.primary_input = InputSource::Remote;
        else throw ConfigError("--input: expected keyboard, gamepad or remote");
      }
    } else throw ConfigError(flag + ": unknown option");
  }
  return result;
}
}  // namespace anakin::env
