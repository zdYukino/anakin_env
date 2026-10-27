#pragma once
#include <anakin_env/gate.hpp>
#include <anakin_sdk/msg/inference_status.hpp>
namespace anakin::env {
// Both ROS callbacks feed this cumulative receipt; neither arrival order nor a skipped
// first ACTIVE sample can postpone confirmation of a later valid ACTIVE target.
class ActiveConfirmation {
 public:
  void begin(const Uuid& session, const Uuid& instance, const Uuid& request, uint64_t epoch) {
    session_ = session;
    instance_ = instance;
    request_ = request;
    epoch_ = epoch;
    first_active_ = 0;
    received_active_ = 0;
    failed_ = false;
  }
  void status(const anakin_sdk::msg::InferenceStatus& s) {
    if (!matches(s.session_id, s.mc_instance_id, s.request_id, s.epoch)) return;
    if (s.state == anakin_sdk::msg::InferenceStatus::FAILED) {
      failed_ = true;
      return;
    }
    if (s.accepted && (s.published_phase_mask & Target::TARGET_ACTIVE) &&
        s.phase_first_seq[2] > 0) {
      if (!first_active_ || s.phase_first_seq[2] < first_active_)
        first_active_ = s.phase_first_seq[2];
    }
  }
  void target(const Target& t) {
    if (matches(t.session_id, t.mc_instance_id, t.request_id, t.epoch) &&
        t.phase == Target::TARGET_ACTIVE)
      received_active_ = std::max(received_active_, t.seq);
  }
  bool active() const { return !failed_ && first_active_ > 0 && received_active_ >= first_active_; }

 private:
  bool matches(const Uuid& s, const Uuid& i, const Uuid& r, uint64_t e) const {
    return s == session_ && i == instance_ && r == request_ && e == epoch_;
  }
  Uuid session_{}, instance_{}, request_{};
  uint64_t epoch_ = 0, first_active_ = 0, received_active_ = 0;
  bool failed_ = false;
};
class TransactionHealth {
 public:
  void begin(Clock::time_point deadline) {
    deadline_ = deadline;
    had_target_ = false;
    failed_ = false;
  }
  void valid_target() { had_target_ = true; }
  void fail() { failed_ = true; }
  void tick(Clock::time_point now, bool live, bool active) {
    if ((had_target_ && !live) || (now >= deadline_ && !active)) failed_ = true;
  }
  bool failed() const { return failed_; }
  bool finish(bool live, bool active) const { return !failed_ && had_target_ && live && active; }

 private:
  Clock::time_point deadline_{};
  bool had_target_ = false, failed_ = false;
};
}  // namespace anakin::env
