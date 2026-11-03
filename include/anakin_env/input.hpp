#pragma once

#include <anakin_env/state_machine.hpp>

#include <bitset>
#include <memory>
#include <mutex>

namespace anakin::env {

using InputClock = std::chrono::steady_clock;
using InputTime = InputClock::time_point;
constexpr size_t input_button_count = 10;

struct InputSample {
  InputSource source = InputSource::Keyboard;
  InputTime acquired{};
  bool connected = false;
  uint64_t connection = 0;  // Changes when a local device is replaced/reopened.
  std::bitset<input_button_count> buttons;
  uint8_t shoulders = 0;
  std::array<float, 4> axes{};  // left X, forward-positive left Y, right X, right Y
  std::vector<Key> keys;       // Terminal press tokens, not physical held/key-up states.
};

struct InputSnapshot {
  std::array<InputSample, 3> sources;
  std::array<bool, 3> online{};
  bool primary_online = false;
  InputTime primary_acquired{};
  bool emergency_stop = false;
  bool moving = false;
  double height = .78;
  std::array<double, 3> velocity{};  // forward, lateral, yaw
};

// Same normalized decision for the scheduler and the final pre-write check.
bool stop_requested(const InputSnapshot& snapshot);

struct InputFrame {
  InputSnapshot snapshot;
  std::vector<InputAction> actions;
  std::vector<Event> events;
};

// submit may run from remote callbacks. poll consumes ordinary actions once per
// control cycle; snapshot is a synchronized, non-consuming sender view. No devices
// or threads are constructed here. SourceLost is a primary-online -> offline edge;
// StateMachine applies the selected mode's SourceLossPolicy.
class InputHub {
 public:
  InputHub(InputConfig config, std::chrono::milliseconds source_timeout);
  void submit(const InputSample& sample);
  InputFrame poll(InputTime now = InputClock::now());
  InputSnapshot snapshot(InputTime now = InputClock::now()) const;

 private:
  struct SourceState {
    InputSample sample;
    bool initialized = false;
    std::bitset<input_button_count> armed;
    InputTime keyboard_stop_until{};
    std::optional<InputTime> pending_stop;
  };
  struct Pending {
    InputAction action;
    InputTime acquired;
  };
  bool enabled(InputSource source) const;
  InputSnapshot view(InputTime now) const;
  void apply(InputAction action, InputTime acquired, InputTime now);
  InputConfig config_;
  std::chrono::milliseconds timeout_;
  mutable std::mutex mutex_;
  std::array<SourceState, 3> sources_;
  std::vector<Pending> pending_;
  bool primary_was_online_ = false, pending_source_lost_ = false, moving_ = false;
  double height_;
  std::array<double, 3> keyboard_velocity_{};
  std::array<InputTime, 3> keyboard_expiry_{};
};

// Foreground terminal/SSH input. Owns a duplicate of fd and restores termios on
// destruction, restore(), or stack unwinding. Runtime signal shutdown must unwind
// its owner normally. A non-TTY fd is an explicit error, not a silent fallback.
class TerminalKeyboard {
 public:
  explicit TerminalKeyboard(int fd = 0);
  ~TerminalKeyboard();
  TerminalKeyboard(const TerminalKeyboard&) = delete;
  TerminalKeyboard& operator=(const TerminalKeyboard&) = delete;
  InputSample poll(InputTime now = InputClock::now());
  void restore() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Call from the owning SDL thread. Polls current attachment/state every cycle,
// including neutral devices; no window or event-change heartbeat is required.
class SdlGamepad {
 public:
  explicit SdlGamepad(int device_index = 0);
  ~SdlGamepad();
  SdlGamepad(const SdlGamepad&) = delete;
  SdlGamepad& operator=(const SdlGamepad&) = delete;
  InputSample poll(InputTime now = InputClock::now());

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Pure HG lowstate/wirelesscontroller payload adaptation. The caller supplies the
// original local acquisition time; rereading a cached packet must not renew it.
class UnitreeRemoteAdapter {
 public:
  static InputSample lowstate(const std::array<uint8_t, 40>& bytes, InputTime acquired);
  static InputSample wireless(uint16_t keys, const std::array<float, 4>& axes,
                              InputTime acquired, bool valid = true);
};

}  // namespace anakin::env
