# PD_REFERENCE finite checkpoint, 2026-10-27 15:58 UTC

SDK v3 and communication-free A/B1 encoders are implemented and tested. Both PD_REFERENCE profiles remain disabled for native execution because three required execution budgets have no reliable source. No A/B1 floating or native loco/CR7/loco acceptance is claimed. This is a checkpoint for independent review, not an enabled controller or a safety certification.

## Interface and ownership

SDK commit `725c2702c6fd14466e33dfca630c2ec3d0155cc0`, schema `anakin.g1.canonical29.v3`. State, request, queued response, status and target carry execution_profile and command_semantics; the queued response also echoes gains. Unknown type/profile combinations are rejected and journal equality includes the new fields. Source-state acquisition, target TTL, session, instance, request and epoch checks remain in place; even an old-schema source state cannot establish an admissible target source.

- Default `g1_position_strict_sim_v1` / MECHANICAL_POSITION retains the old strict bounded target and its negative fixture.
- `g1_pd_reference_project_sim_v1` / PD_REFERENCE is explicit A: bounded SDK q_s, zero feed-forward. Its PD effort differs from the virtual reference; tests require that difference when projection occurs.
- `g1_pd_reference_equivalent_sim_v1` / PD_REFERENCE is B1: q_s=clip(q_v,l,u), tau_ff=Kp*(q_v-q_s), unchanged SDK Kp/Kd and dq_ref=0. The SDK-representable q_s is rounded inward at a mechanical boundary; compensation then uses the SDK float32 virtual q_v and actual representable q_s. Its bridge-law error is bounded by feed-forward float32 rounding and floating-point arithmetic, independently of measured q/dq. This is not a safety proof.

MC still publishes the final startup/transition mux q_v. Env receives only that final target for encoding, never the raw policy reference. Env alone owns limits, gains, projection, feed-forward, CRC and SDK writes. Expected total effort is computed for rejection/diagnostics, not used as a B2 software-PD control output. Raw action/history are never replaced by q_s or tau_ff. The actual eighth-frame regression uses mux q_v=.5407906210, not raw policy q_ref=.9000669003: ideal compensation .3438124208 Nm, encoded .3438127041 Nm. The difference is documented float32 representation.

## Reliable data and missing budgets

`config/g1_pd_reference_execution_budget_v1.json` records frozen compiled-model evidence and null entries for unavailable budgets. C++ uses immutable frozen metadata; editing JSON or passing a runtime parameter cannot enable either PD profile. Completing the missing values requires a new reviewed/versioned execution configuration and end-to-end pre-write-path validation, including alignment/hold/damping as well as policy targets. The present native integration and matching probe logic have not been commissioned for A/B1.

Read-only mj_loadXML audit, no mj_step or SDK initialization, of the pinned official G1 scene under MuJoCo 3.3.6 confirms:

- 29 joint transmissions, gear [1,0,0,0,0,0], fixed unit gain, no actuator bias/dynamics; model timestep .002 s.
- Every actuated joint has position and joint actuator-force limits; each motor control range is limited. New reference-mode q_s uses the intersection of existing env position bounds with compiled model ranges, preserving the strict default bounds separately.
- The effort/compensation cap is the minimum of compiled control and joint-force caps under unit gear and the original producing policy's merged torque limit. A separately imported Python merge and compiled C++ table oracle agree for both models/all 29 joints.
- Virtual reference envelope is that intersected position range extended by effective effort cap / encoded Kp. Compensation and both unprojected virtual-PD and encoded expected total effort must stay inside the effective cap. Projection cannot hide excessive virtual effort.

Three required data sets remain absent:

1. Per-joint measured velocity limits for this execution profile.
2. A sourced near-limit margin or validated stopping envelope for the supported states/velocities/effort.
3. Maximum allowed saturation duration for this execution profile.

The MJCF's zero joint margins and constraint solref are not a validated braking envelope. The SDK arm example's .5 rad/s is an example desired-command ramp for 17 arm/torso joints, not a measured-velocity safety budget for all 29 joints. No value was inferred from either. Native startup rejects both PD profiles before NativeTransport::start_loopback with all three missing fields named. No unit fixture can authorize native execution.

The pure encoder additionally checks finite reference/q/dq, actual position, configured velocity, a one-2-ms constant-velocity range prediction, configured near-limit outward velocity/effort, full expected effort and sustained expected saturation. The constant-velocity prediction is an additional rejection rule, not a stopping proof. Native policy-target wiring takes a fresh state for each attempted SDK encoding and rechecks source/state/RC freshness immediately before writing. Local strict alignment/hold/damping retain their earlier implementation and require separate budget-path commissioning before any future PD profile activation. Native execution remains hard-disabled in this checkpoint.

