#include <anakin_env/reference.hpp>
#include <cmath>
#include <limits>
#include <sstream>
namespace anakin::env {
const ReferenceBudget& frozen_reference_budget() {
  static const ReferenceBudget missing;
  return missing;
}
void validate_reference_budget(const ReferenceBudget& budget, bool native_runtime) {
  std::string missing;
  if (!budget.velocity_limit) missing += " per_joint_velocity_limit";
  if (!budget.near_limit_margin) missing += " near_limit_guard_or_validated_stopping_envelope";
  if (!budget.saturation_duration) missing += " maximum_saturation_duration";
  if (!missing.empty())
    throw std::runtime_error("PD_REFERENCE execution budget incomplete:" + missing);
  if (native_runtime && budget.source != BudgetSource::FrozenExecutionConfiguration)
    throw std::runtime_error("Unit-test reference budgets cannot authorize native execution");
  if (budget.provenance.empty() || *budget.saturation_duration < Clock::duration::zero())
    throw std::runtime_error("Invalid reference budget provenance/duration");
  const auto lower = reference_lower_limits(), upper = reference_upper_limits();
  for (size_t i = 0; i < 29; ++i)
    if (!std::isfinite((*budget.velocity_limit)[i]) || (*budget.velocity_limit)[i] <= 0 ||
        !std::isfinite((*budget.near_limit_margin)[i]) || (*budget.near_limit_margin)[i] < 0 ||
        (*budget.near_limit_margin)[i] > (upper[i] - lower[i]) / 2)
      throw std::runtime_error("Invalid velocity/near-limit budget at " + joint_names()[i]);
}
void require_native_reference_budget(const std::string& execution, uint8_t semantics,
                                     const ReferenceBudget& budget) {
  if (execution_encoding(execution, semantics) != ExecutionEncoding::StrictPosition)
    validate_reference_budget(budget, true);
}
ReferenceReport encode_reference(const Positions& qv, const Positions& q, const Positions& dq,
                                 uint8_t machine, const std::string& gain, const std::string& model,
                                 const std::string& execution, uint8_t semantics,
                                 const ReferenceBudget& budget) {
  if (!known_execution_contract(execution, semantics, gain))
    throw std::runtime_error("Unknown execution contract");
  const auto mode = execution_encoding(execution, semantics);
  ReferenceReport report;
  report.mux_reference = qv;
  if (mode == ExecutionEncoding::StrictPosition) {
    report.command = encode_position(qv, machine, gain, model);
    return report;
  }
  validate_reference_budget(budget, false);
  const auto lower = reference_lower_limits(), upper = reference_upper_limits();
  const auto& caps = simulation_effort_caps(model);
  const auto& gains = simulation_gains(gain, model);
  Positions sdk{};
  for (size_t i = 0; i < 29; ++i) {
    if (!std::isfinite(qv[i]) || !std::isfinite(q[i]) || !std::isfinite(dq[i]))
      throw std::runtime_error("Nonfinite reference/state at " + joint_names()[i]);
    if (q[i] < lower[i] || q[i] > upper[i])
      throw std::runtime_error("Measured joint position outside compiled range at " +
                               joint_names()[i]);
    if (std::abs(dq[i]) > (*budget.velocity_limit)[i])
      throw std::runtime_error("Measured velocity budget at " + joint_names()[i]);
    const double future =
        q[i] + .002 * dq[i];  // Frozen 2 ms control/model period; not a braking proof.
    if (future < lower[i] || future > upper[i])
      throw std::runtime_error("Next-period position envelope at " + joint_names()[i]);
    const double kp = static_cast<float>(gains.kp[i]);
    if (qv[i] < lower[i] - caps[i] / kp || qv[i] > upper[i] + caps[i] / kp)
      throw std::runtime_error("Virtual reference envelope at " + joint_names()[i]);
    // Retain representable SDK virtual q, matching the original unprojected codec.
    report.sdk_virtual_reference[i] = static_cast<float>(qv[i]);
    float position = static_cast<float>(std::clamp(qv[i], lower[i], upper[i]));
    if (position < lower[i])
      position = std::nextafter(position, std::numeric_limits<float>::infinity());
    if (position > upper[i])
      position = std::nextafter(position, -std::numeric_limits<float>::infinity());
    if (position < lower[i] || position > upper[i])
      throw std::runtime_error("No representable SDK position inside range");
    sdk[i] = position;
    report.sdk_position[i] = position;
    report.projected[i] = report.sdk_virtual_reference[i] != sdk[i];
  }
  report.command = encode_position(sdk, machine, gain, model);
  for (size_t i = 0; i < 29; ++i) {
    auto& motor = report.command.motor_cmd()[i];
    const double kp = motor.kp(), kd = motor.kd();
    const double compensation =
        mode == ExecutionEncoding::EquivalentReference
            ? kp * (report.sdk_virtual_reference[i] - report.sdk_position[i])
            : 0.;
    if (!std::isfinite(compensation) || std::abs(compensation) > caps[i])
      throw std::runtime_error("Feed-forward budget at " + joint_names()[i]);
    motor.tau() = static_cast<float>(compensation);
    report.compensation[i] = motor.tau();
    if (std::abs(report.compensation[i]) > caps[i])
      throw std::runtime_error("Encoded feed-forward budget at " + joint_names()[i]);
    report.virtual_effort[i] = kp * (report.sdk_virtual_reference[i] - q[i]) - kd * dq[i];
    report.encoded_effort[i] = motor.tau() + kp * (motor.q() - q[i]) - kd * dq[i];
    if (!std::isfinite(report.virtual_effort[i]) || !std::isfinite(report.encoded_effort[i]) ||
        std::abs(report.virtual_effort[i]) > caps[i] ||
        std::abs(report.encoded_effort[i]) > caps[i])
      throw std::runtime_error("Expected total effort budget at " + joint_names()[i]);
    const double margin = (*budget.near_limit_margin)[i];
    const bool low = q[i] <= lower[i] + margin, high = q[i] >= upper[i] - margin;
    report.near_limit[i] = low || high;
    if ((low && (dq[i] < 0 || report.virtual_effort[i] < 0 || report.encoded_effort[i] < 0)) ||
        (high && (dq[i] > 0 || report.virtual_effort[i] > 0 || report.encoded_effort[i] > 0)))
      throw std::runtime_error("Outward velocity/effort near joint limit at " + joint_names()[i]);
    report.saturated[i] = std::abs(report.virtual_effort[i]) >= caps[i] ||
                          std::abs(report.encoded_effort[i]) >= caps[i];
  }
  report.command.crc() = command_crc(report.command);
  return report;
}
void SaturationWatch::check(const ReferenceReport& report, Clock::time_point now,
                            const ReferenceBudget& budget) {
  validate_reference_budget(budget, false);
  for (size_t i = 0; i < 29; ++i) {
    if (!report.saturated[i]) {
      started_[i].reset();
      continue;
    }
    if (!started_[i]) started_[i] = now;
    if (now < *started_[i] || now - *started_[i] >= *budget.saturation_duration)
      throw std::runtime_error("Sustained expected saturation budget at " + joint_names()[i]);
  }
}
}  // namespace anakin::env
