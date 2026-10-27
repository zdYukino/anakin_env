> Later SDK v3/reference work and the disabled PD profiles are recorded in [pd-reference-checkpoint.md](pd-reference-checkpoint.md). This file preserves its earlier checkpoint.

# Native review repair checkpoint, 2026-10-27 15:19 UTC

This is a reviewable partial checkpoint. Four review findings have implementation and focused regression evidence. The virtual-PD-reference/physical-position contract conflict remains an open P1. Floating locomotion, native loco/CR7/loco completion and deployment are not accepted. No independent final review has passed. Actual native switch-time gain handover remains unrun because the open P1 blocks initial TARGET_ACTIVE; packet matrices and source-model selection have separate focused coverage.

SDK source and all integrated/standalone runtime locks: `8035fb945eb95987344e8f1ba7d6b4f8e3e1f94a`. Env and MC final commits are recorded in the external checkpoint JSON, avoiding a self-referencing commit hash in this document. Baseline remains SDK aa70d1a, env 8017110, mc 16076f5.

## Review findings

| Finding | Change and exact current status |
| --- | --- |
| P1 effective gains | Implemented and packet-tested. `g1_asap_merged_sim_v1` merges base G1_29DoF gains with the producing policy action_dof. Loco overrides 12 joints; CR7 overrides 23, preserving six base wrist gains. Env authorizes the profile throughout state/request/status/target and encodes using actual target.model_id, including source continuation. `g1_asap_sim_v1` preserves the old full table for reproducibility. All 232 encoded values for both profiles/models match original Python merging to float32 packet precision. Unknown profiles/models are rejected. |
| P1 virtual PD reference | Diagnosed, OPEN. Mapping, scale, defaults and the right ankle limits agree. Python deliberately does not clamp q_ref; the official bridge computes PD effort, limited by the compiled actuator model. Current contract remains a strict bounded mechanical position target. No output projection, raw clipping change, limit widening, or feed-forward adapter has been implemented or enabled. Correcting gains alone still fails target 8. |
| P2 exit damping deadline | Implemented. Check real steady time immediately before SDK writes, skip missed 2 ms slots, refuse exit writes at/after the absolute 100 ms deadline. Fault/RC-stop damping also rechecks the real LocalControl deadline. Fake-clock tests cover 0/2/86/99 ms, no catch-up at repeated timestamps, and rejection at 100/105/1000 ms. Final RC-loss fixture observes 50 damping writes. |
| P2 env-only restart discovery | Implemented. MC emits independent zero-session/request discovery even with retained transaction statuses. Two sequential actual offline env processes complete fresh session/epoch-1 transactions against one unchanged MC PID. Cached session statuses do not authorize the new session. Native construction remains disarmed; this offline test does not replace a physical RC reauthorization test. |
| P2 activity/model acceptance | Implemented. Only successful SDK writes of TARGET_ACTIVE for the current model/epoch count time. Transition writes, non-target writes, epoch changes, and gaps over 40 ms break the continuous tail. Adjacent repeated requests for one model count once. Requested sequence is ordered loco/CR7/loco with increasing epochs. Tests reject three loco epochs as three models. Final floating failure reports active_seconds=0, total=0, modes=0, sequence=[]. |

Native start_request now takes the model by value, preserving the name when an RC reset references request_.target_model before request_ is reset.

## Final evidence

All paths below are under `/home/zdyukino/workspace/anakin_cpp_ws/evidence/`.