## Native discovery coverage correction

The earlier restart_regression.py was only an offline discovery test and did not prove native discovery. `NativeStatusRouter` now contains the actual runtime native routing logic, and native_node.cpp uses it directly. Communication-free tests cover old transaction statuses not discovering a peer, independent zero-session/request discovery, discovery not authorizing execution, fresh process session, held old RC presses, the second fresh RC inference edge after alignment, old-session rejection after authorization, profile mismatch and local revoke. A restarted native env again starts disarmed with a new session; cached transactions cannot revive it. The offline restart test is retained and labeled as separate coverage.

## Evidence

Paths are under `/home/zdyukino/workspace/anakin_cpp_ws/evidence/`:

- `build-pd-reference-v3-20261027T154717Z.log`: SDK v3/env/MC first integrated build pass.
- `build-pd-reference-freeze-20261027T155024Z/results.json`: final integrated and independent env/MC implementation builds pass. Later test-only builds add the second-RC assertion in both prefixes: `build-final-discovery-test-20261027T155341Z.log`, `build-final-standalone-tests-20261027T155430Z.log`.
- `pd-reference-audit-20261027T154152Z/compiled-model.json` and `budget-manifest.json`: compiled limits/transmission provenance and exact missing budget names; original model/research files remain read-only.
- `pd-reference-final-20261027T155637Z/results.json`: all finite expected regressions. Env 3/3 and MC 1/1 CTest pass in both integrated/standalone prefixes.
- `pd-reference-final-20261027T155637Z/reference-unit.json`: 232 two-model/all-joint/lower-upper/exact-outside boundary combinations, A/B1 CRC, packet gains, type/profile mismatch, virtual continuity, source epoch/TTL/old schema/local stop, NaN/Inf, state velocity/position, next-period envelope, near-limit and saturation fault rejection. Maximum B1 effort error 5.9604644775e-7 Nm, within a separately derived maximum ULP/arithmetic bound 1.9073488833e-6 Nm. Unit velocity/proximity/duration numbers are marked UnitTestFixture and rejected for native authorization; they are not proposed execution settings.
- `pd-reference-final-20261027T155637Z/budget-oracle.json`: compiled transmission/range/cap tables match live original Python merge and source hashes, for both models/all joints; native budget remains incomplete.
- `pd-reference-final-20261027T155637Z/gain-oracle/summary.json`: all 232 encoded gain values match original Python.
- `pd-reference-final-20261027T155637Z/numeric-ort120/summary.json`: both policies, 224 frames each, zero observation/history/raw-action/q_ref/done difference under matching isolated ORT 1.20.1. The original Python ORT 1.31 mismatch evidence is preserved, not relabeled.
- `native-budget-refusal-0.log`, `native-budget-refusal-1.log`, `native-type-mismatch.log` in the same final directory: actual C++ native launch refuses incomplete A/B1 or an unknown semantic pair before SDK initialization. No simulated state/RC peer is started for these refusal tests.
- `native-sim-1793116605416977-79277/`: only the unchanged strict profile is run against the official floating scene, no support/band, .25 s alignment. Seven targets accepted, target 8 .5416049427 > .5236 rejected, local TTL stop, zero TARGET_ACTIVE seconds/models. Native success=false. Gate rejection regression pass is not floating success. Short-run writer mean 494.112 Hz, p99 gap 3.895116 ms, max gap 4.830659 ms; no sustained-control/hard-real-time claim.
- `pd-reference-final-20261027T155637Z/strict-trace-replay.json`: that final failed run's 15 observations replay to zero raw-action/reference difference in original Python matched ORT; no acquisition/dynamics equivalence claim.
- `protocol-normal.log`, `protocol-failed_tail.log`, `protocol-stale_tail.log`, `protocol-restart.log` in the final directory: actual offline ROS fixture normal/terminal FAILED/TTL/restart results 0/1/1/1, including new queued-response/profile checks. No SDK channels.
- `restart-regression-1793116611281055354/summary.json`: offline two-env/one-MC recovery, kept separate from the actual native pure test.
- `pd-reference-final-20261027T155637Z/startup-execve.log`: default ./en_run.sh executes C++ ELF applications, no Python exec. Eight-second offline run accepts 364 targets, zero rejection/expiry; not native execution.
- `pd-reference-final-20261027T155637Z/code-source-sha256.json`: tested implementation/data/test manifest before local freeze commits. Exact commits, binary hashes and post-run process check are in the external checkpoint JSON.

No A/B1 native SDK sends, floating acceptance, native model switch sequence or physical-robot operation were attempted. These remain blocked, not passed. No final independent review of this new freeze has passed. No remote Git operation, network-security change or user-process termination occurred. Old failures and the original dirty Anakin tree remain preserved.
