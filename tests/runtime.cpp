// Production controller/encoder/feedback/startup paths with a record-only backend.
#include <anakin_env/runtime.hpp>
#include <anakin_env/startup.hpp>
#include <anakin_env/control.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

using namespace anakin;
using namespace anakin::env;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const std::string& reason) { if (!condition) throw std::runtime_error(reason); }
template<class Fn> void rejected(Fn&& fn, const char* reason) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error(reason);
}
uint32_t object_crc(const LowCommand& command) {
  std::array<uint32_t, sizeof(command) / 4> words{};
  std::memcpy(words.data(), &command, sizeof(command));
  return crc_words(words.data(), words.size() - 1);
}
struct Backend : RuntimeBackend {
  bool simulation, owned, fail_write = false;
  AuthorityProgress progress = AuthorityProgress::Pending;
  uint64_t ticket = 0;
  std::vector<std::string> actions;
  std::vector<LowCommand> commands;
  explicit Backend(bool sim) : simulation(sim), owned(sim) {}
  bool authority_owned() const override { return owned; }
  void begin_acquire(uint64_t generation, std::chrono::milliseconds) override {
    ticket = generation; actions.push_back("acquire");
  }
  AuthorityProgress poll_acquire(uint64_t generation) override {
    check(ticket == generation, "Authority poll is generation-bound");
    if (progress == AuthorityProgress::Owned) owned = true;
    return progress;
  }
  void cancel_acquire(uint64_t generation) override {
    check(ticket == generation, "Cancel is bound to the pending acquire");
    actions.push_back("cancel"); ticket = 0;
  }
  void release_authority() override {
    actions.push_back("authority_release");
    if (!simulation) owned = false;
  }
  bool write(const LowCommand& command) override {
    check(owned, "Unowned writer was called");
    check(command.crc() == object_crc(command), "Actual object handed to writer has valid zero-padding CRC");
    if (fail_write) return false;
    actions.push_back(command.motor_cmd()[0].mode() == 0 ? "zero_release" : "pd");
    commands.push_back(command);
    return true;
  }
};
NativeSample sample(uint64_t generation, uint32_t tick, Clock::time_point now, bool fallen = false) {
  NativeSample out;
  out.generation = generation; out.acquired = now; out.mode_machine = 5;
  out.frame.schema_version = schema_version; out.frame.schema_id = schema_id;
  out.frame.sdk_tick = tick; out.frame.validity = 3;
  out.frame.joints.name = joint_names(); out.frame.joints.position.assign(29, 0.);
  out.frame.joints.velocity.assign(29, 0.);
  out.frame.imu.orientation.w = fallen ? 0. : 1.;
  out.frame.imu.orientation.x = fallen ? 1. : 0.;
  return out;
}
struct Fixture {
  Clock::time_point now = Clock::time_point{} + 10s;
  ModeConfig mode;
  Backend backend;
  InputSnapshot input;
  RuntimeController core;
  uint64_t generation = 0;
  uint32_t sdk_tick = 100;
  Uuid instance = new_uuid();
  std::optional<Frame> frame;
  Fixture(ModeConfig config, bool initial_feedback = true)
      : mode(std::move(config)), backend(mode.mode == RuntimeMode::Sim && !mode.real_checks),
        core(mode, backend, [this] { return input; }, [this] { return now; }) {
    input.primary_online = true; input.primary_acquired = now;
    if (initial_feedback) core.feedback(sample(++generation, sdk_tick, now));
  }
  ActionPlan cycle(const std::vector<Event>& events = {}, bool physical = true, bool fallen = false) {
    now += 2ms; input.primary_acquired = now;
    if (physical) core.feedback(sample(++generation, ++sdk_tick, now, fallen));
    auto plan = core.tick(InputFrame{input, {}, events});
    frame = core.take_frame(input);
    return plan;
  }
  void discover() {
    RuntimeStatus status;
    status.schema_version = schema_version; status.schema_id = schema_id;
    status.mc_instance_id = instance;
    core.status(status);
  }
  Target target(uint64_t sequence = 1) {
    check(core.request() && frame, "Target fixture requires a published transaction frame");
    const auto& request = *core.request();
    Target target;
    target.schema_version = schema_version; target.schema_id = schema_id;
    target.session_id = request.session_id; target.mc_instance_id = request.expected_mc_instance_id;
    target.request_id = request.request_id; target.epoch = request.next_epoch;
    target.seq = sequence; target.source_state_seq = frame->state_seq;
    target.model_id = request.target_model; target.gain_profile = request.gain_profile;
    target.execution_profile = request.execution_profile; target.command_semantics = request.command_semantics;
    target.validity = 1; target.valid_for.nanosec = 40000000; target.phase = Target::TARGET_ACTIVE;
    target.target.joint_names = joint_names();
    trajectory_msgs::msg::JointTrajectoryPoint point;
    const auto pose = stand_pose(); point.positions.assign(pose.begin(), pose.end());
    point.positions[10] = .5416049427; // Original right-ankle PD reference beyond mechanical .5236.
    point.positions[0] = 4.; // Neither a mechanical position nor first-target .35 continuity bound.
    target.target.points.push_back(point);
    return target;
  }
  void active_status(const Target& target, bool done = false) {
    RuntimeStatus status;
    status.schema_version = schema_version; status.schema_id = schema_id;
    status.session_id = target.session_id; status.mc_instance_id = target.mc_instance_id;
    status.request_id = target.request_id; status.epoch = target.epoch;
    status.requested_model = core.request()->target_model; status.active_model = target.model_id;
    status.gain_profile = target.gain_profile; status.execution_profile = target.execution_profile;
    status.command_semantics = target.command_semantics; status.accepted = true;
    status.state = RuntimeStatus::ACTIVE; status.target_active = true; status.target_ready = true;
    status.motion_done = done; status.published_phase_mask = Target::TARGET_ACTIVE;
    status.phase_first_seq[2] = target.seq; status.phase_last_seq[2] = target.seq;
    core.status(status);
  }
};

