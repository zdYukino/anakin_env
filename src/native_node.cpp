#include <anakin_env/runtime.hpp>
#include <anakin_env/authority.hpp>
#include <anakin_env/startup.hpp>
#include <anakin_env/transport.hpp>
#include <anakin_env/input_telemetry.hpp>
#include <anakin_env/web_input.hpp>
#include <std_msgs/msg/string.hpp>
#include <rclcpp/rclcpp.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <sstream>
#include <iomanip>
using namespace std::chrono_literals;

namespace anakin::env {
using Switch = anakin_sdk::srv::SwitchInferenceMode;
class LoopbackBackend final : public RuntimeBackend {
 public:
  explicit LoopbackBackend(NativeTransport& transport) : transport_(transport) {}
  bool authority_owned() const override { return true; } // Simulation has no hardware authority RPC.
  void begin_acquire(uint64_t, std::chrono::milliseconds) override {}
  AuthorityProgress poll_acquire(uint64_t) override { return AuthorityProgress::Owned; }
  void cancel_acquire(uint64_t) override {}
  void release_authority() override {}
  bool write(const LowCommand& command) override { return transport_.write(command); }
 private:
  NativeTransport& transport_;
};

class NativeNode final : public rclcpp::Node {
 public:
  explicit NativeNode(Configuration configuration)
      : rclcpp::Node("anakin_env"), configuration_(std::move(configuration)),
        inputs_(configuration_.inputs, configuration_.mode.guards.input_timeout) {
    if (get_parameter("use_sim_time").as_bool())
      throw std::runtime_error("Native acquisition uses a monotonic clock; use_sim_time must be false");
    if (configuration_.mode.real_checks && (configuration_.mode.mode != RuntimeMode::Sim ||
        !configuration_.paths.real_checks))
      throw std::runtime_error("Real-checks profile is simulation-only");
    if (configuration_.mode.real_checks)
      backend_ = std::make_unique<RealBackend>(make_simulated_motion_switcher(),
          [this](const LowCommand& command) { return transport_.write(command); });
    else if (configuration_.mode.mode == RuntimeMode::Sim)
      backend_ = std::make_unique<LoopbackBackend>(transport_);
    else
      backend_ = std::make_unique<RealBackend>(make_sdk_motion_switcher(),
          [this](const LowCommand& command) { return transport_.write(command); });
    controller_ = std::make_unique<RuntimeController>(configuration_.mode, *backend_,
        [this] { return inputs_.snapshot(); });
    std::ostringstream session;
    for (const auto byte : controller_->session().uuid)
      session << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    event_session_ = session.str();
    web_ = std::make_unique<WebInput>(configuration_.inputs, configuration_.mode.guards.input_timeout, event_session_);
    web_subscription_ = create_subscription<std_msgs::msg::String>("anakin/web_input", rclcpp::QoS(32).reliable().durability_volatile(),
        [this](std_msgs::msg::String::ConstSharedPtr packet) {
          std::lock_guard<std::mutex> lock(mutex_);
          try {
            const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            inputs_.submit(web_->receive(packet->data, InputClock::now(), unix_ms));
            web_error_.clear();
          } catch (const std::exception& error) { web_error_ = error.what(); }
        });
    // Export the loaded configuration, never reread a potentially edited file.
    rcl_interfaces::msg::ParameterDescriptor descriptor;
    descriptor.read_only = true;
    declare_parameter("input_config_yaml", input_config_yaml(configuration_.inputs), descriptor, true);
    declare_parameter("input_config_path", configuration_.paths.inputs.string(), descriptor, true);
    declare_parameter("mode_config_path", configuration_.paths.mode.string(), descriptor, true);
    declare_parameter("real_checks", configuration_.mode.real_checks, descriptor, true);
    declare_parameter("real_checks_config_path", configuration_.paths.real_checks
        ? configuration_.paths.real_checks->string() : std::string{}, descriptor, true);
    declare_parameter("input_timeout_ms", configuration_.mode.guards.input_timeout.count(), descriptor, true);
    input_publisher_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
        "anakin/input_state", rclcpp::QoS(1).best_effort().durability_volatile());
    // Keep telemetry allocation/serialization outside the 500 Hz control worker.
    input_timer_ = create_wall_timer(34ms, [this] {
      try {
        const auto now = InputClock::now();
        const auto input = inputs_.snapshot(now);
        std::vector<SwitchEvent> events;
        diagnostic_msgs::msg::DiagnosticStatus control;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          events = event_log_.events();
          control = control_diagnostics(input);
        }
        auto report = input_diagnostics(input, configuration_.inputs, now, events, event_session_);
        report.status.push_back(std::move(control));
        report.header.stamp = get_clock()->now();
        input_publisher_->publish(report);
      } catch (const std::exception& error) {
        RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "Input telemetry unavailable: %s", error.what());
      }
    });
    publisher_ = create_publisher<Frame>("anakin/state", rclcpp::QoS(1).best_effort().durability_volatile());
    targets_ = create_subscription<Target>("anakin/control_target", rclcpp::QoS(1).best_effort().durability_volatile(),
        [this](Target::ConstSharedPtr target) {
          std::lock_guard<std::mutex> lock(mutex_);
          controller_->target(*target);
        });
    statuses_ = create_subscription<RuntimeStatus>("anakin/inference_status", rclcpp::QoS(128).reliable().durability_volatile(),
        [this](RuntimeStatus::ConstSharedPtr status) {
          std::lock_guard<std::mutex> lock(mutex_);
          controller_->status(*status);
        });
    client_ = create_client<Switch>("anakin/switch_inference_mode");
    transport_.set_remote_sink([this](const RemoteSample& remote) {
      inputs_.submit(UnitreeRemoteAdapter::wireless(remote.keys, remote.axes, remote.acquired, remote.valid));
    });
    transport_.start(configuration_.mode, configuration_.inputs.remote_channel == RemoteChannel::WirelessTopic);
    RCLCPP_INFO(get_logger(), "Env %s: persistent, disarmed, feedback-only startup; config=%s input=%s",
        to_string(configuration_.mode.mode).c_str(),
        configuration_.paths.mode.c_str(), configuration_.paths.inputs.c_str());
    if (configuration_.mode.real_checks)
      RCLCPP_INFO(get_logger(), "Simulation uses real state/guard config=%s; authority service is simulated",
                  configuration_.paths.real_checks->c_str());
    if (configuration_.mode.guards.source_loss == SourceLossPolicy::Fault)
      RCLCPP_WARN(get_logger(), "Legacy source_loss: fault is ignored; input loss zeros motion and retains the current state");
    worker_ = std::thread([this] { run(); });
  }
  ~NativeNode() override {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
  }
 private:
  Configuration configuration_;
  InputHub inputs_;
  std::unique_ptr<WebInput> web_;
  std::string web_error_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr web_subscription_;
  NativeTransport transport_;
  std::unique_ptr<RuntimeBackend> backend_;
  std::unique_ptr<RuntimeController> controller_;
  std::mutex mutex_;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
  uint64_t bad_packets_ = 0;
  std::optional<Uuid> in_flight_;
  Clock::time_point sent_{};
  int64_t pending_id_ = 0;
  std::string last_diagnostic_;
  std::string last_authority_diagnostic_;
  SwitchEventLog event_log_;
  std::string event_session_;
  rclcpp::Publisher<Frame>::SharedPtr publisher_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr input_publisher_;
  rclcpp::TimerBase::SharedPtr input_timer_;
  rclcpp::Subscription<Target>::SharedPtr targets_;
  rclcpp::Subscription<RuntimeStatus>::SharedPtr statuses_;
  rclcpp::Client<Switch>::SharedPtr client_;
  bool enabled(InputSource source) const {
    const auto& sources = configuration_.inputs.enabled_sources;
    return std::find(sources.begin(), sources.end(), source) != sources.end();
  }
  diagnostic_msgs::msg::DiagnosticStatus control_diagnostics(const InputSnapshot& input) const {
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = "anakin_env/web_control";
    status.hardware_id = event_session_;
    status.level = status.OK;
    status.message = web_->owner().empty() ? "INACTIVE" : "ACTIVE";
    const auto add = [&](const std::string& key, const std::string& value) {
      diagnostic_msgs::msg::KeyValue item;
      item.key = key;
      item.value = value;
      status.values.push_back(std::move(item));
    };
    add("schema_version", "1");
    add("timeout_ms", std::to_string(configuration_.mode.guards.input_timeout.count()));
    add("environment", event_session_);
    add("session", web_->owner());
    add("sequence", std::to_string(web_->sequence()));
    add("sdk_publish_count", std::to_string(controller_->writer_count()));
    add("enabled", enabled(InputSource::Web) && (!configuration_.inputs.primary || configuration_.inputs.primary == InputSource::Web) ? "true" : "false");
    add("active_source", input.active_source ? to_string(*input.active_source) : "none");
    add("mode", to_string(configuration_.mode.mode));
    add("real_checks", configuration_.mode.real_checks ? "true" : "false");
    add("state", to_string(controller_->snapshot().state));
    add("requested_state", to_string(controller_->snapshot().requested_state));
    add("moving", input.moving ? "true" : "false");
    add("height", std::to_string(input.height));
    add("error", web_error_);
    for (const auto action : configuration_.inputs.web_actions)
      add("action." + to_string(action), controller_->action_available(action, input) ? "true" : "false");
    return status;
  }
  void send_request(Clock::time_point now) {
    const auto& request = controller_->request();
    if (in_flight_ && (!request || *in_flight_ != request->request_id || now - sent_ >= 500ms)) {
      client_->remove_pending_request(pending_id_);
      in_flight_.reset();
    }
    if (!request || controller_->request_accepted() || in_flight_ || !client_->service_is_ready()) return;
    const auto copy = *request;
    in_flight_ = copy.request_id;
    sent_ = now;
    auto future = client_->async_send_request(std::make_shared<Switch::Request>(copy),
        [this, copy](rclcpp::Client<Switch>::SharedFuture future) {
          std::lock_guard<std::mutex> lock(mutex_);
          if (!in_flight_ || *in_flight_ != copy.request_id) return;
          in_flight_.reset();
          try {
            const auto response = future.get();
            controller_->request_result(copy.request_id, response->accepted &&
                response->mc_instance_id == copy.expected_mc_instance_id && response->epoch == copy.next_epoch &&
                response->execution_profile == copy.execution_profile && response->command_semantics == copy.command_semantics &&
                response->gain_profile == copy.gain_profile);
          } catch (const std::exception& error) { controller_->feedback_fault(error.what()); }
        });
    pending_id_ = future.request_id;
  }
  void run() {
    // SDL is created/polled/destroyed on this owning thread.
    std::unique_ptr<SdlGamepad> gamepad;
    try {
      if (enabled(InputSource::Gamepad)) gamepad = std::make_unique<SdlGamepad>(
          configuration_.inputs.gamepad_device_index, configuration_.inputs.gamepad_mappings);
    } catch (const std::exception& error) {
      RCLCPP_WARN(get_logger(), "Input unavailable: %s", error.what());
    }
    PeriodicSendSchedule schedule(Clock::now());
    while (!stopping_ && rclcpp::ok()) {
      std::this_thread::sleep_until(schedule.next());
      const auto now = Clock::now();
      if (!schedule.consume(now).due) continue;
      try {
        if (gamepad) inputs_.submit(gamepad->poll(now));
        InputFrame input;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (auto expired = web_->expire(InputClock::now())) inputs_.submit(*expired);
          input = inputs_.poll();
        }
        auto sample = transport_.sample();
        const auto odometry = transport_.odometry();
        const auto remote = transport_.remote();
        if (sample) {
          sample->frame.header.frame_id = configuration_.mode.transport.base_frame;
          sample->frame.joints.header = sample->frame.header;
          sample->frame.imu.header = sample->frame.header;
        }
        if (sample && odometry.valid && now >= odometry.acquired && now - odometry.acquired <= 100ms) {
          sample->frame.odometry.header.stamp = odometry.stamp;
          sample->frame.odometry.header.frame_id = configuration_.mode.transport.odometry_frame;
          sample->frame.odometry.child_frame_id = sample->frame.header.frame_id;
          sample->frame.odometry.pose.pose.position.x = odometry.position[0];
          sample->frame.odometry.pose.pose.position.y = odometry.position[1];
          sample->frame.odometry.pose.pose.position.z = odometry.position[2];
          sample->frame.odometry.pose.pose.orientation = sample->frame.imu.orientation;
          const auto velocity = world_to_body(sample->frame.imu.orientation, odometry.velocity);
          sample->frame.odometry.twist.twist.linear.x = velocity[0];
          sample->frame.odometry.twist.twist.linear.y = velocity[1];
          sample->frame.odometry.twist.twist.linear.z = velocity[2];
          sample->frame.odometry.twist.twist.angular = sample->frame.imu.angular_velocity;
          sample->frame.validity |= Frame::VALID_ODOMETRY;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (sample) controller_->feedback(*sample);
        const auto bad_packets = transport_.bad_packets();
        if (bad_packets != bad_packets_) {
          bad_packets_ = bad_packets;
          controller_->malformed_transport_packet();
          if (configuration_.mode.mode == RuntimeMode::Sim && !configuration_.mode.real_checks)
            RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                "Discarded malformed simulation feedback, remote, or odometry data (total: %llu)",
                static_cast<unsigned long long>(bad_packets));
        }
        if (targets_->get_publisher_count() > 1) controller_->feedback_fault("Multiple MC target publishers");
        const auto plan = controller_->tick(input);
        event_log_.observe(input, configuration_.inputs, configuration_.mode, plan, controller_->diagnostic());
        if (controller_->diagnostic() != last_diagnostic_) {
          last_diagnostic_ = controller_->diagnostic();
          if (!last_diagnostic_.empty()) RCLCPP_WARN(get_logger(), "%s", last_diagnostic_.c_str());
        }
        const auto authority_detail = backend_->authority_diagnostic();
        if (authority_detail != last_authority_diagnostic_) {
          last_authority_diagnostic_ = authority_detail;
          if (!authority_detail.empty()) RCLCPP_INFO(get_logger(), "Authority %s", authority_detail.c_str());
        }
        if (plan.decision == Decision::Accepted && plan.before.state != plan.after.state)
          RCLCPP_INFO(get_logger(), "Env state=%s requested=%s generation=%lu",
              to_string(plan.after.state).c_str(), to_string(plan.after.requested_state).c_str(), plan.after.generation);
        if (auto frame = controller_->take_frame(input.snapshot)) {
          if (remote.valid && now >= remote.acquired && now - remote.acquired <= configuration_.mode.guards.input_timeout) {
            frame->remote.header.stamp = remote.stamp;
            frame->remote.header.frame_id = frame->header.frame_id;
            frame->remote.axes.assign(remote.axes.begin(), remote.axes.end());
            for (size_t i = 0; i < 16; ++i) frame->remote.buttons.push_back((remote.keys >> i) & 1u);
            frame->validity |= Frame::VALID_RC;
          }
          publisher_->publish(*frame);
        }
        send_request(now);
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        controller_->feedback_fault(error.what());
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    try { controller_->shutdown(); }
    catch (const std::exception& error) { RCLCPP_ERROR(get_logger(), "Shutdown release failed: %s", error.what()); }
  }
};
}  // namespace anakin::env

int main(int argc, char** argv) {
  try {
    std::vector<std::string> arguments(argv, argv + argc);
    const auto options = anakin::env::parse_startup(arguments);
    auto configuration = anakin::env::load_configuration(options.mode, options.resources);
    const auto domain = std::to_string(configuration.mode.transport.ros_domain);
    if (setenv("ROS_DOMAIN_ID", domain.c_str(), 1) != 0) throw std::runtime_error("Cannot set configured ROS domain");
    auto ros_arguments = options.ros_arguments;
    std::vector<char*> ros_argv;
    for (auto& argument : ros_arguments) ros_argv.push_back(argument.data());
    rclcpp::init(static_cast<int>(ros_argv.size()), ros_argv.data());
    rclcpp::spin(std::make_shared<anakin::env::NativeNode>(std::move(configuration)));
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "anakin_env: " << error.what() << '\n';
    if (rclcpp::ok()) rclcpp::shutdown();
    return 1;
  }
}
