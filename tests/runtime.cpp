// Production controller/encoder/feedback/startup paths with a record-only backend.
#include <anakin_env/runtime.hpp>
#include <anakin_env/startup.hpp>
#include <anakin_env/control.hpp>
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
      : mode(std::move(config)), backend(mode.mode == RuntimeMode::Sim),
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

void controller(const fs::path& configs) {
  const auto sim = load_mode_config(configs / "sim.yaml", RuntimeMode::Sim);
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
  cr7.active_status(cr7target, true); cr7.cycle();
  check(cr7.core.snapshot().state == State::Preparing && cr7.core.request()->target_model == "asap_loco" &&
        cr7.core.request()->next_epoch > cr7target.epoch, "Current CR7 motion completion automatically requests loco");
  check(!cr7.core.target(cr7target), "Superseded CR7 target cannot become a current loco write");

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
  undiscovered.core.feedback(sample(++undiscovered.generation, ++undiscovered.sdk_tick, undiscovered.now));
  const auto unpermitted_frame = undiscovered.core.take_frame(undiscovered.input);
  check(unpermitted_frame && !unpermitted_frame->execution_authorized,
        "Fresh telemetry with a cached expired primary snapshot cannot authorize MC work");
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

  Fixture tilted(real); tilted.cycle({}, true, true);
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
}
void startup(const fs::path& configs) {
  check(parse_startup({"env"}).mode == RuntimeMode::Real, "Default CLI mode is real");
  const auto options = parse_startup({"env", "--sim", "--config", "chosen.yaml", "--input-config", "input.yaml", "--input", "remote", "--ros-args", "-p", "use_sim_time:=false"});
  check(options.mode == RuntimeMode::Sim && options.resources.primary_input == InputSource::Remote &&
        options.resources.config == fs::path("chosen.yaml") && options.ros_arguments.size() == 4, "CLI configuration and ROS forwarding remain separate");
  const std::vector<std::vector<std::string>> invalid_arguments{{"env", "--loopback-sim"}, {"env", "--sim", "--real"},
      {"env", "--sim", "--sim"}, {"env", "--config"}, {"env", "--input", "bogus"}};
  for (const auto& args : invalid_arguments)
    rejected([&] { parse_startup(args); }, "Invalid CLI accepted");
  const auto sim = load_mode_config(configs / "sim.yaml", RuntimeMode::Sim);
  const auto real = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  check(!sim.guards.strict_deadlines && real.guards.strict_deadlines &&
        !sim.guards.enforce_measured_joint_limits && real.guards.enforce_measured_joint_limits &&
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
    else if (suite == "startup") startup(configs); else throw std::runtime_error("Unknown suite");
    std::cout << suite << " runtime contracts passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
