#pragma once
// Shared by the explicit native test probe and communication-free fixtures.
#include <anakin_env/actuation_limits.hpp>
#include <anakin_env/native_codec.hpp>
#include <anakin_sdk/simulation_profile.hpp>
#include <cmath>
#include <limits>

namespace anakin::env::sim_test {
inline constexpr uint16_t align_keys = 0x0403, hold_keys = 0x1003,
                          loco_keys = 0x4001, cr7_keys = 0x1001;
enum class CommandKind { Release, Damping, Position, Invalid };
inline CommandKind classify(const unitree_hg::msg::dds_::LowCmd_& command) {
  if (command.mode_machine() != 5 || command.mode_pr() != 0 ||
      command.crc() != command_crc(command)) return CommandKind::Invalid;
  bool release = true, damping = true;
  for (size_t i = 0; i < command.motor_cmd().size(); ++i) {
    const auto& motor = command.motor_cmd()[i];
    for (const auto value : {motor.q(), motor.dq(), motor.kp(), motor.kd(), motor.tau()})
      if (!std::isfinite(value)) return CommandKind::Invalid;
    release = release && motor.mode() == 0 && motor.q() == 0 && motor.dq() == 0 &&
              motor.kp() == 0 && motor.kd() == 0 && motor.tau() == 0;
    if (i < 29) {
      if (motor.kp() < 0 || motor.kd() < 0) return CommandKind::Invalid;
      damping = damping && motor.mode() == 1 && motor.q() == 0 && motor.dq() == 0 &&
                motor.kp() == 0 && motor.kd() > 0 && motor.tau() == 0;
    } else if (motor.mode() != 0) return CommandKind::Invalid;
  }
  if (release) return CommandKind::Release;
  if (damping) return CommandKind::Damping;
  for (size_t i = 0; i < 29; ++i)
    if (command.motor_cmd()[i].mode() != 1) return CommandKind::Invalid;
  return CommandKind::Position;
}
inline bool uses_projection(ExecutionEncoding encoding) {
  return encoding == ExecutionEncoding::ProjectReference ||
         encoding == ExecutionEncoding::EquivalentReference;
}
inline bool matches_target(const unitree_hg::msg::dds_::LowCmd_& command, const Target& target) {
  if (classify(command) != CommandKind::Position || target.target.points.size() != 1 ||
      target.target.points[0].positions.size() != 29 ||
      target.target.joint_names != joint_names() ||
      (target.model_id != "asap_loco" && target.model_id != "asap_cr7") ||
      !known_execution_contract(target.execution_profile, target.command_semantics, target.gain_profile))
    return false;
  const auto encoding = execution_encoding(target.execution_profile, target.command_semantics);
  const auto& gains = simulation_gains(target.gain_profile, target.model_id);
  const auto lower = reference_lower_limits(), upper = reference_upper_limits();
  for (size_t i = 0; i < 29; ++i) {
    const auto mux = target.target.points[0].positions[i];
    if (!std::isfinite(mux)) return false;
    float q = static_cast<float>(mux), tau = 0;
    if (uses_projection(encoding)) {
      q = static_cast<float>(std::clamp(mux, lower[i], upper[i]));
      if (q < lower[i]) q = std::nextafter(q, std::numeric_limits<float>::infinity());
      if (q > upper[i]) q = std::nextafter(q, -std::numeric_limits<float>::infinity());
      if (encoding == ExecutionEncoding::EquivalentReference)
        tau = static_cast<float>(static_cast<double>(static_cast<float>(gains.kp[i])) *
                                 (static_cast<double>(static_cast<float>(mux)) - q));
    }
    const auto& motor = command.motor_cmd()[i];
    if (!std::isfinite(q) || !std::isfinite(tau) || std::abs(motor.q() - q) >= 1e-6f ||
        std::abs(motor.tau() - tau) >= 1e-6f || motor.dq() != 0 ||
        motor.kp() != static_cast<float>(gains.kp[i]) || motor.kd() != static_cast<float>(gains.kd[i]))
      return false;
  }
  return true;
}
inline bool loss_kept_policy(const Frame& before, const Frame& after) {
  return before.execution_mode == Frame::MODE_ACTIVE && after.execution_mode == Frame::MODE_ACTIVE &&
         before.epoch == after.epoch && before.request_id_seen == after.request_id_seen &&
         (after.validity & Frame::VALID_COMMAND) && !(after.validity & Frame::VALID_RC) &&
         after.command.twist.linear.x == 0 && after.command.twist.linear.y == 0 &&
         after.command.twist.angular.z == 0 && after.sdk_publish_count > before.sdk_publish_count;
}
}  // namespace anakin::env::sim_test
