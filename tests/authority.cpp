// SDK-shaped fakes exercise the production adapter/worker/backend/controller.
// No actual MotionSwitcher client, DDS channel, node or device is constructed.
#include <anakin_env/authority.hpp>
#include <anakin_env/transport.hpp>
#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>

using namespace anakin;
using namespace anakin::env;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const std::string& why) { if (!condition) throw std::runtime_error(why); }
template<class Fn> void rejected(Fn&& fn, const std::string& why) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error(why);
}
struct FakeClock {
  std::atomic<int64_t> ns{10000000000ll};
  Clock::time_point now() const { return Clock::time_point(std::chrono::nanoseconds(ns.load())); }
  void advance(std::chrono::milliseconds amount) { ns += std::chrono::duration_cast<std::chrono::nanoseconds>(amount).count(); }
};
struct Script {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::string> calls;
  std::vector<float> timeouts;
  std::vector<LowCommand> commands;
  int factories = 0, init = 0, checks = 0, releases = 0, selects = 0, integer_timeouts = 0;
  int check_code = 0, release_code = 0;
  bool block_check = false, block_release = false, release_changes_mode = true;
  bool reject_zero_release = false, throw_zero_release = false;
  bool reject_lowcmd = false;
  std::string mode = "factory_motion";
  std::function<void()> after_check, after_release;
  void unblock() {
    std::lock_guard<std::mutex> lock(mutex);
    block_check = block_release = false;
    changed.notify_all();
  }
};
struct FakeClient {
  std::shared_ptr<Script> script;
  explicit FakeClient(std::shared_ptr<Script> s) : script(std::move(s)) {}
  void SetTimeout(int64_t) {
    std::lock_guard<std::mutex> lock(script->mutex);
    ++script->integer_timeouts;
    throw std::runtime_error("Integer microsecond SDK overload was selected");
  }
  void SetTimeout(float seconds) {
    std::lock_guard<std::mutex> lock(script->mutex);
    check(std::isfinite(seconds) && seconds > 0 && seconds <= .25f, "RPC receives bounded explicit float seconds");
    script->timeouts.push_back(seconds);
  }
  void Init() {
    std::lock_guard<std::mutex> lock(script->mutex);
    ++script->init; script->calls.push_back("Init"); script->changed.notify_all();
  }
  int32_t CheckMode(std::string& form, std::string& name) {
    std::unique_lock<std::mutex> lock(script->mutex);
    ++script->checks; script->calls.push_back("CheckMode"); script->changed.notify_all();
    script->changed.wait(lock, [&] { return !script->block_check; });
    const auto code = script->check_code;
    form = "g1"; name = code == 0 ? script->mode : "";
    const auto callback = script->after_check;
    lock.unlock();
    if (callback) callback();
    return code;
  }
  int32_t ReleaseMode() {
    std::unique_lock<std::mutex> lock(script->mutex);
    ++script->releases; script->calls.push_back("ReleaseMode"); script->changed.notify_all();
    script->changed.wait(lock, [&] { return !script->block_release; });
    const auto code = script->release_code;
    if (code == 0 && script->release_changes_mode) script->mode.clear();
    const auto callback = script->after_release;
    lock.unlock();
    if (callback) callback();
    return code;
  }
  int32_t SelectMode(const std::string&) {
    std::lock_guard<std::mutex> lock(script->mutex);
    ++script->selects;
    throw std::runtime_error("Factory activation must never be called");
  }
};
std::unique_ptr<MotionSwitcherApi> api(const std::shared_ptr<Script>& script) {
  return std::make_unique<MotionClientAdapter<FakeClient>>([script] {
    std::lock_guard<std::mutex> lock(script->mutex);
    ++script->factories;
    return std::make_unique<FakeClient>(script);
  });
}
uint32_t object_crc(const LowCommand& command) {
  std::array<uint32_t, sizeof(command) / 4> words{};
  std::memcpy(words.data(), &command, sizeof(command));
  return crc_words(words.data(), words.size() - 1);
}
bool write(const std::shared_ptr<Script>& script, const LowCommand& command) {
  std::lock_guard<std::mutex> lock(script->mutex);
  check(command.crc() == object_crc(command), "Actual real-backend writer object has a valid CRC");
  script->calls.push_back(command.motor_cmd()[0].mode() == 0 ? "zero_release" : "LowCmd");
  script->commands.push_back(command);
  if (command.motor_cmd()[0].mode() == 0) {
    if (script->throw_zero_release) throw std::runtime_error("Injected zero-gain writer exception");
    if (script->reject_zero_release) return false;
  } else if (script->reject_lowcmd) return false;
  return true;
}
template<class Predicate> void until(Predicate&& predicate, const std::string& why) {
  const auto deadline = Clock::now() + 2s;
  while (!predicate()) {
    if (Clock::now() >= deadline) throw std::runtime_error("Fixture wait expired: " + why);
    std::this_thread::sleep_for(1ms);
  }
}
void entered(const std::shared_ptr<Script>& script, bool release) {
  std::unique_lock<std::mutex> lock(script->mutex);
  check(script->changed.wait_for(lock, 2s, [&] { return release ? script->releases > 0 : script->checks > 0; }),
        "Fake blocking RPC was reached");
}
struct Setup {
  FakeClock clock;
  std::shared_ptr<Script> script = std::make_shared<Script>();
  std::unique_ptr<RealBackend> backend;
  Setup() : backend(std::make_unique<RealBackend>(api(script), [s=script](const LowCommand& command) { return write(s, command); },
      AuthorityOptions{250ms, 1ms}, [this] { return clock.now(); })) {}
  ~Setup() { script->unblock(); backend.reset(); }
  void owned(uint64_t generation = 1) {
    until([&] { return backend->poll_acquire(generation) != AuthorityProgress::Pending; }, "authority result");
    check(backend->poll_acquire(generation) == AuthorityProgress::Owned && backend->authority_owned(), "Explicit acquisition succeeds");
  }
};