- `build-review-freeze-20261027T151628Z/results.json`: integrated three-package build and both independent repo builds pass. ROS Humble rosidl emits a CMake CMP0148 developer warning; no compile error remains.
- `review-fixes-final-20261027T151838Z/results.json`: integrated and standalone env 2/2 and MC 1/1 CTest; final negative floating gate regression; RC-loss regression; recorded-observation Python replay; default ELF startup. The floating row's expected exit is 1: the regression pass means the gate rejects the invalid target, not closed-loop success.
- `review-fixes-final-20261027T151838Z/sdk-provenance.json`: installed SDK and four app prefixes match the exact locked SDK commit.
- `review-fixes-checks-20261027T151138Z/gain-packets/summary.json`: 232 packet gain values match the unchanged Python merge.
- `restart-regression-1793113900558202562/summary.json`: same MC PID, two different fresh env sessions, no SDK channels.
- `review-fixes-checks-20261027T151138Z/reference-ort120/summary.json`: both models, 224 frames each, zero difference for observation/history, raw actions, canonical references and done callbacks, matched isolated ORT 1.20.1.
- `review-fixes-final-20261027T151838Z/floating-replay.json`: final failed run's 15 recorded observations produce zero raw-action/canonical-reference difference in original Python ORT 1.20.1. This does not prove acquisition, initialization, or dynamics equivalence.
- `review-fixes-final-20261027T151838Z/first-bound-violation.json`: final exact failure chain.
- `native-sim-1793114318867415-40138/`: official floating G1 scene, no weld/support/band, 0.25 s explicit startup alignment, merged gains. Seven targets accepted, target 8 rejected, local TTL stop. Native success=false; no active model/SDK active ACK. Mean observed write rate 494.188 Hz, p50 2.000458 ms, p99 3.577734 ms, max gap 4.131778 ms over a short failed run; no hard-real-time or sustained-control claim.
- `native-sim-1793114322244839-40255/`: explicitly suspended, separate simulated wireless topic loss, 239.977 ms to exact RC-age fault; 50 damping writes observed, zero positions outside the observer's 20 ms post-fault bound, zero malformed commands. Runner succeeds because env fault exit=1 is expected. This is neither floating stability nor proof of an independent physical radio link. Embedded repeated lowstate RC bytes cannot establish independent RC liveness.
- `review-fixes-final-20261027T151838Z/startup-execve.log`: default ./en_run.sh runs C++ ELF applications, no Python exec. The eight-second offline run accepted 363 targets, zero rejection/expiry, success=true; offline fixture opens no SDK channels.
- `review-fixes-checks-20261027T151138Z/code-source-sha256.json`: code source manifest, verified unchanged after final builds/tests and before commit.

Preserved failures include the old original-Python ORT 1.31 versus C++ 1.20.1 CR7 discrepancy (`reference-p1/summary.json`), all old native failures, the first corrected-gain floating failure (`native-sim-1793113941657359-38699`), and the intermediate cached-provenance launch refusal. No tolerance was relaxed. Original research .venv, model files, Anakin dirty worktree and .trellis remain unchanged.

## Unresolved P1 and next contract change

Final target 8 at canonical index 10: raw action 4.4002676010, scale .25, default -.2 -> virtual reference .9000669003 rad. Startup alpha .7 gives .5407906210 rad, beyond the unchanged .5236 upper position limit. Measured q=-.4354341030, dq=-1.3450906277. Gains 20/.2 imply about 26.9790 Nm for the unramped virtual reference or 19.7935 Nm for the published startup reference, both below the compiled ankle effort limit +/-50 Nm. This establishes a reference semantic mismatch, not a stable controller. The original Python initialization primes a PD command before reset; native initialization intentionally requires disarmed RC alignment and a startup ramp, so those dynamics are not asserted equivalent.

A possible later explicit simulation-only PD_REFERENCE contract would distinguish virtual reference q_v from bounded SDK position q_s. The algebraic decomposition q_s=clamp(q_v, lower, upper), tau_ff=Kp*(q_v-q_s) preserves the original SDK PD law without using measured feedback to add a software PD controller. It would require a new typed semantic/profile revision, independent validation of finite values and virtual continuity, strict bounded SDK q, feed-forward and actuator effort limits, and per-joint projection/effort/saturation diagnostics. It must reject excessive compensation and preserve posture/TTL/stop faults. It must not silently clip the model, hide instability, or be represented as implemented here. That change and the subsequent floating/transition validation remain outstanding.

## Reproduction

Use each repo's ./build.sh, or anakin_cpp_ws/./build.sh for the integrated build. Build entry points now force CMake configuration; revision lock files also trigger reconfiguration. This repairs a real cached-metadata refusal discovered in this run. The launch guard correctly refused before this fix; no stale SDK run was accepted.

Focused test commands are recorded verbatim in the results JSON files. For Python test oracles only, set PYTHONDONTWRITEBYTECODE=1 and use the original .venv Python; matched inference additionally uses PYTHONPATH=anakin_cpp_ws/deps/python_oracle_ort120. Runtime ./en_run.sh does not use these oracles.

Native negative check (expected exit 1):

```sh
./en_run.sh --loopback-sim --duration 15 --alignment-seconds 0.25 --trace-inference --sdk-domain 35 --ros-domain 192
```

RC-loss fixture (expected exit 0, deliberately suspended):

```sh
./en_run.sh --loopback-sim --suspended --rc-loss --duration 10 --sdk-domain 36 --ros-domain 193
```

Only explicit loopback simulation was exercised. All recorded child processes were reaped. No true robot, user process termination, network-security change, remote Git creation, push or merge occurred.
