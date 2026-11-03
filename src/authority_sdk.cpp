#include <anakin_env/authority.hpp>
#include <unitree/robot/b2/motion_switcher/motion_switcher_client.hpp>

namespace anakin::env {
std::unique_ptr<MotionSwitcherApi> make_sdk_motion_switcher() {
  // Constructing this adapter creates no SDK client/channel; initialize is lazy.
  return std::make_unique<MotionClientAdapter<unitree::robot::b2::MotionSwitcherClient>>();
}
}  // namespace anakin::env
