// Pure file/config/event tests. No ROS context, SDK, native transport or devices.
#include <anakin_env/state_machine.hpp>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <unistd.h>

using namespace anakin::env;
namespace fs = std::filesystem;
namespace {
void check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path);
  file << text;
  check(static_cast<bool>(file), "Cannot write fixture " + path.string());
}
struct Temporary {
  fs::path path;
  Temporary() {
    auto pattern = (fs::temp_directory_path() / "anakin-mode-config-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto* directory = mkdtemp(buffer.data());
    check(directory != nullptr, "Cannot create temporary fixture directory");
    path = directory;
  }
  ~Temporary() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
template <class Fn> void error(Fn&& fn, const std::string& field, const std::string& path = {}) {
  try {
    fn();
  } catch (const ConfigError& exception) {
    const std::string message = exception.what();
    check(message.find(field) != std::string::npos, "Error does not identify field: " + message);
    check(path.empty() || message.find(path) != std::string::npos, "Error does not identify file: " + message);
    return;
  }
  throw std::runtime_error("Invalid configuration accepted: " + field);
}
TransactionIdentity identity(uint8_t value) {
  TransactionIdentity result;
  result.session[0] = 1;
  result.instance[0] = 2;
  result.request[0] = value;
  result.epoch = value;
  return result;
}
bool has(const ActionPlan& plan, Action action) {
  for (const auto& item : plan.actions)
    if (item.action == action) return true;
  return false;
}
void finish_policy(StateMachine& machine, uint8_t id) {
  check(machine.snapshot().state == State::Preparing, "Policy request has a preparing stage");
  const auto generation = machine.snapshot().generation;
  const auto transaction = identity(id);
  check(!machine.bind_transaction(generation - 1, transaction), "Late transaction binding rejected");
  check(!machine.bind_transaction(generation, {}), "Empty transaction binding rejected");
  check(machine.bind_transaction(generation, transaction), "Current transaction binding accepted");
  check(!machine.bind_transaction(generation, identity(id + 1)), "Cannot silently replace bound transaction");
  const auto model = *machine.config().states.at(machine.snapshot().requested_state).model;
  const auto ready = Event::policy(EventKind::PolicyReady, generation, transaction, model);
  auto wrong_transaction = ready;
  ++wrong_transaction.transaction->instance[0];
  auto old_generation = ready;
  --old_generation.generation;
  auto wrong_model = ready;
  wrong_model.producing_model = model == PolicyModel::AsapLoco ? PolicyModel::AsapCr7
                                                             : PolicyModel::AsapLoco;
  for (const auto& stale : {Event::completion(EventKind::PolicyReady, generation),
                            wrong_transaction, old_generation, wrong_model})
    check(machine.dispatch({stale}).decision == Decision::IgnoredStale &&
          machine.snapshot().state == State::Preparing && machine.snapshot().generation == generation,
          "Policy readiness requires the current generation, transaction and producing model");
  check(machine.dispatch({ready}).decision == Decision::Accepted, "Current policy completion accepted");
  check(machine.snapshot().effective_state == machine.snapshot().requested_state &&
        machine.snapshot().producing_model == model &&
        machine.snapshot().inference_phase == InferencePhase::Active,
        "Accepted request and completed producing policy are separate observations");
}
void reach(StateMachine& machine, State state) {
  if (state == State::Disarmed) return;
  if (state == State::Damping) {
    check(machine.dispatch({Event::manual(state)}).decision == Decision::Accepted, "Reach damping");
    return;
  }
  if (machine.config().mode == RuntimeMode::Sim) {
    check(machine.dispatch({Event::manual(state)}).decision == Decision::Accepted, "Reach sim mode");
    if (machine.snapshot().state == State::Preparing) finish_policy(machine, 10);
    return;
  }
  machine.dispatch({Event::manual(State::Align)});
  if (state == State::Align) return;
  machine.dispatch({Event::completion(EventKind::AlignmentComplete, machine.snapshot().generation)});
  if (state == State::Hold) return;
  machine.dispatch({Event::manual(State::AsapLoco)});
  finish_policy(machine, 10);
  if (state == State::AsapLoco) return;
  machine.dispatch({Event::manual(State::AsapCr7)});
  finish_policy(machine, 11);
}
bool real_edge(State from, State to) {
  if (from == to || to == State::Damping) return true;
  if (to == State::Disarmed) return from != State::Disarmed;
  return (from == State::Disarmed && to == State::Align) ||
         (from == State::Hold && to == State::AsapLoco) ||
         (from == State::AsapLoco && to == State::AsapCr7) ||
         (from == State::AsapCr7 && to == State::AsapLoco);
}

void state_contracts(const fs::path& configs) {
  for (const auto mode : {RuntimeMode::Sim, RuntimeMode::Real}) {
    const auto config = load_mode_config(configs / (to_string(mode) + ".yaml"), mode);
    // Full ordinary requested-mode matrix, including idempotence and intermediate stages.
    for (const auto source : business_states()) {
      for (const auto target : business_states()) {
        StateMachine machine(config);
        reach(machine, source);
        const auto before = machine.snapshot();
        const auto result = machine.dispatch({Event::manual(target)});
        const auto description = to_string(mode) + " " + to_string(source) + " -> " + to_string(target);
        if (source == target) {
          check(result.decision == Decision::Idempotent && result.actions.empty() &&
                machine.snapshot().generation == before.generation &&
                machine.snapshot().transaction == before.transaction, description + " is idempotent");
        } else if (mode == RuntimeMode::Real && !real_edge(source, target)) {
          check(result.decision == Decision::Rejected && result.actions.empty() &&
                machine.snapshot().state == source && machine.snapshot().generation == before.generation,
                description + " rejects a skipped real stage");
        } else {
          check(result.decision == Decision::Accepted && machine.snapshot().requested_state == target,
                description + " accepts the configured edge");
          if (target == State::AsapLoco || target == State::AsapCr7) {
            check(result.after.state == State::Preparing && has(result, Action::RequestPolicy) &&
                  result.after.effective_state == source && !result.after.transaction &&
                  result.inference_allowed && result.after.inference_phase == InferencePhase::Preparing,
                  description + " does not mistake acceptance for policy completion");
          }
          if (mode == RuntimeMode::Real && target == State::Disarmed && source != State::Damping)
            check(result.after.state == State::Damping && result.outputs.lowcmd,
                  description + " stops through damping before release");
        }
      }
    }

    StateMachine startup(config);
    const auto initial = startup.startup_plan();
    check(initial.after.state == State::Disarmed && initial.outputs.ros_state && !initial.outputs.lowcmd &&
          !has(initial, Action::AcquireAuthority) && initial.actions.size() == 2 &&
          initial.actions[0].action == Action::RevokeActuation &&
          initial.actions[1].action == Action::ReleaseIfOwned, "Startup is readonly with conditional release");
    const auto alignment = startup.dispatch({Event::manual(State::Align)});
    check(alignment.command == CommandSource::StandInterpolation &&
          has(alignment, Action::AcquireAuthority) == (mode == RuntimeMode::Real), "Only real explicit align takes authority");
    auto old_alignment = Event::completion(EventKind::AlignmentComplete, alignment.after.generation - 1);
    check(startup.dispatch({old_alignment}).decision == Decision::IgnoredStale, "Old alignment completion ignored");
    const auto hold = startup.dispatch({Event::completion(EventKind::AlignmentComplete, alignment.after.generation)});
    check(hold.after.state == State::Hold && hold.command == CommandSource::FixedStand, "Both modes auto align -> fixed stand hold");

    StateMachine idle(config);
    const auto idle_stop = idle.dispatch({Event::manual(State::Damping)});
    check(idle_stop.after.state == State::Damping && !has(idle_stop, Action::AcquireAuthority),
          "Manual damping from readonly startup never takes authority");

    StateMachine machine(config);
    reach(machine, State::AsapCr7);
    const auto before = machine.snapshot();
    auto done = Event::policy(EventKind::MotionDone, before.generation, *before.transaction, PolicyModel::AsapCr7);
    for (int field = 0; field < 6; ++field) {
      auto stale = done;
      switch (field) {
        case 0: ++stale.transaction->session[0]; break;
        case 1: ++stale.transaction->instance[0]; break;
        case 2: ++stale.transaction->request[0]; break;
        case 3: ++stale.transaction->epoch; break;
        case 4: --stale.generation; break;
        case 5: stale.producing_model = PolicyModel::AsapLoco; break;
      }
      check(machine.dispatch({stale}).decision == Decision::IgnoredStale &&
            machine.snapshot().generation == before.generation, "Every stale transaction/model/generation component is checked");
    }
    check(machine.dispatch({done, Event::manual(State::AsapCr7)}).decision == Decision::Idempotent &&
          machine.snapshot().state == State::AsapCr7, "A legal same-mode manual request suppresses automatic completion");
    const auto back = machine.dispatch({done});
    check(back.after.state == State::Preparing && back.after.requested_state == State::AsapLoco &&
          back.after.effective_state == State::AsapCr7 && has(back, Action::RequestPolicy), "Both modes auto CR7 -> loco transaction");
    const auto pending_generation = machine.snapshot().generation;
    const auto repeat = machine.dispatch({Event::manual(State::AsapLoco)});
    check(repeat.decision == Decision::Idempotent && repeat.after.generation == pending_generation && repeat.actions.empty(),
          "Repeating pending policy request does not initialize again");
    finish_policy(machine, 20);
    machine.dispatch({Event::manual(State::AsapCr7)});
    finish_policy(machine, 21);
    done.generation = machine.snapshot().generation;
    check(machine.dispatch({done}).decision == Decision::IgnoredStale, "Old motion_done cannot finish a new CR7 transaction");
    done = Event::policy(EventKind::MotionDone, machine.snapshot().generation,
                         *machine.snapshot().transaction, PolicyModel::AsapCr7);
    const auto manual = machine.dispatch({done, Event::manual(State::Disarmed)});
    check(manual.cause == EventKind::ManualRequest && manual.after.requested_state == State::Disarmed,
          "Manual stop wins over ordinary completion regardless of arrival order");
    if (mode == RuntimeMode::Real) {
      const auto released = machine.dispatch({Event::completion(EventKind::DampingComplete, machine.snapshot().generation)});
      check(released.after.state == State::Disarmed && !released.outputs.lowcmd &&
            released.actions[0].action == Action::RevokeActuation &&
            released.actions[1].action == Action::ReleaseIfOwned, "Release actions precede lowcmd-off");
    }

    StateMachine safety(config);
    reach(safety, State::Hold);
    const auto stop = safety.dispatch({Event::manual(State::AsapLoco), Event::system(EventKind::Fault),
                                       Event::system(EventKind::EmergencyStop)});
    check(stop.cause == EventKind::EmergencyStop && stop.after.state == State::Damping,
          "All-source emergency wins over fault/manual/automatic");
    const auto fault = safety.dispatch({Event::manual(State::Align), Event::system(EventKind::Fault)});
    check(fault.cause == EventKind::Fault && fault.after.state == State::Fault && !fault.outputs.lowcmd,
          "Fault precedes manual selection");
    for (const auto state : business_states())
      check(safety.dispatch({Event::manual(state)}).decision == Decision::Rejected, "Wildcard/manual cannot escape fault");
    check(safety.dispatch({Event::system(EventKind::ClearFault), Event::system(EventKind::EmergencyStop)}).after.state == State::Fault,
          "Held stop cannot clear a fault");
    check(safety.dispatch({Event::system(EventKind::ClearFault)}).after.state == State::Disarmed,
          "Only explicit clear_fault returns to disarmed");

    StateMachine loss(config);
    reach(loss, State::AsapLoco);
    const auto generation = loss.snapshot().generation;
    const auto lost = loss.dispatch({Event::system(EventKind::SourceLost)});
    if (mode == RuntimeMode::Sim)
      check(lost.after.state == State::AsapLoco && lost.after.generation == generation &&
            has(lost, Action::ZeroMotion), "Sim source loss zeros motion and retains policy/transaction");
    else
      check(lost.after.state == State::Fault && !lost.outputs.lowcmd, "Real source loss follows fault/release path");

    for (const bool loss_first : {false, true}) {
      StateMachine simultaneous(config);
      reach(simultaneous, State::Hold);
      const auto fault_event = Event::system(EventKind::Fault);
      const auto loss_event = Event::system(EventKind::SourceLost);
      const auto result = simultaneous.dispatch(loss_first ? std::vector<Event>{loss_event, fault_event}
                                                          : std::vector<Event>{fault_event, loss_event});
      check(result.cause == EventKind::Fault && result.after.state == State::Fault,
            "Source-loss handling cannot suppress a simultaneous hardware/contract fault");
    }
  }
}

void config_contracts(const fs::path& configs) {
  Temporary temp;
  const auto sim_path = configs / "sim.yaml";
  const auto input_path = configs / "inputs.yaml";
  const auto mode_fixture = temp.path / "mode.yaml";
  const auto input_fixture = temp.path / "inputs.yaml";
  const auto invalid_mode = [&](const std::function<void(YAML::Node&)>& change, const std::string& field,
                                RuntimeMode mode = RuntimeMode::Sim) {
    auto node = YAML::LoadFile((configs / (to_string(mode) + ".yaml")).string());
    change(node);
    write(mode_fixture, YAML::Dump(node));
    error([&] { load_mode_config(mode_fixture, mode); }, field, mode_fixture.string());
  };
  const auto invalid_input = [&](const std::function<void(YAML::Node&)>& change, const std::string& field) {
    auto node = YAML::LoadFile(input_path.string());
    change(node);
    write(input_fixture, YAML::Dump(node));
    error([&] { load_input_config(input_fixture); }, field, input_fixture.string());
  };
  invalid_mode([](auto& n) { n["schema_version"] = 2; }, "schema_version");
  invalid_mode([](auto& n) { n["mode"] = "real"; }, "mode");
  invalid_mode([](auto& n) { n["initial_state"] = "align"; }, "initial_state");
  invalid_mode([](auto& n) { n["states"].remove("hold"); }, "states.hold");
  invalid_mode([](auto& n) { n["states"]["unknown"] = YAML::Node(YAML::NodeType::Map); }, "states.unknown");
  invalid_mode([](auto& n) { n["states"]["hold"]["outputs"]["lowcmd"] = "maybe"; }, "outputs.lowcmd");
  invalid_mode([](auto& n) { n["states"]["hold"]["outputs"]["typo"] = true; }, "outputs.typo");
  invalid_mode([](auto& n) { n["states"]["hold"]["outputs"]["lowcmd"] = false; }, "on_enter");
  invalid_mode([](auto& n) { n["states"]["disarmed"]["on_enter"] = YAML::Load("[release_if_owned, revoke_actuation]"); }, "on_enter");
  invalid_mode([](auto& n) { n["states"]["hold"]["on_enter"] = YAML::Load("[reset_simulator]"); }, "on_enter");
  invalid_mode([](auto& n) { n["states"]["asap_loco"]["model"] = "asap_cr7"; }, "model");
  invalid_mode([](auto& n) { n["transport"]["interface"] = ""; }, "transport.interface");
  invalid_mode([](auto& n) { n["transport"]["sdk_domain"] = -1; }, "transport.sdk_domain");
  invalid_mode([](auto& n) { n["transport"]["odometry_topic"] = ""; }, "transport.odometry_topic");
  invalid_mode([](auto& n) { n["transport"]["base_frame"] = "bad frame"; }, "transport.base_frame");
  invalid_mode([](auto& n) { n["transport"].remove("odometry_frame"); }, "transport.odometry_frame");
  invalid_mode([](auto& n) { n["timing"]["alignment_seconds"] = ".nan"; }, "timing.alignment_seconds");
  invalid_mode([](auto& n) { n["guards"]["strict_deadlines"] = "sometimes"; }, "guards.strict_deadlines");
  invalid_mode([](auto& n) { n["guards"]["max_tilt_radians"] = ".nan"; }, "guards.max_tilt_radians");
  invalid_mode([](auto& n) { n["timing"]["policy_timeout_ms"] = 0; }, "timing.policy_timeout_ms");
  invalid_mode([](auto& n) { n["states"]["preparing"]["preparing_fallback"]["from_hold"] = "policy"; }, "preparing_fallback.from_hold");
  invalid_mode([](auto& n) { n["states"]["preparing"].remove("preparing_fallback"); }, "preparing_fallback");
  invalid_mode([](auto& n) { n["manual_transitions"][0]["to"] = "unknown"; }, ".to");
  invalid_mode([](auto& n) { n["manual_transitions"][0]["from"] = "fault"; }, ".from");
  invalid_mode([](auto& n) { n["manual_transitions"][0]["to"] = "fault"; }, ".to");
  invalid_mode([](auto& n) { n["manual_transitions"].push_back(YAML::Clone(n["manual_transitions"][0])); }, "conflicting transition");
  invalid_mode([](auto& n) { n["automatic_transitions"][0]["event"] = "unknown"; }, ".event");
  invalid_mode([](auto& n) { n["automatic_transitions"][0]["event"] = "clear_fault"; }, ".event");
  invalid_mode([](auto& n) { n["automatic_transitions"][0]["to"] = "requested"; }, ".to");
  invalid_mode([](auto& n) { n["system_transitions"] = YAML::Load("[]"); }, "system_transitions");
  invalid_mode([](auto& n) { n["states"]["align"]["on_enter"] = YAML::Load("[begin_alignment, acquire_authority]"); }, "on_enter", RuntimeMode::Real);
  invalid_mode([](auto& n) {
    n["automatic_transitions"].push_back(YAML::Load("{from: align, event: immediate, to: hold}"));
    n["automatic_transitions"].push_back(YAML::Load("{from: hold, event: immediate, to: align}"));
  }, "no-wait automatic cycle");

  write(mode_fixture, "schema_version: 1\nschema_version: 1\n");
  error([&] { load_mode_config(mode_fixture, RuntimeMode::Sim); }, "duplicate field");
  write(mode_fixture, "[");
  error([&] { load_mode_config(mode_fixture, RuntimeMode::Sim); }, "$", mode_fixture.string());
  error([&] { load_mode_config(temp.path / "missing.yaml", RuntimeMode::Sim); }, "missing.yaml");

  invalid_input([](auto& n) { n["keyboard_bindings"][0]["key"] = "Z"; }, ".key");
  invalid_input([](auto& n) { n["controller_bindings"][0]["button"] = "unknown"; }, ".button");
  invalid_input([](auto& n) { n["controller_bindings"][0]["action"] = "reset"; }, ".action");
  invalid_input([](auto& n) { n["controller_bindings"][0]["shoulders"] = "any"; }, ".shoulders");
  invalid_input([](auto& n) { n["controller_bindings"][0]["shoulders"] = YAML::Load("[R1, R1]"); }, ".shoulders");
  invalid_input([](auto& n) { n["controller_bindings"][1]["trigger"] = "released"; }, ".trigger");
  invalid_input([](auto& n) { n["keyboard_bindings"][0]["trigger"] = "held"; }, ".trigger");
  invalid_input([](auto& n) { n["keyboard_bindings"].push_back(YAML::Clone(n["keyboard_bindings"][0])); }, "conflicting binding");
  invalid_input([](auto& n) { n["controller_bindings"][5]["shoulders"] = YAML::Load("[L1, R1]"); }, "conflicting binding");
  invalid_input([](auto& n) { auto b = YAML::Clone(n["controller_bindings"][5]); b["trigger"] = "held"; n["controller_bindings"].push_back(b); }, "conflicting binding");
  invalid_input([](auto& n) { n["enabled_sources"] = YAML::Load("[gamepad, remote]"); }, "primary");
  invalid_input([](auto& n) { n["enabled_sources"].push_back("remote"); }, "duplicate input source");
  invalid_input([](auto& n) { n["motion"]["deadzone"] = 1.0; }, "motion.deadzone");
  invalid_input([](auto& n) { n["motion"]["velocity_axes"]["yaw"]["axis"] = "left_y"; }, ".axis");

  // A completion-gated cycle is legal; only a no-wait cycle is rejected.
  auto node = YAML::LoadFile(sim_path.string());
  node["automatic_transitions"].push_back(YAML::Load("{from: hold, event: alignment_complete, to: align}"));
  write(mode_fixture, YAML::Dump(node));
  load_mode_config(mode_fixture, RuntimeMode::Sim);

  // Behavior changes with YAML alone: allowed edge, output gating and automatic rule.
  node = YAML::LoadFile(sim_path.string());
  node["manual_transitions"] = YAML::Load("[{from: disarmed, to: hold}]");
  node["states"]["hold"]["outputs"]["ros_state"] = false;
  node["states"]["hold"]["outputs"]["lowcmd"] = false;
  node["states"]["hold"]["on_enter"] = YAML::Load("[revoke_actuation, release_if_owned]");
  node["automatic_transitions"].push_back(YAML::Load("{from: hold, event: immediate, to: align}"));
  write(mode_fixture, YAML::Dump(node));
  StateMachine changed(load_mode_config(mode_fixture, RuntimeMode::Sim));
  check(changed.dispatch({Event::manual(State::Align)}).decision == Decision::Rejected, "YAML changes allowed manual edges");
  const auto gated = changed.dispatch({Event::manual(State::Hold)});
  check(!gated.outputs.ros_state && !gated.outputs.lowcmd && !gated.inference_allowed &&
        has(gated, Action::ReleaseIfOwned), "YAML changes both output switches with release");
  check(changed.dispatch({}).after.state == State::Align, "YAML changes automatic event behavior");

  const auto inputs = load_input_config(input_path);
  const std::map<Key, InputAction> keyboard_expected{
      {Key::Digit1, InputAction::Disarmed}, {Key::Digit2, InputAction::EmergencyStop},
      {Key::Space, InputAction::EmergencyStop}, {Key::Digit3, InputAction::Align},
      {Key::Digit4, InputAction::Hold}, {Key::Digit5, InputAction::AsapLoco},
      {Key::Digit6, InputAction::AsapCr7}, {Key::C, InputAction::ClearFault},
      {Key::W, InputAction::Forward}, {Key::S, InputAction::Backward}, {Key::A, InputAction::Left},
      {Key::D, InputAction::Right}, {Key::Q, InputAction::TurnLeft}, {Key::E, InputAction::TurnRight},
      {Key::G, InputAction::MotionToggle}, {Key::Up, InputAction::HeightUp}, {Key::Down, InputAction::HeightDown}};
  check(inputs.keyboard_bindings.size() == keyboard_expected.size(), "Confirmed keyboard mapping has exact entries");
  for (const auto& binding : inputs.keyboard_bindings)
    check(keyboard_expected.at(std::get<KeyboardBinding>(binding.control).key) == binding.action,
          "Confirmed default keyboard mapping preserved");
  // Exact shoulder sets remain distinct typed values for the Stage B mapper.
  const std::map<std::pair<Button, uint8_t>, InputAction> controller_expected{
      {{Button::Down, shoulder_l1 | shoulder_r1}, InputAction::Disarmed},
      {{Button::X, shoulder_l1 | shoulder_r1}, InputAction::Align},
      {{Button::Up, shoulder_l1 | shoulder_r1}, InputAction::Hold},
      {{Button::Down, shoulder_r1}, InputAction::AsapLoco},
      {{Button::Up, shoulder_r1}, InputAction::AsapCr7},
      {{Button::Y, shoulder_l1 | shoulder_r1}, InputAction::ClearFault},
      {{Button::Left, 0}, InputAction::MotionToggle}, {{Button::Up, 0}, InputAction::HeightUp},
      {{Button::Down, 0}, InputAction::HeightDown}};
  check(inputs.controller_bindings.size() == controller_expected.size() + 2, "Confirmed controller map has exact entries");
  for (const auto& binding : inputs.controller_bindings) {
    const auto button = std::get<ButtonBinding>(binding.control);
    if (button.button == Button::A || button.button == Button::B)
      check(button.any_shoulders && binding.action == InputAction::EmergencyStop &&
            binding.trigger == Trigger::Held, "A/B stop applies with all shoulder combinations");
    else
      check(!button.any_shoulders && controller_expected.at({button.button, button.shoulders}) == binding.action,
            "Confirmed exact controller combinations preserved");
  }
}

fs::path prefix_fixture(const fs::path& parent, const fs::path& configs) {
  const auto executable = parent / "lib/anakin_env/test_elf";
  write(executable, "fixture executable path\n");
  write(parent / "share/ament_index/resource_index/packages/anakin_env", "");
  for (const auto* file : {"sim.yaml", "real.yaml", "inputs.yaml"}) {
    const auto destination = parent / "share/anakin_env/config" / file;
    fs::create_directories(destination.parent_path());
    fs::copy_file(configs / file, destination);
  }
  return executable;
}
void resource_contracts(const fs::path& configs) {
  Temporary temp;
  const auto first = temp.path / "first";
  const auto second = temp.path / "second";
  const auto first_executable = prefix_fixture(first, configs);
  const auto second_executable = prefix_fixture(second, configs);
  fs::create_directories(temp.path / "cwd/config");
  fs::copy_file(configs / "sim.yaml", temp.path / "cwd/config/sim.yaml");
  struct Cwd {
    fs::path previous = fs::current_path();
    ~Cwd() { fs::current_path(previous); }
  } cwd;
  fs::current_path(temp.path / "cwd");
  struct AmentEnv {
    std::optional<std::string> previous;
    AmentEnv() { if (const auto* value = std::getenv("AMENT_PREFIX_PATH")) previous = value; }
    ~AmentEnv() {
      if (previous) setenv("AMENT_PREFIX_PATH", previous->c_str(), 1);
      else unsetenv("AMENT_PREFIX_PATH");
    }
  } ament_env;
  setenv("AMENT_PREFIX_PATH", first.c_str(), 1);
  ResourceOptions options;
  options.executable = second_executable;
  auto fixture = YAML::LoadFile((second / "share/anakin_env/config/sim.yaml").string());
  fixture["states"]["hold"]["outputs"]["ros_state"] = false;
  write(second / "share/anakin_env/config/sim.yaml", YAML::Dump(fixture));
  const auto selected = load_configuration(RuntimeMode::Sim, options);
  check(selected.paths.install_prefix == second && !selected.mode.states.at(State::Hold).outputs.ros_state,
        "Defaults follow selected ELF prefix from temporary cwd despite another ament overlay");
  const auto real = load_configuration(RuntimeMode::Real, options);
  check(real.paths.inputs == selected.paths.inputs && real.mode.mode == RuntimeMode::Real,
        "Both modes use the same installed input YAML");
  options.install_prefix = first;
  error([&] { load_configuration(RuntimeMode::Sim, options); }, "install_prefix");
  options.install_prefix = second;
  const auto link = first / "lib/anakin_env/linked_elf";
  fs::create_symlink(second_executable, link);
  options.executable = link;
  check(resolve_config_paths(RuntimeMode::Sim, options).install_prefix == second,
        "Symlink executable resolves to actual binary prefix");
  options.executable = first_executable;
  options.install_prefix.reset();
  options.config = second / "share/anakin_env/config/sim.yaml";
  options.input_config = second / "share/anakin_env/config/inputs.yaml";
  options.primary_input = InputSource::Gamepad;
  auto overridden = load_configuration(RuntimeMode::Sim, options);
  check(!overridden.mode.states.at(State::Hold).outputs.ros_state &&
        overridden.inputs.primary == InputSource::Gamepad && overridden.paths.install_prefix == first,
        "Explicit config/input/primary overrides are applied at startup");
  fixture["states"]["hold"]["outputs"]["ros_state"] = true;
  write(*options.config, YAML::Dump(fixture));
  check(!overridden.mode.states.at(State::Hold).outputs.ros_state &&
        load_configuration(RuntimeMode::Sim, options).mode.states.at(State::Hold).outputs.ros_state,
        "File edits do not hot reload an existing typed configuration");
  options.config = first / "share/anakin_env/config/real.yaml";
  error([&] { load_configuration(RuntimeMode::Sim, options); }, "mode");
  options.config.reset();
  fixture = YAML::LoadFile(options.input_config->string());
  fixture["enabled_sources"] = YAML::Load("[keyboard]");
  write(*options.input_config, YAML::Dump(fixture));
  error([&] { load_configuration(RuntimeMode::Sim, options); }, "primary");
  options.primary_input.reset();
  options.input_config.reset();
  fs::remove(first / "share/anakin_env/config/sim.yaml");
  error([&] { load_configuration(RuntimeMode::Sim, options); }, "mode config", first.string());
  fs::remove(first / "share/ament_index/resource_index/packages/anakin_env");
  error([&] { resolve_config_paths(RuntimeMode::Real, options); }, "ament package index");
  options.executable = temp.path / "elsewhere/elf";
  write(options.executable, "fixture");
  error([&] { resolve_config_paths(RuntimeMode::Real, options); }, "PREFIX/lib/anakin_env/ELF");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--installed") {
      const auto sim = load_configuration(RuntimeMode::Sim);
      const auto real = load_configuration(RuntimeMode::Real);
      check(sim.paths.inputs == real.paths.inputs, "Installed modes share inputs");
      std::cout << "Installed configuration verified: " << sim.paths.install_prefix << '\n';
      return 0;
    }
    check(argc == 3, "Usage: anakin_env_config_test config|state|resources CONFIG_DIR | --installed");
    const auto configs = fs::canonical(argv[2]);
    const std::string suite = argv[1];
    if (suite == "config") config_contracts(configs);
    else if (suite == "state") state_contracts(configs);
    else if (suite == "resources") resource_contracts(configs);
    else throw std::runtime_error("Unknown suite");
    std::cout << suite << " contracts passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
