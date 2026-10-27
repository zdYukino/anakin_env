#include <anakin_env/confirmation.hpp>
#include <anakin_sdk/srv/switch_inference_mode.hpp>
#include <cmath>
#include <iostream>
#include <rclcpp/rclcpp.hpp>
using namespace std::chrono_literals;
namespace anakin::env {
using Switch = anakin_sdk::srv::SwitchInferenceMode;
using Status = anakin_sdk::msg::InferenceStatus;
class Node final : public rclcpp::Node {
 public:
  Node() : rclcpp::Node("anakin_env") {
    if (get_parameter("use_sim_time").as_bool())
      throw std::runtime_error("No official /clock verified; use_sim_time must be false");
    if (declare_parameter<std::string>("backend", "offline_fixture") != "offline_fixture")
      throw std::runtime_error(
          "Native transport has not yet passed isolated official-simulator acceptance");
    if (declare_parameter<bool>("arm", false))
      throw std::runtime_error("Offline checkpoint cannot arm or publish SDK commands");
    duration_ = declare_parameter<double>("duration", 8.);
    demo_ = declare_parameter<bool>("demo_switch", false);
    if (!std::isfinite(duration_) || duration_ < 1 || duration_ > 300)
      throw std::runtime_error("duration must be 1..300 seconds");
    pub_ =
        create_publisher<Frame>("anakin/state", rclcpp::QoS(1).best_effort().durability_volatile());
    target_sub_ = create_subscription<Target>("anakin/control_target",
                                              rclcpp::QoS(1).best_effort().durability_volatile(),
                                              [this](Target::ConstSharedPtr t) {
                                                std::string why;
                                                if (gate_.accept(*t, Clock::now(), why)) {
                                                  ++accepted_;
                                                  had_target_ = true;
                                                  health_.valid_target();
                                                  confirmation_.target(*t);
                                                  maybe_complete();
                                                } else {
                                                  ++rejected_;
                                                  last_reject_ = why;
                                                }
                                              });
    status_sub_ = create_subscription<Status>("anakin/inference_status",
                                              rclcpp::QoS(128).reliable().durability_volatile(),
                                              [this](Status::ConstSharedPtr s) { on_status(*s); });
    client_ = create_client<Switch>("anakin/switch_inference_mode");
    started_ = Clock::now();
    next_tick_ = started_ + 2ms;
    timer_ = create_wall_timer(2ms, [this] { tick(); });
    RCLCPP_INFO(get_logger(),
                "OFFLINE_FIXTURE: no SDK channels, execution_authorized=false; session %s",
                uuid_string(session_).c_str());
  }
  bool success = false, completed = false;

