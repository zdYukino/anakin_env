#include <anakin_env/confirmation.hpp>
#include <anakin_env/execution.hpp>
#include <anakin_env/transport.hpp>
#include <anakin_sdk/srv/switch_inference_mode.hpp>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <sstream>
#include <thread>
using namespace std::chrono_literals;

namespace anakin::env {
using Switch = anakin_sdk::srv::SwitchInferenceMode;
using Status = anakin_sdk::msg::InferenceStatus;
class NativeNode final : public rclcpp::Node {
 public:
  NativeNode() : rclcpp::Node("anakin_env") {
    if (get_parameter("use_sim_time").as_bool())
      throw std::runtime_error("Native monotonic loop requires use_sim_time=false");
    if (declare_parameter<std::string>("backend", "") != "unitree_loopback_sim")
      throw std::runtime_error(
          "Choose backend=unitree_loopback_sim explicitly; hardware is unsupported");
    if (declare_parameter<bool>("arm", false))
      throw std::runtime_error("Construction cannot arm; deliberate fresh RC edges are required");
    const auto interface = declare_parameter<std::string>("interface", "lo");
    const int domain = declare_parameter<int>("sdk_domain", 31);
    const auto remote = declare_parameter<std::string>("remote_source", "lowstate");
    if (remote != "lowstate" && remote != "wireless_topic")
      throw std::runtime_error("remote_source must be lowstate or wireless_topic");
    gain_profile_ = declare_parameter<std::string>("gain_profile", merged_gain_profile);
    if (!known_gain_profile(gain_profile_))
      throw std::runtime_error("Unknown simulation gain profile");
    const double alignment_seconds = declare_parameter<double>("alignment_seconds", 2.);
    control_.set_alignment_seconds(alignment_seconds);
    duration_ = declare_parameter<double>("duration", 20.);
    min_active_ = declare_parameter<double>("min_active_seconds", 5.);
    expected_modes_ = declare_parameter<int>("expected_modes", 1);
    if (!std::isfinite(duration_) || duration_ < 1 || duration_ > 300 ||
        !std::isfinite(min_active_) || min_active_ < 0 || min_active_ > duration_ ||
        expected_modes_ < 1 || expected_modes_ > 3)
      throw std::runtime_error("Invalid finite run acceptance parameters");
    result_path_ = declare_parameter<std::string>("result_json", "");
    const auto csv_path = declare_parameter<std::string>("telemetry_csv", "");
    if (!csv_path.empty()) {
      telemetry_.open(csv_path);
      if (!telemetry_) throw std::runtime_error("Cannot open native telemetry");
      telemetry_ << "elapsed,sdk_tick,state_seq,epoch,mode,accepted_seq,published_seq,sdk_writes,"
                    "tilt,height,source_age_ms,rc_age_ms\n";
    }
    pub_ =
        create_publisher<Frame>("anakin/state", rclcpp::QoS(1).best_effort().durability_volatile());
    target_sub_ = create_subscription<Target>(
        "anakin/control_target", rclcpp::QoS(1).best_effort().durability_volatile(),
        [this](Target::ConstSharedPtr target) { on_target(*target); });
    status_sub_ = create_subscription<Status>(
        "anakin/inference_status", rclcpp::QoS(128).reliable().durability_volatile(),
        [this](Status::ConstSharedPtr status) { on_status(*status); });
    client_ = create_client<Switch>("anakin/switch_inference_mode");
    gate_.stop();
    // Init opens explicit simulation channels only. It does not authorize a write.
    transport_.start_loopback(interface, domain, remote == "wireless_topic");
    RCLCPP_INFO(
        get_logger(),
        "NATIVE_SIM disarmed; interface=lo SDK-domain=%d; RC source=%s; no hardware backend",
        domain, remote.c_str());
    started_ = Clock::now();
    worker_ = std::thread([this] { run(); });
  }
  ~NativeNode() override {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
  }
  std::atomic<bool> completed{false}, success{false};

