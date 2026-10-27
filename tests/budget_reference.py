"""Check compiled C++ reference budgets against compiled MJCF and Python configs.
Only config merging and read-only model audit data are used; opens no SDK channel.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-root", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--compiled-model", required=True, type=Path)
    parser.add_argument("--execution-config", required=True, type=Path)
    parser.add_argument("--cpp-driver", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.reference_root))
    from robojudo.config.g1.env.g1_env_cfg import G1_29DoF
    from robojudo.config.g1.policy.g1_asap_policy_cfg import G1AsapLocoPolicyCfg, G1AsapPolicyCfg
    from robojudo.tools.dof import merge_dof_cfgs

    audit = json.loads(args.compiled_model.read_text())
    config = json.loads(args.execution_config.read_text())
    cpp = json.loads(subprocess.check_output([str(args.cpp_driver), "--dump-limits"], text=True))
    base = G1_29DoF()
    acts = audit["actuators"]
    assert [a["joint"] for a in acts] == base.joint_names
    assert len(acts) == 29 and audit["timestep_s"] == .002
    for a in acts:
        assert a["transmission_type"] == 0 and a["gear"] == [1, 0, 0, 0, 0, 0]
        assert a["gain_type"] == a["bias_type"] == a["dynamics_type"] == 0 and a["gain0"] == 1
        assert a["position_limited"] and a["control_limited"] and a["joint_actuator_force_limited"]
        assert a["control_range"][0] == -a["control_range"][1]
        assert a["joint_actuator_force_range"][0] == -a["joint_actuator_force_range"][1]
    expected_lower = [max(a["position_range"][0], low) for a, low in zip(acts, cpp["strict_lower"])]
    expected_upper = [min(a["position_range"][1], high) for a, high in zip(acts, cpp["strict_upper"])]
    assert cpp["reference_lower"] == expected_lower and cpp["reference_upper"] == expected_upper
    for model, cls, field in [("asap_loco", G1AsapLocoPolicyCfg, "effort_caps_loco"),
                              ("asap_cr7", G1AsapPolicyCfg, "effort_caps_cr7")]:
        merged = merge_dof_cfgs(base, cls().action_dof)
        expected = [min(a["control_range"][1], a["joint_actuator_force_range"][1], value)
                    for a, value in zip(acts, merged.torque_limits)]
        assert cpp[field] == expected == config["known_budgets"][model]["effective_effort_cap_nm"]
    for name, expected in config["source_sha256"].items():
        assert hashlib.sha256((args.workspace.parent/name).read_bytes()).hexdigest() == expected
    assert not config["complete_for_native_execution"]
    assert config["velocity_limits_rad_s"] is None and config["near_limit_margin_rad"] is None
    assert config["saturation_duration_budget_s"] is None
    report = {"pass": True, "sdk_channels": False, "joints": 29, "models": 2,
              "compiled_gear_and_effort_match": True, "position_intersection_match": True,
              "native_reference_budget_complete": False,
              "missing_execution_budgets": config["missing_execution_budgets"],
              "missing_data_not_inferred_from_arm_example_ramp_or_solver_solref": True}
    args.out.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
