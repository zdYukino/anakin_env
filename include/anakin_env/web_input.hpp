#pragma once

#include <anakin_env/input.hpp>

namespace anakin::env {
// Serialized by the node owner. Transport-neutral lease validation; no ROS or SDK.
class WebInput {
 public:
  WebInput(InputConfig config, std::chrono::milliseconds timeout, std::string environment);
  InputSample receive(const std::string& packet, InputTime now, int64_t unix_ms);
  std::optional<InputSample> expire(InputTime now);
  const std::string& owner() const { return owner_; }
  uint64_t sequence() const { return sequence_; }
 private:
  InputConfig config_;
  std::chrono::milliseconds timeout_;
  std::string environment_, owner_;
  uint64_t sequence_ = 0, connection_ = 0;
  InputTime acquired_{};
};
}  // namespace anakin::env
