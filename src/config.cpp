#include <anakin_env/config.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <tuple>

namespace anakin::env {
namespace {
template <class T> using Names = std::vector<std::pair<std::string, T>>;
const Names<RuntimeMode> modes{{"sim", RuntimeMode::Sim}, {"real", RuntimeMode::Real}};
const Names<State> states{{"disarmed", State::Disarmed}, {"damping", State::Damping},
    {"align", State::Align}, {"hold", State::Hold}, {"asap_loco", State::AsapLoco},
    {"asap_cr7", State::AsapCr7}, {"preparing", State::Preparing}, {"fault", State::Fault}};
const Names<PolicyModel> models{{"asap_loco", PolicyModel::AsapLoco},
                               {"asap_cr7", PolicyModel::AsapCr7}};
const Names<Action> actions{{"revoke_actuation", Action::RevokeActuation},
    {"release_if_owned", Action::ReleaseIfOwned}, {"acquire_authority", Action::AcquireAuthority},
    {"begin_alignment", Action::BeginAlignment}, {"request_policy", Action::RequestPolicy},
    {"zero_motion", Action::ZeroMotion}};
const Names<CommandSource> commands{{"none", CommandSource::None},
    {"damping", CommandSource::Damping}, {"stand_interpolation", CommandSource::StandInterpolation},
    {"fixed_stand", CommandSource::FixedStand}, {"policy", CommandSource::Policy}};
const Names<EventKind> events{{"emergency_stop", EventKind::EmergencyStop}, {"fault", EventKind::Fault},
    {"clear_fault", EventKind::ClearFault}, {"alignment_complete", EventKind::AlignmentComplete},
    {"policy_ready", EventKind::PolicyReady}, {"motion_done", EventKind::MotionDone},
    {"damping_complete", EventKind::DampingComplete}, {"immediate", EventKind::Immediate}};
const Names<InputSource> sources{{"keyboard", InputSource::Keyboard}, {"gamepad", InputSource::Gamepad},
                                  {"remote", InputSource::Remote}};
const Names<Trigger> triggers{{"pressed", Trigger::Pressed}, {"held", Trigger::Held},
                              {"released", Trigger::Released}};
const Names<Button> buttons{{"A", Button::A}, {"B", Button::B}, {"X", Button::X}, {"Y", Button::Y},
    {"Up", Button::Up}, {"Down", Button::Down}, {"Left", Button::Left}, {"Right", Button::Right},
    {"Start", Button::Start}, {"Select", Button::Select}};
const Names<Key> keys{{"1", Key::Digit1}, {"2", Key::Digit2}, {"3", Key::Digit3},
    {"4", Key::Digit4}, {"5", Key::Digit5}, {"6", Key::Digit6}, {"Space", Key::Space},
    {"C", Key::C}, {"W", Key::W}, {"A", Key::A}, {"S", Key::S}, {"D", Key::D}, {"Q", Key::Q},
    {"E", Key::E}, {"G", Key::G}, {"Up", Key::Up}, {"Down", Key::Down}, {"Left", Key::Left},
    {"Right", Key::Right}};
const Names<Axis> axes{{"left_x", Axis::LeftX}, {"left_y", Axis::LeftY},
                       {"right_x", Axis::RightX}, {"right_y", Axis::RightY}};
const Names<InputAction> input_actions{{"disarmed", InputAction::Disarmed},
    {"damping", InputAction::Damping}, {"align", InputAction::Align}, {"hold", InputAction::Hold},
    {"asap_loco", InputAction::AsapLoco}, {"asap_cr7", InputAction::AsapCr7},
    {"clear_fault", InputAction::ClearFault}, {"emergency_stop", InputAction::EmergencyStop},
    {"forward", InputAction::Forward}, {"backward", InputAction::Backward}, {"left", InputAction::Left},
    {"right", InputAction::Right}, {"turn_left", InputAction::TurnLeft},
    {"turn_right", InputAction::TurnRight}, {"motion_toggle", InputAction::MotionToggle},
    {"height_up", InputAction::HeightUp}, {"height_down", InputAction::HeightDown}};

template <class T> std::string name(T value, const Names<T>& names) {
  for (const auto& item : names)
    if (item.second == value) return item.first;
  throw std::invalid_argument("Unknown typed configuration value");
}

class Parser {
 public:
  explicit Parser(std::filesystem::path path) : path_(std::move(path)) {}
  [[noreturn]] void fail(const std::string& field, const std::string& message) const {
    throw ConfigError(path_.string() + ": " + field + ": " + message);
  }
  YAML::Node load() const {
    try {
      return YAML::LoadFile(path_.string());
    } catch (const YAML::Exception& error) {
      fail("$", error.what());
    }
  }
  void object(const YAML::Node& node, const std::string& field,
              std::initializer_list<const char*> allowed) const {
    if (!node.IsMap()) fail(field, "expected a mapping");
    std::set<std::string> seen;
    for (const auto& item : node) {
      const auto key = scalar(item.first, field);
      if (!seen.insert(key).second) fail(field + "." + key, "duplicate field");
      if (std::none_of(allowed.begin(), allowed.end(), [&](const char* value) { return key == value; }))
        fail(field + "." + key, "unknown field");
    }
  }
  YAML::Node required(const YAML::Node& node, const std::string& key,
                      const std::string& field) const {
    if (!node[key] || node[key].IsNull()) fail(field + "." + key, "missing required field");
    return node[key];
  }
  void sequence(const YAML::Node& node, const std::string& field, bool nonempty = false) const {
    if (!node.IsSequence() || (nonempty && node.size() == 0))
      fail(field, nonempty ? "expected a nonempty sequence" : "expected a sequence");
  }
  std::string scalar(const YAML::Node& node, const std::string& field) const {
    if (!node.IsScalar()) fail(field, "expected a scalar");
    return node.Scalar();
  }
  template <class T> T value(const YAML::Node& node, const std::string& field) const {
    scalar(node, field);
    try {
      return node.as<T>();
    } catch (const YAML::Exception& error) {
      fail(field, error.what());
    }
  }
  bool boolean(const YAML::Node& node, const std::string& field) const {
    const auto text = scalar(node, field);
    if (text != "true" && text != "false") fail(field, "expected true or false");
    return text == "true";
  }
  double number(const YAML::Node& node, const std::string& field, double min, double max) const {
    const auto result = value<double>(node, field);
    if (!std::isfinite(result) || result < min || result > max)
      fail(field, "expected a finite number in " + std::to_string(min) + ".." + std::to_string(max));
    return result;
  }
  int integer(const YAML::Node& node, const std::string& field, int min, int max) const {
    const auto result = value<int>(node, field);
    if (result < min || result > max) fail(field, "integer outside allowed range");
    return result;
  }
  template <class T> T choice(const YAML::Node& node, const std::string& field,
                              const Names<T>& names) const {
    const auto text = scalar(node, field);
    for (const auto& item : names)
      if (item.first == text) return item.second;
    fail(field, "unknown value '" + text + "'");
  }
 private:
  std::filesystem::path path_;
};

std::string element(const std::string& field, size_t index) {
  return field + "[" + std::to_string(index) + "]";
}

std::vector<State> state_list(const Parser& p, const YAML::Node& node, const std::string& field,
                              bool wildcard) {
  std::vector<State> result;
  if (wildcard && node.IsScalar() && node.Scalar() == "*") {
    for (const auto& state : states)
      if (state.second != State::Fault) result.push_back(state.second);
    return result;
  }
  if (node.IsSequence()) {
    p.sequence(node, field, true);
    for (size_t i = 0; i < node.size(); ++i)
      result.push_back(p.choice(node[i], element(field, i), states));
  } else {
    result.push_back(p.choice(node, field, states));
  }
  std::set<State> unique(result.begin(), result.end());
  if (unique.size() != result.size()) p.fail(field, "duplicate state reference");
  return result;
}

std::vector<Action> action_list(const Parser& p, const YAML::Node& node, const std::string& field) {
  p.sequence(node, field);
  std::vector<Action> result;
  for (size_t i = 0; i < node.size(); ++i) {
    const auto action = p.choice(node[i], element(field, i), actions);
    if (std::find(result.begin(), result.end(), action) != result.end())
      p.fail(element(field, i), "duplicate action");
    result.push_back(action);
  }
  return result;
}

void validate_state(const Parser& p, State id, const StateConfig& state, RuntimeMode mode) {
  const auto field = "states." + to_string(id);
  const auto position = [&](Action action) {
    return std::find(state.on_enter.begin(), state.on_enter.end(), action);
  };
  if (state.outputs.lowcmd && state.command == CommandSource::None)
    p.fail(field + ".command", "lowcmd enabled without a command source");
  if (!state.outputs.lowcmd && (position(Action::RevokeActuation) == state.on_enter.end() ||
      position(Action::ReleaseIfOwned) == state.on_enter.end() ||
      position(Action::RevokeActuation) > position(Action::ReleaseIfOwned)))
    p.fail(field + ".on_enter", "lowcmd-off requires revoke_actuation before release_if_owned");
  if ((id == State::Disarmed || id == State::Fault) &&
      (state.outputs.lowcmd || state.command != CommandSource::None))
    p.fail(field + ".outputs.lowcmd", "disarmed/fault must not authorize commands");
  if (id == State::AsapLoco || id == State::AsapCr7) {
    const auto expected = id == State::AsapLoco ? PolicyModel::AsapLoco : PolicyModel::AsapCr7;
    if (state.command != CommandSource::Policy || state.model != expected)
      p.fail(field + ".model", "policy state requires its registered model and policy command");
  } else if (state.model) {
    p.fail(field + ".model", "model is only valid on a registered policy state");
  }
  if (state.command == CommandSource::Policy && id != State::Preparing && !state.model)
    p.fail(field + ".command", "policy command requires a policy state or preparing");
  if (id == State::Preparing && state.command != CommandSource::Policy)
    p.fail(field + ".command", "preparing requires the requested policy command");
  if ((id == State::Preparing) != state.preparing_fallback.has_value())
    p.fail(field + ".preparing_fallback", "only preparing requires a local fallback rule");
  for (const auto& list : {state.on_enter, state.on_exit}) {
    for (const auto action : list) {
      if (action == Action::AcquireAuthority && id != State::Align)
        p.fail(field, "acquire_authority is only valid at explicit align entry");
      if (action == Action::BeginAlignment && id != State::Align)
        p.fail(field, "begin_alignment is only valid at align entry");
      if (action == Action::RequestPolicy && id != State::Preparing)
        p.fail(field, "request_policy is only valid at preparing entry");
    }
  }
  for (const auto action : state.on_exit)
    if (action == Action::AcquireAuthority || action == Action::BeginAlignment ||
        action == Action::RequestPolicy)
      p.fail(field + ".on_exit", "entry-only action used on exit");
  if (id == State::Align && position(Action::BeginAlignment) == state.on_enter.end())
    p.fail(field + ".on_enter", "align requires begin_alignment");
  if (id == State::Align && mode == RuntimeMode::Real &&
      (position(Action::AcquireAuthority) == state.on_enter.end() ||
       position(Action::AcquireAuthority) > position(Action::BeginAlignment)))
    p.fail(field + ".on_enter", "real align requires acquire_authority before begin_alignment");
  if (id == State::Preparing && position(Action::RequestPolicy) == state.on_enter.end())
    p.fail(field + ".on_enter", "preparing requires request_policy");
}

std::vector<Transition> transitions(const Parser& p, const YAML::Node& node,
                                     const std::string& field, bool manual, bool system) {
  p.sequence(node, field);
  std::vector<Transition> result;
  std::set<std::tuple<State, EventKind, std::optional<State>>> seen;
  for (size_t i = 0; i < node.size(); ++i) {
    const auto entry = node[i];
    const auto at = element(field, i);
    p.object(entry, at, manual ? std::initializer_list<const char*>{"from", "to", "via"}
                               : std::initializer_list<const char*>{"from", "to", "via", "event"});
    const auto from = state_list(p, p.required(entry, "from", at), at + ".from", true);
    const auto event = manual ? EventKind::ManualRequest
                             : p.choice(p.required(entry, "event", at), at + ".event", events);
    const bool system_event = event == EventKind::EmergencyStop || event == EventKind::Fault ||
                              event == EventKind::ClearFault;
    if (!manual && system_event != system) p.fail(at + ".event", "event is in the wrong rule list");
    const auto to_node = p.required(entry, "to", at);
    std::vector<std::optional<State>> targets;
    if (!manual && to_node.IsScalar() && to_node.Scalar() == "requested") {
      if (event != EventKind::PolicyReady) p.fail(at + ".to", "requested requires policy_ready");
      targets.push_back(std::nullopt);
    } else {
      for (const auto target : state_list(p, to_node, at + ".to", false)) targets.push_back(target);
    }
    if (!manual && targets.size() != 1) p.fail(at + ".to", "event rule must have one target");
    std::optional<State> via;
    if (entry["via"]) via = p.choice(entry["via"], at + ".via", states);
    for (const auto source : from) {
      for (const auto target : targets) {
        if (source == State::Fault && event != EventKind::ClearFault && event != EventKind::Fault)
          p.fail(at + ".from", "only clear_fault can leave fault");
        if (manual && (!target || !is_business_state(*target)))
          p.fail(at + ".to", "manual requests must select a business state");
        if (via && *via != State::Preparing && *via != State::Damping)
          p.fail(at + ".via", "only preparing/damping are intermediate stages");
        if (target && (*target == State::Preparing || (*target == State::Fault && !system)))
          p.fail(at + ".to", "internal stage cannot be selected as a business destination");
        if (event == EventKind::PolicyReady &&
            (source != State::Preparing || target || via))
          p.fail(at, "policy_ready requires preparing -> requested without via");
        if (target && (*target == State::AsapLoco || *target == State::AsapCr7) &&
            via != State::Preparing)
          p.fail(at + ".via", "policy entry must pass through preparing");
        if (via == State::Preparing &&
            (!target || (*target != State::AsapLoco && *target != State::AsapCr7)))
          p.fail(at + ".via", "preparing requires a registered policy destination");
        if (via == State::Damping && target != State::Disarmed)
          p.fail(at + ".via", "damping intermediate requires disarmed destination");
        // Automatic/system rules are unambiguous by (state,event); manual rules by target.
        const auto key = std::make_tuple(source, event, manual ? target : std::nullopt);
        if (!seen.insert(key).second) p.fail(at, "conflicting transition rule");
        result.push_back({source, target, via, event});
      }
    }
  }
  return result;
}

void validate_graph(const Parser& p, const ModeConfig& config) {
  for (const auto& entry : states) {
    const auto state = entry.second;
    for (const auto event : {EventKind::EmergencyStop, EventKind::Fault}) {
      if (state == State::Fault && event == EventKind::EmergencyStop) continue;
      const auto target = event == EventKind::Fault ? State::Fault : State::Damping;
      const auto found = std::find_if(config.system_transitions.begin(), config.system_transitions.end(),
          [&](const Transition& rule) {
            return rule.from == state && rule.event == event && rule.to == target && !rule.via;
          });
      if (found == config.system_transitions.end())
        p.fail("system_transitions", "missing " + to_string(event) + " route from " + to_string(state));
    }
  }
  for (const auto& rule : config.system_transitions) {
    if (rule.event == EventKind::ClearFault &&
        (rule.from != State::Fault || rule.to != State::Disarmed || rule.via))
      p.fail("system_transitions", "clear_fault must be fault -> disarmed");
  }
  if (std::none_of(config.system_transitions.begin(), config.system_transitions.end(),
                   [](const Transition& rule) { return rule.event == EventKind::ClearFault; }))
    p.fail("system_transitions", "missing explicit clear_fault route");
  // Completion events wait on a fresh generation/transaction and may form cycles.
  // Only immediate edges can cause an unbounded no-wait cycle.
  std::map<State, int> colors;
  std::function<void(State)> visit = [&](State state) {
    if (colors[state] == 1) p.fail("automatic_transitions", "no-wait automatic cycle");
    if (colors[state] == 2) return;
    colors[state] = 1;
    for (const auto& rule : config.automatic_transitions)
      if (rule.from == state && rule.event == EventKind::Immediate)
        visit(rule.via.value_or(*rule.to));
    colors[state] = 2;
  };
  for (const auto& state : states) visit(state.second);
}

bool overlap(Trigger left, Trigger right) {
  return left == right || left == Trigger::Held || right == Trigger::Held;
}
void validate_bindings(const Parser& p, const std::vector<InputBinding>& bindings,
                       const std::string& field) {
  bool has_stop = false;
  for (size_t i = 0; i < bindings.size(); ++i) {
    const auto& binding = bindings[i];
    has_stop = has_stop || binding.action == InputAction::EmergencyStop;
    for (size_t j = 0; j < i; ++j) {
      bool same = false;
      if (const auto* key = std::get_if<KeyboardBinding>(&binding.control)) {
        same = key->key == std::get<KeyboardBinding>(bindings[j].control).key;
      } else {
        const auto& button = std::get<ButtonBinding>(binding.control);
        const auto& other = std::get<ButtonBinding>(bindings[j].control);
        same = button.button == other.button &&
               (button.any_shoulders || other.any_shoulders || button.shoulders == other.shoulders);
      }
      if (same && overlap(binding.trigger, bindings[j].trigger))
        p.fail(element(field, i), "conflicting binding with " + element(field, j));
    }
  }
  if (!has_stop) p.fail(field, "requires an emergency_stop binding");
}

std::filesystem::path existing(const std::filesystem::path& path, const std::string& field,
                                bool directory = false) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || (directory ? !std::filesystem::is_directory(canonical)
                         : !std::filesystem::is_regular_file(canonical)))
    throw ConfigError(path.string() + ": " + field + ": missing or invalid " +
                      (directory ? "directory" : "file"));
  return canonical;
}
}  // namespace

