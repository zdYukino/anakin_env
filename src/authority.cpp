#include <anakin_env/authority.hpp>
#include <algorithm>

namespace anakin::env {
struct AsyncAuthority::Job {
  AuthorityRecord record;
  Clock::time_point deadline;
};

AsyncAuthority::AsyncAuthority(std::unique_ptr<MotionSwitcherApi> api, AuthorityOptions options, Now now)
    : api_(std::move(api)), options_(options), now_(std::move(now)) {
  if (!api_ || options_.rpc_timeout.count() <= 0 || options_.rpc_timeout > std::chrono::seconds(1) ||
      options_.retry_interval.count() <= 0 || options_.retry_interval > std::chrono::seconds(1))
    throw std::invalid_argument("Authority requires an API and bounded positive RPC/retry timings");
  worker_ = std::thread([this] { run(); });
}
AsyncAuthority::~AsyncAuthority() { shutdown(); }

void AsyncAuthority::begin(uint64_t generation, std::chrono::milliseconds timeout) {
  if (!generation || timeout.count() <= 0 || timeout > std::chrono::seconds(30))
    throw std::invalid_argument("Invalid authority generation/deadline");
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) throw std::logic_error("Authority worker is stopped");
  if (records_.count(generation)) {
    if (latest_ == generation && (records_.at(generation)->record.state == AuthorityState::Pending ||
        records_.at(generation)->record.state == AuthorityState::Owned)) return;
    throw std::invalid_argument("A cancelled/completed authority generation cannot be reused");
  }
  if (generation <= latest_)
    throw std::invalid_argument("Authority generation must advance, including after diagnostic pruning");
  for (auto& item : records_) {
    auto& record = item.second->record;
    if (record.state == AuthorityState::Pending || record.state == AuthorityState::Owned) {
      record.state = AuthorityState::Cancelled;
      record.detail = "Superseded; local permission revoked";
    }
  }
  owner_ = 0;
  auto job = std::make_shared<Job>();
  job->record.generation = generation;
  job->record.state = AuthorityState::Pending;
  job->record.detail = "Explicit acquisition queued";
  job->deadline = now_() + timeout; // Queue time counts; a blocked old RPC cannot renew this.
  records_[generation] = job;
  queued_ = job;
  latest_ = generation;
  while (records_.size() > 32) records_.erase(records_.begin());
  condition_.notify_all();
}
AuthorityProgress AsyncAuthority::poll(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(generation);
  if (found == records_.end() || generation != latest_) return AuthorityProgress::Failed;
  auto& record = found->second->record;
  if (record.state == AuthorityState::Pending && now_() >= found->second->deadline) {
    record.state = AuthorityState::Failed;
    record.detail = "Overall authority deadline expired; local permission revoked";
    condition_.notify_all();
  }
  if (record.state == AuthorityState::Owned && owner_ == generation) return AuthorityProgress::Owned;
  return record.state == AuthorityState::Pending ? AuthorityProgress::Pending : AuthorityProgress::Failed;
}
void AsyncAuthority::cancel(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(generation);
  if (found == records_.end()) return;
  auto& record = found->second->record;
  if (record.state == AuthorityState::Pending || record.state == AuthorityState::Owned) {
    record.state = AuthorityState::Cancelled;
    record.detail = "Cancelled; local permission revoked";
  }
  if (owner_ == generation) owner_ = 0;
  condition_.notify_all();
}
void AsyncAuthority::release_local() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& item : records_) {
    auto& record = item.second->record;
    if (record.state == AuthorityState::Owned || record.state == AuthorityState::Pending) {
      record.state = AuthorityState::Released;
      record.detail = "Local authority released; factory mode was not activated";
    }
  }
  owner_ = 0;
  condition_.notify_all();
}
bool AsyncAuthority::owned() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && owner_ != 0;
}
AuthorityRecord AsyncAuthority::record(uint64_t generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!generation) generation = latest_;
  const auto found = records_.find(generation);
  return found == records_.end() ? AuthorityRecord{} : found->second->record;
}
bool AsyncAuthority::current(const std::shared_ptr<Job>& job) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !stopping_ && latest_ == job->record.generation && job->record.state == AuthorityState::Pending;
}
float AsyncAuthority::rpc_budget(const std::shared_ptr<Job>& job) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || latest_ != job->record.generation || job->record.state != AuthorityState::Pending) return 0.f;
  const auto remaining = job->deadline - now_();
  if (remaining <= std::chrono::milliseconds(1)) {
    job->record.state = AuthorityState::Failed;
    job->record.detail = "Overall authority deadline exhausted before RPC";
    return 0.f;
  }
  return static_cast<float>(std::chrono::duration<double>(
      std::min(remaining, std::chrono::duration_cast<Clock::duration>(options_.rpc_timeout))).count());
}
void AsyncAuthority::failed(const std::shared_ptr<Job>& job, const std::string& detail) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (job->record.state == AuthorityState::Pending) {
    job->record.state = AuthorityState::Failed;
    job->record.detail = detail;
  } else {
    job->record.detail += "; " + detail;
  }
  if (owner_ == job->record.generation) owner_ = 0;
}
void AsyncAuthority::acquire(const std::shared_ptr<Job>& job) {
  try {
    auto budget = rpc_budget(job);
    if (budget <= 0) return;
    api_->initialize(budget);
    while (current(job)) {
      budget = rpc_budget(job);
      if (budget <= 0) return;
      std::string form, name;
      const auto checked = api_->check_mode(form, name, budget);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        job->record.check_code = checked;
        if (checked == 0 && name.empty()) {
          job->record.observed_empty = true;
          job->record.outcome_unknown = false;
          if (!stopping_ && latest_ == job->record.generation &&
              job->record.state == AuthorityState::Pending && now_() < job->deadline) {
            owner_ = job->record.generation;
            job->record.state = AuthorityState::Owned;
            job->record.detail = "CheckMode returned 0 with empty mode; current generation owns local authority";
          } else {
            if (job->record.state == AuthorityState::Pending) job->record.state = AuthorityState::Failed;
            job->record.detail += "; late empty-mode result did not grant local permission";
          }
          return;
        }
      }
      if (checked != 0) {
        failed(job, "CheckMode failed code=" + std::to_string(checked) + "; empty output is not success");
        return;
      }
      budget = rpc_budget(job);
      if (budget <= 0) return;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || latest_ != job->record.generation || job->record.state != AuthorityState::Pending) return;
        // RPC commit point. Cancellation can revoke permission while the SDK call
        // is in flight; its eventual side-effect/outcome remains recorded.
        job->record.release_attempted = true;
        job->record.outcome_unknown = true;
        job->record.detail = "Explicit ReleaseMode in flight; local authority not yet granted";
      }
      const auto released = api_->release_mode(budget);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        job->record.release_code = released;
        job->record.release_succeeded = job->record.release_succeeded || released == 0;
        job->record.detail += "; ReleaseMode returned " + std::to_string(released);
      }
      if (released != 0) {
        failed(job, "ReleaseMode failed code=" + std::to_string(released) + "; remote outcome may be partial/unknown");
        return;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait_for(lock, options_.retry_interval, [&] {
        return stopping_ || latest_ != job->record.generation || job->record.state != AuthorityState::Pending;
      });
    }
  } catch (const std::exception& error) { failed(job, std::string("Authority exception: ") + error.what()); }
  catch (...) { failed(job, "Unknown authority exception; local permission was not granted"); }
}
void AsyncAuthority::run() {
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [&] { return stopping_ || queued_; });
      if (stopping_) return;
      job = std::move(queued_);
    }
    acquire(job);
  }
}
void AsyncAuthority::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    owner_ = 0;
    for (auto& item : records_) {
      auto& record = item.second->record;
      if (record.state == AuthorityState::Pending || record.state == AuthorityState::Owned) {
        record.state = AuthorityState::Cancelled;
        record.detail = "Shutdown revoked local permission; in-flight RPC outcome retained";
      }
    }
    condition_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
}

