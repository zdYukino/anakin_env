# Send-decision P2 checkpoint, 2026-10-27

This checkpoint closes the narrow communication-free send-decision regression gap. It does not close the open execution-budget/full-send-path P1, enable A/B1, or establish safe dynamics. The parent-provided independent review accepted the preceding v3 checkpoint only with PD_REFERENCE disabled. This new P2 refactor awaits its own review.

## Actual production path

NativeNode::write_command now calls the shared guarded_send implementation in include/anakin_env/send.hpp. Tests inject a controlled clock, observation changes, the encoder and a recording-only transport into that same implementation. There is no parallel test-only send gate. Production still owns the real transport; the test does not construct it, start ROS, initialize an SDK channel or launch a simulator.

The shared path performs admission, encoding, fresh admission, then a single writer invocation. It checks the existing 40 ms state and 250 ms RC ages, the source-derived target deadline, authorization generation, explicit stop/revoke and the real damping/exit deadline. A state-dependent reference encoder binds its feedback generation and acquisition time; changed feedback discards that attempt and requires a later explicit slot. It never retries/bursts inside guarded_send. Generation checks are not a physics envelope.

NativeNode snapshots the attempted target by value so local revocation during an encoding failure cannot invalidate the pointer used by that attempt. Local stop, new request, deliberate stand authorization and final shutdown advance the send generation. An old attempt cannot be revived by a later authorization. A fresh A/B stop observed after encoding discards a position call. Expired RC data remains a freshness fault even if it also carries a stop bit.

The sender's nominal 2 ms loop uses the same PeriodicSendSchedule tested for skipped slots. Exit damping retains the real-clock DeadlineSchedule, with the absolute deadline rechecked in the shared send path. The tests count actual recorded position/damping packet types and writer invocations separately from successful receipts; transport false/exception cannot increment a successful receipt.

A transport invocation already in progress cannot be retroactively cancelled. The test records that limit explicitly and verifies that revocation prevents the next invocation. This is a software admission/call-count property, not proof of physical command arrival, actuator cancellation, or a device watchdog. Production serializes control/transaction mutation with its existing mutex; asynchronous sample changes are observed between stages, not made atomic with the SDK/device.

## Finite tests

Evidence directory: /home/zdyukino/workspace/anakin_cpp_ws/evidence/send-p2-20261027T162758Z/ . Final results are results.json and send-test.json; send-network-syscalls.log traces the recording test's network syscalls. Final integrated and standalone env CTest each report 4/4. The recording send test makes zero network syscalls under strace. The earlier 20-case evidence is preserved separately in initial-20/.

The 21 controlled-clock cases cover:

- One healthy position invocation with a CRC-valid packet.
- Target, state and RC expiry after encoding, including stale encoded feedback hidden behind a newer observer sample.
- A fresh feedback generation change, with no send or same-call retry, followed by a new explicit encoding attempt.
- Feedback integrity changes and the actual frozen missing-budget exception before transport.
- Stop/revoke during encoding, newly observed A/B, stale RC plus A/B priority, old attempts after stop, and non-revival after a new authorization.
- Local alignment/hold authorization admission, post-encoding damping expiry, absolute shutdown deadline and a copied true damping flag that cannot extend its deadline.
- Writer rejection/exception, stop during an already-entered writer, and no subsequent call.
- Worker missed-slot skipping without catch-up writes and delayed damping writes only at 0/2/86 ms, none at or beyond 100 ms.

Only builds and communication-free tests run in this turn. No native application, ROS context, SDK channel, floating simulation or new A/B1 experiment is started. The frozen budget JSON, reference encoder and actuation-limit tables are byte-identical to the accepted disabled-v3 checkpoint. Earlier native/simulation evidence remains historical and is not relabeled as a run of this refactor.

## P1 remains open

Reference budgeting still applies only in the target && PD_REFERENCE encoder branch. Alignment, hold, preparing/fallback and stopping damping have not passed unified budget acceptance. The shared timing/authorization gate does not supply those missing dynamic checks.

q + .002*dq only predicts one nominal period at the sampled state. It does not cover the allowed 40 ms feedback age, acceleration, delay or stopping dynamics. Sampled expected virtual/encoded effort does not bound the bridge's subsequent actual effort while q/dq evolve, nor prove an acceptable sustained-saturation envelope. No broader safety inference is made from the B1 algebra or these send-count tests.

Both PD_REFERENCE profiles remain refused before NativeTransport::start_loopback. No budget values or enabling flags were changed. Completing the three engineering definitions and unifying every send mode remains necessary before a new reviewed execution-profile revision and any active A/B1 fault/float/switch experiment. See engineering-budget-requirements.md for the concise configuration request.