void authority_contracts() {
  {
    Setup f;
    check(!f.backend->authority_owned() && f.script->factories == 0 && f.script->commands.empty(), "Backend construction is fully inert");
    check(!f.backend->write(encode_release(5)), "Readonly backend cannot send its first release packet");
    f.backend->shutdown();
    check(f.script->calls.empty() && f.script->factories == 0, "Readonly exit does not Init, acquire, activate or write");
  }
  {
    Setup f;
    f.backend->begin_acquire(1, 3000ms); f.owned();
    const auto record = f.backend->authority_record();
    check(record.release_attempted && record.release_succeeded && record.observed_empty && !record.outcome_unknown,
          "Acquisition checks active mode, releases and verifies successful empty-mode observation");
    check(f.script->calls == std::vector<std::string>{"Init", "CheckMode", "ReleaseMode", "CheckMode"},
          "Lazy SDK adapter follows exact initialization, check, release and verification order");
    const auto q = stand_pose();
    const auto sim = encode_policy_pd(q, 5, merged_gain_profile, "asap_loco");
    check(f.backend->write(sim), "Owned real backend uses C's common PD command");
    auto real = f.script->commands.back(); finalize_command(real);
    auto canonical = sim; finalize_command(canonical);
    check(std::memcmp(&real, &canonical, sizeof(real)) == 0, "Real writer preserves identical common PD bytes");
    check(!f.backend->write(encode_policy_pd(q, 7)), "Real backend rejects unsupported machine modes");
    f.backend->shutdown();
    check(!f.backend->authority_owned() && f.script->commands.size() == 2 &&
          f.script->commands.back().motor_cmd()[0].kp() == 0 && f.script->commands.back().motor_cmd()[0].kd() == 0 &&
          f.backend->authority_record().state == AuthorityState::Released, "Owned exit writes zero gains before ending local permission");
    check(f.script->integer_timeouts == 0 && !f.script->timeouts.empty() && f.script->selects == 0 && f.script->init == 1,
          "Production SDK-shaped adapter uses float seconds and never factory activation");
  }
  for (const bool throwing : {false, true}) {
    Setup f;
    f.backend->begin_acquire(1, 3000ms); f.owned();
    check(f.backend->write(encode_policy_pd(stand_pose(), 5)), "Owned backend writes before a release failure");
    f.script->reject_zero_release = !throwing;
    f.script->throw_zero_release = throwing;
    rejected([&] { f.backend->release_authority(); }, "Rejected zero release was not reported");
    check(!f.backend->authority_owned() && !f.backend->write(encode_policy_pd(stand_pose(), 5)),
          "Failed explicit release revokes local permission and prevents subsequent PD writes");
    check(f.backend->authority_diagnostic().find("unconfirmed") != std::string::npos,
          "Failed release retains unconfirmed packet outcome rather than a success receipt");
  }
  for (const bool throwing : {false, true}) {
    Setup f;
    f.backend->begin_acquire(1, 3000ms); f.owned();
    check(f.backend->write(encode_damping(5)), "Owned backend writes damping before shutdown failure");
    f.script->reject_zero_release = !throwing; f.script->throw_zero_release = throwing;
    f.backend->shutdown();
    check(!f.backend->authority_owned() && !f.backend->write(encode_policy_pd(stand_pose(), 5)) &&
          f.backend->authority_diagnostic().find("unconfirmed") != std::string::npos,
          "Rejected or throwing shutdown release revokes and retains the failed packet outcome");
  }
  {
    Setup f;
    f.backend->begin_acquire(1, 3000ms); f.owned();
    f.script->reject_lowcmd = true;
    check(!f.backend->write(encode_damping(5)) && f.backend->authority_owned() &&
          !f.backend->write(encode_pd(PdReference{}, 5)) && f.backend->authority_owned(),
          "Damping and enabled zero PD packets are not mistaken for a disabled release");
  }
  {
    Setup f;
    f.script->check_code = 31; // Outputs remain empty on failure, as a real failed RPC may do.
    f.backend->begin_acquire(1, 3000ms);
    until([&] { return f.backend->poll_acquire(1) != AuthorityProgress::Pending; }, "failed CheckMode");
    check(!f.backend->authority_owned() && f.script->releases == 0 &&
          f.backend->authority_record().check_code == 31 && f.backend->authority_diagnostic().find("CheckMode failed") != std::string::npos,
          "A failed CheckMode never masquerades as empty successful mode");
  }
  {
    Setup f; f.script->release_code = 42;
    f.backend->begin_acquire(1, 3000ms);
    until([&] { return f.backend->poll_acquire(1) != AuthorityProgress::Pending; }, "failed ReleaseMode");
    const auto record = f.backend->authority_record();
    check(!f.backend->authority_owned() && f.script->releases == 1 && record.release_code == 42 &&
          record.release_attempted && record.outcome_unknown && !record.observed_empty,
          "ReleaseMode failure is bounded and retains honest partial/unknown outcome");
  }
  {
    Setup f; f.script->after_check = [] { throw std::runtime_error("Injected CheckMode exception"); };
    f.backend->begin_acquire(1, 3000ms);
    until([&] { return f.backend->poll_acquire(1) != AuthorityProgress::Pending; }, "CheckMode exception");
    check(!f.backend->authority_owned() && f.script->releases == 0 &&
          f.backend->authority_diagnostic().find("Injected CheckMode exception") != std::string::npos,
          "A throwing CheckMode cannot grant permission or silently release the service");
  }
  {
    Setup f; f.script->after_release = [] { throw std::runtime_error("Injected ReleaseMode exception"); };
    f.backend->begin_acquire(1, 3000ms);
    until([&] { return f.backend->poll_acquire(1) != AuthorityProgress::Pending; }, "ReleaseMode exception");
    const auto record = f.backend->authority_record();
    check(!f.backend->authority_owned() && record.release_attempted && record.outcome_unknown && !record.release_code &&
          f.backend->authority_diagnostic().find("Injected ReleaseMode exception") != std::string::npos,
          "A throwing ReleaseMode retains its unconfirmed side effect without granting permission");
  }
  {
    Setup f; f.script->after_check = [&] { f.clock.advance(200ms); };
    f.script->after_release = [&] { f.clock.advance(101ms); };
    f.backend->begin_acquire(1, 300ms);
    until([&] { return f.backend->authority_record().release_code.has_value(); }, "remaining RPC budget");
    check(f.backend->poll_acquire(1) == AuthorityProgress::Failed && f.script->timeouts.back() <= .100001f &&
          f.script->timeouts.back() >= .099f && !f.backend->authority_owned(),
          "Each RPC receives float seconds capped by the remaining overall deadline");
  }
  {
    Setup f; f.script->mode.clear();
    f.script->after_check = [&] { f.clock.advance(301ms); };
    f.backend->begin_acquire(1, 300ms);
    until([&] { return f.backend->poll_acquire(1) != AuthorityProgress::Pending; }, "late successful CheckMode");
    check(!f.backend->authority_owned(), "An empty successful check after overall deadline cannot grant permission");
  }
  {
    Setup f; f.script->after_release = [&] { f.clock.advance(3001ms); };
    f.backend->begin_acquire(1, 3000ms);
    until([&] { return f.backend->poll_acquire(1) != AuthorityProgress::Pending; }, "post-release timeout");
    until([&] { return f.backend->authority_record().release_code.has_value(); }, "late ReleaseMode return retained");
    const auto record = f.backend->authority_record();
    check(record.release_succeeded && record.outcome_unknown && !f.backend->authority_owned(),
          "A partial takeover ending at deadline is recorded without permission or factory restoration");
  }
}