void pd() {
  PdReference reference;
  for (size_t i = 0; i < 29; ++i) {
    reference.q[i] = -.5 + .1 * i; reference.dq[i] = .001 * i;
    reference.kp[i] = 10 + i; reference.kd[i] = .1 + .02 * i; reference.tau[i] = -.03 * i;
  }
  reference.q[10] = .5416049427;
  auto sim = encode_pd(reference, 5);
  auto real = encode_pd(reference, 5);
  finalize_command(sim); finalize_command(real);
  check(sim == real && std::memcmp(&sim, &real, sizeof(sim)) == 0 && sim.crc() == object_crc(sim),
        "Identical SDK PD inputs produce identical actual bytes/CRC regardless of mode");
  for (size_t i = 0; i < 29; ++i) {
    const auto& motor = sim.motor_cmd()[i];
    check(motor.q() == static_cast<float>(reference.q[i]) && motor.dq() == static_cast<float>(reference.dq[i]) &&
          motor.kp() == static_cast<float>(reference.kp[i]) && motor.kd() == static_cast<float>(reference.kd[i]) &&
          motor.tau() == static_cast<float>(reference.tau[i]) && motor.mode() == 1, "All HG PD fields preserve their references");
  }
  check(sim.motor_cmd()[10].q() > upper_limits()[10] && sim.mode_machine() == 5 && sim.mode_pr() == 0,
        "Right ankle reference is neither projected nor compensated");
  const auto standing = stand_pose();
  const auto loco = encode_policy_pd(standing, 5, merged_gain_profile, "asap_loco");
  for (size_t i = 12; i < 29; ++i) {
    const bool wrist = (i >= 19 && i <= 21) || (i >= 26 && i <= 28);
    const auto kp = i < 15 ? 200.f : (wrist ? 20.f : 40.f);
    const auto kd = i < 15 ? 6.f : 2.f;
    const auto& motor = loco.motor_cmd()[i];
    check(motor.mode() == 1 && motor.q() == static_cast<float>(standing[i]) &&
          motor.dq() == 0.f && motor.tau() == 0.f && motor.kp() == kp && motor.kd() == kd,
          "Loco non-policy joints use basic G1 standing PD gains and reference");
  }
  for (size_t i = 29; i < sim.motor_cmd().size(); ++i)
    check(sim.motor_cmd()[i].mode() == 0 && sim.motor_cmd()[i].kp() == 0, "Unused HG motor slots stay disabled");
  LowCommand assigned;
  auto* raw = reinterpret_cast<unsigned char*>(&assigned);
  std::fill(raw, raw + sizeof(assigned), 0xa5);
  assigned = sim;
  finalize_command(assigned);
  check(assigned.crc() == object_crc(assigned) && std::memcmp(&sim, &assigned, sizeof(sim)) == 0,
        "CRC normalization is valid after assignment into poisoned padding");
  for (const auto field : {0, 1, 2, 3, 4}) {
    auto bad = reference;
    auto* values = field == 0 ? &bad.q : field == 1 ? &bad.dq : field == 2 ? &bad.kp : field == 3 ? &bad.kd : &bad.tau;
    (*values)[0] = std::numeric_limits<double>::max();
    rejected([&] { encode_pd(bad, 5); }, "Float overflow in SDK PD field accepted");
    (*values)[0] = std::numeric_limits<double>::quiet_NaN();
    rejected([&] { encode_pd(bad, 5); }, "Nonfinite SDK PD field accepted");
  }
  auto release = encode_release(5); finalize_command(release);
  for (const auto& motor : release.motor_cmd())
    check(motor.mode() == 0 && motor.kp() == 0 && motor.kd() == 0 && motor.tau() == 0,
          "Release clears the simulator's stored-command effort rather than retaining damping");
  check(release.crc() == object_crc(release), "Zero-gain release has a valid actual-object CRC");
}

void availability(const fs::path& configs) {
  const auto real = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  Fixture missing(real, false);
  check(!missing.core.action_available(InputAction::Align, missing.input) && missing.core.action_available(InputAction::EmergencyStop, missing.input),
        "Missing real feedback grays Align while emergency remains available");
  Fixture robot(real);
  check(robot.core.action_available(InputAction::Align, robot.input) && !robot.core.action_available(InputAction::Hold, robot.input) &&
        !robot.core.action_available(InputAction::AsapLoco, robot.input), "Real Disarmed exposes only valid entry routes");
  const auto before = robot.core.snapshot().generation;
  for (int i = 0; i < 10; ++i) (void)robot.core.action_available(InputAction::Align, robot.input);
  check(robot.core.snapshot().generation == before && robot.backend.actions.empty(), "Availability probing never changes state or requests authority");
  robot.input.primary_online = false;
  check(!robot.core.action_available(InputAction::Align, robot.input), "Real entry availability uses the same input guard as tick");
  robot.input.primary_online = true;
  robot.now += real.guards.state_timeout + 1ms;
  check(!robot.core.action_available(InputAction::Align, robot.input), "Feedback staleness immediately grays real actions");
  auto sim = load_mode_config(configs / "sim.yaml", RuntimeMode::Sim);
  sim.manual_transitions.erase(std::remove_if(sim.manual_transitions.begin(), sim.manual_transitions.end(), [](const Transition& route) {
    return route.to == State::Hold;
  }), sim.manual_transitions.end());
  Fixture configured(sim);
  check(configured.core.action_available(InputAction::Align, configured.input) && !configured.core.action_available(InputAction::Hold, configured.input),
        "Loaded YAML route changes alter availability without a frontend transition table");
  configured.cycle({Event::system(EventKind::Fault)});
  check(configured.core.action_available(InputAction::ClearFault, configured.input) && !configured.core.action_available(InputAction::Align, configured.input),
        "Latched Fault exposes clear_fault rather than ordinary state switches");
}

