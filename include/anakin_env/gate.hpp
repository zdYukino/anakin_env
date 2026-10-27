#pragma once
#include <anakin_sdk/contract.hpp>
#include <anakin_sdk/msg/control_target.hpp>
#include <anakin_sdk/msg/robot_state_frame.hpp>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
namespace anakin::env {
using Frame = anakin_sdk::msg::RobotStateFrame;
using Target = anakin_sdk::msg::ControlTarget;
using Positions = std::array<double, 29>;
using Clock = std::chrono::steady_clock;
struct Sample {
  uint64_t seq, epoch;
  Clock::time_point acquired;
  Positions q;
};
class Gate {
 public:
  void begin(const Uuid& session, const Uuid& instance, const Uuid& request, uint64_t epoch,
             const std::string& model, bool continuation,
             const std::string& profile = legacy_gain_profile,
             const std::string& execution = strict_execution_profile,
             uint8_t semantics = mechanical_position);
  void state(const Frame& f, Clock::time_point acquired);
  bool accept(const Target& t, Clock::time_point now, std::string& reason);
  bool live(Clock::time_point now) const;
  Clock::time_point deadline() const { return expires_; }
  void stop();
  uint64_t accepted_seq() const { return accepted_seq_; }
  uint8_t phase_mask() const { return phase_mask_; }

 private:
  Uuid session_{}, instance_{}, request_{};
  uint64_t epoch_ = 0, accepted_seq_ = 0;
  std::string model_, gain_profile_ = legacy_gain_profile,
                      execution_profile_ = strict_execution_profile;
  uint8_t semantics_ = mechanical_position;
  bool continuation_ = false;
  std::deque<Sample> samples_;
  std::optional<Positions> previous_;
  Clock::time_point expires_{};
  bool live_ = false;
  bool revoked_ = true;
  uint8_t phase_mask_ = 0;
};
const Positions& lower_limits();
const Positions& upper_limits();
}  // namespace anakin::env
