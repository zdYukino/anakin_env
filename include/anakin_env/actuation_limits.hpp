#pragma once
#include <anakin_env/gate.hpp>
namespace anakin::env {
// Frozen MuJoCo 3.3.6 G1 model, unitree_mujoco@1eb6642. Joint transmission,
// gear=[1,0,0,0,0,0], fixed unit gain, zero bias/dynamics for all 29 actuators.
// These are hard model data, not evidence of physical safety or a velocity limit.
inline const Positions& compiled_sim_lower() {
  static const Positions values = {-2.5306999999999999,
                                   -0.52359999999999995,
                                   -2.7576000000000001,
                                   -0.087266999999999997,
                                   -0.87266999999999995,
                                   -0.26179999999999998,
                                   -2.5306999999999999,
                                   -2.9670999999999998,
                                   -2.7576000000000001,
                                   -0.087266999999999997,
                                   -0.87266999999999995,
                                   -0.26179999999999998,
                                   -2.6179999999999999,
                                   -0.52000000000000002,
                                   -0.52000000000000002,
                                   -3.0891999999999999,
                                   -1.5882000000000001,
                                   -2.6179999999999999,
                                   -1.0471999999999999,
                                   -1.9722200000000001,
                                   -1.61443,
                                   -1.61443,
                                   -3.0891999999999999,
                                   -2.2515000000000001,
                                   -2.6179999999999999,
                                   -1.0471999999999999,
                                   -1.9722200000000001,
                                   -1.61443,
                                   -1.61443};
  return values;
}
inline const Positions& compiled_sim_upper() {
  static const Positions values = {2.8797999999999999,
                                   2.9670999999999998,
                                   2.7576000000000001,
                                   2.8797999999999999,
                                   0.52359999999999995,
                                   0.26179999999999998,
                                   2.8797999999999999,
                                   0.52359999999999995,
                                   2.7576000000000001,
                                   2.8797999999999999,
                                   0.52359999999999995,
                                   0.26179999999999998,
                                   2.6179999999999999,
                                   0.52000000000000002,
                                   0.52000000000000002,
                                   2.6703999999999999,
                                   2.2515000000000001,
                                   2.6179999999999999,
                                   2.0943999999999998,
                                   1.9722200000000001,
                                   1.61443,
                                   1.61443,
                                   2.6703999999999999,
                                   1.5882000000000001,
                                   2.6179999999999999,
                                   2.0943999999999998,
                                   1.9722200000000001,
                                   1.61443,
                                   1.61443};
  return values;
}
inline const Positions& simulation_effort_caps(const std::string& model) {
  static const Positions loco = {88, 88, 88, 139, 50, 50, 88, 88, 88, 139, 50, 50, 88, 50, 50,
                                 25, 25, 18, 18,  10, 5,  5,  25, 25, 18,  18, 10, 5,  5};
  static const Positions mimic = {88, 88, 88, 139, 50, 50, 88, 88, 88, 139, 50, 50, 88, 50, 50,
                                  25, 25, 25, 25,  10, 5,  5,  25, 25, 25,  25, 10, 5,  5};
  if (model == "asap_loco") return loco;
  if (model == "asap_cr7") return mimic;
  throw std::invalid_argument("Unknown effort-profile model");
}
inline Positions reference_lower_limits() {
  Positions q{};
  for (size_t i = 0; i < 29; ++i) q[i] = std::max(lower_limits()[i], compiled_sim_lower()[i]);
  return q;
}
inline Positions reference_upper_limits() {
  Positions q{};
  for (size_t i = 0; i < 29; ++i) q[i] = std::min(upper_limits()[i], compiled_sim_upper()[i]);
  return q;
}
}  // namespace anakin::env
