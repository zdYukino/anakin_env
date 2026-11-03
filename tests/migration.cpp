// Pure shared production helpers: never initializes ROS or native SDK channels.
#include "sim_contract.hpp"
#include "sim_plan.hpp"
#include <anakin_env/input.hpp>
#include <anakin_env/offline_fixture.hpp>
#include <anakin_env/startup.hpp>
#include <anakin_sdk/srv/switch_inference_mode.hpp>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace anakin;
using namespace anakin::env;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Temporary {
  fs::path path;
  Temporary() {
    std::string pattern = (fs::temp_directory_path() / "anakin-migration-XXXXXX").string();
    auto* made = mkdtemp(pattern.data());
    if (!made) throw std::runtime_error("mkdtemp failed");
    path = made;
  }
  ~Temporary() { std::error_code error; fs::remove_all(path, error); }
};
void fixture_contract() {
  Frame frame;
  frame.schema_version = schema_version; frame.schema_id = schema_id;
  frame.header.frame_id = offline_fixture_frame_id;
  frame.execution_mode = Frame::MODE_PREPARING;
  use_offline_execution_contract(frame);
  frame.validity = Frame::VALID_JOINTS | Frame::VALID_IMU | Frame::VALID_COMMAND;
  diagnostic_msgs::msg::DiagnosticStatus diagnostic;
  diagnostic.name = "anakin_env/backend"; diagnostic.message = offline_fixture_diagnostic;
  frame.diagnostics.status.push_back(diagnostic);
  check(is_offline_fixture(frame) && !offline_inference_permitted(frame), "Labelled idle fixture remains discoverable");
  frame.execution_authorized = true;
  check(offline_inference_permitted(frame), "Preparing permission is accepted by explicit protocol fixture");
  frame.sdk_publish_count = 1;
  check(!is_offline_fixture(frame), "No-native label cannot claim writer receipts");
  frame.sdk_publish_count = 0; frame.sdk_published_phase_mask = 4;
  check(!is_offline_fixture(frame), "No-native label cannot claim publication phase");
  frame.sdk_published_phase_mask = 0; frame.sdk_published_epoch = 1;
  check(!is_offline_fixture(frame), "No-native label cannot claim a writer epoch");
  frame.sdk_published_epoch = 0; frame.sdk_published_request_id = new_uuid();
  check(!is_offline_fixture(frame), "No-native label cannot claim a writer identity");
  frame.sdk_published_request_id = Uuid{}; frame.header.frame_id = "unitree_sim_base";
  check(!is_offline_fixture(frame), "Protocol fixture never follows native Env");
  frame.header.frame_id = offline_fixture_frame_id;
  anakin_sdk::srv::SwitchInferenceMode::Request request;
  use_offline_execution_contract(request);
  check(request.execution_profile == frame.execution_profile && request.command_semantics == frame.command_semantics &&
        request.gain_profile == frame.gain_profile && request.execution_profile == sdk_pd_execution_profile,
        "Explicit ASAP fixture request and frame share the common PD contract");
  frame.session_id = new_uuid(); frame.mc_instance_seen = new_uuid(); frame.request_id_seen = new_uuid();
  frame.state_seq = frame.epoch = 1;
  frame.joints.name = joint_names(); frame.joints.position.assign(29, 0);
  Gate gate;
  gate.begin(frame.session_id, frame.mc_instance_seen, frame.request_id_seen, frame.epoch, "asap_loco", true,
             request.gain_profile, request.execution_profile, request.command_semantics);
  const auto acquired = Clock::time_point{} + 10s;
  gate.state(frame, acquired);
  Target target;
  target.schema_version = schema_version; target.schema_id = schema_id;
  use_offline_execution_contract(target);
  target.session_id = frame.session_id; target.mc_instance_id = frame.mc_instance_seen;
  target.request_id = frame.request_id_seen; target.epoch = frame.epoch;
  target.seq = target.source_state_seq = 1; target.phase = Target::TARGET_ACTIVE;
  target.model_id = "asap_loco"; target.validity = 1; target.valid_for.nanosec = 40000000;
  target.target.joint_names = joint_names(); target.target.points.resize(1);
  target.target.points[0].positions.assign(29, 0);
  target.target.points[0].positions[0] = lower_limits()[0] - .7;
  std::string reason;
  check(gate.accept(target, acquired + 2ms, reason),
        "Explicit ASAP fixture accepts finite original PD reference beyond mechanical/discontinuity limits");
  frame.execution_profile = strict_execution_profile; frame.command_semantics = mechanical_position;
  check(!is_offline_fixture(frame), "Historical mechanical fixtures are not the migrated ASAP caller");
  use_offline_execution_contract(frame); frame.diagnostics.status.clear();
  check(!is_offline_fixture(frame), "Explicit no-native diagnostic required");
}
void probe_contract(const fs::path& source) {
  check(!sim_test::uses_projection(ExecutionEncoding::SdkPdReference) &&
        !sim_test::uses_projection(ExecutionEncoding::StrictPosition) &&
        sim_test::uses_projection(ExecutionEncoding::ProjectReference) &&
        sim_test::uses_projection(ExecutionEncoding::EquivalentReference), "Only historical reference profiles project");
  Target target;
  target.gain_profile = merged_gain_profile; target.execution_profile = sdk_pd_execution_profile;
  target.command_semantics = pd_reference; target.model_id = "asap_loco";
  target.target.joint_names = joint_names(); target.target.points.resize(1);
  auto& positions = target.target.points[0].positions;
  positions.assign(29, 0); positions[0] = reference_lower_limits()[0] - .7;
  Positions q{}; std::copy(positions.begin(), positions.end(), q.begin());
  auto command = encode_policy_pd(q, 5);
  finalize_command(command);
  check(sim_test::matches_target(command, target), "Raw SDK PD outside mechanical bounds matches unprojected reference");
  target.execution_profile = project_execution_profile;
  check(!sim_test::matches_target(command, target), "Historical projection does not match an unprojected command");
  auto release = encode_release(5); finalize_command(release);
  check(sim_test::classify(release) == sim_test::CommandKind::Release, "Disabled zero packet is release, not position");
  auto damping = encode_damping(5); finalize_command(damping);
  check(sim_test::classify(damping) == sim_test::CommandKind::Damping, "Enabled damping remains distinct");
  release.motor_cmd()[0].tau() = 1; finalize_command(release);
  check(sim_test::classify(release) == sim_test::CommandKind::Invalid, "Mixed disabled torque packet is not release");
  command.motor_cmd()[0].dq() = 1; finalize_command(command);
  target.execution_profile = sdk_pd_execution_profile;
  check(!sim_test::matches_target(command, target), "Comparison includes dq");
  auto inputs = load_input_config(source / "inputs.yaml");
  inputs.primary = InputSource::Remote;
  inputs.enabled_sources = {InputSource::Remote};
  const std::array<std::pair<uint16_t, InputAction>, 4> pulses{{
      {sim_test::align_keys, InputAction::Align}, {sim_test::hold_keys, InputAction::Hold},
      {sim_test::loco_keys, InputAction::AsapLoco}, {sim_test::cr7_keys, InputAction::AsapCr7}}};
  for (const auto& pulse : pulses) {
    InputHub hub(inputs, 250ms);
    const auto now = InputTime{} + 10s;
    hub.submit(UnitreeRemoteAdapter::wireless(0, {}, now)); hub.poll(now);
    hub.submit(UnitreeRemoteAdapter::wireless(pulse.first, {}, now + 20ms));
    const auto result = hub.poll(now + 20ms);
    check(result.actions.size() == 1 && result.actions[0] == pulse.second, "Probe pulses follow the production exact-shoulder mapper");
  }
  Frame before, after;
  before.execution_mode = after.execution_mode = Frame::MODE_ACTIVE;
  before.epoch = after.epoch = 3; before.request_id_seen = after.request_id_seen = new_uuid();
  after.validity = Frame::VALID_COMMAND; after.sdk_publish_count = 1;
  check(sim_test::loss_kept_policy(before, after), "Sim loss requires zero command and same live policy transaction");
  after.command.twist.linear.x = .1;
  check(!sim_test::loss_kept_policy(before, after), "Nonzero motion after loss rejected");
  after.command.twist.linear.x = 0; after.epoch++;
  check(!sim_test::loss_kept_policy(before, after), "Loss must not fabricate a new policy epoch");
}
void plan_contract(const fs::path& source) {
  Temporary temporary;
  const auto prefix = temporary.path / "selected install";
  const auto run = temporary.path / "run";
  fs::create_directories(prefix / "share/anakin_env"); fs::create_directory(run);
  fs::copy(source, prefix / "share/anakin_env/config", fs::copy_options::recursive);
  const auto plan = sim_test::make_plan(prefix, run, "/package/en_run.sh", 32, 89, .5);
  const auto startup = parse_startup(plan.env_arguments);
  check(startup.mode == RuntimeMode::Sim && startup.resources.primary_input == InputSource::Remote &&
        startup.resources.config == plan.mode_config && startup.resources.input_config == plan.input_config,
        "Explicit runner uses current startup contract and test-owned configs");
  const auto mode = load_mode_config(plan.mode_config, RuntimeMode::Sim);
  const auto inputs = load_input_config(plan.input_config);
  check(mode.transport.sdk_domain == 32 && mode.transport.ros_domain == 89 && mode.alignment_seconds == .5,
        "Test timing/endpoints are in typed YAML, not removed native ROS parameters");
  check(mode.guards.source_loss == SourceLossPolicy::ZeroMotionKeepMode &&
        inputs.remote_channel == RemoteChannel::WirelessTopic, "Probe and native primary use same remote channel/loss policy");
  check(load_mode_config(source / "sim.yaml", RuntimeMode::Sim).transport.sdk_domain == 31 &&
        load_input_config(source / "inputs.yaml").remote_channel == RemoteChannel::Lowstate,
        "Harness does not rewrite installed/default resources");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Expected config directory");
    fixture_contract(); probe_contract(argv[1]); plan_contract(argv[1]);
    std::cout << "PASS explicit caller/fixture migration contracts (no channels)\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
