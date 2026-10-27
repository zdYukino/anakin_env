#include <anakin_env/control.hpp>
#include <anakin_env/execution.hpp>
#include <anakin_env/transport.hpp>
#include <anakin_sdk/simulation_profile.hpp>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
using namespace std::chrono_literals;
using namespace anakin::env;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void execution_regressions(Clock::time_point now) {
  DeadlineSchedule schedule(now, 100ms);
  check(schedule.due(now), "First stop slot");
  check(!schedule.due(now), "No same-time catch-up burst");
  check(schedule.due(now + 2ms), "Second stop slot");
  check(schedule.due(now + 86ms), "Delayed stop slot skips missed periods");
  check(schedule.next() == now + 88ms && !schedule.due(now + 86ms),
        "Skipped stop slots stay skipped");
  check(schedule.due(now + 99ms), "Last pre-deadline stop slot");
  for (const auto delay : {100ms, 105ms, 1000ms})
    check(schedule.done(now + delay) && !schedule.due(now + delay),
          "No stop write at or after real deadline");
  ActiveRunAcceptance active;
  active.begin("asap_loco", 1);
  for (int i = 0; i < 100; ++i) active.sdk_write(now + i * 2ms, false, "asap_loco", 1);
  check(
      active.tail_seconds() == 0 && active.total_seconds() == 0 && !active.confirm("asap_loco", 1),
      "Transition entry cannot count active time or completion");
  active.sdk_write(now + 200ms, true, "asap_loco", 1);
  active.sdk_write(now + 202ms, true, "asap_loco", 1);
  check(
      active.tail_seconds() > .0019 && active.confirm("asap_loco", 1) && active.sequence_matches(1),
      "Actual TARGET_ACTIVE writes count and confirm");
  active.sdk_write(now + 243ms, true, "asap_loco", 1);
  check(active.tail_seconds() == 0, "A write gap beyond TTL breaks continuous active time");
  active.sdk_write(now + 245ms, true, "asap_loco", 1);
  active.sdk_write(now + 247ms, false, "asap_loco", 1);
  check(active.tail_seconds() == 0, "Transition phase breaks active tail");
  for (uint64_t epoch = 2; epoch <= 3; ++epoch) {
    active.begin("asap_loco", epoch);
    check(active.tail_seconds() == 0, "New epoch starts a new active tail");
    active.sdk_write(now, true, "asap_loco", epoch);
    check(active.confirm("asap_loco", epoch), "Fresh same-model epoch can confirm");
  }
  check(active.models().size() == 1 && !active.sequence_matches(3),
        "Repeated loco requests cannot impersonate a three-model sequence");
  active.begin("asap_cr7", 4);
  active.sdk_write(now, true, "asap_loco", 3);
  check(!active.confirm("asap_cr7", 4), "Previous epoch write cannot confirm next model");
  active.sdk_write(now, true, "asap_cr7", 4);
  check(active.confirm("asap_cr7", 4) && active.sequence_matches(2), "Ordered second model");
  active.begin("asap_loco", 5);
  check(!active.sequence_matches(3), "Pending third model cannot count");
  active.sdk_write(now, true, "asap_loco", 5);
  check(active.confirm("asap_loco", 5) && active.sequence_matches(3) &&
            !active.confirm("asap_cr7", 4),
        "Ordered loco/CR7/loco with fresh epochs");
  bool threw = false;
  try {
    active.begin("asap_loco", 5);
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "Acceptance epoch cannot be reused");
}
void dump_gains() {
  using namespace anakin;
  std::cout << std::setprecision(10) << '{';
  bool first = true;
  for (const auto* profile : {legacy_gain_profile, merged_gain_profile}) {
    if (!first) std::cout << ',';
    first = false;
    std::cout << std::quoted(profile) << ":{";
    bool first_model = true;
    for (const auto* model : {"asap_loco", "asap_cr7"}) {
      if (!first_model) std::cout << ',';
      first_model = false;
      const auto packet = encode_position(Positions{}, 5, profile, model);
      check(packet.crc() == command_crc(packet), "Profile packet CRC");
      std::cout << std::quoted(model) << ":{\"kp\":[";
      for (size_t i = 0; i < 29; ++i) {
        if (i) std::cout << ',';
        std::cout << packet.motor_cmd()[i].kp();
      }
      std::cout << "],\"kd\":[";
      for (size_t i = 0; i < 29; ++i) {
        if (i) std::cout << ',';
        std::cout << packet.motor_cmd()[i].kd();
      }
      std::cout << "]}";
    }
    std::cout << '}';
  }
  std::cout << "}\n";
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--dump-gains") {
      dump_gains();
      return 0;
    }
    const auto now = Clock::now();
    execution_regressions(now);
    for (const auto* profile : {anakin::legacy_gain_profile, anakin::merged_gain_profile}) {
      bool invalid_model = false;
      try {
        encode_position(Positions{}, 5, profile, "unapproved_model");
      } catch (const std::exception&) {
        invalid_model = true;
      }
      check(invalid_model, "Unknown producing model cannot select gains");
    }
    bool invalid_profile = false;
    try {
      encode_position(Positions{}, 5, "unapproved_profile", "asap_loco");
    } catch (const std::exception&) {
      invalid_profile = true;
    }
    check(invalid_profile, "Unknown gain profile cannot encode a command");
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
                 "loopback gate, finite feedback, CRC, absolute stop deadline and active/model "
                 "acceptance\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
