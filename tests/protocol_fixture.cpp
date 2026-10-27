// Offline ROS fault fixture. Never opens native SDK channels; only accepts the
// explicitly labelled, disarmed offline state source.
#include <anakin_env/gate.hpp>
#include <anakin_sdk/msg/inference_status.hpp>
#include <anakin_sdk/srv/switch_inference_mode.hpp>
#include <chrono>
#include <rclcpp/rclcpp.hpp>
using namespace std::chrono_literals;
using Status = anakin_sdk::msg::InferenceStatus;
using Switch = anakin_sdk::srv::SwitchInferenceMode;
class Fixture final : public rclcpp::Node {
 public:
  Fixture() : Node("anakin_offline_protocol_fixture") {
    mode_ = declare_parameter<std::string>("fault", "normal");
    start_ = std::chrono::steady_clock::now();
    targets_ = create_publisher<anakin::env::Target>("anakin/control_target",
                                                     rclcpp::QoS(1).best_effort());
    statuses_ = create_publisher<Status>("anakin/inference_status", rclcpp::QoS(128).reliable());
    states_ = create_subscription<anakin::env::Frame>(
        "anakin/state", rclcpp::QoS(1).best_effort(), [this](anakin::env::Frame::ConstSharedPtr f) {
          if (f->header.frame_id == "offline_fixture_base" && !f->execution_authorized) latest_ = f;
        });
    service_ = create_service<Switch>(
        "anakin/switch_inference_mode",
        [this](Switch::Request::SharedPtr q, Switch::Response::SharedPtr a) {
          a->mc_instance_id = instance_;
          a->epoch = q->next_epoch;
          a->accepted = q->expected_mc_instance_id == instance_ &&
                        anakin::known_execution_contract(q->execution_profile, q->command_semantics,
                                                         q->gain_profile);
          a->execution_profile = q->execution_profile;
          a->command_semantics = q->command_semantics;
          a->gain_profile = q->gain_profile;
          if (a->accepted && (!have_ || q->request_id != request_.request_id)) {
            request_ = *q;
            have_ = true;
            ticks_ = 0;
            first_ = seq_ + 1;
            RCLCPP_INFO(get_logger(), "FIXTURE_REQUEST model=%s epoch=%lu", q->target_model.c_str(),
                        q->next_epoch);
          }
        });
    timer_ = create_wall_timer(20ms, [this] { tick(); });
  }

 private:
  anakin::Uuid instance_ = anakin::new_uuid();
  std::string mode_;
  bool have_ = false, restarted_ = false;
  uint64_t seq_ = 0, first_ = 0, ticks_ = 0;
  Switch::Request request_{};
  anakin::env::Frame::ConstSharedPtr latest_;
  std::chrono::steady_clock::time_point start_;
  rclcpp::Publisher<anakin::env::Target>::SharedPtr targets_;
  rclcpp::Publisher<Status>::SharedPtr statuses_;
  rclcpp::Subscription<anakin::env::Frame>::SharedPtr states_;
  rclcpp::Service<Switch>::SharedPtr service_;
  rclcpp::TimerBase::SharedPtr timer_;
  void tick() {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    if (mode_ == "stale_tail" && elapsed >= 1.2) return;
    if (mode_ == "restart" && elapsed >= 1.2 && !restarted_) {
      instance_ = anakin::new_uuid();
      restarted_ = true;
    }
    Status s;
    s.header.stamp = now();
    s.schema_version = anakin::schema_version;
    s.schema_id = anakin::schema_id;
    s.mc_instance_id = instance_;
    if (!have_ || !latest_ || latest_->epoch != request_.next_epoch) {
      statuses_->publish(s);
      return;
    }
    s.session_id = request_.session_id;
    s.request_id = request_.request_id;
    s.epoch = request_.next_epoch;
    s.accepted = true;
    s.state = Status::ACTIVE;
    s.target_active = true;
    s.target_ready = true;
    s.requested_model = request_.target_model;
    s.active_model = request_.target_model;
    s.gain_profile = request_.gain_profile;
    s.execution_profile = request_.execution_profile;
    s.command_semantics = request_.command_semantics;
    s.phase_first_seq[2] = first_;
    s.phase_last_seq[2] = ++seq_;
    s.published_phase_mask = 4;
    s.source_state_seq = latest_->state_seq;
    s.motion_done = request_.target_model == "asap_cr7" && ticks_ >= 6;
    if (mode_ == "failed_tail" && elapsed >= 1.2) {
      s.state = Status::FAILED;
      s.detail = "Injected terminal failure after successful active targets";
    }
    statuses_->publish(s);  // Deliberately always first; first ACTIVE target is dropped.
    anakin::env::Target t;
    t.header = s.header;
    t.schema_version = anakin::schema_version;
    t.schema_id = anakin::schema_id;
    t.session_id = s.session_id;
    t.mc_instance_id = instance_;
    t.request_id = request_.request_id;
    t.epoch = s.epoch;
    t.seq = seq_;
    t.source_state_seq = latest_->state_seq;
    t.model_id = request_.target_model;
    t.gain_profile = request_.gain_profile;
    t.execution_profile = request_.execution_profile;
    t.command_semantics = request_.command_semantics;
    t.validity = 1;
    t.valid_for.nanosec = 40000000;
    t.phase = 4;
    t.target.joint_names = anakin::joint_names();
    trajectory_msgs::msg::JointTrajectoryPoint point;
    point.positions = latest_->joints.position;
    t.target.points.push_back(point);
    if (ticks_++ > 0) targets_->publish(t);
  }
};
int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<Fixture>());
  rclcpp::shutdown();
  return 0;
}
