#pragma once
#include <algorithm>
#include <anakin_env/gate.hpp>
#include <optional>
#include <stdexcept>
#include <vector>
namespace anakin::env {
// Absolute stop time is checked against the real clock at every attempted write.
class DeadlineSchedule {
 public:
  DeadlineSchedule(Clock::time_point start, std::chrono::milliseconds budget)
      : next_(start), end_(start + budget) {}
  bool done(Clock::time_point now) const { return now >= end_; }
  bool due(Clock::time_point now) {
    if (done(now) || now < next_) return false;
    do {
      next_ += std::chrono::milliseconds(2);
    } while (next_ <= now);
    return true;
  }
  Clock::time_point next() const { return std::min(next_, end_); }
  Clock::time_point end() const { return end_; }

 private:
  Clock::time_point next_, end_;
};
struct ConfirmedModel {
  std::string model;
  uint64_t epoch;
};
class ActiveRunAcceptance {
 public:
  void begin(const std::string& model, uint64_t epoch) {
    if (epoch <= epoch_) throw std::invalid_argument("Acceptance epoch must advance");
    model_ = model;
    epoch_ = epoch;
    seen_active_ = false;
    break_tail();
  }
  void sdk_write(Clock::time_point now, bool target_active, const std::string& model,
                 uint64_t epoch) {
    if (!target_active || model != model_ || epoch != epoch_) {
      break_tail();
      return;
    }
    seen_active_ = true;
    if (last_) {
      const auto gap = now - *last_;
      if (gap >= Clock::duration::zero() && gap <= std::chrono::milliseconds(40)) {
        const double seconds = std::chrono::duration<double>(gap).count();
        tail_ += seconds;
        total_ += seconds;
      } else
        tail_ = 0;
    }
    last_ = now;
  }
  bool confirm(const std::string& model, uint64_t epoch) {
    if (!seen_active_ || model != model_ || epoch != epoch_) return false;
    if (!models_.empty() && epoch <= models_.back().epoch) return false;
    if (!models_.empty() && models_.back().model == model)
      models_.back().epoch = epoch;
    else
      models_.push_back({model, epoch});
    return true;
  }
  void break_tail() {
    tail_ = 0;
    last_.reset();
  }
  double tail_seconds() const { return tail_; }
  double total_seconds() const { return total_; }
  const std::vector<ConfirmedModel>& models() const { return models_; }
  bool sequence_matches(int required) const {
    static const std::vector<std::string> expected{"asap_loco", "asap_cr7", "asap_loco"};
    if (required < 1 || required > 3 || models_.size() != static_cast<size_t>(required))
      return false;
    for (int i = 0; i < required; ++i)
      if (models_[i].model != expected[i] || (i && models_[i].epoch <= models_[i - 1].epoch))
        return false;
    return model_ == expected[required - 1] && models_.back().epoch == epoch_;
  }

 private:
  std::string model_;
  uint64_t epoch_ = 0;
  bool seen_active_ = false;
  double tail_ = 0, total_ = 0;
  std::optional<Clock::time_point> last_;
  std::vector<ConfirmedModel> models_;
};
}  // namespace anakin::env