void real_posture_switching(const ModeConfig& real) {
  // Exercise the production controller with fresh but fallen, low and out-of-range feedback.
  for (const bool checked_sim : {false, true}) {
    auto mode = real;
    if (checked_sim) {
      mode.mode = RuntimeMode::Sim;
      mode.real_checks = true;
    }
    Fixture f(mode);
    f.backend.progress = AuthorityProgress::Owned;
    const auto step = [&](const std::vector<Event>& events = {}) {
      f.now += 2ms;
      f.input.primary_acquired = f.now;
      auto measured = sample(++f.generation, ++f.sdk_tick, f.now, true);
      measured.frame.joints.position[0] = 3.5;
      measured.frame.joints.position[3] = -1.5;
      measured.frame.validity |= Frame::VALID_ODOMETRY;
      measured.frame.odometry.pose.pose.position.z = .05;
      f.core.feedback(measured);
      const auto result = f.core.tick(InputFrame{f.input, {}, events});
      f.frame = f.core.take_frame(f.input);
      return result;
    };
    step();
    check(f.core.action_available(InputAction::Align, f.input),
          "Real rules allow Align despite base tilt, low height and measured joint angles");
    check(step({Event::manual(State::Align)}).decision == Decision::Accepted &&
          f.backend.owned && !f.backend.commands.empty() &&
          f.backend.commands.front().motor_cmd()[0].q() == 3.5f,
          "Alignment acquires authority and starts from the actual out-of-range angle");
    for (int i = 0; i < 1002; ++i) step();
    check(f.core.snapshot().state == State::Hold && f.backend.commands.back().motor_cmd()[0].kp() > 0 &&
          f.core.action_available(InputAction::AsapLoco, f.input),
          "Alignment continues through Hold with the same unrestricted measured posture");
    f.discover();
    step({Event::manual(State::AsapLoco)});
    auto loco = f.target();
    check(f.core.target(loco), "Loco target accepted for unrestricted measured posture");
    f.active_status(loco); step(); step();
    check(f.core.snapshot().state == State::AsapLoco &&
          f.core.action_available(InputAction::AsapCr7, f.input),
          "Loco becomes active and permits the next configured transition");
    step({Event::manual(State::AsapCr7)});
    auto mimic = f.target();
    check(f.core.target(mimic), "Mimic target accepted for unrestricted measured posture");
    f.active_status(mimic); step(); step();
    check(f.core.snapshot().state == State::AsapCr7, "Mimic becomes active at unrestricted angles");
    f.active_status(mimic, true); step();
    check(f.core.snapshot().state == State::Preparing && f.core.request() &&
          f.core.request()->target_model == "asap_loco", "Mimic automatically requests Loco at unrestricted angles");
    auto returned = f.target();
    check(f.core.target(returned), "Automatic Loco return accepts a target at unrestricted angles");
    f.active_status(returned); step(); step();
    check(f.core.snapshot().state == State::AsapLoco, "Automatic return completes at unrestricted angles");
  }
  auto guarded = real;
  guarded.guards.enforce_measured_joint_limits = true;
  Fixture limited(guarded);
  auto measured = sample(++limited.generation, ++limited.sdk_tick, limited.now);
  measured.frame.joints.position[0] = 3.5;
  limited.core.feedback(measured);
  check(!limited.core.action_available(InputAction::Align, limited.input),
        "A custom configuration can still enable measured joint-angle gating");
  Fixture nonfinite(real);
  measured = sample(++nonfinite.generation, ++nonfinite.sdk_tick, nonfinite.now);
  measured.frame.joints.position[0] = std::numeric_limits<double>::quiet_NaN();
  nonfinite.core.feedback(measured);
  check(!nonfinite.core.action_available(InputAction::Align, nonfinite.input),
        "Nonfinite measured feedback remains invalid");
}

