#pragma once
#include <anakin_env/gate.hpp>
#include <optional>
#include <string>
#include <utility>

namespace anakin::env {
// Sampled admission only, not a dynamics/effort bound or an atomic device revoke.
struct SendSnapshot {
  bool position_authorized = false, damping_allowed = false, stop_requested = false;
  bool state_available = false, feedback_integrity_ok = true, remote_valid = false,
       target_authorized = false;
  uint64_t authorization_generation = 0, feedback_generation = 0;
  Clock::time_point state_acquired{}, remote_acquired{}, target_deadline{}, damping_deadline{};
  bool require_primary_input = true, release_allowed = false;
  std::chrono::milliseconds state_timeout{40}, input_timeout{250};
};
struct SendRequest {
  bool damping = false, has_target = false;
  uint64_t authorization_generation = 0;
  Clock::time_point deadline = Clock::time_point::max();
  bool release = false;
};
struct EncodedFeedback {
  uint64_t generation;
  Clock::time_point acquired;
};
template <class Command>
struct PreparedSend {
  Command command;
  // Only an encoder that evaluated a particular feedback sample binds it here.
  std::optional<EncodedFeedback> feedback;
};
enum class SendStatus {
  Written,
  DeadlineExpired,
  DampingExpired,
  Revoked,
  RemoteStop,
  StateStale,
  RemoteStale,
  TargetExpired,
  FeedbackInvalid,
  FeedbackChanged,
  EncodingFailure,
  TransportRejected,
  TransportFailure
};
enum class SendPhase { BeforeEncoding, Encoding, AfterEncoding, Transport };
struct SendResult {
  SendStatus status;
  SendPhase phase;
  Clock::time_point completed_at;
  std::string detail;
  bool written() const { return status == SendStatus::Written; }
};
inline std::optional<SendStatus> send_admission(const SendRequest& request, const SendSnapshot& s,
                                                Clock::time_point now) {
  if (now >= request.deadline) return SendStatus::DeadlineExpired;
  if (s.authorization_generation != request.authorization_generation) return SendStatus::Revoked;
  if (request.release) return s.release_allowed ? std::nullopt : std::optional<SendStatus>(SendStatus::Revoked);
  if (request.damping) {
    if (!s.position_authorized && (!s.damping_allowed || now >= s.damping_deadline))
      return SendStatus::DampingExpired;
    return std::nullopt;
  }
  if (!s.position_authorized) return SendStatus::Revoked;
  if (!s.feedback_integrity_ok) return SendStatus::FeedbackInvalid;
  if (!s.state_available || now < s.state_acquired ||
      now - s.state_acquired > s.state_timeout)
    return SendStatus::StateStale;
  if (s.require_primary_input && (!s.remote_valid || now < s.remote_acquired ||
      now - s.remote_acquired > s.input_timeout))
    return SendStatus::RemoteStale;
  if (s.stop_requested) return SendStatus::RemoteStop;
  if (request.has_target && (!s.target_authorized || now >= s.target_deadline))
    return SendStatus::TargetExpired;
  return std::nullopt;
}
// NativeNode uses this exact orchestration. Tests replace only clock, observation,
// encoding and transport; no second test-only implementation of the send gate.
// The caller serializes control/transaction mutations. A successful invocation is
// a writer receipt, not actuator execution, and cannot retract an in-flight Write.
template <class Command, class Now, class Observe, class Encode, class Write>
SendResult guarded_send(const SendRequest& request, Now&& now, Observe&& observe, Encode&& encode,
                        Write&& write) {
  const auto before = observe();
  const auto t0 = now();
  if (const auto rejected = send_admission(request, before, t0))
    return {*rejected, SendPhase::BeforeEncoding, t0, {}};
  PreparedSend<Command> prepared;
  try {
    prepared = encode(before);
  } catch (const std::exception& error) {
    return {SendStatus::EncodingFailure, SendPhase::Encoding, now(), error.what()};
  }
  const auto after = observe();
  const auto t1 = now();
  if (const auto rejected = send_admission(request, after, t1))
    return {*rejected, SendPhase::AfterEncoding, t1, {}};
  if (prepared.feedback) {
    const auto& used = *prepared.feedback;
    if (!after.state_available || t1 < used.acquired ||
        t1 - used.acquired > after.state_timeout)
      return {SendStatus::StateStale, SendPhase::AfterEncoding, t1, "Encoded feedback expired"};
    if (after.feedback_generation != used.generation || after.state_acquired != used.acquired)
      return {SendStatus::FeedbackChanged, SendPhase::AfterEncoding, t1,
              "Feedback changed; discard and retry on a later slot"};
  }
  try {
    if (!write(prepared.command))
      return {SendStatus::TransportRejected, SendPhase::Transport, now(), {}};
  } catch (const std::exception& error) {
    return {SendStatus::TransportFailure, SendPhase::Transport, now(), error.what()};
  }
  return {SendStatus::Written, SendPhase::Transport, now(), {}};
}
struct PeriodicSlot {
  bool due = false;
  uint64_t skipped = 0;
  Clock::duration lateness{};
};
class PeriodicSendSchedule {
 public:
  explicit PeriodicSendSchedule(Clock::time_point start) : next_(start) {}
  Clock::time_point next() const { return next_; }
  PeriodicSlot consume(Clock::time_point now) {
    if (now < next_) return {};
    PeriodicSlot slot;
    slot.due = true;
    slot.lateness = now - next_;
    next_ += std::chrono::milliseconds(2);
    while (next_ <= now) {
      next_ += std::chrono::milliseconds(2);
      ++slot.skipped;
    }
    return slot;
  }

 private:
  Clock::time_point next_;
};
}  // namespace anakin::env
