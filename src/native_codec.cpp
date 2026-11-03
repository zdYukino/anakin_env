#include <anakin_env/native_codec.hpp>
#include <anakin_sdk/simulation_profile.hpp>
#include <cmath>
#include <cstring>
#include <limits>
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
uint32_t command_crc(const unitree_hg::msg::dds_::LowCmd_& c) {
  // Explicit HG words keep CRC independent of C++ padding changed by copies.
  std::array<uint32_t, 250> words{};
  static_assert(sizeof(c) == 251 * sizeof(uint32_t), "Pinned HG command layout changed");
  size_t index = 0;
  words[index++] = c.mode_pr() | static_cast<uint32_t>(c.mode_machine()) << 8;
  const auto bits = [](float value) { uint32_t out; std::memcpy(&out, &value, sizeof(out)); return out; };
  for (const auto& motor : c.motor_cmd()) {
    words[index++] = motor.mode();
    for (const auto value : {motor.q(), motor.dq(), motor.tau(), motor.kp(), motor.kd()})
      words[index++] = bits(value);
    words[index++] = motor.reserve();
  }
  for (const auto reserve : c.reserve()) words[index++] = reserve;
  return crc_words(words.data(), words.size());
}
void finalize_command(unitree_hg::msg::dds_::LowCmd_& command) {
  const auto bytes = [](auto& field) { return reinterpret_cast<unsigned char*>(&field); };
  std::fill(bytes(command.mode_machine()) + 1, bytes(command.motor_cmd()[0]), 0);
  for (auto& motor : command.motor_cmd())
    std::fill(bytes(motor.mode()) + 1, bytes(motor.q()), 0);
  command.crc() = command_crc(command);
}
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
unitree_hg::msg::dds_::LowCmd_ encode_pd(const PdReference& reference, uint8_t mode_machine, bool enabled) {
  unitree_hg::msg::dds_::LowCmd_ cmd{};
  cmd.mode_pr() = 0;
  cmd.mode_machine() = mode_machine;
  const auto narrow = [](double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
      throw std::runtime_error("PD field is not representable as finite SDK float");
    const auto result = static_cast<float>(value);
    if (!std::isfinite(result)) throw std::runtime_error("Nonfinite narrowed SDK PD field");
    return result;
  };
  for (size_t i = 0; i < 29; ++i) {
    auto& m = cmd.motor_cmd()[i];
    m.mode() = enabled ? 1 : 0;
    m.q() = narrow(reference.q[i]);
    m.dq() = narrow(reference.dq[i]);
    m.tau() = narrow(reference.tau[i]);
    m.kp() = narrow(reference.kp[i]);
    m.kd() = narrow(reference.kd[i]);
    if (reference.kp[i] < 0 || reference.kd[i] < 0)
      throw std::runtime_error("Negative SDK PD gain");
  }
  finalize_command(cmd);
  return cmd;
}
unitree_hg::msg::dds_::LowCmd_ encode_policy_pd(const Positions& q, uint8_t mode_machine,
    const std::string& profile, const std::string& model) {
  PdReference reference;
  reference.q = q;
  const auto& gains = simulation_gains(profile, model);
  reference.kp = gains.kp;
  reference.kd = gains.kd;
  return encode_pd(reference, mode_machine);
}
unitree_hg::msg::dds_::LowCmd_ encode_position(const Positions& q, uint8_t mode_machine,
    const std::string& profile, const std::string& model) {
  for (size_t i = 0; i < q.size(); ++i)
    if (!std::isfinite(q[i]) || q[i] < lower_limits()[i] || q[i] > upper_limits()[i])
      throw std::runtime_error("Encoder position bounds at " + joint_names()[i]);
  return encode_policy_pd(q, mode_machine, profile, model);
}
unitree_hg::msg::dds_::LowCmd_ encode_damping(uint8_t mode_machine) {
  PdReference reference;
  reference.kd.fill(2.);
  return encode_pd(reference, mode_machine);
}
unitree_hg::msg::dds_::LowCmd_ encode_release(uint8_t mode_machine) { return encode_pd({}, mode_machine, false); }
}  // namespace anakin::env
