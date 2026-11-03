#!/usr/bin/env python3
"""Preserve G1 MJCF visual kinematics in a browser-readable model manifest."""

import argparse
import hashlib
import json
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument(
    "--source", type=Path, default=Path("/home/zdyukino/workspace/unitree_mujoco")
)
args = parser.parse_args()
source = args.source / "unitree_robots/g1/g1_29dof.xml"
root = ET.parse(source).getroot()
out = Path(__file__).resolve().parents[1] / "public/models/g1"
(out / "meshes").mkdir(parents=True, exist_ok=True)
assets = {m.attrib["name"]: m.attrib["file"] for m in root.findall("asset/mesh")}
used = set()
joints = []


def numbers(text, default):
    return [float(x) for x in text.split()] if text else default


def body(node):
    joint = node.find("joint")
    j = None
    if joint is not None and joint.get("type") != "free":
        j = {
            "name": joint.get("name"),
            "axis": numbers(joint.get("axis"), [0, 0, 1]),
            "pos": numbers(joint.get("pos"), [0, 0, 0]),
            "range": numbers(joint.get("range"), [-3.14, 3.14]),
        }
        joints.append(j["name"])
    geoms = []
    # group 1 are official visual meshes; group 0 collision copies must not be doubled.
    for geom in node.findall("geom"):
        if geom.get("type") == "mesh" and geom.get("group") == "1":
            name = assets[geom.attrib["mesh"]]
            used.add(name)
            geoms.append(
                {
                    "mesh": "meshes/" + name,
                    "pos": numbers(geom.get("pos"), [0, 0, 0]),
                    "quat": numbers(geom.get("quat"), [1, 0, 0, 0]),
                    "rgba": numbers(geom.get("rgba"), [0.7, 0.7, 0.7, 1]),
                }
            )
    return {
        "name": node.get("name"),
        "pos": numbers(node.get("pos"), [0, 0, 0]),
        "quat": numbers(node.get("quat"), [1, 0, 0, 0]),
        "joint": j,
        "geoms": geoms,
        "children": [body(n) for n in node.findall("body")],
    }


model = body(root.find("worldbody/body"))
for name in used:
    shutil.copy2(source.parent / "meshes" / name, out / "meshes" / name)
shutil.copy2(args.source / "LICENSE", out / "LICENSE")
manifest = {
    "name": "Unitree G1",
    "dof": 29,
    "source": str(source.relative_to(args.source)),
    "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "coordinate_system": "Z-up, meters, MJCF quaternion wxyz",
    "joint_names": joints,
    "root": model,
}
(out / "model.json").write_text(json.dumps(manifest, indent=2) + "\n")
(out / "SOURCE.md").write_text(
    "# Unitree G1 model\n\nSource: https://github.com/unitreerobotics/unitree_mujoco\n\nMJCF: "
    + manifest["source"]
    + "\n\nSHA256: "
    + manifest["source_sha256"]
    + "\n\nPreserves visual mesh transforms and joint hierarchy. See LICENSE.\nRebuild: python3 scripts/convert_model.py --source /path/to/unitree_mujoco\n"
)
print(f"Converted {len(joints)} joints, {len(used)} meshes to {out}")
assert len(joints) == 29 and len(used) == 36
