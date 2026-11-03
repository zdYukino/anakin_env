// Production adapters and SDL virtual devices only. No ROS/SDK contexts.
#include <anakin_env/input.hpp>
#include <anakin_env/web_input.hpp>
#include <yaml-cpp/yaml.h>
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>

using namespace anakin::env;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
void check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
template <class Fn> void rejected(Fn&& fn, const std::string& message) {
  try { fn(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}
bool action(const InputFrame& frame, InputAction expected) {
  return std::find(frame.actions.begin(), frame.actions.end(), expected) != frame.actions.end();
}
bool event(const InputFrame& frame, EventKind expected) {
  for (const auto& value : frame.events) if (value.kind == expected) return true;
  return false;
}
InputTime start() { return InputTime{} + 10s; }
InputSample web(InputTime time, std::optional<InputAction> command = std::nullopt) {
  InputSample sample;
  sample.source = InputSource::Web;
  sample.connected = true;
  sample.acquired = time;
  sample.action = command;
  return sample;
}
InputSample remote(uint16_t keys, InputTime time, const std::array<float, 4>& axes = {}) {
  return UnitreeRemoteAdapter::wireless(keys, axes, time);
}
void mapper(const fs::path& configs) {
  auto config = load_input_config(configs / "inputs.yaml");
  auto now = start();
  InputHub hub(config, 250ms);
  check(!hub.poll(now).snapshot.primary_online, "Absent inputs do not invent an owner");
  hub.submit(remote(0, now));
  hub.submit(web(now));
  check(!hub.poll(now).snapshot.active_source, "Neutral device heartbeats never seize control");
  hub.submit(web(now += 1ms, InputAction::Align));
  check(action(hub.poll(now), InputAction::Align), "Deliberate web action acquires an idle hub");
  hub.submit(remote(0, now += 1ms));
  check(hub.poll(now).snapshot.active_source == InputSource::Web, "Neutral Unitree heartbeat does not steal web control");
  hub.submit(web(now += 1ms, InputAction::Hold));
  hub.submit(remote(0x0403, now));
  const auto conflict = hub.poll(now);
  check(conflict.snapshot.active_source == InputSource::Remote && conflict.actions.size() == 1 && action(conflict, InputAction::Align),
        "Same-cycle Unitree action wins over web and losing action is discarded");
  hub.submit(web(now += 1ms, InputAction::MotionToggle));
  check(hub.poll(now).snapshot.active_source == InputSource::Web, "A new deliberate web command can take over an idle remote");
  hub.submit(remote(0x0100, now += 1ms));
  check(stop_requested(hub.snapshot(now)), "Non-owner stop reaches the final write admission check");
  const auto stopped = hub.poll(now);
  check(stopped.actions.size() == 1 && action(stopped, InputAction::EmergencyStop) && !stopped.snapshot.moving,
        "Every-source emergency suppresses motion and ordinary requests");
  hub.submit(remote(0, now += 1ms));
  hub.poll(now);
  hub.submit(remote(0x0200, now += 1ms));
  hub.submit(remote(0, now += 1ms));
  check(event(hub.poll(now), EventKind::EmergencyStop), "Brief stop is latched between scheduler cycles");
  config.enabled_sources = {InputSource::Web};
  InputHub disabled(config, 250ms);
  disabled.submit(remote(0x0100, now));
  check(!stop_requested(disabled.snapshot(now)), "Disabled sources cannot inject stops");
  config = load_input_config(configs / "inputs.yaml");
  InputHub three(config, 250ms);
  auto pad = web(now); pad.source = InputSource::Gamepad;
  three.submit(pad); three.submit(web(now)); three.submit(remote(0, now)); three.poll(now);
  pad.acquired = now += 1ms; pad.buttons.set(static_cast<size_t>(Button::X)); pad.shoulders = 3;
  three.submit(pad); three.submit(web(now, InputAction::Hold));
  check(three.poll(now).snapshot.active_source == InputSource::Gamepad, "Physical Joy wins a same-cycle web conflict");
  three.submit(remote(0x0403, now += 1ms));
  pad.acquired = now; pad.buttons.reset(); pad.shoulders = 0; three.submit(pad);
  check(three.poll(now).snapshot.active_source == InputSource::Remote, "Deliberate Unitree action takes over Joy");
  three.submit(web(now += 251ms));
  const auto lost_owner = three.poll(now);
  check(event(lost_owner, EventKind::SourceLost) && lost_owner.lost_source == InputSource::Remote,
        "Another online device cannot mask current-owner freshness loss");
  config = load_input_config(configs / "inputs.yaml");
  config.primary = InputSource::Remote;
  for (const bool explicit_disconnect : {false, true}) {
    InputHub replacement(config, 250ms);
    replacement.submit(remote(0, now));
    replacement.poll(now);
    replacement.submit(remote(0x8000, now += 1ms, {0.f, 1.f, 0.f, 0.f}));
    check(replacement.poll(now).snapshot.moving, "Original primary enables movement");
    replacement.submit(remote(0x1003, now += 1ms));  // Queued old-device Hold request.
    if (explicit_disconnect)
      replacement.submit(UnitreeRemoteAdapter::wireless(0, {}, now += 1ms, false));
    auto reconnected = remote(0x8000, now += 1ms, {0.f, 1.f, 0.f, 0.f});
    reconnected.connection = 1;
    replacement.submit(reconnected);
    check(replacement.snapshot(now).velocity == std::array<double, 3>{},
          "Sender snapshot zeros motion while primary loss awaits scheduler consumption");
    const auto replaced = replacement.poll(now);
    check(replaced.snapshot.primary_online && event(replaced, EventKind::SourceLost) &&
          replaced.actions.empty() && !replaced.snapshot.moving &&
          replaced.snapshot.velocity == std::array<double, 3>{},
          "Primary replacement/loss between polls is retained, zeros motion and drops queued controls");
    check(!event(replacement.poll(now), EventKind::SourceLost), "Latched primary loss is consumed once");
  }
  InputHub out_of_order(config, 250ms);
  out_of_order.submit(remote(0, now));
  out_of_order.poll(now);
  out_of_order.submit(UnitreeRemoteAdapter::wireless(0, {}, now + 2ms, false));
  out_of_order.submit(remote(0, now + 1ms));
  const auto disconnected = out_of_order.poll(now + 3ms);
  check(!disconnected.snapshot.primary_online && event(disconnected, EventKind::SourceLost),
        "Older cached input cannot resurrect a disconnected primary");

  for (const auto mode : {RuntimeMode::Sim, RuntimeMode::Real}) {
    const auto mode_config = load_mode_config(configs / (to_string(mode) + ".yaml"), mode);
    config = load_input_config(configs / "inputs.yaml");
    config.primary = InputSource::Remote;
    InputHub lost(config, mode_config.guards.input_timeout);
    StateMachine machine(mode_config);
    if (mode == RuntimeMode::Sim) machine.dispatch({Event::manual(State::Hold)});
    else {
      machine.dispatch({Event::manual(State::Align)});
      machine.dispatch({Event::completion(EventKind::AlignmentComplete, machine.snapshot().generation)});
    }
    lost.submit(remote(0, now));
    lost.poll(now);
    auto frame = lost.poll(now += 251ms);
    check(event(frame, EventKind::SourceLost) && frame.snapshot.velocity == std::array<double, 3>{},
          "Primary freshness loss emits a SourceLost edge and zeros velocity");
    const auto result = machine.dispatch(frame.events);
    check(result.after.state == State::Hold,
          "Input loss retains the current state in both simulation and real mode");
    check(!event(lost.poll(now + 1ms), EventKind::SourceLost), "Lost event is an edge, not an idle heartbeat");
    lost.submit(remote(0x1003, now += 2ms));
    check(lost.poll(now).events.empty(), "Reconnect with held ordinary combo creates no fake press");
    lost.submit(remote(0, now += 1ms));
    lost.poll(now);
    lost.submit(remote(0x1003, now += 1ms));
    check(action(lost.poll(now), InputAction::Hold), "Release and deliberate re-press works after reconnect");
  }

  config = load_input_config(configs / "inputs.yaml");
  config.primary = InputSource::Remote;
  InputHub axes(config, 250ms);
  axes.submit(remote(0, now));
  axes.poll(now);
  axes.submit(remote(0x8000, now += 1ms, {.01f, 1.f, -.5f, 0.f}));
  const auto moving = axes.poll(now);
  check(moving.snapshot.moving && moving.snapshot.velocity == std::array<double, 3>{.5, 0., .5},
        "Configured deadzone/scales and motion toggle apply to normalized axes");
  axes.submit(remote(0, now += 1ms, {.5f, 0.f, 0.f, 0.f}));
  check(axes.poll(now).snapshot.velocity[1] == -.25, "Configured lateral sign is retained");
  auto invalid = remote(0, now);
  invalid.axes[0] = std::numeric_limits<float>::quiet_NaN();
  rejected([&] { axes.submit(invalid); }, "Nonfinite normalized input accepted");

  // Exercise the same mutex-backed API used by callbacks and the sender thread.
  InputHub concurrent(config, 250ms);
  std::thread producer([&] {
    for (int i = 0; i < 1000; ++i) concurrent.submit(remote(i % 2 ? 0x0100 : 0, now + std::chrono::nanoseconds(i)));
  });
  for (int i = 0; i < 1000; ++i) (void)concurrent.snapshot(now);
  producer.join();
  check(stop_requested(concurrent.snapshot(now + 1000ns)), "Concurrent samples preserve pending/held stop");
}

void web_contracts(const fs::path& configs) {
  const auto config = load_input_config(configs / "inputs.yaml");
  auto now = start();
  WebInput adapter(config, 250ms, "env-test");
  YAML::Node packet = YAML::Load("{v: 1, environment: env-test, session: abc123, sequence: 1, sent_ms: 1000, operation: activate, axes: [0,0,0,0], buttons: [false,false,false,false,false,false,false,false,false,false], shoulders: 0}");
  auto send = [&] { return adapter.receive(YAML::Dump(packet), now, 1000); };
  const auto initial = send();
  check(initial.connected && initial.activate && adapter.owner() == "abc123", "Explicit neutral activation obtains a lease");
  rejected(send, "Repeated activation accepted");
  packet["session"] = "def456"; packet["operation"] = "input"; packet["sequence"] = 2;
  rejected(send, "Another web session stole a lease");
  packet["session"] = "abc123"; packet["action"] = "align";
  const auto command = send();
  check(command.action == InputAction::Align && command.connection == initial.connection, "Configured action keeps the current lease");
  rejected(send, "Replayed web sequence accepted");
  packet["sequence"] = 3; packet["axes"][0] = ".nan";
  rejected(send, "Nonfinite web axis accepted");
  packet["axes"][0] = 0; packet["sent_ms"] = 500;
  rejected(send, "Queued expired web command accepted");
  packet["sent_ms"] = 1000; packet["environment"] = "old-env";
  rejected(send, "Old ENV session accepted");
  packet["environment"] = "env-test"; packet["action"] = "publish";
  rejected(send, "Unconfigured web action accepted");
  now += 251ms;
  check(adapter.expire(now).has_value() && adapter.owner().empty(), "Missing heartbeat expires and disconnects the lease");
  packet.remove("action");
  rejected(send, "Input automatically reactivated an expired lease");
  packet["operation"] = "activate"; packet["sequence"] = 1;
  check(send().connection != initial.connection, "Explicit reactivation receives a new connection identity");
  packet["operation"] = "release"; packet["sequence"] = 2;
  check(!send().connected && adapter.owner().empty(), "Release immediately revokes the lease");
}

void remote_contracts(const fs::path& configs) {
  auto config = load_input_config(configs / "inputs.yaml");
  config.primary = InputSource::Remote;
  auto now = start();
  const std::vector<std::pair<uint16_t, InputAction>> routes{
      {0x1001, InputAction::AsapCr7}, {0x1003, InputAction::Hold},
      {0x4001, InputAction::AsapLoco}, {0x4003, InputAction::Disarmed},
      {0x0403, InputAction::Align}, {0x0803, InputAction::ClearFault},
      {0x8000, InputAction::MotionToggle}, {0x1000, InputAction::HeightUp},
      {0x4000, InputAction::HeightDown}};
  for (const auto& route : routes) {
    InputHub hub(config, 250ms);
    std::array<uint8_t, 40> bytes{};
    bytes[0] = 0xfe; bytes[1] = 0xef;
    hub.submit(UnitreeRemoteAdapter::lowstate(bytes, now));
    hub.poll(now);
    bytes[2] = route.first & 0xff;
    bytes[3] = route.first >> 8;
    const auto sample = UnitreeRemoteAdapter::lowstate(bytes, now += 1ms);
    hub.submit(sample);
    const auto result = hub.poll(now);
    check(result.actions.size() == 1 && action(result, route.second), "Recorded HG combination maps to exactly one confirmed action");
    check(sample.shoulders == ((route.first & 3) == 1 ? shoulder_r1 : (route.first & 3)),
          "Unitree raw R1 bit0 converts semantically to normalized R1=2");
  }
  InputHub held(config, 250ms);
  held.submit(remote(0x1001, now));
  check(held.poll(now).events.empty(), "Held startup remote ordinary buttons are suppressed");
  held.submit(remote(0x1103, now += 1ms));
  const auto stopped = held.poll(now);
  check(stopped.actions.size() == 1 && action(stopped, InputAction::EmergencyStop), "A dominates shoulders and ordinary buttons");
  InputHub initial_stop(config, 250ms);
  initial_stop.submit(remote(0x0200, now));
  check(event(initial_stop.poll(now), EventKind::EmergencyStop), "Held startup emergency remains effective");
  check(UnitreeRemoteAdapter::wireless(2, {}, now).shoulders == shoulder_l1, "Raw L1 bit1 maps to normalized L1=1");
  std::array<uint8_t, 40> bytes{};
  check(!UnitreeRemoteAdapter::lowstate(bytes, now).connected, "Missing HG remote header is unavailable");
  bytes[0] = 0xfe; bytes[1] = 0xef;
  const float values[]{.25f, .5f, -.75f, -1.f};
  const size_t offsets[]{4, 20, 8, 12};
  for (size_t i = 0; i < 4; ++i) std::memcpy(bytes.data() + offsets[i], &values[i], sizeof(float));
  const auto recorded = UnitreeRemoteAdapter::lowstate(bytes, now);
  check(recorded.axes == std::array<float, 4>{.25f, .5f, -.75f, -1.f} && recorded.acquired == now,
        "HG byte offsets and original monotonic acquisition time are preserved");
  const auto bad = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(bytes.data() + 4, &bad, sizeof(float));
  rejected([&] { UnitreeRemoteAdapter::lowstate(bytes, now); }, "Malformed remote axis accepted");
}

struct VirtualPad {
  int device = -1;
  SDL_Joystick* joystick = nullptr;
  VirtualPad() {
    device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                      SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    check(device >= 0, std::string("Cannot attach virtual SDL pad: ") + SDL_GetError());
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device), guid, sizeof(guid));
    const std::string mapping = std::string(guid) + ",Anakin Virtual,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,"
        "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
        "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Linux,";
    check(SDL_GameControllerAddMapping(mapping.c_str()) >= 0, "Cannot install virtual pad mapping");
    joystick = SDL_JoystickOpen(device);
    check(joystick && SDL_IsGameController(device), "Virtual device is a usable GameController");
  }
  ~VirtualPad() { detach(); }
  void detach() {
    if (joystick) { SDL_JoystickClose(joystick); joystick = nullptr; }
    if (device >= 0) { SDL_JoystickDetachVirtual(device); device = -1; }
  }
  void button(SDL_GameControllerButton button, bool down) {
    check(SDL_JoystickSetVirtualButton(joystick, button, down) == 0, "Cannot inject virtual button");
  }
};
void sdl(const fs::path& configs) {
  check(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0, "Cannot initialize SDL fixture");
  {
    VirtualPad pad;
    auto config = load_input_config(configs / "inputs.yaml");
    config.primary = InputSource::Gamepad;
    InputHub hub(config, 250ms);
    SdlGamepad adapter(pad.device);
    auto now = start();
    pad.button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_UP, true);
    auto sample = adapter.poll(now);
    const auto connection = sample.connection;
    hub.submit(sample);
    check(hub.poll(now).actions.empty(), "SDL startup-held ordinary combo creates no fake edge");
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
    hub.submit(adapter.poll(now += 1ms));
    hub.poll(now);
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_UP, true);
    hub.submit(adapter.poll(now += 1ms));
    check(action(hub.poll(now), InputAction::AsapCr7), "SDL physical RB maps to R1-only CR7");
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
    hub.submit(adapter.poll(now += 1ms)); hub.poll(now);
    pad.button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, true);
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_UP, true);
    hub.submit(adapter.poll(now += 1ms));
    const auto both = hub.poll(now);
    check(both.actions.size() == 1 && action(both, InputAction::Hold), "SDL LB+RB Up selects Hold only");
    pad.button(SDL_CONTROLLER_BUTTON_B, true);
    hub.submit(adapter.poll(now += 1ms));
    check(stop_requested(hub.snapshot(now)) && action(hub.poll(now), InputAction::EmergencyStop),
          "SDL B stop feeds the same pre-write/scheduler predicate");
    pad.button(SDL_CONTROLLER_BUTTON_B, false);
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
    pad.button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, false);
    pad.button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, false);
    hub.submit(adapter.poll(now += 1ms)); hub.poll(now);
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, true);
    check(SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_LEFTY, -32768) == 0, "Cannot inject virtual axis");
    hub.submit(adapter.poll(now += 1ms));
    check(hub.poll(now).snapshot.velocity[0] == .5, "SDL negative-up Y is normalized to positive-forward");
    pad.button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, false);
    SDL_JoystickSetVirtualAxis(pad.joystick, SDL_CONTROLLER_AXIS_LEFTY, 0);
    for (int i = 0; i < 100; ++i) {
      hub.submit(adapter.poll(now += 20ms));
      const auto idle = hub.poll(now);
      check(idle.snapshot.primary_online && !event(idle, EventKind::SourceLost),
            "Neutral attached SDL controller stays online without state changes");
    }
    pad.detach();
    hub.submit(adapter.poll(now += 1ms));
    check(event(hub.poll(now), EventKind::SourceLost), "SDL detach produces primary loss");
    VirtualPad replacement;
    replacement.button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, true);
    replacement.button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
    sample = adapter.poll(now += 1ms);
    hub.submit(sample);
    check(sample.connected && sample.connection != connection && hub.poll(now).actions.empty(),
          "SDL hotplug replacement gets a new identity and no startup-held mode edge");
  }
  {
    // Reproduce the Series X Bluetooth layout, which differs from old SDL's
    // generic Xbox fallback. Use the production YAML with a virtual device GUID.
    VirtualPad xbox;
    const auto config = load_input_config(configs / "inputs.yaml");
    auto mapping = config.gamepad_mappings.at(0);
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(xbox.device), guid, sizeof(guid));
    mapping.replace(0, 32, guid);
    SdlGamepad adapter(xbox.device, {mapping});
    const auto axis = [&](int index, Sint16 value) {
      check(SDL_JoystickSetVirtualAxis(xbox.joystick, index, value) == 0, "Cannot inject Xbox axis");
    };
    auto now = start();
    axis(2, 8192); axis(3, -16384); axis(4, -32768); axis(5, -32768);
    auto sample = adapter.poll(now);
    check(std::abs(sample.axes[2] - .25f) < .001f && sample.axes[3] == -.5f &&
          sample.triggers && (*sample.triggers)[0] == 0.f && (*sample.triggers)[1] == 0.f,
          "Xbox right stick is independent of released LT/RT");
    axis(4, 32767);
    sample = adapter.poll(now += 1ms);
    check(sample.axes[3] == -.5f && (*sample.triggers)[1] == 1.f && (*sample.triggers)[0] == 0.f,
          "Full RT cannot move the right stick or LT");
    axis(5, 32767);
    SDL_JoystickSetVirtualButton(xbox.joystick, 6, 1);
    SDL_JoystickSetVirtualButton(xbox.joystick, 7, 1);
    SDL_JoystickSetVirtualButton(xbox.joystick, 3, 1);
    sample = adapter.poll(now += 1ms);
    check((*sample.triggers)[0] == 1.f && (*sample.triggers)[1] == 1.f &&
          sample.shoulders == (shoulder_l1 | shoulder_r1) && sample.buttons.test(static_cast<size_t>(Button::X)),
          "Xbox LB/RB and face buttons use their actual Bluetooth indices");
    check(sample.device && sample.device->mapping.find("righty:a3") != std::string::npos,
          "Effective device mapping is available for diagnostics");
  }
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}

