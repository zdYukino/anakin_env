#include <anakin_env/gate.hpp>
#include <cmath>
namespace anakin::env {
const Positions& lower_limits() {
  static const Positions a = {
      -2.5307, -.5236,   -2.7576,      -.087267,     -.87267,      -.2618,       -2.5307, -2.9671,
      -2.7576, -.087267, -.87267,      -.2618,       -2.618,       -.52,         -.52,    -3.0892,
      -1.5882, -2.618,   -1.0472,      -1.972222054, -1.614429558, -1.614429558, -3.0892, -2.2515,
      -2.618,  -1.0472,  -1.972222054, -1.614429558, -1.614429558};
  return a;
}
const Positions& upper_limits() {
  static const Positions a = {2.8798, 2.9671,      2.7576,      2.8798,      .5236,      .2618,
                              2.8798, .5236,       2.7576,      2.8798,      .5236,      .2618,
                              2.618,  .52,         .52,         2.6704,      2.2515,     2.618,
                              2.0944, 1.972222054, 1.614429558, 1.614429558, 2.6704,     1.5882,
                              2.618,  2.0944,      1.972222054, 1.614429558, 1.614429558};
  return a;
}
void Gate::begin(const Uuid& s, const Uuid& i, const Uuid& r, uint64_t e, const std::string& model,
                 bool continuation) {
  if (s == session_ && (e <= epoch_ || r == request_))
    throw std::invalid_argument("Gate reauthorization requires a new transaction and epoch");
  revoked_ = false;
  session_ = s;
  instance_ = i;
  request_ = r;
  epoch_ = e;
  model_ = model;
  continuation_ = continuation;
  accepted_seq_ = 0;
  phase_mask_ = 0;
  live_ = false;
  expires_ = {};
}
void Gate::state(const Frame& f, Clock::time_point t) {
  if (f.session_id != session_ || f.epoch != epoch_ || f.joints.position.size() != 29 ||
      f.joints.name != joint_names() || (f.validity & 3) != 3)
    return;
  if (!samples_.empty() && f.state_seq <= samples_.back().seq) return;
  Positions q;
  for (size_t i = 0; i < 29; ++i) {
    q[i] = f.joints.position[i];
    if (!std::isfinite(q[i])) return;
  }
  samples_.push_back({f.state_seq, f.epoch, t, q});
  while (samples_.size() > 256) samples_.pop_front();
}
bool Gate::accept(const Target& t, Clock::time_point now, std::string& why) {
  auto reject = [&](const char* s) {
    why = s;
    return false;
  };
  if (revoked_) return reject("stop latched");
  if (t.schema_version != schema_version || t.schema_id != schema_id) return reject("schema");
  if (t.session_id != session_ || t.mc_instance_id != instance_ || t.request_id != request_)
    return reject("session/instance/request");
  if (t.epoch != epoch_) return reject("epoch");
  if (t.seq <= accepted_seq_) return reject("sequence");
  if (t.gain_profile != "g1_asap_sim_v1" || t.validity != 1) return reject("gain/validity");
  if (t.phase != Target::SOURCE_CONTINUATION && t.phase != Target::TRANSITION_ENTRY &&
      t.phase != Target::TARGET_ACTIVE)
    return reject("phase");
  if (t.model_id != model_ &&
      !(continuation_ && t.model_id == "asap_loco" && t.phase != Target::TARGET_ACTIVE))
    return reject("model");
  if (t.phase == Target::TARGET_ACTIVE && t.model_id != model_) return reject("active model");
  if (t.valid_for.sec != 0 || t.valid_for.nanosec == 0 || t.valid_for.nanosec > 60000000)
    return reject("TTL budget");
  const Sample* source = nullptr;
  for (const auto& s : samples_)
    if (s.seq == t.source_state_seq && s.epoch == epoch_) source = &s;
  if (!source) return reject("unknown source state");
  const auto deadline = source->acquired + std::chrono::nanoseconds(t.valid_for.nanosec);
  if (now < source->acquired || now >= deadline) return reject("expired source state");
  if (t.target.joint_names != joint_names() || t.target.points.size() != 1)
    return reject("trajectory dimensions/names");
  const auto& p = t.target.points[0];
  if (p.positions.size() != 29 || !p.velocities.empty() || !p.accelerations.empty() ||
      !p.effort.empty() || p.time_from_start.sec != 0 || p.time_from_start.nanosec != 0)
    return reject("position-only target required");
  Positions q;
  for (size_t i = 0; i < 29; ++i) {
    q[i] = p.positions[i];
    if (!std::isfinite(q[i]) || q[i] < lower_limits()[i] || q[i] > upper_limits()[i])
      return reject("position limit/nonfinite");
    const double previous = previous_ ? (*previous_)[i] : source->q[i];
    if (std::abs(q[i] - previous) > .35) return reject("target discontinuity");
  }
  previous_ = q;
  accepted_seq_ = t.seq;
  expires_ = deadline;
  live_ = true;
  phase_mask_ |= t.phase;
  why = "accepted";
  return true;
}
bool Gate::live(Clock::time_point now) const { return !revoked_ && live_ && now < expires_; }
void Gate::stop() {
  revoked_ = true;
  live_ = false;
  expires_ = {};
  previous_.reset();
}
}  // namespace anakin::env
