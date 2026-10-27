# Architecture comparison, 2026-10-27

Parent-provided read-only research inspected `/home/zdyukino/下载/zdev.zip`, SHA256 `52ce1f428d903ae20f05af8e11999df32d8fa7bf38a9819e1b14a42ce841deee`. The archive was not executed or copied into this project. It has no whole-project LICENSE and no models/mc_param submodule.

Useful concepts are an explicit lifecycle, Load/Predict/Release boundaries, per-model observation/history context, ROS adapters separated from policy core, monotonic absolute scheduling and shell-to-C++ execution. We retain the approved ROS2 interface repository rather than adopting AIMRT/private protocols or dynamic module loading. The current Policy core already owns its own history and inference session; the experimental switch/mux will be extracted from the ROS adapter during the next milestone.

Do not copy zdev's ready boolean, early RUNNING state, synchronous warmup in control loops, shared body-part outputs, index/min(size) action blending, map[] fallback to joint zero, autonomous controller mode switching or environment-variable-driven real/sim selection. Our asynchronous accepted response, cumulative phase/sequence status, env authorization and canonical29 single publisher remain authoritative.
