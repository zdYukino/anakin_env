"""Compare actual encoded C++ packets with the original Python named gain merge.

Imports configuration and merge code only: no environment, SDK channel or simulator.
The reference tree is read-only, including its bytecode cache.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-root", type=Path, required=True)
    parser.add_argument("--cpp-driver", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.reference_root.resolve()))
    from robojudo.config.g1.env.g1_env_cfg import G1_29DoF
    from robojudo.config.g1.policy.g1_asap_policy_cfg import (
        G1_29AsapDoF, G1AsapLocoPolicyCfg, G1AsapPolicyCfg,
    )
    from robojudo.tools.dof import merge_dof_cfgs

    packets = json.loads(subprocess.check_output([str(args.cpp_driver), "--dump-gains"], text=True))
    policies = {"asap_loco": G1AsapLocoPolicyCfg(), "asap_cr7": G1AsapPolicyCfg()}
    results = []
    for profile in ["g1_asap_sim_v1", "g1_asap_merged_sim_v1"]:
        for model, policy in policies.items():
            reference = (G1_29AsapDoF() if profile == "g1_asap_sim_v1"
                         else merge_dof_cfgs(G1_29DoF(), policy.action_dof))
            errors = []
            for field, property_name in [("kp", "stiffness"), ("kd", "damping")]:
                expected = getattr(reference, property_name)
                actual = packets[profile][model][field]
                if len(actual) != 29 or len(expected) != 29:
                    raise AssertionError("Incomplete canonical gain packet")
                for name, ref, value in zip(reference.joint_names, expected, actual):
                    ref_float = struct.unpack("f", struct.pack("f", ref))[0]
                    if not math.isfinite(value) or not math.isclose(value, ref_float, rel_tol=0, abs_tol=1e-8):
                        errors.append({"joint": name, "field": field, "python_float32": ref_float, "encoded": value})
            results.append({"profile": profile, "model": model, "pass": not errors, "mismatches": errors})
    source_files = ["robojudo/config/g1/env/g1_env_cfg.py", "robojudo/config/g1/policy/g1_asap_policy_cfg.py", "robojudo/tools/dof.py"]
    report = {"pass": all(row["pass"] for row in results), "sdk_channels": False,
              "encoded_gain_values": len(results) * 29 * 2, "results": results,
              "reference_sources_sha256": {name: hashlib.sha256((args.reference_root/name).read_bytes()).hexdigest() for name in source_files}}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out/"encoded-packets.json").write_text(json.dumps(packets, indent=2)+"\n")
    (args.out/"summary.json").write_text(json.dumps(report, indent=2)+"\n")
    if not report["pass"]:
        raise AssertionError(f"Encoded gain comparison failed; evidence={args.out}")
    print(f"PASS all {report['encoded_gain_values']} encoded gains against original Python merge; evidence={args.out}")


if __name__ == "__main__":
    main()
