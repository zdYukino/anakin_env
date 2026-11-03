#include <anakin_env/runtime.hpp>
#include <anakin_env/control.hpp>
#include <cmath>

namespace anakin::env {
namespace {
uint8_t wire_mode(State state) {
  switch (state) {
    case State::Disarmed: return Frame::MODE_DISARMED;
    case State::Damping: return Frame::MODE_DAMPING;
    case State::Align: return Frame::MODE_ALIGNING;
    case State::Hold: return Frame::MODE_HOLD;
    case State::Preparing: return Frame::MODE_PREPARING;
    case State::Fault: return Frame::MODE_FAULT;
    case State::AsapLoco: case State::AsapCr7: return Frame::MODE_ACTIVE;
  }
  throw std::invalid_argument("Unknown state");
}
}  // namespace

RuntimeController::RuntimeController(ModeConfig config, RuntimeBackend& backend, Inputs inputs, Now now)
    : mode_(std::move(config)), backend_(backend), inputs_(std::move(inputs)), now_(std::move(now)),
      machine_(mode_), plan_(machine_.startup_plan()) {
  authority_ready_ = mode_.mode == RuntimeMode::Sim && backend_.authority_owned();
  entered_ = last_tick_ = now_();
  execute(plan_);
}
bool RuntimeController::fresh(Clock::time_point now) const {
  return feedback_ && now >= feedback_->acquired &&
         now - feedback_->acquired <= mode_.guards.state_timeout;
}
bool RuntimeController::input_fresh(const InputSnapshot& input) const {
  const auto now = now_();
  return input.primary_online && now >= input.primary_acquired &&
         now - input.primary_acquired <= mode_.guards.input_timeout;
}
bool RuntimeController::permitted_input(const InputSnapshot& input) const {
  return !mode_.guards.require_primary_input || input_fresh(input);
}
bool RuntimeController::measured_valid() const {
  if (!feedback_) return false;
  const auto& frame = feedback_->frame;
  if ((frame.validity & 3) != 3 || frame.joints.name != joint_names() ||
      frame.joints.position.size() != 29 || frame.joints.velocity.size() != 29) return false;
  for (size_t i = 0; i < 29; ++i) {
    const auto q = frame.joints.position[i];
    if (!std::isfinite(q) || !std::isfinite(frame.joints.velocity[i])) return false;
  }
  const auto& q = frame.imu.orientation;
  const auto norm = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
  if (!std::isfinite(norm) || std::abs(norm - 1.) > .001) return false;
  return true;
}
bool RuntimeController::actual_valid() const {
  if (!measured_valid()) return false;
  const auto& frame = feedback_->frame;
  if (mode_.guards.enforce_measured_joint_limits)
    for (size_t i = 0; i < 29; ++i)
      if (frame.joints.position[i] < lower_limits()[i] || frame.joints.position[i] > upper_limits()[i]) return false;
  const auto& q = frame.imu.orientation;
  if (!mode_.guards.allow_fallen &&
      std::acos(std::clamp(1. - 2. * (q.x*q.x + q.y*q.y), -1., 1.)) > mode_.guards.max_tilt_radians)
    return false;
  if (!mode_.guards.allow_fallen && (frame.validity & Frame::VALID_ODOMETRY) &&
      frame.odometry.pose.pose.position.z < mode_.guards.min_base_height) return false;
  return true;
}
void RuntimeController::feedback(const NativeSample& sample) {
  if (feedback_ && (sample.generation <= feedback_->generation || sample.acquired < feedback_->acquired)) return;
  if (feedback_ && sample.frame.sdk_tick < feedback_->frame.sdk_tick)
    rollback_fault_ = mode_.guards.fault_on_tick_rollback;
  feedback_ = sample;
}
void RuntimeController::malformed_transport_packet() {
  if (mode_.mode == RuntimeMode::Real || mode_.real_checks)
    feedback_fault("Malformed native feedback or remote packet");
}
void RuntimeController::feedback_fault(const std::string& reason) {
  diagnostic_ = reason;
  pending_fault_ = true;
}
TransactionIdentity RuntimeController::identity() const {
  TransactionIdentity result;
  if (request_) {
    result.session = session_.uuid;
    result.instance = mc_instance_.uuid;
    result.request = request_->request_id.uuid;
    result.epoch = request_->next_epoch;
  }
  return result;
}
void RuntimeController::status(const RuntimeStatus& status) {
  auto route = router_.observe(status);
  if (route == NativeStatusRouter::Decision::InstanceChanged) {
    if (request_) feedback_fault("MC instance changed; explicit fault clear required");
    if (zero(status.session_id) && zero(status.request_id) && status.epoch == 0 && !status.accepted) {
      router_ = NativeStatusRouter(session_);
      route = router_.observe(status);
    }
  }
  if (route == NativeStatusRouter::Decision::Discovered) {
    mc_instance_ = router_.instance();
  } else if (route == NativeStatusRouter::Decision::ContractMismatch) {
    feedback_fault("MC status contract mismatch");
  } else if (route == NativeStatusRouter::Decision::Transaction) {
    status_ = status;
    confirmation_.status(status);
    if (status.state == RuntimeStatus::FAILED) feedback_fault("MC transaction failed: " + status.detail);
  }
}
bool RuntimeController::target(const Target& target) {
  if (!request_ || !plan_.inference_allowed || pending_fault_ || stopped_) return false;
  std::string reason;
  if (!gate_.accept(target, now_(), reason)) { diagnostic_ = "Target rejected: " + reason; return false; }
  target_ = target;
  handover_target_.reset();
  handover_deadline_ = {};
  confirmation_.target(target);
  return true;
}
void RuntimeController::request_result(const Uuid& request, bool accepted) {
  if (!request_ || request != request_->request_id) return;
  request_accepted_ = accepted;
  if (!accepted) feedback_fault("MC rejected current transaction");
}
void RuntimeController::release_command() {
  if (!wrote_) return;
  if (!backend_.authority_owned()) { wrote_ = false; return; }
  if (!send(true)) throw std::runtime_error("Zero-gain release write failed");
  wrote_ = false;
}
void RuntimeController::revoke(bool preserve_handover) {
  if (acquisition_) backend_.cancel_acquire(*acquisition_);
  acquisition_.reset();
  alignment_started_.reset();
  alignment_pending_ = false;
  ++authorization_;
  gate_.stop();
  router_.revoke();
  target_.reset();
  last_written_target_.reset();
  last_written_deadline_ = {};
  if (!preserve_handover) {
    handover_target_.reset();
    handover_deadline_ = {};
  }
  request_.reset();
  status_.reset();
  need_policy_ = false;
  published_seq_ = 0;
  published_mask_ = 0;
  published_deadline_ = {};
}
void RuntimeController::start_policy() {
  if (!need_policy_ || zero(mc_instance_) || !plan_.inference_allowed) return;
  RuntimeRequest request;
  request.session_id = session_;
  request.expected_mc_instance_id = mc_instance_;
  request.request_id = new_uuid();
  request.next_epoch = ++epoch_;
  request.source_state_seq = state_seq_ + 1;
  request.schema_id = schema_id;
  request.target_model = to_string(*plan_.model);
  request.gain_profile = merged_gain_profile;
  request.execution_profile = sdk_pd_execution_profile;
  request.command_semantics = pd_reference;
  request.transition_profile = plan_.before.effective_state == State::Hold &&
      request.target_model == "asap_loco" ? "g1_startup_v1" : "g1_direct_v1";
  request.allow_source_continuation = true;
  request.timeout.sec = static_cast<int32_t>(mode_.policy_timeout.count() / 1000);
  request.timeout.nanosec = static_cast<uint32_t>(mode_.policy_timeout.count() % 1000) * 1000000;
  request_ = request;
  router_.bind(request, true);
  gate_.begin(session_, mc_instance_, request.request_id, epoch_, request.target_model, true,
              merged_gain_profile, sdk_pd_execution_profile, pd_reference);
  confirmation_.begin(session_, mc_instance_, request.request_id, epoch_);
  if (!machine_.bind_transaction(machine_.snapshot().generation, identity()))
    throw std::runtime_error("Policy transaction no longer matches Preparing generation");
  request_accepted_ = false;
  need_policy_ = false;
}
void RuntimeController::execute(const ActionPlan& plan) {
  if (plan.decision == Decision::Accepted && plan.before.state != plan.after.state) entered_ = now_();
  bool preserve_handover = false;
  if (plan.decision == Decision::Accepted && plan.after.state == State::Preparing &&
      plan.before.state != State::Preparing && plan.before.producing_model && request_ &&
      last_written_target_ && last_written_target_->request_id == request_->request_id &&
      last_written_target_->epoch == request_->next_epoch && now_() < last_written_deadline_) {
    handover_target_ = *last_written_target_;
    handover_deadline_ = last_written_deadline_;
    preserve_handover = true;
  }
  for (const auto& action : plan.actions) {
    try {
      switch (action.action) {
        case Action::RevokeActuation: revoke(preserve_handover); break;
        case Action::ReleaseIfOwned:
          release_command();
          if (backend_.authority_owned()) backend_.release_authority();
          authority_ready_ = mode_.mode == RuntimeMode::Sim && backend_.authority_owned();
          break;
        case Action::AcquireAuthority:
          if (!authority_ready_ || !backend_.authority_owned()) {
            acquisition_ = authorization_;
            acquisition_deadline_ = now_() + mode_.authority_timeout;
            backend_.begin_acquire(*acquisition_, mode_.authority_timeout);
          }
          break;
        case Action::BeginAlignment: alignment_pending_ = true; break;
        case Action::RequestPolicy: {
          need_policy_ = true;
          requested_ = now_();
          const auto fallback = *mode_.states.at(State::Preparing).preparing_fallback;
          preparing_fallback_ = plan.before.effective_state == State::Hold ? fallback.from_hold : fallback.otherwise;
          break;
        }
        case Action::ZeroMotion: break;  // InputHub already zeroed the shared command.
      }
    } catch (const std::exception& error) {
      feedback_fault(error.what());
      break;  // A failed action never authorizes later actions or outputs.
    }
  }
}

bool RuntimeController::manual_entry_allowed(const Event& event, const InputSnapshot& input) const {
  return event.kind != EventKind::ManualRequest || !event.requested_state ||
      *event.requested_state == State::Disarmed || *event.requested_state == State::Damping ||
      ((!mode_.guards.strict_deadlines || (fresh(now_()) && actual_valid())) && permitted_input(input));
}

bool RuntimeController::action_available(InputAction action, const InputSnapshot& input) const {
  if (stopped_) return false;
  if (const auto event = input_event(action)) {
    if (action == InputAction::EmergencyStop) return true;
    if (pending_fault_ || stop_requested(input) || !manual_entry_allowed(*event, input)) return false;
    auto probe = machine_;
    const auto decision = probe.dispatch({*event});
    return decision.cause == event->kind &&
        (decision.decision == Decision::Accepted || decision.decision == Decision::Idempotent);
  }
  return !pending_fault_ && !stop_requested(input) && input.primary_online && machine_.snapshot().state != State::Fault;
}

ActionPlan RuntimeController::tick(const InputFrame& input) {
  const auto now = now_();
  auto events = input.events;
  const auto before = machine_.snapshot();
  const bool valid = fresh(now) && actual_valid();
  if (stopped_) return plan_;
  bool fault_needed = pending_fault_ || (plan_.outputs.lowcmd && authority_ready_ &&
      ((mode_.guards.strict_deadlines && !valid) || rollback_fault_));
  if (mode_.guards.strict_deadlines && plan_.inference_allowed &&
      ((request_ && target_ && !gate_.live(now)) ||
       (before.state == State::Preparing && now - requested_ >= mode_.policy_timeout)))
    fault_needed = true;
  if (fault_needed) events.push_back(Event::system(EventKind::Fault));
  // Apply configurable real entry guards before the state engine can acquire.
  events.erase(std::remove_if(events.begin(), events.end(), [&](const Event& event) {
    return !manual_entry_allowed(event, input.snapshot);
  }), events.end());
  if (before.state == State::Align && alignment_started_ && valid && feedback_was_fresh_)
    alignment_elapsed_ += std::chrono::duration<double>(now - last_tick_).count();
  if (before.state == State::Align && alignment_started_ && valid && alignment_elapsed_ >= mode_.alignment_seconds)
    events.push_back(Event::completion(EventKind::AlignmentComplete, before.generation));
  if (before.state == State::Damping && now - entered_ >= std::chrono::duration<double>(mode_.damping_seconds))
    events.push_back(Event::completion(EventKind::DampingComplete, before.generation));
  if (before.state == State::Preparing && request_ && target_ && gate_.live(now) &&
      target_->phase == Target::TARGET_ACTIVE && target_->model_id == request_->target_model &&
      (published_mask_ & Target::TARGET_ACTIVE) && published_seq_ > 0 && now < published_deadline_ && confirmation_.active())
    events.push_back(Event::policy(EventKind::PolicyReady, before.generation, identity(), *plan_.model));
  if (before.state == State::AsapCr7 && request_ && status_ && status_->motion_done &&
      status_->active_model == "asap_cr7" && status_->target_active && confirmation_.active())
    events.push_back(Event::policy(EventKind::MotionDone, before.generation, identity(), PolicyModel::AsapCr7));
  auto decision = machine_.dispatch(events);
  if (decision.decision == Decision::Accepted) {
    if (decision.cause == EventKind::MotionDone &&
        decision.after.requested_state == State::AsapLoco) {
      return_motion_inhibited_ = true;
      return_neutral_seen_ = false;
    } else if (decision.cause == EventKind::ManualRequest ||
               decision.cause == EventKind::EmergencyStop || decision.cause == EventKind::Fault) {
      return_motion_inhibited_ = false;
      return_neutral_seen_ = false;
    }
  }
  if (return_motion_inhibited_ && input.snapshot.primary_online && input.snapshot.moving) {
    const bool neutral = std::all_of(input.snapshot.velocity.begin(), input.snapshot.velocity.end(),
                                     [](double velocity) { return std::abs(velocity) < 1e-6; });
    if (neutral) return_neutral_seen_ = true;
    else if (return_neutral_seen_ && machine_.snapshot().state == State::AsapLoco)
      return_motion_inhibited_ = false;
  }
  // Keep the original transition context while waiting for delayed MC discovery.
  if (decision.decision == Decision::Accepted) plan_ = decision;
  else {
    plan_.outputs = decision.outputs;
    plan_.inference_allowed = decision.inference_allowed;
  }
  pending_fault_ = fault_needed && decision.after.state != State::Fault;
  rollback_fault_ = false;
  execute(decision);
  if (acquisition_) {
    const auto generation = *acquisition_;
    const auto progress = backend_.poll_acquire(generation);
    if (progress == AuthorityProgress::Failed || now >= acquisition_deadline_) {
      backend_.cancel_acquire(generation);
      acquisition_.reset();
      feedback_fault("Authority acquisition failed or timed out: " + backend_.authority_diagnostic());
    } else if (progress == AuthorityProgress::Owned) {
      authority_ready_ = backend_.authority_owned();
      acquisition_.reset();
    }
  }
  if (alignment_pending_ && machine_.snapshot().state == State::Align &&
      authority_ready_ && valid && !pending_fault_) {
    std::copy_n(feedback_->frame.joints.position.begin(), 29, alignment_source_.begin());
    alignment_elapsed_ = 0.;
    alignment_started_ = now;
    alignment_pending_ = false;
  }
  try {
    start_policy();
    if (pending_fault_ || !send()) release_command();
  } catch (const std::exception& error) { feedback_fault(error.what()); }
  feedback_was_fresh_ = valid;
  last_tick_ = now;
  return decision;
}

Frame RuntimeController::make_frame(const InputSnapshot& input) const {
  auto frame = feedback_->frame;
  frame.session_id = session_;
  frame.env_instance_id = env_instance_;
  frame.epoch = epoch_;
  frame.state_seq = state_seq_;
  frame.gain_profile = merged_gain_profile;
  frame.execution_profile = sdk_pd_execution_profile;
  frame.command_semantics = pd_reference;
  frame.execution_mode = wire_mode(machine_.snapshot().state);
  // In Preparing this explicitly permits MC warmup, not a motor writer call.
  frame.execution_authorized = plan_.inference_allowed && actual_valid() && !pending_fault_;
  if (!diagnostic_.empty()) {
    diagnostic_msgs::msg::DiagnosticStatus diagnostic;
    diagnostic.name = "anakin_env/runtime";
    diagnostic.level = machine_.snapshot().state == State::Fault ? diagnostic.ERROR : diagnostic.WARN;
    diagnostic.message = diagnostic_;
    frame.diagnostics.status.push_back(diagnostic);
  }
  if (!actual_valid()) {
    diagnostic_msgs::msg::DiagnosticStatus diagnostic;
    diagnostic.name = "anakin_env/actuation_guards";
    diagnostic.level = diagnostic.WARN;
    diagnostic.message = "Measured telemetry is valid; actuation posture/joint guard is not satisfied";
    frame.diagnostics.status.push_back(diagnostic);
  }
  frame.command_base_height = input.height;
  const bool motion_allowed = !return_motion_inhibited_ && input_fresh(input) && input.moving;
  frame.command_stand = motion_allowed;
  frame.command.header = frame.header;
  frame.command.twist.linear.x = motion_allowed ? input.velocity[0] : 0.;
  frame.command.twist.linear.y = motion_allowed ? input.velocity[1] : 0.;
  frame.command.twist.angular.z = motion_allowed ? input.velocity[2] : 0.;
  frame.validity |= Frame::VALID_COMMAND; // A lost or stale primary yields a neutral command.
  frame.mc_instance_seen = mc_instance_;
  if (request_) {
    frame.request_id_seen = request_->request_id;
    frame.target_seq_seen = gate_.accepted_seq();
    frame.phase_mask_seen = gate_.phase_mask();
    frame.sdk_published_mc_instance_id = mc_instance_;
    frame.sdk_published_request_id = request_->request_id;
    frame.sdk_published_epoch = epoch_;
    frame.sdk_published_target_seq = published_seq_;
    frame.sdk_published_phase_mask = published_mask_;
  }
  frame.sdk_publish_count = writer_count_;
  return frame;
}
std::optional<Frame> RuntimeController::take_frame(const InputSnapshot& input) {
  if (stopped_ || !plan_.outputs.ros_state || !fresh(now_()) || !measured_valid() ||
      feedback_->generation == published_generation_) return std::nullopt;
  published_generation_ = feedback_->generation;
  ++state_seq_;
  auto frame = make_frame(input);
  gate_.state(frame, feedback_->acquired);
  return frame;
}
bool RuntimeController::send(bool release) {
  if (!feedback_ || (!release && (!plan_.outputs.lowcmd || pending_fault_ || stopped_))) return false;
  auto source = plan_.command;
  bool handover = false;
  if (!release && source == CommandSource::Policy && (!request_ || !target_ || !gate_.live(now_()))) {
    if (machine_.snapshot().state != State::Preparing) return false;
    handover = handover_target_ && now_() < handover_deadline_;
    if (!handover) source = preparing_fallback_;
  }
  const bool damping = !release && source == CommandSource::Damping;
  const bool policy = !release && source == CommandSource::Policy;
  if (!release && source == CommandSource::None) return false;
  if (!release && source == CommandSource::StandInterpolation && !alignment_started_) return false;
  const auto request = SendRequest{damping, policy, authorization_, Clock::time_point::max(), release};
  const auto observe = [&] {
    const auto input = inputs_();
    SendSnapshot snapshot;
    snapshot.authorization_generation = authorization_;
    snapshot.position_authorized = authority_ready_ && backend_.authority_owned() && !stopped_;
    snapshot.damping_allowed = snapshot.position_authorized;
    snapshot.damping_deadline = now_() + mode_.guards.state_timeout;
    snapshot.release_allowed = backend_.authority_owned() && wrote_;
    snapshot.feedback_integrity_ok = actual_valid() && !pending_fault_;
    snapshot.state_available = feedback_.has_value();
    snapshot.state_acquired = feedback_->acquired;
    snapshot.feedback_generation = feedback_->generation;
    // The primary was required for manual entry; a later disconnect must not release actuation.
    snapshot.require_primary_input = false;
    snapshot.state_timeout = mode_.guards.state_timeout;
    snapshot.input_timeout = mode_.guards.input_timeout;
    snapshot.remote_valid = input.primary_online;
    snapshot.remote_acquired = input.primary_acquired;
    snapshot.stop_requested = stop_requested(input);
    snapshot.target_authorized = handover ? handover_target_ &&
        machine_.snapshot().state == State::Preparing && now_() < handover_deadline_ : gate_.live(now_());
    snapshot.target_deadline = handover ? handover_deadline_ : gate_.deadline();
    return snapshot;
  };
  const auto encode = [&](const SendSnapshot&) {
    LowCommand command;
    if (release) command = encode_release(feedback_->mode_machine);
    else if (damping) command = encode_damping(feedback_->mode_machine);
    else {
      Positions q = stand_pose();
      std::string model = "asap_loco";
      if (policy) {
        const auto& chosen = handover ? *handover_target_ : *target_;
        std::copy_n(chosen.target.points[0].positions.begin(), 29, q.begin());
        model = chosen.model_id;
      } else if (source == CommandSource::StandInterpolation) {
        const auto alpha = std::clamp(alignment_elapsed_ / mode_.alignment_seconds, 0., 1.);
        for (size_t i = 0; i < q.size(); ++i) q[i] = (1. - alpha) * alignment_source_[i] + alpha * q[i];
      }
      command = encode_policy_pd(q, feedback_->mode_machine, merged_gain_profile, model);
    }
    return PreparedSend<LowCommand>{command, std::nullopt};
  };
  const auto result = guarded_send<LowCommand>(request, now_, observe, encode, [&](const LowCommand& command) {
    LowCommand packet = command;
    finalize_command(packet);
    return backend_.write(packet);
  });
  if (!result.written()) {
    if (result.status == SendStatus::TransportFailure || result.status == SendStatus::TransportRejected ||
        result.status == SendStatus::EncodingFailure) feedback_fault("SDK send failure: " + result.detail);
    return false;
  }
  ++writer_count_;
  if (!release) wrote_ = true;
  if (policy && !handover) {
    published_seq_ = target_->seq;
    published_mask_ |= target_->phase;
    published_deadline_ = gate_.deadline();
    if (!last_written_target_ || last_written_target_->seq != target_->seq)
      last_written_target_ = *target_;
    last_written_deadline_ = gate_.deadline();
  }
  return true;
}
void RuntimeController::shutdown() {
  if (stopped_) return;
  stopped_ = true;
  revoke();
  release_command();
  if (backend_.authority_owned()) backend_.release_authority();
  authority_ready_ = false;
}
}  // namespace anakin::env
