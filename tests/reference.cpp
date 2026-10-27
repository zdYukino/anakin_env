// Pure unit fixtures: no channel factory, ROS context, simulator or actuation.
// Numeric velocity/proximity/duration values below are UNIT-TEST INPUTS ONLY;
// they are deliberately rejected as a source for native execution.
#include <anakin_env/discovery.hpp>
#include <anakin_env/reference.hpp>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
using namespace anakin;
using namespace anakin::env;
using namespace std::chrono_literals;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
void rejects(const std::function<void()>& action, const std::string& part) {
  try {
    action();
  } catch (const std::exception& e) {
    if (std::string(e.what()).find(part) != std::string::npos) return;
    throw std::runtime_error(std::string("Wrong rejection; expected ") + part + ", got " +
                             e.what());
  }
  throw std::runtime_error("Missing rejection: " + part);
}
ReferenceBudget fixture_budget(const std::string& model) {
  ReferenceBudget b;
  b.source = BudgetSource::UnitTestFixture;
  b.provenance = "Unit tests only; NOT an execution configuration";
  Positions velocity{}, margin{};
  velocity.fill(2.);
  const auto& kp = simulation_gains(merged_gain_profile, model).kp;
  const auto& cap = simulation_effort_caps(model);
  for (size_t i = 0; i < 29; ++i) margin[i] = cap[i] / (16 * kp[i]);
  b.velocity_limit = velocity;
  b.near_limit_margin = margin;
  b.saturation_duration = 4ms;
  return b;
}
Positions midpoint() {
  Positions q{};
  const auto l = reference_lower_limits(), u = reference_upper_limits();
  for (size_t i = 0; i < 29; ++i) q[i] = (l[i] + u[i]) / 2;
  return q;
}
void gate_regression() {
  const auto session = new_uuid(), mc = new_uuid(), request = new_uuid();
  const auto now = Clock::now();
  Gate gate;
  gate.begin(session, mc, request, 1, "asap_loco", true, merged_gain_profile,
             equivalent_execution_profile, pd_reference);
  Frame state;
  state.schema_version = schema_version;
  state.schema_id = schema_id;
  state.session_id = session;
  state.epoch = 1;
  state.state_seq = 10;
  state.validity = 3;
  state.gain_profile = merged_gain_profile;
  state.execution_profile = equivalent_execution_profile;
  state.command_semantics = pd_reference;
  state.joints.name = joint_names();
  auto q = midpoint();
  q[10] = .5;
  state.joints.position.assign(q.begin(), q.end());
  auto wrong = state;
  wrong.execution_profile = strict_execution_profile;
  gate.state(wrong, now);
  Target t;
  t.schema_version = schema_version;
  t.schema_id = schema_id;
  t.session_id = session;
  t.mc_instance_id = mc;
  t.request_id = request;
  t.epoch = 1;
  t.seq = 1;
  t.source_state_seq = 10;
  t.model_id = "asap_loco";
  t.gain_profile = merged_gain_profile;
  t.execution_profile = equivalent_execution_profile;
  t.command_semantics = pd_reference;
  t.validity = 1;
  t.phase = Target::TRANSITION_ENTRY;
  t.valid_for.nanosec = 40000000;
  t.target.joint_names = joint_names();
  trajectory_msgs::msg::JointTrajectoryPoint p;
  p.positions.assign(q.begin(), q.end());
  p.positions[10] = .5407906210422515;
  t.target.points.push_back(p);
  std::string why;
  check(!gate.accept(t, now + 1ms, why) && why == "unknown source state",
        "Wrong state profile cannot establish source identity");
  wrong = state;
  wrong.schema_version = schema_version - 1;
  gate.state(wrong, now);
  check(!gate.accept(t, now + 1ms, why) && why == "unknown source state",
        "Old state schema cannot establish virtual reference source");
  gate.state(state, now);
  check(gate.accept(t, now + 1ms, why),
        "Typed virtual mux reference accepted within continuity/envelope");
  auto changed = t;
  changed.seq = 2;
  changed.command_semantics = mechanical_position;
  check(!gate.accept(changed, now + 2ms, why) && why == "execution profile/semantics",
        "Target type mismatch rejected");
  changed = t;
  changed.seq = 2;
  changed.execution_profile = project_execution_profile;
  check(!gate.accept(changed, now + 2ms, why), "Target profile mismatch rejected");
  changed = t;
  changed.seq = 2;
  changed.target.points[0].positions[0] += .36;
  check(!gate.accept(changed, now + 2ms, why) && why == "target discontinuity",
        "Virtual continuity bound is unchanged");
  check(gate.live(now + 39ms) && !gate.live(now + 40ms),
        "Virtual target source TTL cannot be prolonged");
  changed = t;
  changed.seq = 2;
  changed.epoch = 0;
  check(!gate.accept(changed, now + 2ms, why), "Old reference epoch rejected");
  gate.stop();
  changed = t;
  changed.seq = 2;
  check(!gate.accept(changed, now + 2ms, why) && why == "stop latched",
        "Late virtual reference cannot undo stop");
  rejects(
      [&] {
        gate.begin(session, mc, new_uuid(), 1, "asap_loco", true, merged_gain_profile,
                   equivalent_execution_profile, pd_reference);
      },
      "new transaction");
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--dump-limits") {
      std::cout << std::setprecision(17) << '{';
      bool first = true;
      const auto values = [&](const char* name, const auto& data) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << std::quoted(name) << ":[";
        for (size_t i = 0; i < 29; ++i) {
          if (i) std::cout << ',';
          std::cout << data[i];
        }
        std::cout << ']';
      };
      values("strict_lower", lower_limits());
      values("strict_upper", upper_limits());
      values("reference_lower", reference_lower_limits());
      values("reference_upper", reference_upper_limits());
      values("effort_caps_loco", simulation_effort_caps("asap_loco"));
      values("effort_caps_cr7", simulation_effort_caps("asap_cr7"));
      std::cout << "}\n";
      return 0;
    }
    check(!known_execution_contract(equivalent_execution_profile, mechanical_position,
                                    merged_gain_profile),
          "Unknown semantic/profile pair");
    check(
        !known_execution_contract(equivalent_execution_profile, pd_reference, legacy_gain_profile),
        "New semantics require the frozen merged gains");
    rejects([&] { execution_encoding("unknown_profile", pd_reference); }, "Unknown");
    require_native_reference_budget(strict_execution_profile, mechanical_position,
                                    frozen_reference_budget());
    for (const auto* profile : {project_execution_profile, equivalent_execution_profile}) {
      rejects(
          [&] {
            require_native_reference_budget(profile, pd_reference, frozen_reference_budget());
          },
          "per_joint_velocity_limit");
      auto b = fixture_budget("asap_loco");
      rejects([&] { require_native_reference_budget(profile, pd_reference, b); },
              "Unit-test reference budgets");
    }
    const auto lower = reference_lower_limits(), upper = reference_upper_limits();
    size_t cases = 0;
    double max_error = 0, max_ulp_bound = 0;
    for (const auto* model : {"asap_loco", "asap_cr7"}) {
      auto budget = fixture_budget(model);
      const auto& gains = simulation_gains(merged_gain_profile, model);
      const auto& caps = simulation_effort_caps(model);
      for (size_t i = 0; i < 29; ++i)
        for (int side : {-1, 1})
          for (bool outside : {false, true}) {
            Positions q = midpoint(), qv = q, dq{};
            const double boundary = side < 0 ? lower[i] : upper[i];
            const double gap = caps[i] / (8 * gains.kp[i]);
            q[i] = boundary - side * gap;
            qv[i] = boundary + (outside ? side * gap : 0);
            const auto unchanged = qv;
            auto a = encode_reference(qv, q, dq, 5, merged_gain_profile, model,
                                      project_execution_profile, pd_reference, budget);
            auto b = encode_reference(qv, q, dq, 5, merged_gain_profile, model,
                                      equivalent_execution_profile, pd_reference, budget);
            check(qv == unchanged, "Encoding cannot feed back projected reference or compensation");
            check(a.command.crc() == command_crc(a.command) &&
                      b.command.crc() == command_crc(b.command),
                  "A/B1 HG CRC");
            for (size_t j = 0; j < 29; ++j) {
              check(b.command.motor_cmd()[j].kp() == static_cast<float>(gains.kp[j]) &&
                        b.command.motor_cmd()[j].kd() == static_cast<float>(gains.kd[j]),
                    "Producing-model gain handover is unchanged in B1");
              check(b.sdk_position[j] >= lower[j] && b.sdk_position[j] <= upper[j],
                    "Float32 SDK position inside compiled/intersected bounds");
              check(a.compensation[j] == 0, "Explicit A projection has zero feed-forward");
              const double exact =
                  b.command.motor_cmd()[j].kp() * (b.sdk_virtual_reference[j] - b.sdk_position[j]);
              const float rounded = static_cast<float>(exact);
              const double ulp = std::max(
                  std::abs(static_cast<double>(std::nextafter(rounded, INFINITY)) - rounded),
                  std::abs(static_cast<double>(rounded) - std::nextafter(rounded, -INFINITY)));
              const double bound =
                  ulp + 16 * std::numeric_limits<double>::epsilon() *
                            (1 + std::abs(b.virtual_effort[j]) + std::abs(b.encoded_effort[j]));
              const double error = std::abs(b.virtual_effort[j] - b.encoded_effort[j]);
              check(error <= bound,
                    "B1 bridge-law equality within derived float32 feed-forward ULP and double "
                    "arithmetic error");
              max_error = std::max(max_error, error);
              max_ulp_bound = std::max(max_ulp_bound, bound);
            }
            if (outside)
              check(std::abs(a.virtual_effort[i] - a.encoded_effort[i]) > 1e-4,
                    "A explicitly changes PD effort; it is not B1");
            ++cases;
          }
      auto q = midpoint(), qv = q;
      Positions dq{};
      auto encode = [&] {
        return encode_reference(qv, q, dq, 5, merged_gain_profile, model,
                                equivalent_execution_profile, pd_reference, budget);
      };
      for (double bad : std::initializer_list<double>{std::numeric_limits<double>::quiet_NaN(),
                                                      INFINITY, -INFINITY}) {
        qv = midpoint();
        qv[0] = bad;
        rejects(encode, "Nonfinite");
        qv = midpoint();
        q = midpoint();
        q[0] = bad;
        rejects(encode, "Nonfinite");
        q = midpoint();
        dq[0] = bad;
        rejects(encode, "Nonfinite");
        dq[0] = 0;
      }
      qv = midpoint();
      qv[0] = upper[0] + caps[0] / gains.kp[0] + .01;
      rejects(encode, "Virtual reference envelope");
      qv = midpoint();
      q[0] = lower[0] - .01;
      rejects(encode, "Measured joint position");
      q = midpoint();
      dq[0] = 2.01;
      rejects(encode, "Measured velocity budget");
      dq[0] = 0;
      q[0] = upper[0] - .00001;
      dq[0] = .1;
      rejects(encode, "Next-period position envelope");
      q = midpoint();
      dq[0] = 0;
      // Both q_s and feed-forward are valid here, but full virtual PD effort is not.
      qv[12] = upper[12] + caps[12] / (8 * gains.kp[12]);
      rejects(encode, "Expected total effort budget");
      qv = midpoint();
      q[10] = upper[10] - (*budget.near_limit_margin)[10] / 2;
      qv[10] = upper[10];
      dq[10] = .1;
      rejects(encode, "Outward velocity/effort near joint limit");
      SaturationWatch watch;
      ReferenceReport report;
      report.saturated[0] = true;
      const auto now = Clock::now();
      watch.check(report, now, budget);
      watch.check(report, now + 2ms, budget);
      rejects([&] { watch.check(report, now + 4ms, budget); }, "Sustained expected saturation");
      report.saturated[0] = false;
      watch.check(report, now + 5ms, budget);
      report.saturated[0] = true;
      watch.check(report, now + 6ms, budget);
    }
    // Regression uses the actual post-startup mux reference, never raw .9000669.
    auto q = midpoint(), qv = q;
    Positions dq{};
    q[10] = -.43543410301208496;
    dq[10] = -1.345090627670288;
    qv[10] = .5407906210422515;
    const auto budget = fixture_budget("asap_loco");
    auto eighth = encode_reference(qv, q, dq, 5, merged_gain_profile, "asap_loco",
                                   equivalent_execution_profile, pd_reference, budget);
    check(eighth.compensation[10] > .34 && eighth.compensation[10] < .35,
          "Mux-based eighth-frame compensation is about .3438124 Nm, not raw-policy compensation");
    rejects([&] { encode_position(qv, 5, merged_gain_profile, "asap_loco"); },
            "Encoder position bounds");
    gate_regression();
    std::cout << std::setprecision(17)
              << "{\"pass\":true,\"no_sdk_channels\":true,\"boundary_cases\":" << cases
              << ",\"max_b1_effort_error_nm\":" << max_error
              << ",\"max_derived_ulp_bound_nm\":" << max_ulp_bound
              << ",\"eighth_mux_compensation_nm\":" << eighth.compensation[10]
              << ",\"runtime_budget_complete\":false}\n";
  } catch (const std::exception& e) {
    std::cerr << "FAIL reference contract: " << e.what() << '\n';
    return 1;
  }
}
