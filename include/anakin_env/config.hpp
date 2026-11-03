#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace anakin::env {

enum class RuntimeMode { Sim, Real };
enum class State { Disarmed, Damping, Align, Hold, AsapLoco, AsapCr7, Preparing, Fault };
enum class PolicyModel { AsapLoco, AsapCr7 };
enum class CommandSource { None, Damping, StandInterpolation, FixedStand, Policy };
enum class Action { RevokeActuation, ReleaseIfOwned, AcquireAuthority, BeginAlignment, RequestPolicy,
                    ZeroMotion };
enum class EventKind { EmergencyStop, Fault, ManualRequest, ClearFault, SourceLost,
                       AlignmentComplete, PolicyReady, MotionDone, DampingComplete, Immediate };
// Fault is accepted from legacy configuration files but no longer changes behavior.
enum class SourceLossPolicy { ZeroMotionKeepMode, Fault };

std::string to_string(RuntimeMode value);
std::string to_string(State value);
std::string to_string(PolicyModel value);
std::string to_string(Action value);
std::string to_string(EventKind value);
bool is_business_state(State value);
const std::array<State, 6>& business_states();

struct OutputPolicy {
  bool ros_state = false;
  bool lowcmd = false;
};

struct StateConfig {
  struct PreparingFallback { CommandSource from_hold, otherwise; };
  OutputPolicy outputs;
  CommandSource command = CommandSource::None;
  std::optional<PolicyModel> model;
  std::vector<Action> on_enter, on_exit;
  std::optional<PreparingFallback> preparing_fallback;
};

// A missing target means the currently requested business state (policy_ready only).
struct Transition {
  State from;
  std::optional<State> to;
  std::optional<State> via;
  EventKind event = EventKind::ManualRequest;
};

struct TransportConfig {
  std::string interface;
  int sdk_domain = 0;
  int ros_domain = 0;
  std::string odometry_topic = "rt/odommodestate";
  std::string base_frame = "unitree_base", odometry_frame = "unitree_odom";
};

struct GuardConfig {
  bool allow_fallen = false;
  bool fault_on_tick_rollback = true;
  bool require_primary_input = true;
  bool strict_deadlines = true, enforce_measured_joint_limits = true;
  double max_tilt_radians = 1., min_base_height = .3;
  std::chrono::milliseconds state_timeout{40};
  std::chrono::milliseconds input_timeout{250};
  SourceLossPolicy source_loss = SourceLossPolicy::ZeroMotionKeepMode;
};

struct ModeConfig {
  RuntimeMode mode = RuntimeMode::Real;
  // Simulation transport can deliberately exercise the real policy/guards.
  bool real_checks = false;
  State initial_state = State::Disarmed;
  TransportConfig transport;
  GuardConfig guards;
  double alignment_seconds = 2.;
  double damping_seconds = .1;
  std::chrono::milliseconds authority_timeout{3000};
  std::chrono::milliseconds policy_timeout{5000};
  std::map<State, StateConfig> states;
  std::vector<Transition> manual_transitions, automatic_transitions, system_transitions;
};

enum class InputSource { Web, Gamepad, Remote };
enum class Trigger { Pressed, Held, Released };
enum class Button { A, B, X, Y, Up, Down, Left, Right, Start, Select };
enum class Axis { LeftX, LeftY, RightX, RightY };
enum class InputAction { Disarmed, Damping, Align, Hold, AsapLoco, AsapCr7, ClearFault,
                         EmergencyStop, MotionToggle, HeightUp, HeightDown };
enum class RemoteChannel { Lowstate, WirelessTopic };

std::string to_string(InputSource value);
std::string to_string(InputAction value);
InputAction input_action(const std::string& value);

// Adapters normalize LB/RB to L1/R1; normal bindings always match the exact mask.
constexpr uint8_t shoulder_l1 = 1;
constexpr uint8_t shoulder_r1 = 2;
struct ButtonBinding {
  Button button;
  uint8_t shoulders = 0;
  bool any_shoulders = false;  // Permitted only for emergency stops.
};
struct InputBinding {
  ButtonBinding control;
  Trigger trigger = Trigger::Pressed;
  InputAction action;
};
struct AxisBinding {
  Axis axis;
  double scale;
};
struct InputConfig {
  std::optional<InputSource> primary;  // Empty: automatic, remote > gamepad > web on contention.
  std::vector<InputSource> enabled_sources;
  int gamepad_device_index = 0;
  std::vector<std::string> gamepad_mappings;  // SDL mappings, each scoped to one exact device GUID.
  RemoteChannel remote_channel = RemoteChannel::Lowstate;
  double deadzone = .04;
  std::array<AxisBinding, 3> velocity_axes;  // forward, lateral, yaw
  double initial_height = .78, height_step = .05;
  std::vector<InputBinding> controller_bindings;
  std::vector<InputAction> web_actions;
};

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

ModeConfig load_mode_config(const std::filesystem::path& path, RuntimeMode expected_mode);
InputConfig load_input_config(const std::filesystem::path& path);
// Snapshot of the effective, already validated config, including CLI overrides.
std::string input_config_yaml(const InputConfig& config);

struct ResourceOptions {
  // Empty means /proc/self/exe. Test/launcher callers may pass the selected installed ELF.
  std::filesystem::path executable;
  std::optional<std::filesystem::path> install_prefix;
  std::optional<std::filesystem::path> config, input_config, real_config;
  std::optional<InputSource> primary_input;
  bool automatic_input = false, real_checks = false;
};
struct ConfigPaths {
  std::filesystem::path install_prefix, mode, inputs;
  std::optional<std::filesystem::path> real_checks;
};
struct Configuration {
  ConfigPaths paths;
  ModeConfig mode;
  InputConfig inputs;
};

// Uses the selected executable's prefix and its local ament package marker, never cwd
// or a different AMENT_PREFIX_PATH overlay. Explicit config paths may live elsewhere.
ConfigPaths resolve_config_paths(RuntimeMode mode, const ResourceOptions& options = {});
Configuration load_configuration(RuntimeMode mode, const ResourceOptions& options = {});

}  // namespace anakin::env