std::string to_string(RuntimeMode value) { return name(value, modes); }
std::string to_string(State value) { return name(value, states); }
std::string to_string(PolicyModel value) { return name(value, models); }
std::string to_string(Action value) { return name(value, actions); }
std::string to_string(EventKind value) {
  if (value == EventKind::ManualRequest) return "manual_request";
  if (value == EventKind::SourceLost) return "source_lost";
  return name(value, events);
}
std::string to_string(InputSource value) { return name(value, sources); }
const std::array<State, 6>& business_states() {
  static const std::array<State, 6> result{State::Disarmed, State::Damping, State::Align,
                                        State::Hold, State::AsapLoco, State::AsapCr7};
  return result;
}
bool is_business_state(State value) {
  const auto& values = business_states();
  return std::find(values.begin(), values.end(), value) != values.end();
}

ModeConfig load_mode_config(const std::filesystem::path& path, RuntimeMode expected_mode) {
  const Parser p(path);
  const auto root = p.load();
  p.object(root, "$", {"schema_version", "mode", "initial_state", "transport", "guards", "timing",
                       "states", "manual_transitions", "automatic_transitions", "system_transitions"});
  if (p.integer(p.required(root, "schema_version", "$"), "schema_version", 1, 1) != 1)
    p.fail("schema_version", "unsupported schema");
  ModeConfig result;
  result.mode = p.choice(p.required(root, "mode", "$"), "mode", modes);
  if (result.mode != expected_mode) p.fail("mode", "does not match selected CLI mode");
  result.initial_state = p.choice(p.required(root, "initial_state", "$"), "initial_state", states);
  if (result.initial_state != State::Disarmed) p.fail("initial_state", "startup must be disarmed");

  const auto transport = p.required(root, "transport", "$");
  p.object(transport, "transport", {"interface", "sdk_domain", "ros_domain", "odometry_topic", "base_frame", "odometry_frame"});
  result.transport.interface = p.scalar(p.required(transport, "interface", "transport"),
                                         "transport.interface");
  if (result.transport.interface.empty() || result.transport.interface.size() > 15 ||
      result.transport.interface.find_first_of(" \t\r\n/") != std::string::npos)
    p.fail("transport.interface", "expected an explicit network interface name (1..15 characters)");
  result.transport.sdk_domain = p.integer(p.required(transport, "sdk_domain", "transport"),
                                             "transport.sdk_domain", 0, 232);
  result.transport.ros_domain = p.integer(p.required(transport, "ros_domain", "transport"),
                                             "transport.ros_domain", 0, 232);
  for (const auto* key : {"odometry_topic", "base_frame", "odometry_frame"}) {
    const auto field = std::string("transport.") + key;
    const auto value = p.scalar(p.required(transport, key, "transport"), field);
    if (value.empty() || value.size() > 128 || value.find_first_of(" \t\r\n") != std::string::npos)
      p.fail(field, "expected a nonempty topic/frame without whitespace (1..128 characters)");
    if (std::string(key) == "odometry_topic") result.transport.odometry_topic = value;
    else if (std::string(key) == "base_frame") result.transport.base_frame = value;
    else result.transport.odometry_frame = value;
  }
  const auto guards = p.required(root, "guards", "$");
  p.object(guards, "guards", {"allow_fallen", "fault_on_tick_rollback", "require_primary_input",
                               "state_timeout_ms", "input_timeout_ms", "source_loss", "strict_deadlines",
                               "enforce_measured_joint_limits", "max_tilt_radians", "min_base_height"});
  result.guards.allow_fallen = p.boolean(p.required(guards, "allow_fallen", "guards"), "guards.allow_fallen");
  result.guards.fault_on_tick_rollback = p.boolean(p.required(guards, "fault_on_tick_rollback", "guards"),
                                                    "guards.fault_on_tick_rollback");
  result.guards.require_primary_input = p.boolean(p.required(guards, "require_primary_input", "guards"),
                                                    "guards.require_primary_input");
  result.guards.strict_deadlines = p.boolean(p.required(guards, "strict_deadlines", "guards"), "guards.strict_deadlines");
  result.guards.enforce_measured_joint_limits = p.boolean(p.required(guards, "enforce_measured_joint_limits", "guards"), "guards.enforce_measured_joint_limits");
  result.guards.max_tilt_radians = p.number(p.required(guards, "max_tilt_radians", "guards"), "guards.max_tilt_radians", 0., 3.141592653589793);
  result.guards.min_base_height = p.number(p.required(guards, "min_base_height", "guards"), "guards.min_base_height", 0., 2.);
  result.guards.state_timeout = std::chrono::milliseconds(p.integer(
      p.required(guards, "state_timeout_ms", "guards"), "guards.state_timeout_ms", 1, 5000));
  result.guards.input_timeout = std::chrono::milliseconds(p.integer(
      p.required(guards, "input_timeout_ms", "guards"), "guards.input_timeout_ms", 1, 10000));
  result.guards.source_loss = p.choice<SourceLossPolicy>(p.required(guards, "source_loss", "guards"),
      "guards.source_loss", {{"zero_motion_keep_mode", SourceLossPolicy::ZeroMotionKeepMode},
                              {"fault", SourceLossPolicy::Fault}});
  const auto timing = p.required(root, "timing", "$");
  p.object(timing, "timing", {"alignment_seconds", "damping_seconds", "authority_timeout_ms", "policy_timeout_ms"});
  result.alignment_seconds = p.number(p.required(timing, "alignment_seconds", "timing"),
                                         "timing.alignment_seconds", .25, 5.);
  result.damping_seconds = p.number(p.required(timing, "damping_seconds", "timing"),
                                       "timing.damping_seconds", .002, 5.);
  result.authority_timeout = std::chrono::milliseconds(p.integer(
      p.required(timing, "authority_timeout_ms", "timing"), "timing.authority_timeout_ms", 1, 30000));
  result.policy_timeout = std::chrono::milliseconds(p.integer(
      p.required(timing, "policy_timeout_ms", "timing"), "timing.policy_timeout_ms", 1000, 10000));

  const auto definitions = p.required(root, "states", "$");
  p.object(definitions, "states", {"disarmed", "damping", "align", "hold", "asap_loco", "asap_cr7",
                                      "preparing", "fault"});
  for (const auto& known : states) {
    const auto field = "states." + known.first;
    const auto definition = p.required(definitions, known.first, "states");
    p.object(definition, field, {"outputs", "command", "model", "on_enter", "on_exit", "preparing_fallback"});
    StateConfig state;
    const auto outputs = p.required(definition, "outputs", field);
    p.object(outputs, field + ".outputs", {"ros_state", "lowcmd"});
    state.outputs.ros_state = p.boolean(p.required(outputs, "ros_state", field + ".outputs"),
                                            field + ".outputs.ros_state");
    state.outputs.lowcmd = p.boolean(p.required(outputs, "lowcmd", field + ".outputs"),
                                         field + ".outputs.lowcmd");
    state.command = p.choice(p.required(definition, "command", field), field + ".command", commands);
    if (definition["model"]) state.model = p.choice(definition["model"], field + ".model", models);
    state.on_enter = action_list(p, p.required(definition, "on_enter", field), field + ".on_enter");
    state.on_exit = action_list(p, p.required(definition, "on_exit", field), field + ".on_exit");
    if (definition["preparing_fallback"]) {
      const auto fallback = definition["preparing_fallback"];
      const auto at = field + ".preparing_fallback";
      p.object(fallback, at, {"from_hold", "otherwise"});
      const Names<CommandSource> local{{"none", CommandSource::None}, {"fixed_stand", CommandSource::FixedStand}, {"damping", CommandSource::Damping}};
      state.preparing_fallback = StateConfig::PreparingFallback{
          p.choice(p.required(fallback, "from_hold", at), at + ".from_hold", local),
          p.choice(p.required(fallback, "otherwise", at), at + ".otherwise", local)};
    }
    validate_state(p, known.second, state, result.mode);
    result.states.emplace(known.second, std::move(state));
  }
  result.manual_transitions = transitions(p, p.required(root, "manual_transitions", "$"),
                                            "manual_transitions", true, false);
  result.automatic_transitions = transitions(p, p.required(root, "automatic_transitions", "$"),
                                               "automatic_transitions", false, false);
  result.system_transitions = transitions(p, p.required(root, "system_transitions", "$"),
                                            "system_transitions", false, true);
  validate_graph(p, result);
  return result;
}