 private:
  std::mutex mutex_;
  NativeTransport transport_;
  LocalControl control_;
  RemoteEdges remote_edges_;
  MotionCommand commands_;
  Gate gate_;
  ActiveConfirmation confirmation_;
  TransactionHealth health_;
  ActiveRunAcceptance run_acceptance_;
  Uuid session_ = new_uuid(), env_instance_ = new_uuid(), mc_instance_{};
  Switch::Request request_{};
  std::optional<Target> target_;
  uint64_t epoch_ = 0, state_seq_ = 0, generation_ = 0, accepted_ = 0, rejected_ = 0;
  uint64_t bad_baseline_ = 0, reset_baseline_ = 0, sdk_writes_ = 0, active_writes_ = 0,
           damping_writes_ = 0, local_writes_ = 0, loop_ticks_ = 0, skipped_periods_ = 0;
  bool terminal_failure_ = false, have_request_ = false, accepted_request_ = false,
       in_flight_ = false, completion_counted_ = false, preparing_damp_ = false;
  int64_t pending_id_ = 0;
  int expected_modes_ = 1, completed_modes_ = 0;
  double duration_ = 20., min_active_ = 5., min_height_ = 100., max_tilt_ = 0.,
         active_seconds_ = 0., max_write_gap_ms_ = 0., max_lateness_ms_ = 0.;
  Clock::time_point started_{}, sent_{}, last_write_{}, first_write_{}, final_write_{};
  std::vector<double> sdk_gaps_ms_;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
  std::string failure_, last_reject_, result_path_, gain_profile_;
  std::ofstream telemetry_;
  Frame frame_;
  std::optional<NativeSample> native_;
  struct Receipt {
    Uuid instance{}, request{};
    uint64_t epoch = 0, seq = 0;
    uint8_t mask = 0;
  } published_;
  rclcpp::Publisher<Frame>::SharedPtr pub_;
  rclcpp::Subscription<Target>::SharedPtr target_sub_;
  rclcpp::Subscription<Status>::SharedPtr status_sub_;
  rclcpp::Client<Switch>::SharedPtr client_;

