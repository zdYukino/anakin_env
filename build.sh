#!/usr/bin/env bash
# Standalone native CMake build; all generated files stay in this repository.
set -eo pipefail
repo=$(cd -- "$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")" && pwd)
package=anakin_env
jobs=${ANAKIN_BUILD_JOBS:-8}
build_type=RelWithDebInfo
run_tests=false
cmake_args=()
usage() {
 cat <<EOF
Usage: ./build.sh [--jobs N] [--build-type TYPE] [--test] [-- CMake options]
Build and install $package with native CMake, without a colcon workspace.
Outputs: build/$package, install/$package, log/ inside this repository.
Dependencies: ROS_PREFIX (default /opt/ros/humble), ANAKIN_SDK_PREFIX (consumers),
UNITREE_SDK2_PREFIX (Env), ONNXRUNTIME_ROOT (MC), ANAKIN_PYTHON (ROS system Python).
EOF
}
while (( $# )); do
 case "$1" in
  --help|-h) usage; exit 0 ;;
  --jobs|-j|--build-type)
   option=$1; shift
   [[ $# -gt 0 && -n "$1" ]] || { echo "$option requires a value" >&2; exit 2; }
   if [[ "$option" == --build-type ]]; then build_type=$1; else jobs=$1; fi
   shift ;;
  --test) run_tests=true; shift ;;
  --) shift; cmake_args+=("$@"); break ;;
  -D*|-U*) cmake_args+=("$1"); shift ;;
  *) echo "Unknown build option: $1 (see --help)" >&2; exit 2 ;;
 esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo '--jobs must be a positive integer' >&2; exit 2; }
ros_prefix=${ROS_PREFIX:-/opt/ros/${ROS_DISTRO:-humble}}
[[ -f "$ros_prefix/setup.bash" ]] || { echo "Missing ROS installation: $ros_prefix; see README.md" >&2; exit 2; }
source "$ros_prefix/setup.bash"
set -u
python=${ANAKIN_PYTHON:-/usr/bin/python3}
prefixes="$ros_prefix${CMAKE_PREFIX_PATH:+;${CMAKE_PREFIX_PATH//:/;}}"
extra=()
if [[ "$package" != anakin_sdk ]]; then
 sdk_prefix=${ANAKIN_SDK_PREFIX:-"$repo/../anakin_sdk/install/anakin_sdk"}
 [[ -f "$sdk_prefix/share/anakin_sdk/cmake/anakin_sdkConfig.cmake" ]] || {
  echo 'Build the locked anakin_sdk first; set ANAKIN_SDK_PREFIX for a separate checkout.' >&2; exit 2;
 }
 sdk_prefix=$(realpath -e -- "$sdk_prefix")
 export AMENT_PREFIX_PATH="$sdk_prefix:${AMENT_PREFIX_PATH:-}"
 prefixes="$sdk_prefix;$prefixes"
 extra+=("-Danakin_sdk_DIR=$sdk_prefix/share/anakin_sdk/cmake")
fi
if [[ "$package" == anakin_env ]]; then
 unitree_prefix=${UNITREE_SDK2_PREFIX:-/usr/local}
 prefixes="$unitree_prefix;$prefixes"
fi
if [[ "$package" == anakin_mc ]]; then
 ort=${ONNXRUNTIME_ROOT:-"$repo/.deps/onnxruntime-linux-x64-1.20.1"}
 [[ -f "$ort/VERSION_NUMBER" && -f "$ort/include/onnxruntime_cxx_api.h" && -f "$ort/lib/libonnxruntime.so" ]] || {
  echo 'Missing ONNX Runtime 1.20.1; run python3 scripts/setup_ort.py or set ONNXRUNTIME_ROOT.' >&2; exit 2;
 }
 ort=$(realpath -e -- "$ort")
 extra+=(-UORT_INCLUDE -UORT_LIBRARY "-DONNXRUNTIME_ROOT=$ort")
fi
mkdir -p -- "$repo/log"
log="$repo/log/build-$(date -u +%Y%m%dT%H%M%S)-$$.log"
(
 cmake -S "$repo" -B "$repo/build/$package" "${cmake_args[@]}" \
  "-DCMAKE_PREFIX_PATH=$prefixes" "-DCMAKE_INSTALL_PREFIX=$repo/install/$package" \
  "-DCMAKE_BUILD_TYPE=$build_type" "-DPython3_EXECUTABLE=$python" "-DPYTHON_EXECUTABLE=$python" \
  -DAMENT_CMAKE_ENVIRONMENT_GENERATION=ON -DBUILD_TESTING=ON "${extra[@]}"
 cmake --build "$repo/build/$package" --parallel "$jobs"
 cmake --install "$repo/build/$package"
 if [[ "$run_tests" == true && "$package" != anakin_sdk ]]; then
  ctest --test-dir "$repo/build/$package" -L offline --output-on-failure --no-tests=error
 fi
) 2>&1 | tee "$log"
printf 'Installed %s; build log: %s\n' "$package" "$log"