InputConfig load_input_config(const std::filesystem::path& path) {
  const Parser p(path);
  const auto root = p.load();
  p.object(root, "$", {"schema_version", "primary", "enabled_sources", "keyboard", "gamepad", "remote",
                       "motion", "keyboard_bindings", "controller_bindings"});
  p.integer(p.required(root, "schema_version", "$"), "schema_version", 1, 1);
  InputConfig result;
  result.primary = p.choice(p.required(root, "primary", "$"), "primary", sources);
  const auto enabled = p.required(root, "enabled_sources", "$");
  p.sequence(enabled, "enabled_sources", true);
  for (size_t i = 0; i < enabled.size(); ++i) {
    const auto source = p.choice(enabled[i], element("enabled_sources", i), sources);
    if (std::find(result.enabled_sources.begin(), result.enabled_sources.end(), source) != result.enabled_sources.end())
      p.fail(element("enabled_sources", i), "duplicate input source");
    result.enabled_sources.push_back(source);
  }
  if (std::find(result.enabled_sources.begin(), result.enabled_sources.end(), result.primary) == result.enabled_sources.end())
    p.fail("primary", "primary input must be enabled");
  const auto keyboard = p.required(root, "keyboard", "$");
  p.object(keyboard, "keyboard", {"command_timeout_ms"});
  result.keyboard_command_timeout = std::chrono::milliseconds(p.integer(
      p.required(keyboard, "command_timeout_ms", "keyboard"), "keyboard.command_timeout_ms", 1, 10000));
  const auto gamepad = p.required(root, "gamepad", "$");
  p.object(gamepad, "gamepad", {"device_index"});
  result.gamepad_device_index = p.integer(p.required(gamepad, "device_index", "gamepad"),
                                            "gamepad.device_index", 0, 255);
  const auto remote = p.required(root, "remote", "$");
  p.object(remote, "remote", {"channel"});
  result.remote_channel = p.choice<RemoteChannel>(p.required(remote, "channel", "remote"), "remote.channel",
      {{"lowstate", RemoteChannel::Lowstate}, {"wireless_topic", RemoteChannel::WirelessTopic}});
  const auto motion = p.required(root, "motion", "$");
  p.object(motion, "motion", {"deadzone", "velocity_axes", "initial_height", "height_step"});
  result.deadzone = p.number(p.required(motion, "deadzone", "motion"), "motion.deadzone", 0., .99);
  result.initial_height = p.number(p.required(motion, "initial_height", "motion"), "motion.initial_height", .01, 2.);
  result.height_step = p.number(p.required(motion, "height_step", "motion"), "motion.height_step", .001, 1.);
  const auto velocity = p.required(motion, "velocity_axes", "motion");
  p.object(velocity, "motion.velocity_axes", {"forward", "lateral", "yaw"});
  size_t index = 0;
  std::set<Axis> mapped_axes;
  for (const auto* component : {"forward", "lateral", "yaw"}) {
    const auto field = std::string("motion.velocity_axes.") + component;
    const auto binding = p.required(velocity, component, "motion.velocity_axes");
    p.object(binding, field, {"axis", "scale"});
    auto& output = result.velocity_axes[index++];
    output.axis = p.choice(p.required(binding, "axis", field), field + ".axis", axes);
    output.scale = p.number(p.required(binding, "scale", field), field + ".scale", -10., 10.);
    if (output.scale == 0.) p.fail(field + ".scale", "velocity scale must be nonzero");
    if (!mapped_axes.insert(output.axis).second) p.fail(field + ".axis", "axis already mapped");
  }
  for (const bool controller : {false, true}) {
    const std::string field = controller ? "controller_bindings" : "keyboard_bindings";
    const auto bindings = p.required(root, field, "$");
    p.sequence(bindings, field, true);
    auto& output = controller ? result.controller_bindings : result.keyboard_bindings;
    for (size_t i = 0; i < bindings.size(); ++i) {
      const auto binding = bindings[i];
      const auto at = element(field, i);
      p.object(binding, at, controller ? std::initializer_list<const char*>{"button", "shoulders", "trigger", "action"}
                                      : std::initializer_list<const char*>{"key", "trigger", "action"});
      InputBinding value;
      value.trigger = p.choice(p.required(binding, "trigger", at), at + ".trigger", triggers);
      value.action = p.choice(p.required(binding, "action", at), at + ".action", input_actions);
      if (controller) {
        ButtonBinding button;
        button.button = p.choice(p.required(binding, "button", at), at + ".button", buttons);
        const auto shoulders = p.required(binding, "shoulders", at);
        if (shoulders.IsScalar() && shoulders.Scalar() == "any") {
          button.any_shoulders = true;
          if (value.action != InputAction::EmergencyStop)
            p.fail(at + ".shoulders", "ordinary bindings require an exact shoulder set");
        } else {
          p.sequence(shoulders, at + ".shoulders");
          for (size_t j = 0; j < shoulders.size(); ++j) {
            const auto shoulder = p.choice<uint8_t>(shoulders[j], element(at + ".shoulders", j),
                {{"L1", shoulder_l1}, {"R1", shoulder_r1}});
            if (button.shoulders & shoulder) p.fail(at + ".shoulders", "duplicate shoulder");
            button.shoulders |= shoulder;
          }
        }
        if (value.action == InputAction::EmergencyStop && value.trigger == Trigger::Released)
          p.fail(at + ".trigger", "emergency_stop cannot wait for release");
        value.control = button;
      } else {
        if (value.trigger != Trigger::Pressed) p.fail(at + ".trigger", "terminal keys support pressed events only");
        value.control = KeyboardBinding{p.choice(p.required(binding, "key", at), at + ".key", keys)};
      }
      output.push_back(value);
    }
    validate_bindings(p, output, field);
  }
  return result;
}