void cancellation_contracts() {
  {
    Setup f; f.script->block_check = true;
    f.backend->begin_acquire(1, 3000ms); entered(f.script, false);
    auto ending = std::async(std::launch::async, [&] { f.backend->shutdown(); });
    const bool joining = ending.wait_for(20ms) == std::future_status::timeout;
    const bool revoked = !f.backend->authority_owned();
    f.script->unblock(); ending.get();
    check(joining && revoked && f.script->releases == 0 && f.script->commands.empty(),
          "Shutdown revokes promptly, joins only at teardown and never continues a cancelled acquisition");
  }
  {
    Setup f; f.script->block_check = true;
    f.backend->begin_acquire(1, 3000ms); entered(f.script, false);
    for (uint64_t generation = 2; generation <= 40; ++generation) f.backend->begin_acquire(generation, 3000ms);
    check(f.backend->authority_record(1).generation == 0, "Old diagnostic records are bounded");
    rejected([&] { f.backend->begin_acquire(1, 3000ms); }, "Pruning diagnostics must not permit reuse of an old authority generation");
    f.script->unblock(); f.owned(40);
  }
  {
    Setup f; f.script->block_check = true;
    f.backend->begin_acquire(1, 3000ms); entered(f.script, false);
    auto cancellation = std::async(std::launch::async, [&] { f.backend->cancel_acquire(1); });
    const bool prompt = cancellation.wait_for(200ms) == std::future_status::ready;
    if (!prompt) f.script->unblock();
    check(prompt, "Cancellation never waits for blocked SDK CheckMode");
    cancellation.get();
    check(!f.backend->authority_owned() && f.backend->poll_acquire(1) == AuthorityProgress::Failed,
          "Local cancellation is immediately visible while RPC is in flight");
    auto next = std::async(std::launch::async, [&] { f.backend->begin_acquire(2, 50ms); });
    const bool queued = next.wait_for(200ms) == std::future_status::ready;
    if (!queued) f.script->unblock();
    check(queued, "A new explicit request queues without joining an old blocked RPC"); next.get();
    f.clock.advance(51ms);
    check(f.backend->poll_acquire(2) == AuthorityProgress::Failed, "Queued request retains original overall deadline");
    f.script->unblock();
    f.backend->begin_acquire(3, 1000ms); f.owned(3);
    check(f.backend->poll_acquire(1) == AuthorityProgress::Failed && f.backend->poll_acquire(2) == AuthorityProgress::Failed &&
          f.backend->authority_record().generation == 3, "Late old result cannot authorize a newer generation");
  }
  {
    Setup f; f.script->block_release = true;
    f.backend->begin_acquire(1, 3000ms); entered(f.script, true);
    auto cancellation = std::async(std::launch::async, [&] { f.backend->cancel_acquire(1); });
    const bool prompt = cancellation.wait_for(200ms) == std::future_status::ready;
    if (!prompt) f.script->unblock();
    check(prompt, "Cancellation never waits for blocked SDK ReleaseMode"); cancellation.get();
    check(f.backend->authority_record(1).outcome_unknown && !f.backend->authority_owned(), "In-flight release has no local ownership permission");
    f.backend->begin_acquire(2, 3000ms);
    f.script->unblock(); f.owned(2);
    const auto old = f.backend->authority_record(1);
    check(old.state == AuthorityState::Cancelled && old.release_succeeded && old.outcome_unknown,
          "Late successful release remains cancelled and records unverified partial outcome");
    check(f.script->checks == 2 && f.script->releases == 1 && f.script->init == 1 && f.script->selects == 0 && f.script->commands.empty(),
          "New request verifies actual mode again, without stale reuse, first LowCmd or automatic restoration");
    rejected([&] { f.backend->begin_acquire(1, 3000ms); }, "Cancelled generation was reused");
  }
}

