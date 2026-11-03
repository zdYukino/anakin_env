// Production adapters, PTY and SDL virtual devices only. No ROS/SDK contexts.
#include <anakin_env/input.hpp>
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

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
InputSample keyboard(InputTime time, std::vector<Key> keys = {}) {
  InputSample sample;
  sample.source = InputSource::Keyboard;
  sample.connected = true;
  sample.acquired = time;
  sample.keys = std::move(keys);
  return sample;
}
InputSample remote(uint16_t keys, InputTime time, const std::array<float, 4>& axes = {}) {
  return UnitreeRemoteAdapter::wireless(keys, axes, time);
}
void mapper(const fs::path& configs) {
  auto config = load_input_config(configs / "inputs.yaml");
  auto now = start();
  InputHub hub(config, 250ms);
  check(!hub.poll(now).snapshot.primary_online && hub.poll(now).events.empty(),
        "Absent primary waits without fabricating startup source loss");
  hub.submit(keyboard(now));
  hub.submit(remote(0, now));
  hub.poll(now);
  hub.submit(remote(0x0403, now += 1ms));
  check(hub.poll(now).events.empty(), "Non-primary ordinary control is ignored");
  hub.submit(keyboard(now, {Key::Digit3, Key::G}));
  hub.submit(remote(0x0103, now));
  check(stop_requested(hub.snapshot(now)), "Sender sees enabled non-primary stop before scheduler");
  const auto stopped = hub.poll(now);
  check(stopped.actions.size() == 1 && action(stopped, InputAction::EmergencyStop) &&
        event(stopped, EventKind::EmergencyStop) && !stopped.snapshot.moving,
        "All-source stop dominates primary mode and motion actions in the same cycle");
  hub.submit(remote(0, now += 1ms));
  check(!stop_requested(hub.snapshot(now)), "Released controller stop clears normalized held predicate");
  hub.submit(remote(0x0200, now += 1ms));
  hub.submit(remote(0, now += 1ms));
  check(stop_requested(hub.snapshot(now)) && event(hub.poll(now), EventKind::EmergencyStop),
        "Brief stop between scheduler cycles is latched and delivered");

  config.enabled_sources = {InputSource::Keyboard};
  InputHub disabled(config, 250ms);
  disabled.submit(keyboard(now));
  disabled.submit(remote(0x0100, now));
  check(!stop_requested(disabled.snapshot(now)), "Disabled sources cannot inject stops");

  config = load_input_config(configs / "inputs.yaml");
  // Remap the production predicate itself: no hidden A/B test is involved.
  auto& bindings = config.controller_bindings;
  bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [](const InputBinding& binding) {
    const auto button = std::get<ButtonBinding>(binding.control).button;
    return button == Button::A || button == Button::B || button == Button::X;
  }), bindings.end());
  bindings.push_back({ButtonBinding{Button::X, 0, true}, Trigger::Held, InputAction::EmergencyStop});
  InputHub remapped(config, 250ms);
  remapped.submit(remote(0x0100, now));
  check(!stop_requested(remapped.snapshot(now)), "A is not secretly hardcoded as a stop");
  remapped.submit(remote(0x0400, now += 1ms));
  check(stop_requested(remapped.snapshot(now)) && event(remapped.poll(now), EventKind::EmergencyStop),
        "One mapping drives both sender predicate and scheduler stop");

  config = load_input_config(configs / "inputs.yaml");
  for (const auto source : {InputSource::Keyboard, InputSource::Remote}) {
    InputHub stale_stop(config, 250ms);
    const auto stopped_sample = source == InputSource::Keyboard
        ? keyboard(now, {Key::Space}) : remote(0x0100, now);
    stale_stop.submit(stopped_sample);
    check(!stop_requested(stale_stop.snapshot(now - 1ms)) &&
          !event(stale_stop.poll(now - 1ms), EventKind::EmergencyStop),
          "Future-acquired stop is not usable before its acquisition time");
    stale_stop.submit(stopped_sample);
    check(event(stale_stop.poll(now), EventKind::EmergencyStop), "Fresh stop remains effective");
    stale_stop.submit(stopped_sample);
    check(!stop_requested(stale_stop.snapshot(now + 251ms)) &&
          !event(stale_stop.poll(now + 251ms), EventKind::EmergencyStop),
          "Rereading a cached stop cannot bypass acquisition freshness");
  }

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
    check(result.after.state == (mode == RuntimeMode::Sim ? State::Hold : State::Fault),
          "A's selected mode config owns retain-mode versus fault behavior");
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
    for (int i = 0; i < 1000; ++i) concurrent.submit(remote(i % 2 ? 0x0100 : 0, now));
  });
  for (int i = 0; i < 1000; ++i) (void)concurrent.snapshot(now);
  producer.join();
  check(stop_requested(concurrent.snapshot(now)), "Concurrent samples preserve pending/held stop");
}

