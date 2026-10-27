#pragma once
#include <anakin_env/actuation_limits.hpp>
#include <anakin_env/native_codec.hpp>
#include <anakin_sdk/simulation_profile.hpp>
#include <optional>

namespace anakin::env {
enum class BudgetSource { FrozenExecutionConfiguration, UnitTestFixture };
struct ReferenceBudget {
  BudgetSource source = BudgetSource::FrozenExecutionConfiguration;
  std::string provenance =
      "g1_pd_reference_execution_budget_v1; velocity/near-limit/saturation budgets missing";
  std::optional<Positions> velocity_limit, near_limit_margin;
  std::optional<Clock::duration> saturation_duration;
};
const ReferenceBudget& frozen_reference_budget();
void validate_reference_budget(const ReferenceBudget& budget, bool native_runtime);
void require_native_reference_budget(const std::string& execution, uint8_t semantics,
                                     const ReferenceBudget& budget);
struct ReferenceReport {
  unitree_hg::msg::dds_::LowCmd_ command;
  Positions mux_reference{}, sdk_virtual_reference{}, sdk_position{}, compensation{},
      virtual_effort{}, encoded_effort{};
  std::array<bool, 29> projected{}, saturated{}, near_limit{};
};
ReferenceReport encode_reference(const Positions& mux_reference, const Positions& measured_q,
                                 const Positions& measured_dq, uint8_t machine,
                                 const std::string& gain, const std::string& model,
                                 const std::string& execution, uint8_t semantics,
                                 const ReferenceBudget& budget);
class SaturationWatch {
 public:
  void check(const ReferenceReport& report, Clock::time_point now, const ReferenceBudget& budget);
  void reset() {
    for (auto& t : started_) t.reset();
  }

 private:
  std::array<std::optional<Clock::time_point>, 29> started_{};
};
}  // namespace anakin::env