NativeSample measured(uint64_t generation, Clock::time_point time, double position = 0.) {
  NativeSample sample;
  sample.generation = generation; sample.acquired = time; sample.mode_machine = 5;
  auto& frame = sample.frame;
  frame.schema_version = schema_version; frame.schema_id = schema_id;
  frame.sdk_tick = static_cast<uint32_t>(generation); frame.validity = 3;
  frame.joints.name = joint_names(); frame.joints.position.assign(29, 0.);
  frame.joints.position[0] = position; frame.joints.velocity.assign(29, 0.);
  frame.imu.orientation.w = 1.;
  return sample;
}
struct ControlSetup {
  Setup setup;
  InputSnapshot input;
  RuntimeController core;
  std::optional<Frame> publication;
  uint64_t generation = 0;
  explicit ControlSetup(ModeConfig config) : core(std::move(config), *setup.backend,
      [this] { return input; }, [this] { return setup.clock.now(); }) {}
  ActionPlan step(const std::vector<Event>& events = {}, double pose = 0., bool physical = true) {
    setup.clock.advance(2ms);
    input.primary_online = true; input.primary_acquired = setup.clock.now();
    if (physical) core.feedback(measured(++generation, setup.clock.now(), pose));
    const auto plan = core.tick(InputFrame{input, {}, events});
    publication = core.take_frame(input);
    return plan;
  }
  void failure() {
    until([&] { step(); return core.snapshot().state == State::Fault; }, "controller fault");
  }
  void alignment() {
    until([&] { step({}, .12); return !setup.script->commands.empty(); }, "fresh measured alignment command");
  }
};
void lifecycle(const fs::path& configs) {
  const auto config = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  {
    ControlSetup f(config);
    for (const auto request : {State::Hold, State::AsapLoco, State::AsapCr7})
      check(f.step({Event::manual(request)}).decision == Decision::Rejected, "Real shortcuts are rejected before any authority operation");
    f.step({Event::manual(State::Damping)});
    f.setup.clock.advance(101ms); f.step();
    check(f.core.snapshot().state == State::Disarmed && f.setup.script->factories == 0 && f.setup.script->commands.empty(),
          "Readonly startup/damping/disarmed stay receive-only");
    f.core.shutdown();
    check(f.setup.script->calls.empty(), "Unowned runtime exit performs no SDK calls or writes");
  }
  {
    ControlSetup f(config); f.setup.script->check_code = 51;
    f.step({Event::manual(State::Align)}); f.failure();
    check(f.core.diagnostic().find("51") != std::string::npos && f.setup.script->commands.empty(), "Authority error is observable in recoverable fault");
    check(f.step({Event::manual(State::AsapLoco)}).decision == Decision::Rejected, "Fault cannot skip directly to a policy");
    f.step({Event::system(EventKind::ClearFault)});
    check(f.core.snapshot().state == State::Disarmed && !f.setup.backend->authority_owned(), "Explicit clear returns readonly Disarmed");
    { std::lock_guard<std::mutex> lock(f.setup.script->mutex); f.setup.script->check_code = 0; }
    f.step({Event::manual(State::Align)}); f.alignment();
    check(f.setup.script->commands.front().motor_cmd()[0].q() == static_cast<float>(.12),
          "Alignment is seeded from fresh measurement after authority readiness, not the old request pose");
    check(f.step({Event::manual(State::Hold)}, .12).decision == Decision::Rejected, "Real alignment cannot be completed by a shortcut");
    for (int i = 0; i < 1002; ++i) f.step({}, .12);
    check(f.core.snapshot().state == State::Hold, "Actual alignment timing advances to Hold through A's completion rule");
    f.step({Event::manual(State::Disarmed)}, .12);
    check(f.core.snapshot().state == State::Damping && f.setup.script->commands.back().motor_cmd()[0].kd() == 2,
          "Owned real stop first enters damping through the common encoder");
    f.setup.clock.advance(101ms); f.step({}, .12);
    check(f.core.snapshot().state == State::Disarmed && !f.setup.backend->authority_owned() &&
          f.setup.script->calls.back() == "zero_release", "Damping completes with zero release before output-off and local authority end");
    const auto count = f.setup.script->commands.size(); f.core.shutdown();
    check(f.setup.script->commands.size() == count && f.setup.script->selects == 0, "Released exit neither writes again nor activates factory mode");
  }
  for (const bool throwing : {false, true}) {
    ControlSetup f(config);
    f.step({Event::manual(State::Align)}); f.alignment();
    check(f.publication.has_value(), "Fresh alignment publishes measured telemetry");
    const auto receipts = f.publication->sdk_publish_count;
    f.setup.script->reject_zero_release = !throwing;
    f.setup.script->throw_zero_release = throwing;
    f.step({Event::system(EventKind::Fault)});
    check(f.core.snapshot().state == State::Fault && !f.setup.backend->authority_owned(),
          "Rejected controller zero release ends local authority while fault remains latched");
    check(f.publication && f.publication->sdk_publish_count == receipts,
          "Rejected or throwing release cannot increment the SDK publication receipt count");
    const auto count = f.setup.script->commands.size();
    f.step();
    check(f.setup.script->commands.size() == count, "Failed stop release cannot admit stale PD or repeated unowned writes");
    check(f.setup.backend->authority_diagnostic().find("unconfirmed") != std::string::npos && !f.core.diagnostic().empty(),
          "Controller and backend preserve the failed stop diagnostic");
    f.setup.script->reject_zero_release = f.setup.script->throw_zero_release = false;
    f.step({Event::system(EventKind::ClearFault)});
    check(f.core.snapshot().state == State::Disarmed && !f.setup.backend->authority_owned(),
          "Fault clear cannot inherit authority from the failed release");
    f.setup.script->block_check = true;
    const auto previous_checks = f.setup.script->checks;
    f.step({Event::manual(State::Align)});
    until([&] { std::lock_guard<std::mutex> lock(f.setup.script->mutex); return f.setup.script->checks > previous_checks; },
          "fresh authority check after failed release");
    check(!f.setup.backend->authority_owned() && f.setup.script->commands.size() == count,
          "New Align waits for a fresh SDK check and cannot send with stale ownership");
    f.setup.script->unblock();
    until([&] { f.step({}, .12); return f.setup.script->commands.size() > count; }, "new alignment after manual recovery");
    check(f.setup.script->commands.size() > count, "Fresh successful acquisition restores alignment after manual recovery");
    f.core.shutdown();
  }
  {
    ControlSetup f(config); f.setup.script->block_release = true;
    f.step({Event::manual(State::Align)}); entered(f.setup.script, true);
    auto stopping = std::async(std::launch::async, [&] { return f.step({Event::system(EventKind::EmergencyStop)}); });
    const bool prompt = stopping.wait_for(200ms) == std::future_status::ready;
    if (!prompt) f.setup.script->unblock();
    check(prompt, "Production controller emergency stop is independent of blocked authority RPC"); stopping.get();
    f.setup.script->unblock();
    until([&] { return f.setup.backend->authority_record().release_code.has_value(); }, "late release result");
    f.setup.clock.advance(101ms); f.step();
    check(f.core.snapshot().state == State::Disarmed && f.setup.script->commands.empty() && !f.setup.backend->authority_owned(),
          "Late cancelled success never begins alignment or sends a first release command");
    f.step({Event::manual(State::Align)}); f.alignment();
    check(f.setup.script->checks >= 2, "Rearmed explicit alignment performs a new CheckMode after cancellation");
    f.core.shutdown();
    check(f.setup.script->calls.back() == "zero_release" && !f.setup.backend->authority_owned(),
          "Owned exit zeroes the common PD packet and revokes, with no service activation");
  }
  {
    ControlSetup f(config); f.setup.script->block_check = true;
    f.step({Event::manual(State::Align)}); entered(f.setup.script, false);
    f.setup.clock.advance(config.authority_timeout + 1ms); f.step(); f.step();
    check(f.core.snapshot().state == State::Fault && !f.setup.backend->authority_owned() && f.setup.script->commands.empty(),
          "Overall authority deadline faults promptly while the bounded SDK RPC is still in flight");
    f.setup.script->unblock();
  }
}