void source_disconnect(const ModeConfig& sim, const ModeConfig& real) {
  auto checked_sim = real;
  checked_sim.mode = RuntimeMode::Sim;
  checked_sim.real_checks = true;
  auto legacy_real = real;
  legacy_real.guards.source_loss = SourceLossPolicy::Fault;
  for (const auto& mode : {sim, real, checked_sim, legacy_real}) {
    Fixture f(mode);
    f.backend.progress = AuthorityProgress::Owned;
    f.cycle({Event::manual(State::Align)});
    check(f.core.snapshot().state == State::Align && f.backend.owned && !f.backend.commands.empty(),
          "Alignment starts with authority and fresh input");
    const auto alignment_writes = f.core.writer_count();
    f.input.primary_online = false;
    f.input.moving = true;
    f.input.velocity = {.4, .3, .2};
    f.cycle({Event::system(EventKind::SourceLost)});
    check(f.core.snapshot().state == State::Align && f.backend.owned &&
          f.core.writer_count() > alignment_writes && f.backend.commands.back().motor_cmd()[0].kp() > 0 &&
          f.frame && !f.frame->command_stand && f.frame->command.twist.linear.x == 0. &&
          f.frame->command.twist.linear.y == 0. && f.frame->command.twist.angular.z == 0.,
          "Disconnected Align keeps PD output and clears even a stale moving snapshot");
    for (int i = 0; i < 1002; ++i) f.cycle();
    check(f.core.snapshot().state == State::Hold && f.backend.owned &&
          f.backend.commands.back().motor_cmd()[0].kp() > 0,
          "Alignment completes and holds during a disconnect longer than input timeout");
    if (mode.guards.require_primary_input) {
      check(!f.core.action_available(InputAction::AsapLoco, f.input),
            "A new manual real-mode transition still requires a live primary");
      f.cycle({Event::manual(State::AsapLoco)});
      check(f.core.snapshot().state == State::Hold, "Disconnected manual policy request is rejected");
    }
    f.input.primary_online = true;
    f.input.moving = false;
    f.input.velocity = {};
    f.discover();
    f.cycle({Event::manual(State::AsapLoco)});
    check(f.core.snapshot().state == State::Preparing && f.core.request(), "Live input can request Loco");
    f.input.primary_online = false;
    f.cycle({Event::system(EventKind::SourceLost)});
    check(f.core.snapshot().state == State::Preparing && f.frame && f.frame->execution_authorized,
          "Preparation keeps inference authorized after input loss");
    uint64_t sequence = 1;
    auto loco = f.target(sequence);
    check(f.core.target(loco), "Fresh Loco output accepted while disconnected");
    f.active_status(loco); f.cycle(); f.cycle();
    check(f.core.snapshot().state == State::AsapLoco, "Preparing completes while disconnected");
    const auto epoch = f.core.request()->next_epoch;
    const auto request_id = f.core.request()->request_id;
    const auto loco_writes = f.core.writer_count();
    f.input.moving = true;
    f.input.velocity = {.4, .3, .2};
    for (int i = 0; i < 200; ++i) {
      if (i % 8 == 0) {
        auto fresh_target = f.target(++sequence);
        check(f.core.target(fresh_target), "Policy outputs stay current during a long input disconnect");
      }
      f.cycle();
    }
    check(f.core.snapshot().state == State::AsapLoco && f.core.request()->next_epoch == epoch &&
          f.core.request()->request_id == request_id && f.backend.owned && f.core.writer_count() > loco_writes &&
          f.backend.commands.back().motor_cmd()[0].kp() > 0 && f.frame && f.frame->execution_authorized &&
          !f.frame->command_stand && f.frame->command.twist.linear.x == 0. &&
          f.frame->command.twist.linear.y == 0. && f.frame->command.twist.angular.z == 0.,
          "Disconnected Loco retains its transaction and continuous PD with neutral motion");
    f.input.primary_online = true;
    f.input.moving = false;
    f.input.velocity = {};
    f.cycle();
    check(f.frame && !f.frame->command_stand && f.frame->command.twist.linear.x == 0.,
          "Reconnected neutral input does not replay previous motion");
    f.input.moving = true;
    f.input.velocity = {.1, .2, .3};
    f.cycle();
    check(f.frame && f.frame->command_stand && f.frame->command.twist.linear.x == .1,
          "New deliberate motion works after reconnection");
    f.cycle({Event::manual(State::AsapCr7)});
    auto mimic = f.target();
    check(f.core.target(mimic), "Mimic target accepted after input reconnect");
    f.active_status(mimic); f.cycle(); f.cycle();
    check(f.core.snapshot().state == State::AsapCr7, "Mimic is active before another input loss");
    f.input.primary_online = false;
    f.cycle({Event::system(EventKind::SourceLost)});
    f.active_status(mimic, true); f.cycle();
    check(f.core.snapshot().state == State::Preparing && f.core.request() &&
          f.core.request()->target_model == "asap_loco" && f.frame->execution_authorized,
          "Mimic can automatically return to Loco without an input source");
    auto returned = f.target();
    check(f.core.target(returned), "Automatic return accepts a new Loco target while disconnected");
    f.active_status(returned); f.cycle(); f.cycle();
    check(f.core.snapshot().state == State::AsapLoco && f.backend.owned,
          "Automatic return completes without losing authority");
  }

  Fixture pending(real);
  pending.cycle({Event::manual(State::Align)});
  check(pending.backend.commands.empty(), "Acquisition remains pending before input loss");
  pending.input.primary_online = false;
  pending.backend.progress = AuthorityProgress::Owned;
  pending.cycle({Event::system(EventKind::SourceLost)});
  check(pending.core.snapshot().state == State::Align && pending.backend.owned &&
        !pending.backend.commands.empty() && pending.backend.commands.back().motor_cmd()[0].kp() > 0,
        "A previously requested acquisition can start alignment after input disconnect");
  pending.now += real.guards.state_timeout + 1ms;
  pending.cycle({}, false);
  check(pending.core.snapshot().state == State::Fault && !pending.backend.owned,
        "Loss of robot feedback still faults even when the operator source is disconnected");

  Fixture stopped(real);
  stopped.backend.progress = AuthorityProgress::Owned;
  stopped.cycle({Event::manual(State::Align)});
  stopped.input.primary_online = false;
  stopped.input.emergency_stop = true;
  stopped.cycle({Event::system(EventKind::SourceLost), Event::system(EventKind::EmergencyStop)});
  check(stopped.core.snapshot().state == State::Damping && stopped.backend.commands.back().motor_cmd()[0].kp() == 0.,
        "Emergency stop still takes priority while the primary input is disconnected");
}

