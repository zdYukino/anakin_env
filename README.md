# anakin_env

Native C++ execution authority. Independent ROS 2 Humble ament package.

Depends on the exact `anakin_sdk` revision in `anakin_sdk.revision`. Build with the same installed SDK prefix, or symlink all three independent repositories into `../anakin_cpp_ws/src` and run its build.sh.

Run `./en_run.sh` after building. Default ROS domain is 87 and discovery stays on localhost. See [checkpoint scope](docs/checkpoint.md). No source build, dependency install, Python node or Python environment script runs in en_run.sh.

Independent build (after SDK is installed): `ANAKIN_SDK_PREFIX=/path/to/sdk/install/anakin_sdk ./build.sh`. MC additionally accepts `ONNXRUNTIME_ROOT=/path/to/official/native/ort`. The default build uses the adjacent shared workspace's SDK and ORT dependency prefixes. Per-repository en_run.sh prefers that repository's standalone install when present; override ANAKIN_PREFIX to select a specific install.

## Native review checkpoint

The current C++ native implementation is restricted to explicit loopback simulation. Read [docs/native-checkpoint.md](docs/native-checkpoint.md) for the exact commands, current passing contracts, native RC-loss verification and the preserved floating-policy bound failure. Sustained native policy execution and the full native loco/mimic cycle are not accepted. `./en_run.sh` continues to exec ELF C++ binaries with locked SDK build provenance and no Python startup.