void inspect(const fs::path& configs) {
  const auto config = load_input_config(configs / "inputs.yaml");
  SdlGamepad adapter(config.gamepad_device_index, config.gamepad_mappings);
  InputSample sample;
  for (int i = 0; i < 30; ++i) {
    sample = adapter.poll();
    std::this_thread::sleep_for(10ms);
  }
  check(sample.connected && sample.device, "No connected gamepad");
  std::cout << "Read-only, no ROS/SDK/control context\n" << sample.device->name << '\n'
            << sample.device->guid << '\n' << sample.device->mapping << '\n';
  std::cout << "Sticks:";
  for (const auto axis : sample.axes) std::cout << ' ' << axis;
  if (sample.triggers) std::cout << " LT=" << (*sample.triggers)[0] << " RT=" << (*sample.triggers)[1];
  std::cout << " LB=" << bool(sample.shoulders & shoulder_l1) << " RB=" << bool(sample.shoulders & shoulder_r1) << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 3, "Usage: anakin_env_input_test mapper|web|sdl|remote CONFIG_DIR");
    const auto configs = fs::canonical(argv[2]);
    const std::string suite = argv[1];
    if (suite == "mapper") mapper(configs);
    else if (suite == "web") web_contracts(configs);
    else if (suite == "sdl") sdl(configs);
    else if (suite == "remote") remote_contracts(configs);
    else if (suite == "inspect") inspect(configs);
    else throw std::runtime_error("Unknown input suite");
    std::cout << suite << " input contracts passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
