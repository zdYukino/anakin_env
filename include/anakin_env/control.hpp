#pragma once
#include <algorithm>
#include <anakin_env/gate.hpp>
#include <array>
#include <cmath>
#include <cstdint>

namespace anakin::env {
enum class ExecutionMode : uint8_t {
  Disarmed = 0,
  Aligning = 1,
  Hold = 2,
  Active = 3,
  Damping = 4,
  Fault = 5,
  Preparing = 6
};
struct RemoteSample {
  std::array<float, 4> axes{};  // left X, left Y, right X, right Y
  uint16_t keys = 0;
  Clock::time_point acquired{};
  builtin_interfaces::msg::Time stamp;
  bool valid = false;
};
struct RemoteIntent {
  bool x = false, stop = false, reset = false, mimic = false, loco = false, motion_toggle = false,
       height_up = false, height_down = false;
};
class RemoteEdges {
 public:
  RemoteIntent update(const RemoteSample& sample) {
    RemoteIntent intent;
    if (!sample.valid) return intent;
    for (float axis : sample.axes)
      if (!std::isfinite(axis) || std::abs(axis) > 1.001f)
        throw std::runtime_error("Invalid RC axis");
    const uint16_t rising = initialized_ ? sample.keys & ~last_ : 0;
    initialized_ = true;
    last_ = sample.keys;
    intent.stop = sample.keys & ((1u << 8) | (1u << 9));  // A/B, highest priority
    if (intent.stop) return intent;
    const bool deliberate = (sample.keys & 3u) == 3u;  // both shoulders held
    intent.x = deliberate && (rising & (1u << 10));
    intent.reset = deliberate && (rising & (1u << 11));
    intent.mimic = (sample.keys & 1u) && (rising & (1u << 12));
    intent.loco = (sample.keys & 1u) && (rising & (1u << 14));
    if (!(sample.keys & 3u)) {
      intent.motion_toggle = rising & (1u << 15);
      intent.height_up = rising & (1u << 12);
      intent.height_down = rising & (1u << 14);
    }
    return intent;
  }

 private:
  uint16_t last_ = 0;
  bool initialized_ = false;
};
class MotionCommand {
 public:
  void update(const RemoteIntent& intent) {
    if (intent.motion_toggle) moving_ = !moving_;
    if (intent.height_up) height_ += .05;
    if (intent.height_down) height_ -= .05;
  }
  bool moving() const { return moving_; }
  double height() const { return height_; }
  std::array<double, 3> velocity(const RemoteSample& remote) const {
    const auto deadzone = [](float a) { return std::abs(a) < .04f ? 0. : static_cast<double>(a); };
    if (!moving_) return {0, 0, 0};
    return {.5 * deadzone(remote.axes[1]), -.5 * deadzone(remote.axes[0]),
            -deadzone(remote.axes[2])};
  }

 private:
  bool moving_ = false;
  double height_ = .78;
};
inline std::array<double, 3> world_to_body(const geometry_msgs::msg::Quaternion& q,
                                           const std::array<float, 3>& v) {
  return {(1 - 2 * (q.y * q.y + q.z * q.z)) * v[0] + 2 * (q.x * q.y + q.w * q.z) * v[1] +
              2 * (q.x * q.z - q.w * q.y) * v[2],
          2 * (q.x * q.y - q.w * q.z) * v[0] + (1 - 2 * (q.x * q.x + q.z * q.z)) * v[1] +
              2 * (q.y * q.z + q.w * q.x) * v[2],
          2 * (q.x * q.z + q.w * q.y) * v[0] + 2 * (q.y * q.z - q.w * q.x) * v[1] +
              (1 - 2 * (q.x * q.x + q.y * q.y)) * v[2]};
}
inline Positions stand_pose() {
  return Positions{-.1, 0,  0, .3, -.2, 0, -.1, 0, 0,   .3, -.2, 0, 0, 0, 0,
                   0,   .3, 0, 1,  0,   0, 0,   0, -.3, 0,  1,   0, 0, 0};
}
class LocalControl {
 public:
  void set_alignment_seconds(double seconds) {
    if (mode_ != ExecutionMode::Disarmed || !std::isfinite(seconds) || seconds < .25 ||
        seconds > 5.)
      throw std::runtime_error("Alignment duration must be .25..5 seconds while disarmed");
    alignment_seconds_ = seconds;
  }
  ExecutionMode mode() const { return mode_; }
  bool authorized() const {
    return mode_ == ExecutionMode::Aligning || mode_ == ExecutionMode::Hold ||
           mode_ == ExecutionMode::Preparing || mode_ == ExecutionMode::Active;
  }
  bool stand(const Positions& measured, Clock::time_point now) {
    if (mode_ != ExecutionMode::Disarmed) return false;
    start_ = measured;
    started_ = now;
    mode_ = ExecutionMode::Aligning;
    return true;
  }
  Positions local_target(Clock::time_point now) {
    if (mode_ != ExecutionMode::Aligning) return stand_pose();
    const double alpha = std::clamp(
        std::chrono::duration<double>(now - started_).count() / alignment_seconds_, 0., 1.);
    Positions q{};
    const auto goal = stand_pose();
    for (size_t i = 0; i < q.size(); ++i) q[i] = (1 - alpha) * start_[i] + alpha * goal[i];
    if (alpha >= 1.) mode_ = ExecutionMode::Hold;
    return q;
  }
  void prepare() {
    if (mode_ != ExecutionMode::Hold && mode_ != ExecutionMode::Active &&
        mode_ != ExecutionMode::Preparing)
      throw std::runtime_error("Inference requires explicit stand authorization");
    mode_ = ExecutionMode::Preparing;
  }
  void active() {
    if (mode_ != ExecutionMode::Preparing && mode_ != ExecutionMode::Active)
      throw std::runtime_error("Target cannot authorize local execution");
    mode_ = ExecutionMode::Active;
  }
  void stop(Clock::time_point now, bool failure) {
    const bool was_authorized = authorized();
    mode_ = failure ? ExecutionMode::Fault : ExecutionMode::Damping;
    damping_until_ = was_authorized ? now + std::chrono::milliseconds(100) : now;
  }
  bool damping(Clock::time_point now) const { return now < damping_until_; }
  Clock::time_point damping_deadline() const { return damping_until_; }
  void settle(Clock::time_point now) {
    if (mode_ == ExecutionMode::Damping && !damping(now)) mode_ = ExecutionMode::Disarmed;
  }

 private:
  double alignment_seconds_ = 2.;
  ExecutionMode mode_ = ExecutionMode::Disarmed;
  Positions start_{};
  Clock::time_point started_{}, damping_until_{};
};
}  // namespace anakin::env
