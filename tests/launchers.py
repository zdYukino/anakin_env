"""Development-only launch tests. Every selected executable is a copied stub ELF.

Never executes a native Env/MC, ROS fixture, SDK client, simulator or runner.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env-source", type=Path, required=True)
    parser.add_argument("--stub", type=Path, required=True)
    parser.add_argument("--mc-source", type=Path)
    parser.add_argument("--workspace-entry", type=Path)
    args = parser.parse_args()
    revision = (args.env_source / "anakin_sdk.revision").read_text().strip()
    other_revision = "0" * 40
    with tempfile.TemporaryDirectory(prefix="anakin launch ") as directory:
        root = Path(directory)
        repo, mc, ws = [root / name for name in ("anakin_env", "anakin_mc", "anakin_cpp_ws")]
        repo.mkdir(); mc.mkdir(); ws.mkdir()
        (repo / "scripts").mkdir()
        for relative in ("em_run.sh", "scripts/launch_common.sh", "scripts/integration_run.sh", "anakin_sdk.revision"):
            shutil.copy2(args.env_source / relative, repo / relative)
        if args.mc_source:
            for relative in ("em_run.sh", "anakin_sdk.revision"):
                shutil.copy2(args.mc_source / relative, mc / relative)
        if args.workspace_entry:
            require(not args.workspace_entry.is_symlink(), "Workspace entry must replace the old fixture alias")
            shutil.copy2(args.workspace_entry, ws / "em_run.sh")
        sdk = ws / "install/anakin_sdk"
        sdk_metadata = sdk / "share/anakin_sdk/cmake/revision.cmake"
        sdk_metadata.parent.mkdir(parents=True)
        sdk_metadata.write_text(f'set(ANAKIN_SDK_REVISION "{revision}")\n')
        sdk_marker = sdk / "share/ament_index/resource_index/packages/anakin_sdk"
        sdk_marker.parent.mkdir(parents=True)
        sdk_marker.touch()
        capture = root / "capture"
        models = root / "model.onnx"
        models.touch()
        standalone = repo / "install/anakin_env"
        shared = ws / "install/anakin_env"
        explicit = root / "explicit Env"
        mc_prefix = mc / "install/anakin_mc"

        def install(prefix, package, names):
            marker = prefix / f"share/ament_index/resource_index/packages/{package}"
            marker.parent.mkdir(parents=True)
            marker.touch()
            resources = prefix / f"share/{package}"
            resources.mkdir(exist_ok=True)
            (resources / "runtime-sdk.revision").write_text(revision + "\n")
            if package == "anakin_env":
                shutil.copytree(args.env_source / "config", resources / "config")
            for name in names:
                binary = prefix / f"lib/{package}/{name}"
                binary.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(args.stub, binary)

        for prefix in (standalone, shared, explicit):
            install(prefix, "anakin_env", ("anakin_env_native_node", "anakin_env_node", "anakin_sim_runner"))
        mc_shared = ws / "install/anakin_mc"
        mc_explicit = root / "explicit MC"
        for selected_mc in (mc_prefix, mc_shared, mc_explicit):
            install(selected_mc, "anakin_mc", ("anakin_mc_node",))
        environment = os.environ.copy()
        # No host prefix, overlay or model selection may affect these captures.
        for key in ("ANAKIN_PREFIX", "ANAKIN_SDK_PREFIX", "ANAKIN_WS", "ROS_DOMAIN_ID"):
            environment.pop(key, None)
        environment.update(ANAKIN_WS=str(ws), ANAKIN_CAPTURE=str(capture),
                           AMENT_PREFIX_PATH="/unrelated/overlay", LD_LIBRARY_PATH="/unrelated/lib",
                           ANAKIN_LOCO_MODEL=str(models), ANAKIN_MIMIC_MODEL=str(models))
        invocations = 0

        def run(script, arguments=(), overrides=None, error=False):
            nonlocal invocations
            invocations += 1
            capture.unlink(missing_ok=True)
            env = dict(environment)
            if overrides:
                env.update(overrides)
            process = subprocess.Popen([str(script), *arguments], cwd=root, env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            stdout, stderr = process.communicate(timeout=5)
            if error:
                require(process.returncode == 2, f"Expected preflight rejection: {arguments}: {process.returncode}: {stderr}")
                require(not capture.exists(), "Invalid launch executed a stub")
                return None
            require(process.returncode == 0, f"Launch failed: {arguments}: {stderr} {stdout}")
            fields = capture.read_bytes().decode().split("\0")
            delimiter = fields.index("--ENV--")
            pid_index = fields.index("--PID--")
            values = dict(zip(fields[delimiter + 1:pid_index:2], fields[delimiter + 2:pid_index:2]))
            require(int(fields[pid_index + 1]) == process.pid, "Launcher must exec its ELF without a supervisor")
            require(values["ROS_LOCALHOST_ONLY"] == "1", "Local ROS discovery")
            require(values["ANAKIN_SDK_PREFIX"] == str(sdk), "Explicit coherent SDK prefix")
            return fields[:delimiter], values

        entry = repo / "em_run.sh"
        argv, values = run(entry)
        require(argv == [str(standalone / "lib/anakin_env/anakin_env_native_node"), "--real"], "Default real native-only entry")
        require(values["AMENT_PREFIX_PATH"].startswith(f"{sdk}:{standalone}:/opt/ros/humble"), "Selected library/resource order")
        forwards = ["--sim", "--input", "remote", "--config", str(root / "mode override.yaml"),
                    "--input-config", str(root / "input override.yaml"), "--ros-args", "-p", "use_sim_time:=false"]
        argv, _ = run(entry, forwards)
        require(argv[1:] == forwards, "Forward exact options, paths with spaces and ROS args")
        argv, _ = run(entry, ["--real", "--input", "gamepad"])
        require(argv[1:] == ["--real", "--input", "gamepad"], "Explicit real input")
        argv, _ = run(entry, ["--input", "keyboard"])
        require(argv[1:] == ["--real", "--input", "keyboard"], "Default real input forwarding")
        argv, _ = run(entry, ["--sim"], {"ANAKIN_PREFIX": str(explicit), "ANAKIN_SDK_PREFIX": str(sdk)})
        require(argv[0] == str(explicit / "lib/anakin_env/anakin_env_native_node"), "Explicit selected prefix")
        if args.workspace_entry:
            argv, _ = run(ws / "em_run.sh", ["--sim", "--input", "remote"])
            require(argv[0] == str(standalone / "lib/anakin_env/anakin_env_native_node"), "Workspace forwards normal package selection")
        moved = standalone.with_name("saved")
        standalone.rename(moved)
        argv, _ = run(entry, ["--sim"])
        require(argv[0] == str(shared / "lib/anakin_env/anakin_env_native_node"), "Workspace install fallback when no standalone install exists")
        moved.rename(standalone)
        for arguments in (["--loopback-sim"], ["--sim", "--real"], ["--real", "--real"], ["--sim", "--sim"],
                          ["--input"], ["--input", "unknown"], ["--config"], ["--input-config"], ["--unknown"],
                          ["--input", "remote", "--input", "keyboard"], ["-p", "duration:=1"], [""]):
            run(entry, arguments, error=True)
        run(entry, overrides={"ANAKIN_PREFIX": str(root / "missing"), "AMENT_PREFIX_PATH": str(shared)}, error=True)
        marker = standalone / "share/ament_index/resource_index/packages/anakin_env"
        marker.unlink()
        run(entry, error=True)
        marker.touch()
        for name in ("sim.yaml", "real.yaml", "inputs.yaml"):
            resource = standalone / "share/anakin_env/config" / name
            text = resource.read_text(); resource.unlink()
            run(entry, error=True)
            resource.write_text(text)
        provenance = standalone / "share/anakin_env/runtime-sdk.revision"
        for text in (other_revision + "\n", revision + "\nextra\n", ""):
            provenance.write_text(text)
            run(entry, error=True)
        provenance.write_text(revision + "\n")
        bad_sdk_metadata = (
            f'# {revision}\nset(ANAKIN_SDK_REVISION "{other_revision}")\n',
            f'set(ANAKIN_SDK_REVISION "prefix{revision}")\n',
            f'set(ANAKIN_SDK_REVISION "{revision}")\n' * 2,
            f'set(ANAKIN_SDK_REVISION "{revision}")\n set(ANAKIN_SDK_REVISION "{other_revision}")\n',
            f'set(ANAKIN_SDK_REVISION "{revision}")\nset(ANAKIN_SDK_REVISION "malformed")\n',
            f'set(ANAKIN_SDK_REVISION "{revision}")\nset(ANAKIN_SDK_REVISION\n "{other_revision}")\n',
            f'set(OTHER_REVISION "{revision}")\n',
        )
        for text in bad_sdk_metadata:
            sdk_metadata.write_text(text)
            run(entry, error=True)
        sdk_metadata.write_text(f'# SDK build record\n\nset(ANAKIN_SDK_REVISION "{revision}")')
        run(entry)
        sdk_metadata.write_text(f'set(ANAKIN_SDK_REVISION "{revision}")\n')
        sdk_marker.unlink()
        run(entry, error=True)
        sdk_marker.touch()
        binary = standalone / "lib/anakin_env/anakin_env_native_node"
        binary.unlink(); binary.symlink_to(shared / "lib/anakin_env/anakin_env_native_node")
        run(entry, error=True)
        binary.unlink(); binary.write_text("#!/bin/sh\nexit 0\n"); binary.chmod(0o755)
        run(entry, error=True)
        shutil.copy2(args.stub, binary)
        integration = repo / "scripts/integration_run.sh"
        run(integration, error=True)
        run(integration, ["--loopback-sim"], error=True)
        argv, _ = run(integration, ["--sim-test", "--duration", "17", "--rc-loss"])
        require(argv[0] == str(standalone / "lib/anakin_env/anakin_sim_runner") and
                argv[1:] == ["--workspace", str(ws), "--duration", "17", "--rc-loss"], "Only explicit test route selects runner")
        if args.mc_source:
            argv, _ = run(mc / "em_run.sh")
            require(argv[0] == str(mc_prefix / "lib/anakin_mc/anakin_mc_node"), "Actual MC standalone entry")
            require(argv[1:] == ["--ros-args", "-p", f"loco_model:={models}", "-p", f"mimic_model:={models}"], "MC model forwarding")
            saved_mc = mc_prefix.with_name("saved")
            mc_prefix.rename(saved_mc)
            argv, _ = run(mc / "em_run.sh")
            require(argv[0] == str(mc_shared / "lib/anakin_mc/anakin_mc_node"), "MC workspace fallback when standalone is absent")
            saved_mc.rename(mc_prefix)
            argv, _ = run(mc / "em_run.sh", overrides={"ANAKIN_PREFIX": str(mc_explicit)})
            require(argv[0] == str(mc_explicit / "lib/anakin_mc/anakin_mc_node"), "MC explicit install selection")
            run(mc / "em_run.sh", overrides={"ANAKIN_PREFIX": str(root / "missing MC")}, error=True)
            mc_marker = mc_prefix / "share/ament_index/resource_index/packages/anakin_mc"
            mc_marker.unlink(); run(mc / "em_run.sh", error=True); mc_marker.touch()
            lock = mc_prefix / "share/anakin_mc/runtime-sdk.revision"
            lock.write_text(other_revision + "\n"); run(mc / "em_run.sh", error=True)
            lock.write_text(revision + "\n")
            for text in bad_sdk_metadata:
                sdk_metadata.write_text(text)
                run(mc / "em_run.sh", error=True)
            sdk_metadata.write_text(f'# SDK build record\n\nset(ANAKIN_SDK_REVISION "{revision}")')
            run(mc / "em_run.sh")
        print(f"PASS {invocations} launch captures/rejections; compiled stub ELFs only")


if __name__ == "__main__":
    main()