RealBackend::RealBackend(std::unique_ptr<MotionSwitcherApi> api, Writer writer, AuthorityOptions options,
                         RuntimeController::Now now)
    : authority_(std::move(api), options, std::move(now)), writer_(std::move(writer)) {
  if (!writer_) throw std::invalid_argument("Real backend requires a writer");
}
RealBackend::~RealBackend() {
  try { shutdown(); } catch (...) { authority_.release_local(); }
}
bool RealBackend::authority_owned() const { return authority_.owned(); }
void RealBackend::begin_acquire(uint64_t generation, std::chrono::milliseconds timeout) { authority_.begin(generation, timeout); }
AuthorityProgress RealBackend::poll_acquire(uint64_t generation) { return authority_.poll(generation); }
void RealBackend::cancel_acquire(uint64_t generation) { authority_.cancel(generation); }
void RealBackend::release_authority() { release_owned(); }
void RealBackend::failed_release() {
  authority_.release_local();
  wrote_ = false;
  release_error_ = "Previous zero-gain LowCmd writer failed; packet outcome unconfirmed; local permission was revoked";
}
void RealBackend::release_owned() {
  try {
    if (wrote_ && authority_.owned()) {
      auto command = encode_release(5);
      finalize_command(command);
      if (!writer_(command)) throw std::runtime_error("Owned backend zero-gain release failed");
    }
  } catch (...) { failed_release(); throw; }
  wrote_ = false;
  authority_.release_local();
}
bool RealBackend::write(const LowCommand& command) {
  if (stopped_ || !authority_.owned() || command.mode_machine() != 5 || command.mode_pr() != 0) return false;
  LowCommand packet = command;
  finalize_command(packet);
  const bool release = std::all_of(packet.motor_cmd().begin(), packet.motor_cmd().end(), [](const auto& motor) {
    return motor.mode() == 0 && motor.q() == 0 && motor.dq() == 0 && motor.kp() == 0 && motor.kd() == 0 && motor.tau() == 0;
  });
  try {
    if (!writer_(packet)) {
      if (release) failed_release();
      return false;
    }
  } catch (...) {
    if (release) failed_release();
    throw;
  }
  wrote_ = std::any_of(packet.motor_cmd().begin(), packet.motor_cmd().end(), [](const auto& motor) {
    return motor.kp() != 0 || motor.kd() != 0 || motor.tau() != 0;
  });
  return true;
}
std::string RealBackend::authority_diagnostic() const {
  const auto record = authority_.record();
  if (record.state == AuthorityState::Idle && release_error_.empty()) return {};
  return "generation=" + std::to_string(record.generation) + ": " + record.detail +
      (record.outcome_unknown ? "; remote outcome partial/unknown; local permission remains revoked" : "") +
      (release_error_.empty() ? "" : "; " + release_error_);
}
AuthorityRecord RealBackend::authority_record(uint64_t generation) const { return authority_.record(generation); }
void RealBackend::shutdown() {
  if (stopped_) return;
  try { release_owned(); }
  catch (const std::exception& error) { release_error_ += std::string(": ") + error.what(); authority_.release_local(); }
  catch (...) { authority_.release_local(); }
  stopped_ = true;
  authority_.shutdown();
}
}  // namespace anakin::env
