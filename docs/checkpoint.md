# Validated offline checkpoint — 2026-10-27 UTC

The inherited final source was rebuilt directly on GTR after the old writer stopped. SDK source and installed revision are `d67c4c79473beb9cfae42b7f5e70b07cb382aa18`. All three packages built together; env and mc also built independently against that SDK. The deployed entry points execute ELF C++ binaries without Python startup utilities.

Fresh evidence is in `/home/zdyukino/workspace/anakin_cpp_ws/evidence/resume-checks-20261027T120155Z/results.json`; the integrated build is `evidence/build-resume-20261027T120048Z.log`. Its source digest manifest is `evidence/resume-source-20261027T120048Z.json`.

Validated fixes: local stop remains latched until an explicit new transaction/epoch; return to loco preserves the 25-callback independent warmup; CR7 phase freezes at the terminal sample and publishes motion completion on the original post-step boundary; active confirmation uses cumulative receipts rather than a moving last sequence; FAILED, source TTL expiry and MC restart remain terminal failures. The request source sequence is a lower bound and launcher cleanup has finite escalation for its own children only.

Fresh checks: CTest execution contracts passed; both actual Python transition-manager schedules match for 112 frames with zero error (target takeover at 101/25, completion at 101); both policy traces match observations/history/raw action/canonical targets/motion-done for 224 frames with matching official ORT 1.20.1 and zero error. Four protocol scenarios have their expected exits; suspended-owned-MC launcher cleanup exits 143 in 2.269 seconds with no survivors. The 8-second actual-ORT offline run accepted 364 targets with zero rejection/expiry.

The original research Python ORT 1.31.0 versus native ORT 1.20.1 failure remains in `evidence/reference-p1/summary.json`; tolerances were not relaxed. The matching 1.20.1 oracle is an isolated comparison environment. The earlier fixed-state CR7 joint-limit failure also remains evidence, not a successful physical test.

This checkpoint creates no Unitree SDK channels and cannot arm. Native acquisition/RC/FSM, the independent 500 Hz native sender, publication receipts and an owned loopback official-simulator closed loop are the next milestone. Original Anakin files and dirty changes are references only. No real-robot authorization or remote Git operation is implied.
