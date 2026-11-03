#pragma once
#include <anakin_env/config.hpp>

namespace anakin::env {
struct StartupOptions {
  RuntimeMode mode = RuntimeMode::Real;
  ResourceOptions resources;
  std::vector<std::string> ros_arguments;
};
// Parse/load before ROS or SDK construction. E uses the same CLI contract.
StartupOptions parse_startup(const std::vector<std::string>& arguments);
}  // namespace anakin::env