void transport(const fs::path& configs) {
  const auto sim = load_mode_config(configs / "sim.yaml", RuntimeMode::Sim);
  const auto real = load_mode_config(configs / "real.yaml", RuntimeMode::Real);
  check(sim.transport.odometry_topic == "rt/sportmodestate" && real.transport.odometry_topic == "rt/odommodestate" &&
        real.transport.base_frame == "unitree_base" && real.transport.odometry_frame == "unitree_odom",
        "Selected config distinguishes G1 odometry and real coordinate frames from simulator metadata");
  validate_transport_endpoint(RuntimeMode::Real, "robot_nic", 0);
  rejected([&] { validate_transport_endpoint(RuntimeMode::Real, "lo", 0); }, "Real loopback accepted");
  rejected([&] { validate_transport_endpoint(RuntimeMode::Real, "", 0); }, "Implicit real NIC accepted");
  rejected([&] { validate_transport_endpoint(RuntimeMode::Real, "robot_nic", 233); }, "Invalid domain accepted");
  NativeTransport inactive;
  check(!inactive.write(encode_release(5)), "Unstarted actual transport is inert");
  LowState low;
  low.mode_machine() = 5; low.tick() = 1; low.imu_state().quaternion()[0] = 1;
  const auto sign = [&] {
    std::array<uint32_t, sizeof(low) / 4> words{};
    std::memcpy(words.data(), &low, sizeof(low));
    low.crc() = crc_words(words.data(), words.size() - 1);
  };
  sign();
  check(decode_g1_feedback(low).joints.position.size() == 29, "Production HG decoder accepts canonical29 mode5");
  low.mode_machine() = 4; sign();
  rejected([&] { decode_g1_feedback(low); }, "Unsupported G1 machine variant accepted");
  low.mode_machine() = 5; low.motor_state()[0].q() = std::numeric_limits<float>::quiet_NaN(); sign();
  rejected([&] { decode_g1_feedback(low); }, "Malformed physical HG feedback accepted");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    check(argc == 3, "Usage: authority_test authority|cancel|lifecycle|transport CONFIG_DIR");
    const auto configs = fs::canonical(argv[2]); const std::string suite = argv[1];
    if (suite == "authority") authority_contracts(); else if (suite == "cancel") cancellation_contracts();
    else if (suite == "lifecycle") lifecycle(configs); else if (suite == "transport") transport(configs);
    else throw std::runtime_error("Unknown real backend suite");
    std::cout << suite << " real backend contracts passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