ConfigPaths resolve_config_paths(RuntimeMode mode, const ResourceOptions& options) {
  const auto executable = existing(options.executable.empty() ? "/proc/self/exe" : options.executable,
                                    "executable");
  const auto package_dir = executable.parent_path();
  const auto lib_dir = package_dir.parent_path();
  if (package_dir.filename() != "anakin_env" || lib_dir.filename() != "lib")
    throw ConfigError(executable.string() + ": executable: expected PREFIX/lib/anakin_env/ELF");
  const auto prefix = lib_dir.parent_path();
  if (options.install_prefix && existing(*options.install_prefix, "install_prefix", true) != prefix)
    throw ConfigError(options.install_prefix->string() + ": install_prefix: does not match selected executable " + executable.string());
  // Validate the package's ament index at the selected prefix directly. A global
  // ament search can select a different overlay and is intentionally not a fallback.
  existing(prefix / "share/ament_index/resource_index/packages/anakin_env", "ament package index");
  const auto directory = prefix / "share/anakin_env/config";
  return {prefix, existing(options.config.value_or(directory / (to_string(mode) + ".yaml")), "mode config"),
                  existing(options.input_config.value_or(directory / "inputs.yaml"), "input config")};
}

Configuration load_configuration(RuntimeMode mode, const ResourceOptions& options) {
  Configuration result;
  result.paths = resolve_config_paths(mode, options);
  result.mode = load_mode_config(result.paths.mode, mode);
  result.inputs = load_input_config(result.paths.inputs);
  if (options.primary_input) {
    if (std::find(result.inputs.enabled_sources.begin(), result.inputs.enabled_sources.end(),
                  *options.primary_input) == result.inputs.enabled_sources.end())
      throw ConfigError(result.paths.inputs.string() + ": primary: CLI input override is not enabled");
    result.inputs.primary = *options.primary_input;
  }
  return result;
}

}  // namespace anakin::env
