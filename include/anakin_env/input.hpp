#pragma once

#include <anakin_env/state_machine.hpp>

#include <bitset>
#include <memory>
#include <mutex>

namespace anakin::env {

using InputClock = std::chrono::steady_clock;
using InputTime = InputClock::time_point;
constexpr size_t input_button_count = 10;

struct GamepadDevice {
  std::string name, guid, mapping;
};

struct InputSample {
  InputSource source = InputSource::Web;
  InputTime acquired{};
  bool connected = false;
  uint64_t connection = 0;  // Changes when a local device is replaced/reopened.
  std::bitset<input_button_count> buttons;
  uint8_t shoulders = 0;
  std::array<float, 4> axes{};  // left X, forward-positive left Y, right X, right Y
  std::optional<InputAction> action;  // One explicit web command, never repeated by a heartbeat.
  bool activate = false;  // An explicit neutral web activation may request ownership.
  std::optional<std::array<float, 2>> triggers;  // LT, RT in 0..1, independent of the four stick axes.
  std::shared_ptr<const GamepadDevice> device;
};

struct InputSnapshot {
  std::array<InputSample, 3> sources;
  std::array<bool, 3> online{};
  std::optional<InputSource> active_source;
  bool primary_online = false;
  InputTime primary_acquired{};
  bool emergency_stop = false;
  std::array<bool, 3> emergency_sources{};
  bool moving = false;
  double height = .78;
  std::array<double, 3> velocity{};  // forward, lateral, yaw
};

// Same normalized decision for the scheduler and the final pre-write check.
bool stop_requested(const InputSnapshot& snapshot);
std::optional<Event> input_event(InputAction action);

struct InputFrame {
  InputSnapshot snapshot;
  std::vector<InputAction> actions;
  std::vector<Event> events;
  std::optional<InputSource> lost_source = std::nullopt;
};

// submit may run from remote callbacks. poll consumes ordinary actions once per
// control cycle; snapshot is a synchronized, non-consuming sender view. No devices
// or threads are constructed here. SourceLost is a primary-online -> offline edge;
// StateMachine applies the selected mode's SourceLossPolicy.
class InputHub {
 public:
  InputHub(InputConfig config, std::chrono::milliseconds source_timeout);
  void submit(const InputSample& sample);
  InputFrame poll(InputTime now = {});
  InputSnapshot snapshot(InputTime now = {}) const;

 private:
  struct SourceState {
    InputSample sample;
    bool initialized = false;
    std::bitset<input_button_count> armed;
    bool axes_armed = false, activity = false;
    std::optional<InputTime> pending_stop;
  };
  struct Pending {
    InputAction action;
    InputTime acquired;
    InputSource source;
  };
  bool enabled(InputSource source) const;
  InputSnapshot view(InputTime now) const;
  void apply(InputAction action);
  InputConfig config_;
  std::chrono::milliseconds timeout_;
  mutable std::mutex mutex_;
  std::array<SourceState, 3> sources_;
  std::vector<Pending> pending_;
  bool primary_was_online_ = false, pending_source_lost_ = false, moving_ = false;
  std::optional<InputSource> active_;
  double height_;
};

// Call from the owning SDL thread. Polls current attachment/state every cycle,
// including neutral devices; no window or event-change heartbeat is required.
class SdlGamepad {
 public:
  explicit SdlGamepad(int device_index = 0, const std::vector<std::string>& mappings = {});
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
