#!/usr/bin/env bash
# Package-local selection shared by the native entry and explicit test entry.
launch_fail() { echo "anakin_env: $*" >&2; exit 2; }
read_revision() {
 local path=$1
 local -a lines=()
 [[ -r "$path" ]] || launch_fail "Missing SDK provenance: $path"
 mapfile -t lines < "$path"
 [[ ${#lines[@]} == 1 && ${lines[0]} =~ ^[0-9a-f]{40}$ ]] || launch_fail "Invalid SDK provenance: $path"
 revision_value=${lines[0]}
}
select_env_binary() {
 local name=$1 installed_revision='' line revision_pattern ignored_pattern
 sdk_prefix=${ANAKIN_SDK_PREFIX:-"$repo/../anakin_sdk/install/anakin_sdk"}
 local default_prefix="$repo/install/anakin_env"
 prefix=${ANAKIN_PREFIX:-"$default_prefix"}
 prefix=$(realpath -e -- "$prefix") || launch_fail "Missing selected Env prefix: $prefix"
 sdk_prefix=$(realpath -e -- "$sdk_prefix") || launch_fail "Missing selected SDK prefix: $sdk_prefix"
 [[ -f "$prefix/share/ament_index/resource_index/packages/anakin_env" ]] || launch_fail "Selected Env prefix has no local package marker"
 [[ -f "$sdk_prefix/share/ament_index/resource_index/packages/anakin_sdk" ]] || launch_fail "Selected SDK prefix has no local package marker"
 binary="$prefix/lib/anakin_env/$name"
 [[ -f "$binary" && -x "$binary" ]] || launch_fail "Missing C++ binary: $binary"
 local canonical_binary
 canonical_binary=$(realpath -e -- "$binary") || launch_fail "Invalid selected ELF: $binary"
 [[ "$canonical_binary" == "$binary" ]] || launch_fail "Selected ELF escapes its install prefix: $binary"
 [[ $(od -An -tx1 -N4 -- "$binary" | tr -d ' \n') == 7f454c46 ]] || launch_fail "Selected executable is not an ELF: $binary"
 for line in sim.yaml real.yaml inputs.yaml; do
  [[ -f "$prefix/share/anakin_env/config/$line" ]] || launch_fail "Missing selected Env resource: $line"
 done
 read_revision "$repo/anakin_sdk.revision"
 sdk_revision=$revision_value
 read_revision "$prefix/share/anakin_env/runtime-sdk.revision"
 [[ "$revision_value" == "$sdk_revision" ]] || launch_fail 'Binary was built against another SDK revision; rebuild this prefix'
 local metadata="$sdk_prefix/share/anakin_sdk/cmake/revision.cmake"
 [[ -r "$metadata" ]] || launch_fail "Missing installed SDK provenance: $metadata"
 revision_pattern='^set\(ANAKIN_SDK_REVISION "([0-9a-f]{40})"\)$'
 ignored_pattern='^[[:space:]]*(#|$)'
 while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ "$line" =~ $ignored_pattern ]]; then continue; fi
  [[ "$line" =~ $revision_pattern ]] || launch_fail 'Invalid installed SDK revision declaration'
  [[ -z "$installed_revision" ]] || launch_fail 'Duplicate installed SDK revision'
  installed_revision=${BASH_REMATCH[1]}
 done < "$metadata"
 [[ "$installed_revision" == "$sdk_revision" ]] || launch_fail 'Installed SDK revision mismatch'
 export ANAKIN_PREFIX="$prefix" ANAKIN_SDK_PREFIX="$sdk_prefix"
 export ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-87}
 export ROS_LOCALHOST_ONLY=1 RMW_IMPLEMENTATION=rmw_fastrtps_cpp
 local ros_prefix=${ROS_PREFIX:-/opt/ros/${ROS_DISTRO:-humble}}
 local unitree_prefix=${UNITREE_SDK2_PREFIX:-/usr/local}
 export AMENT_PREFIX_PATH="$sdk_prefix:$prefix:$ros_prefix${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
 export LD_LIBRARY_PATH="$sdk_prefix/lib:$prefix/lib:$unitree_prefix/lib:$unitree_prefix/lib64:$ros_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
 export ROS_LOG_DIR=${ROS_LOG_DIR:-"$repo/log/ros"}
}
