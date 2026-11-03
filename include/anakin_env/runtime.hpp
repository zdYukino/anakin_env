#pragma once
#include <anakin_env/confirmation.hpp>
#include <anakin_env/discovery.hpp>
#include <anakin_env/feedback.hpp>
#include <anakin_env/input.hpp>
#include <anakin_env/send.hpp>
#include <functional>

namespace anakin::env {
using RuntimeRequest = anakin_sdk::srv::SwitchInferenceMode::Request;
using RuntimeStatus = anakin_sdk::msg::InferenceStatus;
using LowCommand = unitree_hg::msg::dds_::LowCmd_;
enum class AuthorityProgress { Pending, Owned, Failed };
class RuntimeBackend {
 public:
  virtual ~RuntimeBackend() = default;
  virtual bool authority_owned() const = 0;
  // All acquisition operations must return promptly; D owns an async RPC worker.
  virtual void begin_acquire(uint64_t generation, std::chrono::milliseconds timeout) = 0;
  virtual AuthorityProgress poll_acquire(uint64_t generation) = 0;
  virtual void cancel_acquire(uint64_t generation) = 0;
  virtual void release_authority() = 0;  // Never activate factory motion service.
  virtual bool write(const LowCommand&) = 0;
  virtual std::string authority_diagnostic() const { return {}; }
};

// Production controller, with no ROS nodes or started channels. Its owner
// serializes callbacks/ticks/shutdown. Only the input snapshot callback is used
// directly at final admission, keeping B's stop predicate common with the node.
class RuntimeController {
 public:
  using Now = std::function<Clock::time_point()>;
  using Inputs = std::function<InputSnapshot()>;
  RuntimeController(ModeConfig config, RuntimeBackend& backend, Inputs inputs, Now now = Clock::now);
  const StateSnapshot& snapshot() const { return machine_.snapshot(); }
  const ModeConfig& config() const { return machine_.config(); }
  const Uuid& session() const { return session_; }
  const std::optional<RuntimeRequest>& request() const { return request_; }
  bool request_accepted() const { return request_accepted_; }
  uint64_t writer_count() const { return writer_count_; }
  uint64_t state_sequence() const { return state_seq_; }
  const std::string& diagnostic() const { return diagnostic_; }
  void feedback(const NativeSample& sample);
  void feedback_fault(const std::string& reason);
  void status(const RuntimeStatus& status);
  bool target(const Target& target);
  void request_result(const Uuid& request, bool accepted);
  ActionPlan tick(const InputFrame& input);
  std::optional<Frame> take_frame(const InputSnapshot& input);
  void shutdown();
 private:
  bool fresh(Clock::time_point now) const;
  bool actual_valid() const;
  bool measured_valid() const;
  bool permitted_input(const InputSnapshot&) const;
  void execute(const ActionPlan& plan);
  void revoke();
  void release_command();
  void start_policy();
  bool send(bool release = false);
  Frame make_frame(const InputSnapshot& input) const;
  TransactionIdentity identity() const;
  ModeConfig mode_;
  RuntimeBackend& backend_;
  Inputs inputs_;
  Now now_;
  StateMachine machine_;
  ActionPlan plan_;
  Gate gate_;
  ActiveConfirmation confirmation_;
  Uuid session_ = new_uuid(), env_instance_ = new_uuid(), mc_instance_{};
  NativeStatusRouter router_{session_};
  std::optional<NativeSample> feedback_;
  std::optional<Target> target_;
  std::optional<RuntimeRequest> request_;
  std::optional<RuntimeStatus> status_;
  std::optional<uint64_t> acquisition_;
  std::optional<Clock::time_point> alignment_started_;
  Clock::time_point entered_{}, requested_{}, acquisition_deadline_{};
  Positions alignment_source_{};
  double alignment_elapsed_ = 0.;
  Clock::time_point last_tick_{};
  uint64_t epoch_ = 0, state_seq_ = 0, published_generation_ = 0, authorization_ = 1;
  uint64_t writer_count_ = 0, published_seq_ = 0;
  uint8_t published_mask_ = 0;
  Clock::time_point published_deadline_{};
  CommandSource preparing_fallback_ = CommandSource::None;
  bool authority_ready_ = false, wrote_ = false, need_policy_ = false, request_accepted_ = false;
  bool pending_fault_ = false, rollback_fault_ = false, stopped_ = false;
  bool alignment_pending_ = false, feedback_was_fresh_ = false;
  std::string diagnostic_;
};
}  // namespace anakin::env
