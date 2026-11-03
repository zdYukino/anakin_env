#pragma once
#include <anakin_env/runtime.hpp>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace anakin::env {
// This interface intentionally exposes no SelectMode/factory activation operation.
class MotionSwitcherApi {
 public:
  virtual ~MotionSwitcherApi() = default;
  virtual void initialize(float timeout_seconds) = 0;
  virtual int32_t check_mode(std::string& form, std::string& name, float timeout_seconds) = 0;
  virtual int32_t release_mode(float timeout_seconds) = 0;
};

// The same adapter is instantiated with the real SDK and SDK-shaped test fakes.
// Even Client construction is deferred until explicit acquisition on the worker.
template <class Client>
class MotionClientAdapter final : public MotionSwitcherApi {
 public:
  using Factory = std::function<std::unique_ptr<Client>()>;
  explicit MotionClientAdapter(Factory factory = [] { return std::make_unique<Client>(); })
      : factory_(std::move(factory)) {}
  void initialize(float timeout_seconds) override {
    if (client_) return;
    auto client = factory_();
    client->SetTimeout(static_cast<float>(timeout_seconds)); // SDK also has int64_t microseconds.
    client->Init();
    client_ = std::move(client);
  }
  int32_t check_mode(std::string& form, std::string& name, float timeout_seconds) override {
    if (!client_) throw std::logic_error("MotionSwitcher not initialized");
    client_->SetTimeout(static_cast<float>(timeout_seconds));
    return client_->CheckMode(form, name);
  }
  int32_t release_mode(float timeout_seconds) override {
    if (!client_) throw std::logic_error("MotionSwitcher not initialized");
    client_->SetTimeout(static_cast<float>(timeout_seconds));
    return client_->ReleaseMode();
  }
 private:
  Factory factory_;
  std::unique_ptr<Client> client_;
};

enum class AuthorityState { Idle, Pending, Owned, Failed, Cancelled, Released };
struct AuthorityRecord {
  uint64_t generation = 0;
  AuthorityState state = AuthorityState::Idle;
  bool release_attempted = false, release_succeeded = false, observed_empty = false;
  bool outcome_unknown = false;
  std::optional<int32_t> check_code, release_code;
  std::string detail;
};
struct AuthorityOptions {
  std::chrono::milliseconds rpc_timeout{250}, retry_interval{50};
};

class AsyncAuthority {
 public:
  using Now = RuntimeController::Now;
  AsyncAuthority(std::unique_ptr<MotionSwitcherApi>, AuthorityOptions = {}, Now = Clock::now);
  ~AsyncAuthority();
  AsyncAuthority(const AsyncAuthority&) = delete;
  AsyncAuthority& operator=(const AsyncAuthority&) = delete;
  // Generations advance monotonically; pruning diagnostics never permits reuse.
  void begin(uint64_t generation, std::chrono::milliseconds total_timeout);
  AuthorityProgress poll(uint64_t generation);
  void cancel(uint64_t generation);
  void release_local();
  bool owned() const;
  AuthorityRecord record(uint64_t generation = 0) const;
  void shutdown(); // Joins only during teardown, never on the 2 ms control path.
 private:
  struct Job;
  bool current(const std::shared_ptr<Job>&) const;
  float rpc_budget(const std::shared_ptr<Job>&);
  void failed(const std::shared_ptr<Job>&, const std::string&);
  void run();
  void acquire(const std::shared_ptr<Job>&);
  std::unique_ptr<MotionSwitcherApi> api_;
  AuthorityOptions options_;
  Now now_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::map<uint64_t, std::shared_ptr<Job>> records_;
  std::shared_ptr<Job> queued_;
  uint64_t latest_ = 0, owner_ = 0;
  bool stopping_ = false;
  std::thread worker_;
};

std::unique_ptr<MotionSwitcherApi> make_sdk_motion_switcher();

class RealBackend final : public RuntimeBackend {
 public:
  using Writer = std::function<bool(const LowCommand&)>;
  RealBackend(std::unique_ptr<MotionSwitcherApi>, Writer, AuthorityOptions = {},
              RuntimeController::Now = Clock::now);
  ~RealBackend() override;
  bool authority_owned() const override;
  void begin_acquire(uint64_t generation, std::chrono::milliseconds timeout) override;
  AuthorityProgress poll_acquire(uint64_t generation) override;
  void cancel_acquire(uint64_t generation) override;
  void release_authority() override;
  bool write(const LowCommand&) override;
  std::string authority_diagnostic() const override;
  AuthorityRecord authority_record(uint64_t generation = 0) const;
  void shutdown();
 private:
  void release_owned();
  void failed_release();
  AsyncAuthority authority_;
  Writer writer_;
  bool wrote_ = false, stopped_ = false;
  std::string release_error_;
};
}  // namespace anakin::env
