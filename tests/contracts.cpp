#include <anakin_env/confirmation.hpp>
#include <anakin_env/gate.hpp>
#include <anakin_env/native_codec.hpp>
#include <cstring>
#include <iostream>
#include <limits>
using namespace std::chrono_literals;
void check(bool value, const char* name) {
  if (!value) throw std::runtime_error(name);
}
int main() {
  try {
    using namespace anakin;
    using namespace anakin::env;
    Gate gate;
    auto session = new_uuid(), mc = new_uuid(), request = new_uuid();
    gate.begin(session, mc, request, 4, "asap_loco", true);
    auto base = Clock::now();
    Frame f;
    f.session_id = session;
    f.epoch = 4;
    f.state_seq = 11;
    f.validity = 3;
    f.joints.name = joint_names();
    f.joints.position.assign(29, 0);
    f.joints.velocity.assign(29, 0);
    gate.state(f, base);
    Target t;
    t.schema_version = schema_version;
    t.schema_id = schema_id;
    t.session_id = session;
    t.mc_instance_id = mc;
    t.request_id = request;
    t.epoch = 4;
    t.seq = 1;
    t.source_state_seq = 11;
    t.model_id = "asap_loco";
    t.gain_profile = "g1_asap_sim_v1";
    t.validity = 1;
    t.valid_for.nanosec = 40000000;
    t.phase = Target::TARGET_ACTIVE;
    t.target.joint_names = joint_names();
    trajectory_msgs::msg::JointTrajectoryPoint p;
    p.positions.assign(29, 0);
    t.target.points.push_back(p);
    std::string why;
    check(gate.accept(t, base + 1ms, why), "valid target");
    check(gate.live(base + 39ms), "valid TTL");
    check(!gate.accept(t, base + 2ms, why) && why == "sequence", "duplicate cannot renew TTL");
    check(!gate.live(base + 40ms), "finite hold after MC exit");
    t.seq = 2;
    t.epoch = 3;
    check(!gate.accept(t, base + 2ms, why) && why == "epoch", "old epoch");
    t.epoch = 4;
    t.mc_instance_id = new_uuid();
    check(!gate.accept(t, base + 2ms, why), "restarted MC");
    t.mc_instance_id = mc;
    t.target.points[0].positions[1] = std::numeric_limits<double>::quiet_NaN();
    check(!gate.accept(t, base + 2ms, why), "NaN");
    t.target.points[0].positions[1] = 0;
    t.target.points[0].positions[0] = 99;
    check(!gate.accept(t, base + 2ms, why) && why == "position limit/nonfinite", "physical bound");
    t.target.points[0].positions[0] = 0;
    t.target.points[0].positions[23] = .36;
    check(!gate.accept(t, base + 2ms, why) && why == "target discontinuity", "jump");
    t.target.points[0].positions[23] = 0;
    t.source_state_seq = 10;
    check(!gate.accept(t, base + 2ms, why) && why == "unknown source state", "unknown source");
    t.source_state_seq = 11;
    check(!gate.accept(t, base + 41ms, why), "stale source");
    t.seq = 100;
    check(gate.accept(t, base + 3ms, why), "latest-depth=1 skipped first active sequence");
    check(gate.phase_mask() == Target::TARGET_ACTIVE, "cumulative active ACK");
    gate.begin(session, mc, new_uuid(), 5, "asap_cr7", true);
    check(!gate.live(base + 4ms), "epoch advance immediately invalidates target");
    check(!gate.accept(t, base + 4ms, why), "old request");
    gate.stop();
    check(!gate.live(base + 4ms), "local stop independent of ACK");
    // Independent word-vector oracle; expected computed by the offline bitwise fixture.
    const uint32_t words[] = {0x12345678, 0x90abcdef, 0x00010203};
    check(crc_words(words, 3) == 0x7d254b69u, "independent CRC golden vector");
    std::cout << "CRC_VECTOR=" << std::hex << crc_words(words, 3) << std::dec << '\n';
    Positions q{};
    for (size_t i = 0; i < 29; ++i) q[i] = .002 * static_cast<double>(i) - .028;
    auto command = encode_position(q, 7);
    check(command.crc() == command_crc(command), "native command CRC");
    check(command.motor_cmd()[23].q() == static_cast<float>(q[23]) &&
              command.motor_cmd()[16].q() == static_cast<float>(q[16]) &&
              command.motor_cmd()[25].q() == static_cast<float>(q[25]),
          "asymmetric SDK motor map");
    check(command.motor_cmd()[25].dq() == 0 && command.motor_cmd()[25].tau() == 0,
          "position target dq/tau zero");
    check(command.mode_machine() == 7, "mode_machine forwarded");
    command.motor_cmd()[23].q() += .02f;
    check(command.crc() != command_crc(command), "CRC corruption detection");
    unitree_hg::msg::dds_::LowState_ state{};
    state.tick() = 42;
    state.imu_state().quaternion()[0] = 1;
    for (size_t i = 0; i < 29; ++i) state.motor_state()[i].q() = q[i];
    std::array<uint32_t, sizeof(state) / 4> raw{};
    std::memcpy(raw.data(), &state, sizeof(state));
    state.crc() = crc_words(raw.data(), raw.size() - 1);
    auto decoded = decode_state(state);
    check(decoded.sdk_tick == 42 && decoded.joints.position[23] != decoded.joints.position[16] &&
              decoded.imu.orientation.w == 1,
          "LowState snapshot");
    state.crc() ^= 1;
    check(!valid_state_crc(state), "LowState bad CRC");
    Gate stopped;
    stopped.begin(session, mc, request, 4, "asap_loco", true);
    stopped.state(f, base);
    Target late = t;
    late.session_id = session;
    late.mc_instance_id = mc;
    late.request_id = request;
    late.epoch = 4;
    late.source_state_seq = 11;
    late.seq = 200;
    late.target.points[0].positions.assign(29, 0);
    check(stopped.accept(late, base + 1ms, why), "pre-stop valid target");
    stopped.stop();
    ++late.seq;
    check(!stopped.accept(late, base + 2ms, why) && why == "stop latched",
          "late same-epoch target cannot undo stop");
    bool refused = false;
    try {
      stopped.begin(session, mc, new_uuid(), 4, "asap_loco", true);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    check(refused, "same-epoch reauthorization refused");
    auto recovery = new_uuid();
    stopped.begin(session, mc, recovery, 5, "asap_loco", true);
    auto fresh = f;
    fresh.epoch = 5;
    fresh.state_seq = 12;
    stopped.state(fresh, base + 3ms);
    late.epoch = 5;
    late.request_id = recovery;
    late.source_state_seq = 12;
    late.seq = 1;
    check(stopped.accept(late, base + 4ms, why), "explicit new transaction/epoch reauthorizes");
    ActiveConfirmation confirmation;
    confirmation.begin(session, mc, request, 4);
    anakin_sdk::msg::InferenceStatus status;
    status.session_id = session;
    status.mc_instance_id = mc;
    status.request_id = request;
    status.epoch = 4;
    status.accepted = true;
    status.state = anakin_sdk::msg::InferenceStatus::ACTIVE;
    status.published_phase_mask = Target::TARGET_ACTIVE;
    status.phase_first_seq[2] = 10;
    status.phase_last_seq[2] = 10;
    confirmation.status(status);
    check(!confirmation.active(), "published is not received");
    // Drop sequence 10 and deliver status first on every subsequent iteration.
    for (uint64_t seq = 11; seq < 15; ++seq) {
      status.phase_last_seq[2] = seq;
      confirmation.status(status);
      auto receipt = t;
      receipt.seq = seq;
      receipt.phase = Target::TARGET_ACTIVE;
      confirmation.target(receipt);
      check(confirmation.active(), "status-first plus lost first active cannot deadlock");
    }
    status.state = anakin_sdk::msg::InferenceStatus::FAILED;
    confirmation.status(status);
    check(!confirmation.active(), "late FAILED revokes confirmation");
    TransactionHealth health;
    health.begin(base + 5s);
    health.tick(base + 1s, false, false);
    check(!health.failed(), "bounded warmup waiting permitted");
    health.valid_target();
    health.tick(base + 2s, true, true);
    check(health.finish(true, true), "healthy active transaction");
    health.fail();
    check(!health.finish(true, true), "historical success cannot mask FAILED tail");
    health.begin(base + 5s);
    health.valid_target();
    health.tick(base + 2s, false, true);
    check(health.failed(), "stale tail is terminal");
    health.begin(base + 5s);
    health.tick(base + 5s, false, false);
    check(health.failed(), "warmup deadline is finite");
    std::cout << "PASS: TTL, stale/epoch/instance, discontinuity, bounds, cumulative ACK, local "
                 "stop, native HG encode/decode/CRC/mapping\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
