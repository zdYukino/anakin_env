#pragma once
#include <anakin_env/gate.hpp>
#include <anakin_sdk/msg/inference_status.hpp>
#include <anakin_sdk/srv/switch_inference_mode.hpp>

namespace anakin::env {
// This is the actual native status path. Discovery never authorizes execution;
// binding a fresh local transaction separately requires local RC authorization.
class NativeStatusRouter {
 public:
  using Request = anakin_sdk::srv::SwitchInferenceMode::Request;
  using Status = anakin_sdk::msg::InferenceStatus;
  enum class Decision { Ignore, Discovered, InstanceChanged, ContractMismatch, Transaction };
  explicit NativeStatusRouter(const Uuid& fresh_session) : session_(fresh_session) {
    if (zero(session_)) throw std::invalid_argument("Native session must be fresh and nonzero");
  }
  const Uuid& instance() const { return instance_; }
  void bind(const Request& request, bool local_rc_authorized) {
    if (!local_rc_authorized || request.session_id != session_ || zero(instance_) ||
        request.expected_mc_instance_id != instance_ || zero(request.request_id) ||
        request.next_epoch == 0 ||
        !known_execution_contract(request.execution_profile, request.command_semantics,
                                  request.gain_profile))
      throw std::invalid_argument(
          "Native transaction requires this session, discovered instance and local RC "
          "authorization");
    request_ = request;
  }
  void revoke() { request_.reset(); }
  Decision observe(const Status& status) {
    if (status.schema_version != schema_version || status.schema_id != schema_id ||
        zero(status.mc_instance_id))
      return Decision::Ignore;
    if (zero(instance_)) {
      if (zero(status.session_id) && zero(status.request_id) && status.epoch == 0 &&
          !status.accepted) {
        instance_ = status.mc_instance_id;
        return Decision::Discovered;
      }
      return Decision::Ignore;
    }
    if (status.mc_instance_id != instance_) return Decision::InstanceChanged;
    if (!request_ || status.session_id != session_ || status.request_id != request_->request_id ||
        status.epoch != request_->next_epoch)
      return Decision::Ignore;
    if (status.requested_model != request_->target_model ||
        status.gain_profile != request_->gain_profile ||
        status.execution_profile != request_->execution_profile ||
        status.command_semantics != request_->command_semantics)
      return Decision::ContractMismatch;
    return Decision::Transaction;
  }

 private:
  Uuid session_, instance_{};
  std::optional<Request> request_;
};
}  // namespace anakin::env
