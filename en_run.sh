#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")" && pwd)
source "$repo/scripts/launch_common.sh"
# Keep shell preflight aligned with parse_startup; no process starts on a CLI error.
arguments=("$@")
declare -A seen=()
mode=''
while (( $# )); do
 flag=$1; shift
 [[ -n "$flag" ]] || launch_fail "Empty option"
 if [[ "$flag" == --ros-args ]]; then break; fi
 [[ -z ${seen[$flag]+x} ]] || launch_fail "$flag: repeated option"
 seen[$flag]=1
 case "$flag" in
  --sim|--real)
   [[ -z "$mode" ]] || launch_fail "$flag: conflicting mode selection"
   mode=$flag ;;
  --config|--input-config|--input)
   [[ $# -gt 0 && -n "$1" && "$1" != -* ]] || launch_fail "$flag: missing value"
   if [[ "$flag" == --input && "$1" != keyboard && "$1" != gamepad && "$1" != remote ]]; then
    launch_fail '--input: expected keyboard, gamepad or remote'
   fi
   shift ;;
  *) launch_fail "$flag: unknown option (use --sim or --real; test routes are scripts/integration_run.sh)" ;;
 esac
done
select_env_binary anakin_env_native_node
if [[ -z "$mode" ]]; then arguments=(--real "${arguments[@]}"); fi
exec "$binary" "${arguments[@]}"
