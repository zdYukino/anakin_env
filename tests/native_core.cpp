#include <anakin_env/control.hpp>
#include <anakin_env/transport.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
using namespace std::chrono_literals;
using namespace anakin::env;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int main() {
  try {
    const auto now = Clock::now();
    RemoteEdges edges;
    RemoteSample remote;
    remote.valid = true;
    remote.keys = 3u | (1u << 10);
    check(!edges.update(remote).x, "Held startup buttons cannot arm");
    remote.keys = 0;
    edges.update(remote);
    remote.keys = 1u << 10;
    check(!edges.update(remote).x, "X without both shoulders cannot arm");
    remote.keys = 3u;
    edges.update(remote);
    remote.keys |= 1u << 10;
    check(edges.update(remote).x, "Deliberate fresh X edge");
    check(!edges.update(remote).x, "Held X does not repeat");
    remote.keys |= 1u << 8;
    check(edges.update(remote).stop, "A is a local stop without waiting for ACK");
    remote.keys = 0;
    edges.update(remote);
    remote.keys = 3u | (1u << 10) | (1u << 8);
    const auto simultaneous = edges.update(remote);
    check(simultaneous.stop && !simultaneous.x, "Stop has priority over a simultaneous arm edge");
    LocalControl control;
    check(!control.authorized(), "Construction is disarmed");
    check(control.stand(Positions{}, now), "Deliberate stand authorization");
    auto midpoint = control.local_target(now + 1s);
    check(std::abs(midpoint[3] - .15) < 1e-12, "Measured-pose interpolation");
    check(control.mode() == ExecutionMode::Aligning, "Alignment takes full two seconds");
    control.local_target(now + 2s);
    check(control.mode() == ExecutionMode::Hold, "Alignment cannot start inference implicitly");
    control.prepare();
    control.active();
    control.stop(now + 3s, true);
    check(!control.authorized() && control.mode() == ExecutionMode::Fault,
          "Terminal local revocation");
    check(control.damping(now + 3050ms) && !control.damping(now + 3100ms),
          "Damping has a finite deadline");
    check(!control.stand(Positions{}, now + 4s), "Fault cannot be silently rearmed");
    bool threw = false;
    try {
      control.active();
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "Late target does not authorize execution");
    std::array<uint8_t, 40> bytes{};
    check(!decode_remote(bytes).valid, "Absent remote is unavailable, not a zero command");
    bytes[0] = 0xfe;
    bytes[1] = 0xef;
    bytes[2] = 3;
    bytes[3] = 4;
    const float axes[] = {.1f, .2f, -.3f, .4f};
    const size_t offsets[] = {4, 20, 8, 12};
    for (size_t i = 0; i < 4; ++i) std::memcpy(bytes.data() + offsets[i], axes + i, 4);
    auto decoded = decode_remote(bytes);
    check(decoded.valid && decoded.keys == (3u | (4u << 8)), "RC bit mapping");
    for (size_t i = 0; i < 4; ++i) check(decoded.axes[i] == axes[i], "RC asymmetric axis mapping");
    const float nan = std::nanf("");
    std::memcpy(bytes.data() + 4, &nan, 4);
    threw = false;
    try {
      decode_remote(bytes);
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "Nonfinite remote axes rejected");
    const auto damping = encode_damping(5);
    check(damping.crc() == command_crc(damping), "Damping CRC");
    for (size_t i = 0; i < 29; ++i)
      check(damping.motor_cmd()[i].kp() == 0 && damping.motor_cmd()[i].kd() > 0 &&
                damping.motor_cmd()[i].tau() == 0,
            "Damping cannot retain a position target");
    MotionCommand commands;
    RemoteSample command_axes;
    command_axes.valid = true;
    command_axes.axes = {-.4f, 1.f, .2f, .03f};
    check(commands.velocity(command_axes) == std::array<double, 3>{0, 0, 0},
          "Legacy stand default suppresses velocity commands");
    RemoteIntent move;
    move.motion_toggle = true;
    commands.update(move);
    const auto velocity = commands.velocity(command_axes);
    check(std::abs(velocity[0] - .5) < 1e-12 && std::abs(velocity[1] - .2) < 1e-7 &&
              std::abs(velocity[2] + .2) < 1e-7,
          "Legacy RC command scales and asymmetric signs");
    RemoteIntent up;
    up.height_up = true;
    commands.update(up);
    check(std::abs(commands.height() - .83) < 1e-12, "Legacy RC height step");
    LocalControl fast;
    fast.set_alignment_seconds(.25);
    fast.stand(Positions{}, now);
    check(std::abs(fast.local_target(now + 125ms)[3] - .15) < 1e-12,
          "Explicit simulation alignment duration");
    check(fast.mode() == ExecutionMode::Aligning, "Configured alignment still waits full duration");
    fast.local_target(now + 250ms);
    check(fast.mode() == ExecutionMode::Hold, "Configured alignment does not authorize inference");
    geometry_msgs::msg::Quaternion yaw90;
    yaw90.w = std::sqrt(.5);
    yaw90.z = std::sqrt(.5);
    const auto body_velocity = world_to_body(yaw90, {1.f, 0.f, .25f});
    check(std::abs(body_velocity[0]) < 1e-12 && std::abs(body_velocity[1] + 1) < 1e-12 &&
              std::abs(body_velocity[2] - .25) < 1e-12,
          "Odometry world velocity rotates into child base frame");
    NativeTransport transport;
    check(!transport.write(damping), "Transport construction opens no publisher");
    threw = false;
    try {
      require_loopback("eth0", 31);
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "Physical interface rejected before channel creation");
    threw = false;
    try {
      require_loopback("lo", 0);
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "Default physical DDS domain rejected");
    LowState state{};
    state.imu_state().quaternion()[0] = 1;
    std::array<uint32_t, sizeof(LowState) / 4> words{};
    std::memcpy(words.data(), &state, sizeof(state));
    state.crc() = crc_words(words.data(), words.size() - 1);
    check(decode_state(state).imu.orientation.w == 1, "Native wxyz to ROS xyzw");
    state.motor_state()[4].dq() = nan;
    std::memcpy(words.data(), &state, sizeof(state));
    state.crc() = crc_words(words.data(), words.size() - 1);
    threw = false;
    try {
      decode_state(state);
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "CRC-valid nonfinite feedback rejected");
    std::cout << "PASS native RC mapping/edges, disarmed construction, FSM, finite stop/damping, "
                 "loopback gate, finite feedback and CRC\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