struct Pty {
  int master = -1, slave = -1;
  Pty() { check(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0, "Cannot open PTY fixture"); }
  ~Pty() { if (master >= 0) close(master); if (slave >= 0) close(slave); }
  void send(const std::string& text) {
    check(write(master, text.data(), text.size()) == static_cast<ssize_t>(text.size()), "Cannot inject PTY bytes");
    pollfd ready{slave, POLLIN, 0};
    check(::poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN),
          "Injected PTY bytes did not reach the terminal input queue");
  }
};
bool same_settings(const termios& left, const termios& right) {
  return left.c_iflag == right.c_iflag && left.c_oflag == right.c_oflag &&
         left.c_cflag == right.c_cflag && left.c_lflag == right.c_lflag &&
         std::memcmp(left.c_cc, right.c_cc, sizeof(left.c_cc)) == 0;
}
void terminal(const fs::path& configs) {
  const auto config = load_input_config(configs / "inputs.yaml");
  auto now = start();
  Pty pty;
  termios original{}, current{};
  check(tcgetattr(pty.slave, &original) == 0, "Cannot read PTY settings");
  pty.send("5\n");
  pollfd queued{pty.slave, POLLIN, 0};
  check(::poll(&queued, 1, 1000) == 1 && (queued.revents & POLLIN),
        "Pre-start bytes have reached the PTY input queue before startup flush");
  {
    TerminalKeyboard keyboard_device(pty.slave);
    InputHub hub(config, 250ms);
    check(tcgetattr(pty.slave, &current) == 0 && !(current.c_lflag & (ICANON | ECHO)),
          "Terminal enters noncanonical no-echo mode");
    auto initial = keyboard_device.poll(now);
    check(initial.connected && initial.keys.empty(), "Queued startup terminal bytes are discarded");
    hub.submit(initial);
    hub.poll(now);
    const auto idle = [&](std::chrono::milliseconds duration) {
      InputFrame frame;
      while (duration.count() > 0) {
        const auto step = std::min(duration, 100ms);
        duration -= step;
        hub.submit(keyboard_device.poll(now += step));
        frame = hub.poll(now);
      }
      return frame;
    };
    const std::vector<std::pair<std::string, InputAction>> keys{
      {"1", InputAction::Disarmed}, {"2", InputAction::EmergencyStop}, {" ", InputAction::EmergencyStop},
      {"3", InputAction::Align}, {"4", InputAction::Hold}, {"5", InputAction::AsapLoco},
      {"6", InputAction::AsapCr7}, {"c", InputAction::ClearFault}, {"C", InputAction::ClearFault}};
    for (const auto& item : keys) {
      idle(300ms);
      pty.send(item.first);
      hub.submit(keyboard_device.poll(now));
      check(action(hub.poll(now), item.second), "PTY confirmed mode/fault/stop key: " + item.first);
    }
    idle(300ms);
    pty.send("gWaq");
    hub.submit(keyboard_device.poll(now));
    const auto moving = hub.poll(now);
    check(moving.snapshot.moving && moving.snapshot.velocity == std::array<double, 3>{.5, .5, 1.},
          "PTY motion keys use configured forward/lateral/yaw scales");
    const auto expired = idle(251ms);
    check(expired.snapshot.primary_online && expired.snapshot.velocity == std::array<double, 3>{},
          "Keyboard commands expire without declaring a neutral connected terminal offline");
    pty.send("sde");
    hub.submit(keyboard_device.poll(now += 1ms));
    check(hub.poll(now).snapshot.velocity == std::array<double, 3>{-.5, -.5, -1.},
          "Opposite terminal velocity keys preserve signs");
    const auto height = hub.snapshot(now).height;
    pty.send("\x1b");
    hub.submit(keyboard_device.poll(now += 1ms));
    check(hub.poll(now).actions.empty(), "Fragmented escape prefix is not a key action");
    pty.send("[");
    hub.submit(keyboard_device.poll(now += 1ms));
    check(hub.poll(now).actions.empty(), "Fragmented CSI sequence waits for direction");
    pty.send("A");
    hub.submit(keyboard_device.poll(now += 1ms));
    check(hub.poll(now).snapshot.height == height + config.height_step, "SSH CSI Up adjusts height");
    pty.send("\x1bOB");
    hub.submit(keyboard_device.poll(now += 1ms));
    check(std::abs(hub.poll(now).snapshot.height - height) < 1e-12, "Application cursor Down adjusts height");
    keyboard_device.restore();
    check(!keyboard_device.poll(now).connected, "Explicit restore closes keyboard input lifecycle");
  }
  check(tcgetattr(pty.slave, &current) == 0 && same_settings(original, current), "Terminal settings restored on normal shutdown");
  try {
    TerminalKeyboard throwing(pty.slave);
    throw std::runtime_error("fixture exception");
  } catch (const std::runtime_error&) {}
  check(tcgetattr(pty.slave, &current) == 0 && same_settings(original, current), "Terminal settings restored on exception unwinding");
  {
    TerminalKeyboard disconnected(pty.slave);
    close(pty.master);
    pty.master = -1;
    check(!disconnected.poll(now).connected, "PTY hangup is reported as disconnected");
  }
  int pipes[2];
  check(pipe(pipes) == 0, "Cannot create non-TTY fixture");
  rejected([&] { TerminalKeyboard invalid(pipes[0]); }, "Non-TTY keyboard accepted");
  close(pipes[0]); close(pipes[1]);
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
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 3, "Usage: anakin_env_input_test mapper|terminal|sdl|remote CONFIG_DIR");
    const auto configs = fs::canonical(argv[2]);
    const std::string suite = argv[1];
    if (suite == "mapper") mapper(configs);
    else if (suite == "terminal") terminal(configs);
    else if (suite == "sdl") sdl(configs);
    else if (suite == "remote") remote_contracts(configs);
    else throw std::runtime_error("Unknown input suite");
    std::cout << suite << " input contracts passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
