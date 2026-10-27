> Later review repairs and the still-open PD-reference P1 are recorded in [review-fixes-20261027.md](review-fixes-20261027.md). This file preserves the earlier checkpoint.

# Native C++ review checkpoint — 2026-10-27 UTC

This is a local review checkpoint. Sustained native floating policy execution has not passed. No physical backend, physical-robot authorization or remote Git operation is included.

## Source and runtime boundaries

The three repositories live directly under `/home/zdyukino/workspace/`; `anakin_cpp_ws/src` links to them. The SDK source/installed/consumer lock is `aa70d1a5e9dc79bc48aeb115972944d6de9cfb2f`, schema `anakin.g1.canonical29.v2`. Standard ROS message dependencies remain native ROS packages. Both env and mc have fresh independent builds against this exact SDK, in addition to the integrated build.

`anakin_env_node` remains the default offline fixture. The explicit `--loopback-sim` entry selects `anakin_env_native_node`; it requires backend `unitree_loopback_sim`, the inspected active `lo` interface and a nonzero SDK domain. It cannot select hardware, and `arm=true` is rejected. The integration script execs the C++ `anakin_sim_runner`, which owns a private Xvfb, the official simulator, MC, native env and a C++ RC/protocol probe. Cleanup signals only its recorded children and reaps them with finite escalation.

Native env checks native HG CRC, finite joints/IMU/RC, quaternion norm and G1 machine mode 5. Duplicate SDK ticks do not renew state age. State, native RC and optional SportModeState odometry retain acquisition stamps; odometry velocity is rotated into the child base frame. A separate C++ monotonic thread schedules 2 ms periods and skips missed slots. It rechecks state age (40 ms), RC age (250 ms), source target TTL, transaction identity, model, schema, gain profile, bounds and continuity before SDK writes. Write success updates separate SDK publication identities/epoch/sequence/masks; it is never actuator-execution confirmation.

Fresh `L1+R1+X` authorizes measured-pose alignment, then HOLD; a second fresh edge requests loco. Held startup buttons cannot arm. The alignment duration is an explicit simulation parameter, default 2 seconds, allowed .25..5 seconds. A/B has priority and stops locally, with at most 100 ms of damping. A terminal fault requires process restart and explicit authorization. R1+Up/Down requests mimic/loco only after confirmed active publication. L1+R1+Y requests a new transaction for the current model. Unshouldered Left toggles the legacy motion command; Up/Down changes desired height by .05 m. Legacy velocity signs/scales and deadzone are covered by tests. Ordinary model switching remains owned by env; motion_done allocates a new fallback epoch there.

MC remains the sole final ControlTarget publisher and keeps original ONNX inference/history/action scaling. Explicit `g1_startup_v1` warms ten observations and ramps canonical positions from the actual first source pose for ten publication callbacks. It publishes TRANSITION_ENTRY until complete and TARGET_ACTIVE afterward. Ordinary `g1_named_upper_v1` loco/mimic timing is unchanged. The original 25/101 callback boundaries and per-model histories remain independently tested. Optional `trace_jsonl` records actual observations, raw actions, mapped policy references, mux goals and published positions without changing inference or execution limits.

## Current verification

- Integrated build: `evidence/build-native-freeze-20261027T133015Z.log`, three packages passed.
- C++ contracts: env 2/2 and MC 1/1 passed; logs `anakin_env-contracts-freeze-20261027T133015Z.log` and `anakin_mc-contracts-freeze-20261027T133015Z.log`.
- Fresh independent builds, matching-ORT numerical traces, actual failing-observation replay, Python-manager transition traces, offline protocol tails/restart, owned-child launcher cleanup and startup execution trace all passed. Exact commands/exits/times are in `evidence/native-freeze-checks-20261027T133832Z/results.json`.
- Both models match 224 frames of observations/history/raw action/canonical q_ref/motion_done with zero error using isolated matching Python ORT 1.20.1. Both ordinary switch directions match the actual Python manager for 112 frames with zero error. Native C++ still uses official ORT 1.20.1; the original research environment and its ORT 1.31.0 failure were not changed or relabeled as passing.
- The current default offline ELF startup accepted 364 targets over eight seconds, with zero rejection/expiry. `startup-execve.log` contains no Python invocation. Both launchers reject binaries lacking the SDK build-provenance file or built against a different locked SDK.
- Native RC loss injection passed in the explicitly suspended official scene: `evidence/native-sim-1793107994572627-30714`. The probe measured 239.983 ms from stopping its RC producer to the exact RC-age fault, observed 49 damping packets and zero position commands after the fault bound. Native env correctly exited 1; runner expected 1 and returned success for the injected-fault scenario. This is not floating-policy stability evidence.

## Preserved native floating failure

The final diagnostic run is `evidence/native-sim-1793108110673417-30858`, using the original official floating G1 scene, one free joint, 29 motors, zero welds and disabled elastic band, with explicit .25-second alignment. Binary/scene/config fingerprints and their structural check are in `native-freeze-checks-20261027T133832Z/official-simulator-source.json`.

Seven policy targets were accepted and 78 SDK policy writes occurred before target sequence 8 failed the existing position bound. `right_ankle_pitch_joint` was requested at `0.5306399053335189` rad after startup interpolation, above upper bound `0.5236` rad; its original mapped model reference was `0.8842353105545044` rad. The actual measured joint was `-0.4255724847316742` rad. `first-bound-violation.json` and `inference.jsonl` retain the exact values. Replaying all 16 recorded observations through the original pinned Python ONNX model and defaults gives zero raw-action and canonical-reference error. This does not establish that acquisition, policy/gain compatibility or the joint-target contract is physically accepted; it makes the present bound conflict reproducible. No threshold was widened and no model output was silently clamped to obtain a pass.

The run stopped locally on target TTL after the rejected target. It ended with success=false, zero completed model modes and no claim of physical execution. The measured short write interval averaged about 495.4 Hz, p50 1.998 ms, p99 3.961 ms, with skipped periods; this is not a hard-real-time 500 Hz guarantee or a sustained stable-policy trial. Earlier initialization, discontinuity, suspended-loop and TTL failures remain intact in the evidence directory.

The current full native loco/mimic/CR7 completion cycle has not been run to acceptance because startup loco does not reach accepted sustained operation. Physical execution remains unauthorized. Independent review must resolve the model/gain/sensor/target-limit compatibility and startup behavior before any acceptance claim or further execution-contract change.

## Reproduction

From `anakin_cpp_ws`:

```sh
./build.sh
ctest --test-dir build/anakin_env --output-on-failure --no-tests=error
ctest --test-dir build/anakin_mc --output-on-failure --no-tests=error
./en_run.sh
./en_run.sh --loopback-sim --suspended --rc-loss --duration 10 --sdk-domain 35 --ros-domain 192
./en_run.sh --loopback-sim --duration 15 --alignment-seconds 0.25 --trace-inference --sdk-domain 33 --ros-domain 190
```

The RC loss command expects an injected native failure; the final floating command currently fails and preserves its own run directory. Do not interpret either as a successful native model deployment. Golden-reference and fault-test command vectors are stored in the results JSON above. Original Anakin dirty files, original model assets and official simulator/SDK sources remain reference inputs.
