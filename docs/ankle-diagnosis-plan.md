# Right ankle boundary diagnosis and finite repair plan

Frozen baseline: SDK aa70d1a, env 8017110, mc 16076f5. The old floating failure and original-model replay remain unchanged.

1. Cross-layer contract assumption: the original Python q_ref is a virtual PD reference. Python position clipping is commented out; the official bridge computes PD effort and the compiled MuJoCo 3.3.6 model limits actuator effort. Index 10, default -.2, scale .25, raw action 4.3369412422 and both ankle mechanical bounds agree. The row-8 reference implies about 26.465 Nm at the observed q/dq, within the compiled ankle effort bound +/-50 Nm. This does not justify widening the existing position-target gate.
2. Independently confirmed gain mismatch: env encoded the full ASAP table rather than merge_dof_cfgs(base G1_29DoF, producing policy.action_dof). Loco differs at 17 joints; CR7 differs at six wrists. The right ankle's own gains match, so this is not asserted as the sole cause.
3. Initialization is intentionally different: native requires disarmed explicit RC alignment and a canonical startup ramp. The Python simulator runner primes its initial PD command before a scene reset. The native failure occurred with measured ankle inside its range and low torso tilt, but matching model outputs alone does not establish equivalent acquisition or dynamics.

First repair: revisioned shared simulation gain metadata, env selection by actual output model/phase, a preserved legacy profile and a Python packet-level merge oracle. No policy inference, action scaling, default angle, physical position bound, target TTL or continuity bound changes. Next, compare strict simulation before designing any explicitly diagnosed PD-reference adaptation.

Also repair the three confirmed P2 findings: real-clock bounded exit damping with skipped missed periods; discovery independent of retained statuses while preserving fresh session authorization; continuous TARGET_ACTIVE write time and ordered distinct model/epoch acceptance.

Change boundary: SDK metadata/docs; env gain codec, native execution bookkeeping, focused pure helpers/tests; MC profile validation and discovery; C++ simulation driver profile selection. Keep original Anakin/.trellis and Python dirty files read-only. Record this analysis in the C++ repositories instead. No physical backend, remote Git or network-security changes. No independent final review has passed.

Trellis break-loop categories: B cross-layer contract, C configuration propagation, D integration coverage gap, E implicit PD-reference assumption. Earlier shorter alignment and startup interpolation addressed initialization/continuity symptoms; neither establishes the missing gain or reference contract. Prevent recurrence with explicit profile IDs, shared metadata, actual encoded-packet comparison, actuator-model introspection and immutable failure/replay evidence.
