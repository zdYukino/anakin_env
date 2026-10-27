#include <anakin_env/native_codec.hpp>
#include <anakin_sdk/simulation_profile.hpp>
#include <cmath>
#include <cstring>
namespace anakin::env {
uint32_t crc_words(const uint32_t* words, size_t count) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < count; ++i) {
    uint32_t bit = 0x80000000;
    for (unsigned b = 0; b < 32; ++b) {
      const bool high = crc & 0x80000000;
      crc <<= 1;
      if (high) crc ^= 0x04c11db7;
      if (words[i] & bit) crc ^= 0x04c11db7;
      bit >>= 1;
    }
  }
  return crc;
}
template <class T>
uint32_t native_crc(const T& value) {
  static_assert(sizeof(T) % 4 == 0, "SDK layout is not word aligned");
  std::array<uint32_t, sizeof(T) / 4> words{};
  std::memcpy(words.data(), &value, sizeof(T));
  return crc_words(words.data(), words.size() - 1);
}
uint32_t command_crc(const unitree_hg::msg::dds_::LowCmd_& c) { return native_crc(c); }
bool valid_state_crc(const unitree_hg::msg::dds_::LowState_& s) { return s.crc() == native_crc(s); }
Frame decode_state(const unitree_hg::msg::dds_::LowState_& s) {
  if (!valid_state_crc(s)) throw std::runtime_error("LowState CRC mismatch");
  Frame f;
  f.schema_version = schema_version;
  f.schema_id = schema_id;
  f.sdk_tick = s.tick();
  f.validity = Frame::VALID_JOINTS | Frame::VALID_IMU;
  f.joints.name = joint_names();
  for (size_t i = 0; i < 29; ++i) {
    const auto& motor = s.motor_state()[i];
    if (!std::isfinite(motor.q()) || !std::isfinite(motor.dq()) || !std::isfinite(motor.tau_est()))
      throw std::runtime_error("Nonfinite native joint feedback");
    f.joints.position.push_back(s.motor_state()[i].q());
    f.joints.velocity.push_back(s.motor_state()[i].dq());
    f.joints.effort.push_back(s.motor_state()[i].tau_est());
  }
  const auto& imu = s.imu_state();
  double norm = 0;
  for (float value : imu.quaternion()) {
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite native quaternion");
    norm += value * value;
  }
  if (std::abs(norm - 1.) > .001) throw std::runtime_error("Non-unit native quaternion");
  for (float value : imu.gyroscope())
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite native gyro");
  for (float value : imu.accelerometer())
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite native acceleration");
  f.imu.orientation.w = imu.quaternion()[0];
  f.imu.orientation.x = imu.quaternion()[1];
  f.imu.orientation.y = imu.quaternion()[2];
  f.imu.orientation.z = imu.quaternion()[3];
  f.imu.angular_velocity.x = imu.gyroscope()[0];
  f.imu.angular_velocity.y = imu.gyroscope()[1];
  f.imu.angular_velocity.z = imu.gyroscope()[2];
  f.imu.linear_acceleration.x = imu.accelerometer()[0];
  f.imu.linear_acceleration.y = imu.accelerometer()[1];
  f.imu.linear_acceleration.z = imu.accelerometer()[2];
  return f;
}
unitree_hg::msg::dds_::LowCmd_ encode_position(const Positions& q, uint8_t mode_machine,
                                               const std::string& profile,
                                               const std::string& model) {
  const auto& gains = simulation_gains(profile, model);
  unitree_hg::msg::dds_::LowCmd_ cmd{};
  cmd.mode_pr() = 0;
  cmd.mode_machine() = mode_machine;
  for (size_t i = 0; i < 29; ++i) {
    if (!std::isfinite(q[i]) || q[i] < lower_limits()[i] || q[i] > upper_limits()[i])
      throw std::runtime_error("Encoder position bounds at " + joint_names()[i] +
                               " q=" + std::to_string(q[i]));
    auto& m = cmd.motor_cmd()[i];
    m.mode() = 1;
    m.q() = q[i];
    m.dq() = 0;
    m.tau() = 0;
    m.kp() = gains.kp[i];
    m.kd() = gains.kd[i];
  }
  cmd.crc() = command_crc(cmd);
  return cmd;
}
unitree_hg::msg::dds_::LowCmd_ encode_damping(uint8_t mode_machine) {
  unitree_hg::msg::dds_::LowCmd_ command{};
  command.mode_pr() = 0;
  command.mode_machine() = mode_machine;
  for (size_t i = 0; i < 29; ++i) {
    auto& motor = command.motor_cmd()[i];
    motor.mode() = 1;
    motor.kp() = 0;
    motor.kd() = 2;
    motor.q() = 0;
    motor.dq() = 0;
    motor.tau() = 0;
  }
  command.crc() = command_crc(command);
  return command;
}
}  // namespace anakin::env
