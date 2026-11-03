#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")" && pwd)
source "$repo/scripts/launch_common.sh"
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
 echo 'Usage: ./em_run.sh [--sim|--real] [--real-checks [--real-config PATH]] [--config PATH] [--input-config PATH] [--input auto|web|gamepad|remote] [--ros-args ...]'
 echo 'Starts Env only (default real). ANAKIN_PREFIX and ANAKIN_SDK_PREFIX select installations.'
 exit 0
fi
# Keep shell preflight aligned with parse_startup; no process starts on a CLI error.
arguments=("$@")
declare -A seen=()
mode=''
real_checks=false
real_config=false
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
  --real-checks) real_checks=true ;;
  --config|--input-config|--real-config|--input)
   [[ $# -gt 0 && -n "$1" && "$1" != -* ]] || launch_fail "$flag: missing value"
   if [[ "$flag" == --real-config ]]; then real_config=true; fi
   if [[ "$flag" == --input && "$1" != auto && "$1" != web && "$1" != gamepad && "$1" != remote ]]; then
    launch_fail '--input: expected auto, web, gamepad or remote'
   fi
   shift ;;
  *) launch_fail "$flag: unknown option (use --sim or --real; test routes are scripts/integration_run.sh)" ;;
 esac
done
if [[ "$real_checks" == true && "$mode" != --sim ]]; then launch_fail '--real-checks requires --sim'; fi
if [[ "$real_config" == true && "$real_checks" != true ]]; then launch_fail '--real-config requires --real-checks'; fi
select_env_binary anakin_env_native_node
if [[ -z "$mode" ]]; then arguments=(--real "${arguments[@]}"); fi
if [[ ${ANAKIN_LAUNCH_CHECK_ONLY:-0} == 1 ]]; then exit 0; fi
mkdir -p -- "$ROS_LOG_DIR"
exec "$binary" "${arguments[@]}"
