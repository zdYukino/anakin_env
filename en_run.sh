#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ws=${ANAKIN_WS:-"$repo/../anakin_cpp_ws"}
ws=$(cd -- "$ws" && pwd)
sdk_prefix=${ANAKIN_SDK_PREFIX:-"$ws/install/anakin_sdk"}
if [[ -d "$repo/install/anakin_env" ]]; then default_prefix="$repo/install/anakin_env"; else default_prefix="$ws/install/anakin_env"; fi
prefix=${ANAKIN_PREFIX:-"$default_prefix"}
read -r sdk_revision < "$repo/anakin_sdk.revision"
grep -Fq -- "$sdk_revision" "$sdk_prefix/share/anakin_sdk/cmake/revision.cmake" || { echo 'SDK revision mismatch' >&2; exit 2; }
binary="$prefix/lib/anakin_env/anakin_env_node"
[[ -x "$binary" ]] || { echo "Missing C++ binary: $binary" >&2; exit 2; }
export ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-87}
export ROS_LOCALHOST_ONLY=1
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export AMENT_PREFIX_PATH="$sdk_prefix:$prefix:/opt/ros/humble${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
export LD_LIBRARY_PATH="$sdk_prefix/lib:$prefix/lib:/opt/ros/humble/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$binary" --ros-args "$@"
