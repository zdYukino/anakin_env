#pragma once
#include <anakin_env/native_codec.hpp>

namespace anakin::env {
struct NativeSample {
  Frame frame;
  Clock::time_point acquired{};
  uint8_t mode_machine = 0;
  uint64_t generation = 0;
};
// Called under the receiver's lock. SDK tick is diagnostic metadata, never an
// Env sequence, transaction epoch, input connection or policy reset trigger.
class FeedbackTracker {
 public:
  bool observe(Frame frame, uint8_t mode_machine, Clock::time_point acquired) {
    if (sample_ && (acquired < sample_->acquired || frame.sdk_tick == sample_->frame.sdk_tick))
      return false;
    if (sample_ && frame.sdk_tick < sample_->frame.sdk_tick) ++rollbacks_;
    sample_ = NativeSample{std::move(frame), acquired, mode_machine, ++generation_};
    return true;
  }
  const std::optional<NativeSample>& sample() const { return sample_; }
  uint64_t rollbacks() const { return rollbacks_; }
 private:
  std::optional<NativeSample> sample_;
  uint64_t generation_ = 0, rollbacks_ = 0;
};
}  // namespace anakin::env