void controller(const fs::path& configs) {
  const auto sim = load_mode_config(configs / "sim.yaml", RuntimeMode::Sim);
  const auto real_for_disconnect = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  source_disconnect(sim, real_for_disconnect);
  for (const auto state : business_states()) {
    Fixture fallen(sim);
    fallen.cycle({Event::manual(state)}, true, true);
    const auto expected = state == State::AsapLoco || state == State::AsapCr7 ? State::Preparing : state;
    check(fallen.core.snapshot().state == expected && fallen.frame,
          "Every sim business mode accepts direct fallen selection without a lower-layer posture veto");
  }
  Fixture waiting_sim(sim);
  waiting_sim.cycle({Event::manual(State::AsapLoco)});
  const auto waiting_generation = waiting_sim.core.snapshot().generation;
  waiting_sim.now += 30s;
  waiting_sim.cycle();
  check(waiting_sim.core.snapshot().state == State::Preparing && !waiting_sim.core.request() &&
        waiting_sim.core.snapshot().generation == waiting_generation,
        "Non-strict sim preparation waits indefinitely for MC without a new transaction/generation");
  Fixture f(sim);
  f.cycle();
  check(f.backend.commands.empty() && f.core.snapshot().state == State::Disarmed, "Startup publishes state without a first LowCmd");
  f.cycle({Event::manual(State::AsapLoco)}, true, true);
  check(f.core.snapshot().state == State::Preparing && !f.core.request() && f.frame &&
        f.frame->execution_mode == Frame::MODE_PREPARING && f.frame->execution_authorized,
        "Sim direct fallen Preparing: state=" + to_string(f.core.snapshot().state) +
        " frame=" + std::to_string(f.frame.has_value()) + " diagnostic=" + f.core.diagnostic());
  f.discover(); f.cycle({}, true, true);
  check(f.core.request() && f.frame->epoch == f.core.request()->next_epoch, "MC arrival binds the existing requested mode once");
  auto target = f.target();
  check(f.core.target(target), "Production Gate accepts common SDK references beyond mechanical/first-step bounds");
  f.active_status(target);
  f.cycle({Event::manual(State::AsapLoco)}, true, true);
  check(f.core.snapshot().state == State::Preparing, "Same-mode manual selection precedes ordinary readiness");
  f.cycle({}, true, true);
  check(f.core.snapshot().state == State::AsapLoco && !f.backend.commands.empty() &&
        f.backend.commands.back().motor_cmd()[10].q() == static_cast<float>(.5416049427),
        "Valid current target uses the one SDK PD encoder while fallen");
  const auto epoch = f.core.request()->next_epoch;
  const auto generation = f.core.snapshot().generation;
  const auto request_id = f.core.request()->request_id;
  f.sdk_tick = 0;
  f.cycle({}, true, true);
  check(f.core.snapshot().state == State::AsapLoco && f.core.snapshot().generation == generation &&
        f.core.request()->next_epoch == epoch && f.core.request()->request_id == request_id,
        "Backwards sim tick changes no mode, generation, epoch or policy transaction");
  f.now += 30s;
  f.input.primary_acquired = f.now;
  f.cycle({}, false);
  check(f.core.snapshot().state == State::AsapLoco && f.core.request()->next_epoch == epoch && !f.frame &&
        f.backend.actions.back() == "zero_release", "A long gap waits, releases cached actuation and preserves selected transaction");
  const auto writes = f.backend.commands.size();
  f.cycle({}, false);
  check(f.backend.commands.size() == writes, "Expired target and duplicate physical frame produce no additional PD writes");
  f.cycle({}, true, true);
  auto resumed = f.target(2);
  check(f.core.target(resumed), "New physical frame resumes the same transaction");
  f.cycle({}, true, true);
  check(f.backend.commands.size() > writes && f.core.request()->request_id == request_id, "Resumed policy write does not reinitialize the transaction");
  f.cycle({Event::manual(State::AsapLoco)}, true, true);
  check(f.core.request()->request_id == request_id, "Idempotent active request is not a model reset button");
  f.cycle({Event::manual(State::Disarmed)});
  check(!f.core.request() && f.core.snapshot().state == State::Disarmed && f.backend.actions.back() == "authority_release",
        "Disarm revokes transaction and releases before the output gate closes");
  check(f.backend.actions[f.backend.actions.size() - 2] == "zero_release" && !f.core.target(resumed),
        "Old queued targets/receipts cannot survive output-off");
  f.cycle();
  check(f.frame && f.frame->sdk_published_target_seq == 0 && f.frame->sdk_published_phase_mask == 0,
        "Revoked writer receipts are not published as current authorization");

  Fixture cr7(sim); cr7.discover(); cr7.cycle({Event::manual(State::AsapCr7)});
  auto cr7target = cr7.target(); check(cr7.core.target(cr7target), "CR7 target accepted");
  cr7.active_status(cr7target); cr7.cycle();
  check(cr7.core.snapshot().state == State::Preparing, "Validated target/status alone are not a writer receipt");
  cr7.cycle();
  check(cr7.core.snapshot().state == State::AsapCr7, "CR7 completion of preparation is separate");
  cr7.input.moving = true;
  cr7.input.velocity = {.3, -.1, .2};
  cr7.active_status(cr7target, true); cr7.cycle();
  check(cr7.core.snapshot().state == State::Preparing && cr7.core.request()->target_model == "asap_loco" &&
        cr7.core.request()->next_epoch > cr7target.epoch &&
        cr7.core.request()->transition_profile == "g1_direct_v1",
        "Current CR7 motion completion automatically requests the direct profile");
  check(!cr7.core.target(cr7target), "Superseded CR7 target cannot become a current loco write");
  check(cr7.frame && !cr7.frame->command_stand && cr7.frame->command.twist.linear.x == 0 &&
        cr7.frame->command.twist.linear.y == 0 && cr7.frame->command.twist.angular.z == 0,
        "Automatic return suppresses a velocity command held during CR7");
  check(cr7.backend.commands.back().motor_cmd()[15].kp() == 90.f &&
        cr7.backend.commands.back().motor_cmd()[0].q() == 4.f &&
        cr7.frame->sdk_published_target_seq == 0,
        "Handover reuses the last written CR7 target and gains without claiming a new-epoch receipt");
  auto returned = cr7.target(2);
  check(cr7.core.target(returned), "New loco epoch accepts its own target");
  cr7.active_status(returned); cr7.cycle(); cr7.cycle();
  check(cr7.core.snapshot().state == State::AsapLoco && !cr7.frame->command_stand &&
        cr7.frame->command.twist.linear.x == 0 && cr7.backend.commands.back().motor_cmd()[15].kp() == 40.f,
        "Held input stays inhibited after active loco is confirmed");
  cr7.input.velocity = {};
  cr7.cycle();
  check(!cr7.frame->command_stand, "Returning the stick to neutral keeps the robot still");
  cr7.input.velocity = {.15, 0., 0.};
  cr7.cycle();
  check(cr7.frame->command_stand && cr7.frame->command.twist.linear.x == .15,
        "A new movement after neutral resumes normal operator control");

  Fixture expiring(sim); expiring.discover(); expiring.cycle({Event::manual(State::AsapLoco)});
  auto previous = expiring.target(); check(expiring.core.target(previous), "Previous policy target accepted");
  expiring.active_status(previous); expiring.cycle(); expiring.cycle();
  check(expiring.core.snapshot().state == State::AsapLoco, "Previous policy is active before switching");
  const auto deadline_start = expiring.now;
  expiring.cycle({Event::manual(State::AsapCr7)});
  check(expiring.core.request()->transition_profile == "g1_direct_v1" &&
        expiring.backend.commands.back().motor_cmd()[0].kp() > 0 &&
        expiring.backend.commands.back().motor_cmd()[0].q() == 4.f,
        "Any policy direction continues its last written target during the short handover");
  check(!expiring.core.target(previous), "Old target is not accepted into the new epoch");
  expiring.now = deadline_start + 50ms;
  expiring.cycle();
  check(expiring.backend.commands.back().motor_cmd()[0].kp() == 0.f,
        "Handover target expires at its original deadline and falls back to damping");

  Fixture stopped_handover(sim); stopped_handover.discover();
  stopped_handover.cycle({Event::manual(State::AsapLoco)});
  auto before_stop = stopped_handover.target();
  check(stopped_handover.core.target(before_stop), "Stop fixture source target accepted");
  stopped_handover.active_status(before_stop); stopped_handover.cycle(); stopped_handover.cycle();
  stopped_handover.cycle({Event::manual(State::AsapCr7)});
  stopped_handover.input.emergency_stop = true;
  stopped_handover.cycle({Event::system(EventKind::EmergencyStop)});
  check(stopped_handover.core.snapshot().state == State::Damping &&
        stopped_handover.backend.commands.back().motor_cmd()[0].kp() == 0.f,
        "Emergency stop immediately revokes an otherwise fresh handover target");

  Fixture reordered(sim); reordered.discover(); reordered.cycle({Event::manual(State::AsapCr7)});
  auto reordered_target = reordered.target();
  reordered.active_status(reordered_target); reordered.cycle();
  check(reordered.core.snapshot().state == State::Preparing && reordered.core.target(reordered_target),
        "An early matching status cannot complete preparation before target arrival");
  reordered.cycle();
  check(reordered.core.snapshot().state == State::Preparing, "Status-before-target still requires a current SDK writer receipt");
  reordered.cycle();
  check(reordered.core.snapshot().state == State::AsapCr7, "Status-before-target completes after the matching writer receipt");

  const auto real = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  real_posture_switching(real);
  auto optional_primary = real;
  optional_primary.guards.require_primary_input = false;
  Fixture absent_entry(optional_primary, false);
  absent_entry.core.tick(InputFrame{absent_entry.input, {}, {Event::manual(State::Align)}});
  check(absent_entry.core.snapshot().state == State::Disarmed && absent_entry.backend.actions.empty(),
        "Optional primary input never bypasses absent measured feedback before acquisition");
  Fixture invalid_entry(optional_primary);
  invalid_entry.now += optional_primary.guards.state_timeout + 1ms;
  invalid_entry.core.tick(InputFrame{invalid_entry.input, {}, {Event::manual(State::Align)}});
  check(invalid_entry.core.snapshot().state == State::Disarmed && invalid_entry.backend.actions.empty(),
        "Optional primary input never bypasses strict measured-entry freshness before acquisition");
  auto invalid_feedback = sample(++invalid_entry.generation, ++invalid_entry.sdk_tick, invalid_entry.now);
  invalid_feedback.frame.validity = 0;
  invalid_entry.core.feedback(invalid_feedback);
  invalid_entry.core.tick(InputFrame{invalid_entry.input, {}, {Event::manual(State::Align)}});
  check(invalid_entry.core.snapshot().state == State::Disarmed && invalid_entry.backend.actions.empty(),
        "Optional primary input never bypasses strict measured-entry integrity before acquisition");
  Fixture stale_primary(real);
  stale_primary.now += real.guards.input_timeout + 1ms;
  stale_primary.core.feedback(sample(++stale_primary.generation, ++stale_primary.sdk_tick, stale_primary.now));
  stale_primary.core.tick(InputFrame{stale_primary.input, {}, {Event::manual(State::Align)}});
  check(stale_primary.core.snapshot().state == State::Disarmed && stale_primary.backend.actions.empty(),
        "A cached online snapshot with expired primary acquisition cannot start authority acquisition");
  stale_primary.input.primary_acquired = stale_primary.now + 1ms;
  stale_primary.core.tick(InputFrame{stale_primary.input, {}, {Event::manual(State::Align)}});
  check(stale_primary.core.snapshot().state == State::Disarmed && stale_primary.backend.actions.empty(),
        "A future primary acquisition cannot start authority acquisition");

  Fixture undiscovered(real);
  undiscovered.backend.progress = AuthorityProgress::Owned;
  undiscovered.cycle({Event::manual(State::Align)});
  for (int i = 0; i < 1002; ++i) undiscovered.cycle();
  check(undiscovered.core.snapshot().state == State::Hold, "Real fake reaches Hold through explicit acquisition/alignment");
  undiscovered.cycle({Event::manual(State::AsapLoco)});
  check(undiscovered.core.snapshot().state == State::Preparing && !undiscovered.core.request(),
        "Authorized policy preparation can wait for undiscovered MC");
  undiscovered.now += 2ms;
  undiscovered.input.primary_acquired = undiscovered.now - real.guards.input_timeout - 1ms;
  undiscovered.input.moving = true;
  undiscovered.input.velocity = {.4, .3, .2};
  undiscovered.core.feedback(sample(++undiscovered.generation, ++undiscovered.sdk_tick, undiscovered.now));
  const auto unpermitted_frame = undiscovered.core.take_frame(undiscovered.input);
  check(unpermitted_frame && unpermitted_frame->execution_authorized && !unpermitted_frame->command_stand &&
        unpermitted_frame->command.twist.linear.x == 0. && unpermitted_frame->command.twist.angular.z == 0.,
        "Expired input neutralizes motion while authorized MC work continues");
  undiscovered.now += real.policy_timeout;
  undiscovered.cycle();
  check(undiscovered.core.snapshot().state == State::Fault && !undiscovered.backend.owned &&
        undiscovered.backend.actions[undiscovered.backend.actions.size() - 2] == "zero_release",
        "Strict configured preparation deadline covers waiting for MC discovery and releases before off");

  Fixture late_discovery(real);
  late_discovery.backend.progress = AuthorityProgress::Owned;
  late_discovery.cycle({Event::manual(State::Align)});
  for (int i = 0; i < 1002; ++i) late_discovery.cycle();
  late_discovery.cycle({Event::manual(State::AsapLoco)});
  late_discovery.now += real.policy_timeout - 4ms;
  late_discovery.discover(); late_discovery.cycle();
  check(late_discovery.core.request() && late_discovery.core.snapshot().state == State::Preparing,
        "Late MC discovery binds the pending generation within its original deadline");
  late_discovery.cycle();
  check(late_discovery.core.snapshot().state == State::Fault && !late_discovery.backend.owned,
        "MC discovery cannot renew the strict preparation budget");

  Fixture readonly_wait(real);
  readonly_wait.now += 30s;
  readonly_wait.cycle({}, false);
  check(readonly_wait.core.snapshot().state == State::Disarmed && readonly_wait.backend.actions.empty(),
        "Readonly real startup has no feedback/MC failure lifetime or authority side effects");

  auto guarded_real = real;
  guarded_real.guards.allow_fallen = false;
  guarded_real.guards.enforce_measured_joint_limits = true;
  Fixture tilted(guarded_real); tilted.cycle({}, true, true);
  check(tilted.frame && !tilted.frame->execution_authorized && !tilted.frame->diagnostics.status.empty() &&
        tilted.core.snapshot().state == State::Disarmed, "Tilted real Disarmed still publishes valid telemetry with actuation disabled");
  Fixture hardware(real);
  check(hardware.backend.actions.empty() && hardware.backend.commands.empty(), "Readonly real startup performs no acquire/release/write");
  hardware.cycle({Event::manual(State::Damping)});
  check(hardware.backend.commands.empty() && hardware.backend.actions.empty(), "Readonly damping never acquires or writes");
  hardware.now += 101ms; hardware.cycle();
  hardware.cycle({Event::manual(State::Align)});
  check(hardware.backend.actions.back() == "acquire" && hardware.backend.commands.empty(), "Explicit legal Align starts an asynchronous acquisition");
  hardware.cycle({Event::system(EventKind::EmergencyStop)});
  check(hardware.backend.actions.back() == "cancel" && hardware.backend.commands.empty(), "Stop cancels pending authority immediately without a stale alignment write");
  hardware.now += 101ms; hardware.cycle();
  hardware.backend.progress = AuthorityProgress::Owned;
  hardware.cycle({Event::manual(State::Align)});
  check(!hardware.backend.commands.empty() && hardware.backend.commands.back().motor_cmd()[0].q() == 0,
        "New generation acquired alignment begins from its fresh measured pose");
  hardware.now += 100ms;
  hardware.cycle({}, false);
  check(hardware.core.snapshot().state == State::Fault && !hardware.backend.owned,
        "Strict real feedback deadline faults and releases independently of MC waiting");

  Fixture warming(sim);
  warming.discover(); warming.cycle({Event::manual(State::Align)});
  for (int i = 0; i < 1002; ++i) warming.cycle();
  check(warming.core.snapshot().state == State::Hold, "Fresh measured alignment automatically completes");
  const auto before_prepare = warming.backend.actions.size();
  warming.cycle({Event::manual(State::AsapLoco)});
  check(warming.core.snapshot().state == State::Preparing && warming.frame->execution_authorized &&
        warming.backend.commands.back().motor_cmd()[0].kp() == 100 &&
        warming.backend.commands.back().motor_cmd()[0].q() == static_cast<float>(-.1) &&
        std::find(warming.backend.actions.begin() + before_prepare, warming.backend.actions.end(), "zero_release") == warming.backend.actions.end(),
        "Hold preparation preserves configured fixed-stand PD while MC warms, without target reuse/release gap");
  auto denied_target = warming.target();
  check(warming.core.target(denied_target), "Writer-reject target is validated");
  warming.active_status(denied_target);
  warming.backend.fail_write = true;
  warming.cycle();
  check(warming.core.snapshot().state == State::Preparing, "Rejected writer cannot complete a merely accepted/validated request");

  Fixture receipts(sim); receipts.discover(); receipts.cycle({Event::manual(State::AsapLoco)});
  auto old = receipts.target(); check(receipts.core.target(old), "Old transaction target accepted");
  receipts.active_status(old); receipts.cycle(); receipts.cycle();
  receipts.cycle({Event::manual(State::AsapCr7)});
  auto newer = receipts.target(); check(receipts.core.target(newer), "New transaction target accepted");
  receipts.active_status(newer); receipts.backend.fail_write = true; receipts.cycle();
  check(receipts.core.snapshot().state == State::Preparing && !receipts.core.target(old),
        "Previous active writer receipt/late target cannot complete a new transaction whose writer rejects");

  Fixture priority(sim); priority.cycle({Event::manual(State::Hold)});
  priority.core.feedback_fault("Recorded contract fault");
  priority.cycle({Event::system(EventKind::EmergencyStop)});
  check(priority.core.snapshot().state == State::Damping && priority.backend.actions.back() == "zero_release",
        "Emergency wins the cycle and immediately revokes cached actuation");
  priority.cycle();
  check(priority.core.snapshot().state == State::Fault, "A simultaneous fault remains pending after emergency precedence");

  auto gated = sim;
  gated.states.at(State::Hold).outputs = {false, false};
  gated.states.at(State::Hold).on_enter = {Action::RevokeActuation, Action::ReleaseIfOwned};
  Fixture off(gated); off.cycle({Event::manual(State::Damping)});
  off.cycle({Event::manual(State::Hold)});
  check(!off.frame && off.backend.actions[off.backend.actions.size() - 2] == "zero_release",
        "Configured state-off/lowcmd-off performs release and stops state publication");
  off.core.shutdown();
}

