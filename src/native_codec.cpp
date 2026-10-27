#include <anakin_env/native_codec.hpp>
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
    f.joints.position.push_back(s.motor_state()[i].q());
    f.joints.velocity.push_back(s.motor_state()[i].dq());
    f.joints.effort.push_back(s.motor_state()[i].tau_est());
  }
  const auto& imu = s.imu_state();
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
unitree_hg::msg::dds_::LowCmd_ encode_position(const Positions& q, uint8_t mode_machine) {
  // This codec does not open any network channel or confer execution authority.
  const Positions kp = {100, 100, 100, 200, 20, 20, 100, 100, 100, 200, 20, 20, 400, 400, 400,
                        90,  60,  20,  60,  4,  4,  4,   90,  60,  20,  60, 4,  4,   4};
  const Positions kd = {2.5, 2.5, 2.5, 5, .2, .1, 2.5, 2.5, 2.5, 5,  .2, .1, 5,  5, 5,
                        2,   1,   .4,  1, .2, .2, .2,  2,   1,   .4, 1,  .2, .2, .2};
  unitree_hg::msg::dds_::LowCmd_ cmd{};
  cmd.mode_pr() = 0;
  cmd.mode_machine() = mode_machine;
  for (size_t i = 0; i < 29; ++i) {
    if (!std::isfinite(q[i]) || q[i] < lower_limits()[i] || q[i] > upper_limits()[i])
      throw std::runtime_error("Encoder position bounds");
    auto& m = cmd.motor_cmd()[i];
    m.mode() = 1;
    m.q() = q[i];
    m.dq() = 0;
    m.tau() = 0;
    m.kp() = kp[i];
    m.kd() = kd[i];
  }
  cmd.crc() = command_crc(cmd);
  return cmd;
}
}  // namespace anakin::env