 private:
  Uuid session_ = new_uuid(), env_instance_ = new_uuid(), mc_instance_{};
  Gate gate_;
  ActiveConfirmation confirmation_;
  TransactionHealth health_;
  Switch::Request request_{};
  bool have_request_ = false, accepted_request_ = false, restart_latched_ = false,
       terminal_failure_ = false, completion_counted_ = false, had_target_ = false, demo_ = false;
  uint64_t epoch_ = 0, state_seq_ = 0, accepted_ = 0, rejected_ = 0, expired_events_ = 0;
  unsigned switch_stage_ = 0, completed_modes_ = 0;
  double duration_ = 8.;
  std::string last_reject_, failure_reason_;
  Clock::time_point started_, next_tick_, sent_{};
  int64_t pending_id_ = 0;
  bool in_flight_ = false;
  rclcpp::Publisher<Frame>::SharedPtr pub_;
  rclcpp::Subscription<Target>::SharedPtr target_sub_;
  rclcpp::Subscription<Status>::SharedPtr status_sub_;
  rclcpp::Client<Switch>::SharedPtr client_;
  rclcpp::TimerBase::SharedPtr timer_;
  Positions fixture_pose() const {
    return Positions{-.1, 0,  0, .3, -.2, 0, -.1, 0, 0,   .3, -.2, 0, 0, 0, 0,
                     0,   .3, 0, 1,  0,   0, 0,   0, -.3, 0,  1,   0, 0, 0};
  }
  void fail(const std::string& reason) {
    gate_.stop();
    health_.fail();
    if (!terminal_failure_) {
      terminal_failure_ = true;
      failure_reason_ = reason;
      RCLCPP_ERROR(get_logger(), "Local stop latched: %s", reason.c_str());
    }
  }
  void maybe_complete() {
    if (!terminal_failure_ && !completion_counted_ && confirmation_.active()) {
      completion_counted_ = true;
      ++completed_modes_;
      RCLCPP_INFO(get_logger(),
                  "Target-active publication and env validation confirmed epoch=%lu (not physical "
                  "execution)",
                  epoch_);
    }
  }
  void start_request(const std::string& model) {
    if (terminal_failure_ || restart_latched_) return;
    ++epoch_;
    request_ = Switch::Request();
    request_.session_id = session_;
    request_.expected_mc_instance_id = mc_instance_;
    request_.request_id = new_uuid();
    request_.next_epoch = epoch_;
    request_.source_state_seq = state_seq_ + 1;
    request_.schema_id = schema_id;
    request_.target_model = model;
    request_.gain_profile = "g1_asap_sim_v1";
    request_.execution_profile = strict_execution_profile;
    request_.command_semantics = mechanical_position;
    request_.transition_profile = "g1_named_upper_v1";
    request_.allow_source_continuation = true;
    request_.timeout.sec = 5;
    gate_.begin(session_, mc_instance_, request_.request_id, epoch_, model, true);
    confirmation_.begin(session_, mc_instance_, request_.request_id, epoch_);
    health_.begin(Clock::now() + 5s);
    have_request_ = true;
    accepted_request_ = false;
    completion_counted_ = false;
    had_target_ = false;
    if (in_flight_) {
      client_->remove_pending_request(pending_id_);
      in_flight_ = false;
    }
    RCLCPP_INFO(get_logger(), "Env requested model=%s epoch=%lu request=%s", model.c_str(), epoch_,
                uuid_string(request_.request_id).c_str());
  }
  void on_status(const Status& s) {
    if (s.schema_version != schema_version || s.schema_id != schema_id || zero(s.mc_instance_id))
      return;
    if (zero(mc_instance_)) {
      mc_instance_ = s.mc_instance_id;
      return;
    }
    if (s.mc_instance_id != mc_instance_) {
      restart_latched_ = true;
      fail("MC restart; explicit reauthorization required");
      return;
    }
    if (!have_request_ || s.session_id != session_ || s.request_id != request_.request_id ||
        s.epoch != epoch_)
      return;
    if (s.gain_profile != request_.gain_profile ||
        s.execution_profile != request_.execution_profile ||
        s.command_semantics != request_.command_semantics) {
      fail("Offline status execution contract mismatch");
      return;
    }
    confirmation_.status(s);
    if (s.state == Status::FAILED) {
      fail("MC transaction failed: " + s.detail);
      return;
    }
    maybe_complete();
    // Motion completion is a separate published fact. Only env creates the new fallback epoch.
    if (!terminal_failure_ && request_.target_model == "asap_cr7" && confirmation_.active() &&
        s.motion_done) {
      switch_stage_ = 2;
      start_request("asap_loco");
    }
  }
  void send_request() {
    if (!client_->service_is_ready()) return;
    const auto id = request_.request_id;
    auto future = client_->async_send_request(
        std::make_shared<Switch::Request>(request_),
        [this, id](rclcpp::Client<Switch>::SharedFuture f) {
          if (id != request_.request_id || terminal_failure_) return;
          in_flight_ = false;
          auto response = f.get();
          if (response->accepted && response->mc_instance_id == mc_instance_ &&
              response->epoch == epoch_ &&
              response->execution_profile == request_.execution_profile &&
              response->command_semantics == request_.command_semantics &&
              response->gain_profile == request_.gain_profile)
            accepted_request_ = true;
          else
            fail("Switch rejected: " + response->detail);
        });
    pending_id_ = future.request_id;
    in_flight_ = true;
    sent_ = Clock::now();
  }
  void tick() {
    const auto t = Clock::now();
    const double elapsed = std::chrono::duration<double>(t - started_).count();
    if (t > next_tick_ + 2ms) {
      while (next_tick_ <= t) next_tick_ += 2ms;
    } else
      next_tick_ += 2ms;
    if (!terminal_failure_ && !restart_latched_ && !zero(mc_instance_) && !have_request_)
      start_request("asap_loco");
    if (demo_ && !terminal_failure_ && completed_modes_ >= 1 && switch_stage_ == 0 &&
        elapsed >= 2.) {
      switch_stage_ = 1;
      start_request("asap_cr7");
    }
    Frame f;
    f.header.stamp = now();
    f.header.frame_id = "offline_fixture_base";
    f.schema_version = schema_version;
    f.schema_id = schema_id;
    f.session_id = session_;
    f.env_instance_id = env_instance_;
    f.epoch = epoch_;
    f.state_seq = ++state_seq_;
    f.validity = 7;
    f.gain_profile = "g1_asap_sim_v1";
    f.execution_profile = strict_execution_profile;
    f.command_semantics = mechanical_position;
    f.execution_authorized = false;
    f.joints.header = f.header;
    f.joints.name = joint_names();
    auto q = fixture_pose();
    f.joints.position.assign(q.begin(), q.end());
    f.joints.velocity.assign(29, 0);
    f.imu.header = f.header;
    f.imu.orientation.w = 1;
    f.command.header = f.header;
    f.command_base_height = .78;
    f.command_stand = false;
    f.mc_instance_seen = mc_instance_;
    f.request_id_seen = request_.request_id;
    f.target_seq_seen = gate_.accepted_seq();
    f.phase_mask_seen = gate_.phase_mask();
    diagnostic_msgs::msg::DiagnosticStatus d;
    d.name = "anakin_env/backend";
    d.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
    d.message = "OFFLINE_FIXTURE: no physics, no native transport, no actuation";
    f.diagnostics.header = f.header;
    f.diagnostics.status.push_back(d);
    gate_.state(f, t);
    pub_->publish(f);
    if (have_request_ && !accepted_request_ && !terminal_failure_ && !restart_latched_) {
      if (in_flight_ && t - sent_ > 500ms) {
        client_->remove_pending_request(pending_id_);
        in_flight_ = false;
      }
      if (!in_flight_) send_request();
    }
    if (have_request_ && !terminal_failure_) {
      health_.tick(t, gate_.live(t), confirmation_.active());
      if (health_.failed()) {
        if (had_target_ && !gate_.live(t)) {
          ++expired_events_;
          fail("Validated target expired; reception/retry cannot extend TTL");
        } else
          fail("Inference transaction deadline expired");
      }
    }
    if (elapsed >= duration_) {
      completed = true;
      success = accepted_ >= 5 && completed_modes_ >= (demo_ ? 3u : 1u) && !terminal_failure_ &&
                !restart_latched_ && health_.finish(gate_.live(t), confirmation_.active());
      gate_.stop();
      RCLCPP_INFO(get_logger(),
                  "OFFLINE_RESULT success=%s accepted=%lu rejected=%lu completed_modes=%u "
                  "expiry_events=%lu last_rejection=%s failure=%s",
                  success ? "true" : "false", accepted_, rejected_, completed_modes_,
                  expired_events_, last_reject_.c_str(), failure_reason_.c_str());
      rclcpp::shutdown();
    }
  }
};
}  // namespace anakin::env
int main(int argc, char** argv) {
  try {
    rclcpp::init(argc, argv);
    auto n = std::make_shared<anakin::env::Node>();
    rclcpp::spin(n);
    if (rclcpp::ok()) rclcpp::shutdown();
    return n->completed ? (n->success ? 0 : 1) : 130;
  } catch (const std::exception& e) {
    std::cerr << "anakin_env: " << e.what() << '\n';
    if (rclcpp::ok()) rclcpp::shutdown();
    return 1;
  }
}