void feedback(const fs::path& configs) {
  FeedbackTracker tracker;
  auto now = Clock::time_point{} + 10s;
  auto first = sample(1, 100, now);
  check(tracker.observe(first.frame, 5, now), "Initial physical state is acquired");
  check(!tracker.observe(first.frame, 5, now + 1s) && tracker.sample()->acquired == now && tracker.sample()->generation == 1,
        "Duplicate SDK tick cannot renew physical age/generation");
  first.frame.sdk_tick = 0;
  check(tracker.observe(first.frame, 5, now + 2s) && tracker.sample()->generation == 2 && tracker.rollbacks() == 1,
        "Backwards tick creates a new physical acquisition and diagnostic only");
  Fixture f(load_mode_config(configs / "sim.yaml", RuntimeMode::Sim));
  f.cycle(); const auto sequence = f.core.state_sequence();
  f.cycle({}, false);
  check(!f.frame && f.core.state_sequence() == sequence, "Controller never republishes cached feedback with a manufactured sequence");
  f.cycle();
  check(f.frame && f.core.state_sequence() == sequence + 1, "New physical generation advances the independent Env sequence");
  Fixture sim(load_mode_config(configs / "sim.yaml", RuntimeMode::Sim));
  sim.cycle({Event::manual(State::Align)});
  sim.core.malformed_transport_packet();
  sim.cycle();
  check(sim.core.snapshot().state == State::Align && sim.frame,
        "Malformed simulation transport data does not fault or prevent later valid feedback");
  sim.now += sim.mode.guards.state_timeout + 1ms;
  sim.cycle({}, false);
  check(!sim.frame && sim.backend.commands.back().motor_cmd()[0].mode() == 0,
        "Stale simulation feedback still stops state publication and releases position commands");
  Fixture real(load_mode_config(configs / "real.yaml", RuntimeMode::Real));
  real.core.malformed_transport_packet();
  real.cycle();
  check(real.core.snapshot().state == State::Fault &&
        real.core.diagnostic() == "Malformed native feedback or remote packet",
        "Malformed real transport data still faults");
  auto checked_sim_config = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  checked_sim_config.mode = RuntimeMode::Sim;
  checked_sim_config.real_checks = true;
  Fixture checked_sim(checked_sim_config);
  checked_sim.core.malformed_transport_packet();
  checked_sim.cycle();
  check(checked_sim.core.snapshot().state == State::Fault &&
        checked_sim.core.diagnostic() == "Malformed native feedback or remote packet",
        "Sim with real checks faults on malformed feedback exactly like real");
  checked_sim_config.guards.allow_fallen = false;
  Fixture fallen_checked(checked_sim_config);
  fallen_checked.cycle({Event::manual(State::Align)}, true, true);
  check(fallen_checked.core.snapshot().state == State::Disarmed,
        "Sim with explicitly enabled posture checks rejects a fallen entry");
}
void startup(const fs::path& configs) {
  check(parse_startup({"env"}).mode == RuntimeMode::Real, "Default CLI mode is real");
  const auto options = parse_startup({"env", "--sim", "--config", "chosen.yaml", "--input-config", "input.yaml", "--input", "remote", "--ros-args", "-p", "use_sim_time:=false"});
  check(options.mode == RuntimeMode::Sim && options.resources.primary_input == InputSource::Remote &&
        options.resources.config == fs::path("chosen.yaml") && options.ros_arguments.size() == 4, "CLI configuration and ROS forwarding remain separate");
  const auto checked = parse_startup({"env", "--sim", "--real-checks", "--real-config", "robot.yaml"});
  check(checked.mode == RuntimeMode::Sim && checked.resources.real_checks &&
        checked.resources.real_config == fs::path("robot.yaml"), "CLI selects real checks only for simulation");
  const std::vector<std::vector<std::string>> invalid_arguments{{"env", "--loopback-sim"}, {"env", "--sim", "--real"},
      {"env", "--sim", "--sim"}, {"env", "--config"}, {"env", "--input", "bogus"},
      {"env", "--real", "--real-checks"}, {"env", "--real-checks"},
      {"env", "--sim", "--real-config", "robot.yaml"}, {"env", "--sim", "--real-checks", "--real-config"}};
  for (const auto& args : invalid_arguments)
    rejected([&] { parse_startup(args); }, "Invalid CLI accepted");
  const auto sim = load_mode_config(configs / "sim.yaml", RuntimeMode::Sim);
  const auto real = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  check(!sim.guards.strict_deadlines && real.guards.strict_deadlines &&
        real.guards.allow_fallen && !real.guards.enforce_measured_joint_limits &&
        real.policy_timeout == 5000ms && real.guards.max_tilt_radians == 1. && real.guards.min_base_height == .3,
        "Mode guards and policy timing come from installed typed config");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    check(argc == 3, "Usage: runtime_test controller|pd|feedback|startup CONFIG_DIR");
    const auto configs = fs::canonical(argv[2]); const std::string suite = argv[1];
    if (suite == "controller") controller(configs);
    else if (suite == "pd") pd(); else if (suite == "feedback") feedback(configs);
    else if (suite == "availability") availability(configs);
    else if (suite == "startup") startup(configs); else throw std::runtime_error("Unknown suite");
    std::cout << suite << " runtime contracts passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