  void stop_local(const std::string& why, bool failure) {
    gate_.stop();
    target_.reset();
    run_acceptance_.break_tail();
    health_.fail();
    control_.stop(Clock::now(), failure);
    if (failure && !terminal_failure_) {
      terminal_failure_ = true;
      failure_ = why;
      RCLCPP_ERROR(get_logger(), "NATIVE local stop latched: %s", why.c_str());
    } else if (!failure) {
      have_request_ = false;
      RCLCPP_INFO(get_logger(), "NATIVE local RC stop; new deliberate authorization required");
    }
  }
  void start_request(std::string model) {
    if (terminal_failure_ || zero(mc_instance_)) return;
    const bool startup_profile = control_.mode() == ExecutionMode::Hold;
    preparing_damp_ = control_.mode() != ExecutionMode::Hold;
    control_.prepare();
    ++epoch_;
    request_ = Switch::Request{};
    request_.session_id = session_;
    request_.expected_mc_instance_id = mc_instance_;
    request_.request_id = new_uuid();
    request_.next_epoch = epoch_;
    request_.source_state_seq = state_seq_ + 1;
    request_.schema_id = schema_id;
    request_.target_model = model;
    run_acceptance_.begin(model, epoch_);
    request_.gain_profile = gain_profile_;
    request_.transition_profile = startup_profile ? "g1_startup_v1" : "g1_named_upper_v1";
    request_.allow_source_continuation = true;
    request_.timeout.sec = 5;
    gate_.begin(session_, mc_instance_, request_.request_id, epoch_, model, true, gain_profile_);
    confirmation_.begin(session_, mc_instance_, request_.request_id, epoch_);
    health_.begin(Clock::now() + 5s);
    target_.reset();
    if (in_flight_) client_->remove_pending_request(pending_id_);
    in_flight_ = false;
    accepted_request_ = false;
    have_request_ = true;
    completion_counted_ = false;
    RCLCPP_INFO(get_logger(),
                "NATIVE env requested %s epoch=%lu; accepted response is not completion",
                model.c_str(), epoch_);
  }
  bool published_active() const {
    return published_.instance == mc_instance_ && published_.request == request_.request_id &&
           published_.epoch == epoch_ && (published_.mask & Target::TARGET_ACTIVE);
  }
  void maybe_complete() {
    if (!terminal_failure_ && !completion_counted_ && confirmation_.active() &&
        published_active()) {
      if (!run_acceptance_.confirm(request_.target_model, epoch_)) return;
      completed_modes_ = static_cast<int>(run_acceptance_.models().size());
      completion_counted_ = true;
      RCLCPP_INFO(get_logger(),
                  "SDK_PUBLISHED epoch=%lu active-seq=%lu; writer receipt, not actuator execution",
                  epoch_, published_.seq);
    }
  }
  void on_target(const Target& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_failure_ || !have_request_ || !control_.authorized()) return;
    std::string why;
    if (gate_.accept(target, Clock::now(), why)) {
      ++accepted_;
      target_ = target;
      health_.valid_target();
      confirmation_.target(target);
    } else {
      ++rejected_;
      if (why != last_reject_)
        RCLCPP_ERROR(get_logger(), "Rejected native target seq=%lu model=%s reason=%s", target.seq,
                     target.model_id.c_str(), why.c_str());
      last_reject_ = why;
    }
  }
  void on_status(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status.schema_version != schema_version || status.schema_id != schema_id ||
        zero(status.mc_instance_id))
      return;
    if (zero(mc_instance_)) {
      if (zero(status.session_id) && zero(status.request_id)) mc_instance_ = status.mc_instance_id;
      return;
    }
    if (status.mc_instance_id != mc_instance_) {
      stop_local("MC instance changed; restart with explicit authorization", true);
      return;
    }
    if (!have_request_ || status.session_id != session_ ||
        status.request_id != request_.request_id || status.epoch != epoch_)
      return;
    if (status.requested_model != request_.target_model ||
        status.gain_profile != request_.gain_profile) {
      stop_local("MC status model/gain mismatch", true);
      return;
    }
    confirmation_.status(status);
    if (status.state == Status::FAILED) {
      stop_local("MC transaction failed: " + status.detail, true);
      return;
    }
    maybe_complete();
    if (!terminal_failure_ && request_.target_model == "asap_cr7" && completion_counted_ &&
        status.motion_done)
      start_request("asap_loco");  // only env allocates the fallback epoch
  }
  void send_request(Clock::time_point now) {
    if (!have_request_ || accepted_request_ || terminal_failure_) return;
    if (in_flight_ && now - sent_ > 500ms) {
      client_->remove_pending_request(pending_id_);
      in_flight_ = false;
    }
    if (in_flight_ || !client_->service_is_ready()) return;
    const auto id = request_.request_id;
    auto future = client_->async_send_request(
        std::make_shared<Switch::Request>(request_),
        [this, id](rclcpp::Client<Switch>::SharedFuture future) {
          std::lock_guard<std::mutex> lock(mutex_);
          if (id != request_.request_id || terminal_failure_) return;
          in_flight_ = false;
          try {
            const auto response = future.get();
            if (response->accepted && response->mc_instance_id == mc_instance_ &&
                response->epoch == epoch_)
              accepted_request_ = true;
            else
              stop_local("Inference request rejected: " + response->detail, true);
          } catch (const std::exception& error) {
            stop_local(error.what(), true);
          }
        });
    pending_id_ = future.request_id;
    in_flight_ = true;
    sent_ = now;
  }
  static double tilt(const Frame& f) {
    const auto& q = f.imu.orientation;
    return std::acos(std::clamp(1. - 2. * (q.x * q.x + q.y * q.y), -1., 1.));
  }
  void fill_frame(const NativeSample& sample, const RemoteSample& rc,
                  const OdometrySample& odometry, Clock::time_point now) {
    frame_ = sample.frame;
    frame_.header.frame_id = "unitree_sim_base";
    frame_.joints.header = frame_.header;
    frame_.imu.header = frame_.header;
    frame_.session_id = session_;
    frame_.env_instance_id = env_instance_;
    frame_.epoch = epoch_;
    frame_.state_seq = state_seq_;
    frame_.gain_profile = gain_profile_;
    frame_.execution_mode = static_cast<uint8_t>(control_.mode());
    frame_.execution_authorized = control_.mode() == ExecutionMode::Active && gate_.live(now) &&
                                  now - sample.acquired <= 40ms && rc.valid &&
                                  now - rc.acquired <= 250ms;
    frame_.command.header.stamp = system_stamp();
    frame_.command.header.frame_id = frame_.header.frame_id;
    frame_.command_base_height = commands_.height();
    frame_.command_stand = commands_.moving();
    if (rc.valid && now - rc.acquired <= 250ms) {
      frame_.remote.header.stamp = rc.stamp;
      frame_.remote.header.frame_id = frame_.header.frame_id;
      frame_.remote.axes.assign(rc.axes.begin(), rc.axes.end());
      for (size_t i = 0; i < 16; ++i) frame_.remote.buttons.push_back((rc.keys >> i) & 1u);
      const auto velocity = commands_.velocity(rc);
      frame_.command.twist.linear.x = velocity[0];
      frame_.command.twist.linear.y = velocity[1];
      frame_.command.twist.angular.z = velocity[2];
      frame_.validity |= Frame::VALID_RC | Frame::VALID_COMMAND;
    }
    if (odometry.valid && now - odometry.acquired <= 100ms) {
      frame_.odometry.header.stamp = odometry.stamp;
      frame_.odometry.header.frame_id = "unitree_sim_world";
      frame_.odometry.child_frame_id = frame_.header.frame_id;
      frame_.odometry.pose.pose.position.x = odometry.position[0];
      frame_.odometry.pose.pose.position.y = odometry.position[1];
      frame_.odometry.pose.pose.position.z = odometry.position[2];
      frame_.odometry.pose.pose.orientation = frame_.imu.orientation;
      const auto body_velocity = world_to_body(frame_.imu.orientation, odometry.velocity);
      frame_.odometry.twist.twist.linear.x = body_velocity[0];
      frame_.odometry.twist.twist.linear.y = body_velocity[1];
      frame_.odometry.twist.twist.linear.z = body_velocity[2];
      frame_.odometry.twist.twist.angular = frame_.imu.angular_velocity;
      frame_.validity |= Frame::VALID_ODOMETRY;
    }
    frame_.mc_instance_seen = mc_instance_;
    frame_.request_id_seen = request_.request_id;
    frame_.target_seq_seen = gate_.accepted_seq();
    frame_.phase_mask_seen = gate_.phase_mask();
    frame_.sdk_published_mc_instance_id = published_.instance;
    frame_.sdk_published_request_id = published_.request;
    frame_.sdk_published_epoch = published_.epoch;
    frame_.sdk_published_target_seq = published_.seq;
    frame_.sdk_published_phase_mask = published_.mask;
    frame_.sdk_publish_count = sdk_writes_;
    diagnostic_msgs::msg::DiagnosticStatus diagnostic;
    diagnostic.name = "anakin_env/loopback_native";
    diagnostic.level = terminal_failure_ ? diagnostic.ERROR : diagnostic.OK;
    diagnostic.message = terminal_failure_
                             ? failure_
                             : "loopback simulation; SDK publication is not physical execution";
    frame_.diagnostics.header.stamp = system_stamp();
    frame_.diagnostics.status.push_back(diagnostic);
  }
  bool write_command(const LowCommand& command, Clock::time_point now, const Target* target,
                     bool damping, Clock::time_point absolute_end = Clock::time_point::max()) {
    const auto write_now = Clock::now();
    if (write_now >= absolute_end ||
        (damping && !control_.authorized() && !control_.damping(write_now)))
      return false;
    if (!damping) {
      const auto state = transport_.sample();
      const auto remote = transport_.remote();
      if (!state || write_now - state->acquired > 40ms || !remote.valid ||
          write_now - remote.acquired > 250ms || (target && !gate_.live(write_now))) {
        stop_local("Freshness expired immediately before SDK write", true);
        if (state) write_command(encode_damping(state->mode_machine), write_now, nullptr, true);
        return false;
      }
    }
    const auto immediately_before_write = Clock::now();
    if (immediately_before_write >= absolute_end ||
        (damping && !control_.authorized() && !control_.damping(immediately_before_write)))
      return false;
    bool written = false;
    try {
      written = transport_.write(command);
    } catch (const std::exception& error) {
      stop_local(std::string("SDK writer exception: ") + error.what(), true);
      return false;
    }
    if (!written) {
      stop_local("SDK writer rejected publication", true);
      return false;
    }
    now = Clock::now();
    ++sdk_writes_;
    if (first_write_ == Clock::time_point{}) first_write_ = now;
    final_write_ = now;
    if (last_write_ != Clock::time_point{}) {
      const double gap = std::chrono::duration<double, std::milli>(now - last_write_).count();
      max_write_gap_ms_ = std::max(max_write_gap_ms_, gap);
      sdk_gaps_ms_.push_back(gap);
    }
    last_write_ = now;
    if (target) {
      ++active_writes_;
      run_acceptance_.sdk_write(now, target->phase == Target::TARGET_ACTIVE, target->model_id,
                                target->epoch);
      if (published_.instance != target->mc_instance_id ||
          published_.request != target->request_id || published_.epoch != target->epoch)
        published_ = Receipt{target->mc_instance_id, target->request_id, target->epoch, 0, 0};
      published_.seq = target->seq;
      published_.mask |= target->phase;
      maybe_complete();
    } else {
      run_acceptance_.break_tail();
      if (damping)
        ++damping_writes_;
      else
        ++local_writes_;
    }
    return true;
  }
  void tick(Clock::time_point now) {
    ++loop_ticks_;
    auto sample = transport_.sample();
    const auto rc = transport_.remote();
    const auto odometry = transport_.odometry();
    const bool fresh = sample && now - sample->acquired <= 40ms;
    const bool rc_fresh = rc.valid && now - rc.acquired <= 250ms;
    if (!sample) {
      if (now - started_ > 5s && !terminal_failure_)
        stop_local("No validated SDK state within startup deadline", true);
      return;
    }
    if (control_.authorized()) {
      if (!fresh)
        stop_local("Native state age exceeded 40 ms", true);
      else if (!rc_fresh)
        stop_local("Remote age exceeded 250 ms", true);
      else if (transport_.bad_packets() != bad_baseline_)
        stop_local("Invalid SDK feedback/remote packet", true);
      else if (transport_.resets() != reset_baseline_)
        stop_local("SDK tick reset during authorization", true);
      else if (target_sub_->get_publisher_count() > 1)
        stop_local("Multiple MC target publishers", true);
    }
    const auto intent = remote_edges_.update(rc);
    if (rc_fresh && !intent.stop) commands_.update(intent);
    if (intent.stop && control_.authorized()) stop_local("RC stop", false);
    if (intent.x && fresh && rc_fresh && !terminal_failure_) {
      if (control_.mode() == ExecutionMode::Disarmed) {
        if (tilt(sample->frame) > .7)
          stop_local("Cannot authorize a tilted initial pose", true);
        else {
          Positions measured{};
          std::copy_n(sample->frame.joints.position.begin(), 29, measured.begin());
          for (size_t i = 0; i < 29; ++i)
            if (measured[i] < lower_limits()[i] || measured[i] > upper_limits()[i])
              throw std::runtime_error("Initial measured pose outside bounds: " + joint_names()[i] +
                                       " q=" + std::to_string(measured[i]));
          bad_baseline_ = transport_.bad_packets();
          reset_baseline_ = transport_.resets();
          commands_ = MotionCommand{};
          control_.stand(measured, now);
          RCLCPP_INFO(get_logger(),
                      "RC authorized measured-pose alignment; inference remains disabled");
        }
      } else if (control_.mode() == ExecutionMode::Hold && !zero(mc_instance_))
        start_request("asap_loco");
    }
    if (intent.mimic && have_request_ && completion_counted_ &&
        control_.mode() == ExecutionMode::Active)
      start_request("asap_cr7");
    else if (intent.loco && have_request_ && completion_counted_ &&
             control_.mode() == ExecutionMode::Active)
      start_request("asap_loco");
    else if (intent.reset && have_request_ && completion_counted_ &&
             control_.mode() == ExecutionMode::Active)
      start_request(request_.target_model);
    if (generation_ != sample->generation) {
      generation_ = sample->generation;
      ++state_seq_;
      native_ = sample;
      fill_frame(*sample, rc, odometry, now);
      gate_.state(frame_, sample->acquired);
      pub_->publish(frame_);
    }
    send_request(now);
    if (have_request_ && !terminal_failure_) {
      health_.tick(now, gate_.live(now), confirmation_.active());
      if (health_.failed())
        stop_local("Target TTL or inference transaction deadline expired", true);
    }
    if (control_.authorized()) {
      const double angle = tilt(sample->frame);
      if (control_.mode() == ExecutionMode::Active) {
        max_tilt_ = std::max(max_tilt_, angle);
        if (odometry.valid && now - odometry.acquired <= 100ms)
          min_height_ = std::min(min_height_, static_cast<double>(odometry.position[2]));
      }
      if (angle > 1.0 ||
          (odometry.valid && now - odometry.acquired <= 100ms && odometry.position[2] < .3))
        stop_local("Simulation posture criterion failed", true);
    }
    if (control_.authorized() && fresh && rc_fresh) {
      if (have_request_ && target_ && gate_.live(now)) {
        // Recheck the monotonic target deadline immediately before the SDK write.
        Positions q{};
        std::copy_n(target_->target.points[0].positions.begin(), 29, q.begin());
        const auto command =
            encode_position(q, sample->mode_machine, target_->gain_profile, target_->model_id);
        if (!gate_.live(Clock::now()))
          stop_local("Target expired before SDK write", true);
        else {
          control_.active();
          write_command(command, now, &*target_, false);
        }
      } else if (control_.mode() == ExecutionMode::Aligning ||
                 control_.mode() == ExecutionMode::Hold ||
                 (control_.mode() == ExecutionMode::Preparing && !preparing_damp_)) {
        write_command(encode_position(control_.local_target(now), sample->mode_machine,
                                      gain_profile_, "asap_loco"),
                      now, nullptr, false);
      } else {
        write_command(encode_damping(sample->mode_machine), now, nullptr, true);
      }
    } else if (control_.damping(now)) {
      write_command(encode_damping(sample->mode_machine), now, nullptr, true);
    } else {
      last_write_ = {};
    }
    control_.settle(now);
    if (telemetry_ && loop_ticks_ % 10 == 0) {
      telemetry_ << std::setprecision(10) << std::chrono::duration<double>(now - started_).count()
                 << ',' << sample->frame.sdk_tick << ',' << state_seq_ << ',' << epoch_ << ','
                 << static_cast<int>(control_.mode()) << ',' << gate_.accepted_seq() << ','
                 << published_.seq << ',' << sdk_writes_ << ',' << tilt(sample->frame) << ','
                 << (odometry.valid ? odometry.position[2] : -1.f) << ','
                 << std::chrono::duration<double, std::milli>(now - sample->acquired).count() << ','
                 << (rc.valid ? std::chrono::duration<double, std::milli>(now - rc.acquired).count()
                              : -1.)
                 << '\n';
    }
  }
  void finish(bool natural_completion) {
    active_seconds_ = run_acceptance_.tail_seconds();
    bool passed =
        natural_completion && !terminal_failure_ && control_.mode() == ExecutionMode::Active &&
        run_acceptance_.sequence_matches(expected_modes_) && active_seconds_ >= min_active_ &&
        accepted_ >= 10 && min_height_ < 100. && published_active() && target_ &&
        target_->phase == Target::TARGET_ACTIVE && target_->model_id == request_.target_model &&
        gate_.live(Clock::now()) && confirmation_.active();
    // Stop is independent of ROS reception or any MC acknowledgement.
    gate_.stop();
    target_.reset();
    if (control_.authorized() && native_) {
      const auto stop_start = Clock::now();
      control_.stop(stop_start, false);
      DeadlineSchedule schedule(stop_start, 100ms);
      while (!schedule.done(Clock::now())) {
        std::this_thread::sleep_until(schedule.next());
        const auto now = Clock::now();
        if (!schedule.due(now)) continue;
        write_command(encode_damping(native_->mode_machine), now, nullptr, true, schedule.end());
      }
    }
    passed = passed && !terminal_failure_;
    if (telemetry_) telemetry_.flush();
    std::sort(sdk_gaps_ms_.begin(), sdk_gaps_ms_.end());
    const auto percentile = [this](double p) {
      if (sdk_gaps_ms_.empty()) return 0.;
      return sdk_gaps_ms_[static_cast<size_t>(p * (sdk_gaps_ms_.size() - 1))];
    };
    const double write_span = std::chrono::duration<double>(final_write_ - first_write_).count();
    const double write_hz = write_span > 0 ? static_cast<double>(sdk_writes_ - 1) / write_span : 0.;
    std::ostringstream summary;
    summary << std::boolalpha << std::setprecision(12) << "{\n  \"success\": " << passed
            << ",\n  \"backend\": \"unitree_loopback_sim\",\n"
            << "  \"sdk_publication_is_physical_execution\": false,\n"
            << "  \"accepted_targets\": " << accepted_ << ",\n  \"rejected_targets\": " << rejected_
            << ",\n  \"completed_modes\": " << completed_modes_
            << ",\n  \"active_seconds\": " << active_seconds_
            << ",\n  \"target_active_total_seconds\": " << run_acceptance_.total_seconds()
            << ",\n  \"sdk_writes\": " << sdk_writes_
            << ",\n  \"policy_sdk_writes\": " << active_writes_
            << ",\n  \"local_sdk_writes\": " << local_writes_
            << ",\n  \"damping_sdk_writes\": " << damping_writes_
            << ",\n  \"writer_ticks\": " << loop_ticks_
            << ",\n  \"skipped_2ms_periods\": " << skipped_periods_
            << ",\n  \"max_scheduling_lateness_ms\": " << max_lateness_ms_
            << ",\n  \"max_sdk_write_gap_ms\": " << max_write_gap_ms_
            << ",\n  \"minimum_active_pelvis_height\": " << min_height_
            << ",\n  \"maximum_active_tilt_rad\": " << max_tilt_
            << ",\n  \"sdk_write_mean_hz\": " << write_hz
            << ",\n  \"sdk_write_gap_p50_ms\": " << percentile(.5)
            << ",\n  \"sdk_write_gap_p99_ms\": " << percentile(.99)
            << ",\n  \"terminal_failure\": " << terminal_failure_
            << ",\n  \"failure\": " << std::quoted(failure_)
            << ",\n  \"last_rejection\": " << std::quoted(last_reject_) << "\n}\n";
    if (!result_path_.empty()) {
      std::ofstream sequence(result_path_ + ".model-sequence.json");
      sequence << '[';
      bool first = true;
      for (const auto& entry : run_acceptance_.models()) {
        if (!first) sequence << ',';
        first = false;
        sequence << "{\"model\":" << std::quoted(entry.model) << ",\"epoch\":" << entry.epoch
                 << '}';
      }
      sequence << "]\n";
    }
    if (!result_path_.empty()) {
      std::ofstream result(result_path_);
      result << summary.str();
    }
    RCLCPP_INFO(get_logger(),
                "NATIVE_RESULT success=%s active_seconds=%.3f policy_writes=%lu sdk_writes=%lu "
                "accepted=%lu rejected=%lu modes=%d skipped=%lu failure=%s",
                passed ? "true" : "false", active_seconds_, active_writes_, sdk_writes_, accepted_,
                rejected_, completed_modes_, skipped_periods_, failure_.c_str());
    success = passed;
    completed = true;
  }
  void run() {
    bool natural_completion = false;
    auto next = Clock::now();
    while (!stopping_ && rclcpp::ok()) {
      std::this_thread::sleep_until(next);
      const auto now = Clock::now();
      max_lateness_ms_ =
          std::max(max_lateness_ms_, std::chrono::duration<double, std::milli>(now - next).count());
      next += 2ms;
      while (next <= now) {
        next += 2ms;
        ++skipped_periods_;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
          tick(now);
        } catch (const std::exception& error) {
          stop_local(error.what(), true);
        }
      }
      if (std::chrono::duration<double>(now - started_).count() >= duration_) {
        natural_completion = true;
        break;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finish(natural_completion);
    }
    if (rclcpp::ok()) rclcpp::shutdown();
  }
};
}  // namespace anakin::env
int main(int argc, char** argv) {
  try {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<anakin::env::NativeNode>();
    rclcpp::spin(node);
    return node->completed ? (node->success ? 0 : 1) : 130;
  } catch (const std::exception& error) {
    std::cerr << "anakin_env_native: " << error.what() << '\n';
    if (rclcpp::ok()) rclcpp::shutdown();
    return 1;
  }
}
