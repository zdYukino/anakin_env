#!/usr/bin/env bash
set -eo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ws=${ANAKIN_WS:-"$repo/../anakin_cpp_ws"}
sdk_prefix=${ANAKIN_SDK_PREFIX:-"$ws/install/anakin_sdk"}
[[ -f "$sdk_prefix/share/anakin_sdk/cmake/anakin_sdkConfig.cmake" ]] || { echo 'Build the locked anakin_sdk first; set ANAKIN_SDK_PREFIX' >&2; exit 2; }
source /opt/ros/humble/setup.bash
export AMENT_PREFIX_PATH="$sdk_prefix:${AMENT_PREFIX_PATH:-}"
export CMAKE_PREFIX_PATH="$sdk_prefix:${CMAKE_PREFIX_PATH:-}"
cd -- "$repo"
colcon build --cmake-force-configure --base-paths "$repo" --event-handlers console_direct+ --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo  "$@"
