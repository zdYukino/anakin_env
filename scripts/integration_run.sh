#!/usr/bin/env bash
set -euo pipefail
script=$(realpath -- "${BASH_SOURCE[0]}")
repo=$(cd -- "$(dirname -- "$script")/.." && pwd)
ws=${ANAKIN_WS:-"$repo/../anakin_cpp_ws"}
ws=$(cd -- "$ws" && pwd)
demo=false
duration=8.0
if [[ ${1:-} == --demo-switch ]]; then demo=true; duration=12.0; shift; fi
if (( $# )); then echo 'Usage: ./en_run.sh [--demo-switch]' >&2; exit 2; fi
export ANAKIN_WS="$ws"
export ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-87}
run="$ws/evidence/run-$(date -u +%Y%m%dT%H%M%S)-$$"
mkdir -- "$run"
pids=()
launcher_pid=$BASHPID
owned_running() {
 local child=$1
 [[ -r "/proc/$child/status" ]] || return 1
 grep -Eq "^PPid:[[:space:]]+${launcher_pid}$" "/proc/$child/status" || return 1
 ! grep -Eq '^State:[[:space:]]+Z' "/proc/$child/status"
}
cleanup() {
 local result=$?
 trap - EXIT INT TERM
 local signal child round alive
 for signal in INT TERM KILL; do
  for child in "${pids[@]}"; do
   if owned_running "$child"; then kill -"$signal" "$child" 2>/dev/null || true; fi
  done
  for ((round=0; round<20; ++round)); do
   alive=false
   for child in "${pids[@]}"; do if owned_running "$child"; then alive=true; fi; done
   if [[ "$alive" == false ]]; then break; fi
   sleep 0.05
  done
 done
 for child in "${pids[@]}"; do
  if owned_running "$child"; then echo "Cleanup deadline exceeded for owned child $child" >&2; ((result)) || result=1
  else wait "$child" 2>/dev/null || true; fi
 done
 exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
ANAKIN_PREFIX="$ws/install/anakin_mc" "$ws/../anakin_mc/en_run.sh" >"$run/mc.log" 2>&1 &
mc_pid=$!; pids+=("$mc_pid")
ANAKIN_PREFIX="$ws/install/anakin_env" "$ws/../anakin_env/en_run.sh" -p "duration:=$duration" -p "demo_switch:=$demo" >"$run/env.log" 2>&1 &
env_pid=$!; pids+=("$env_pid")
set +e
wait -n -p finished "$mc_pid" "$env_pid"
result=$?
set -e
if [[ "$finished" != "$env_pid" ]]; then echo 'MC exited before env completion' >&2; ((result)) || result=1; fi
printf 'Run evidence: %s\n' "$run"
tail -8 -- "$run/env.log"
exit "$result"
