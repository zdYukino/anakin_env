#!/usr/bin/env bash
set -eo pipefail
cd "$(dirname "$0")/.."
# Only source ROS/SDK into this child process; never edit the user's shell.
if [[ -f /opt/ros/humble/setup.bash ]]; then source /opt/ros/humble/setup.bash; fi
if [[ -n "${TELEMETRY_ROS_SETUP:-}" ]]; then
  source "$TELEMETRY_ROS_SETUP"
fi
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-87}"
export ROS_LOCALHOST_ONLY="${ROS_LOCALHOST_ONLY:-1}"
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
exec .venv/bin/python -m uvicorn gateway.server:app --host "${TELEMETRY_HOST:-127.0.0.1}" --port "${TELEMETRY_PORT:-8765}" "$@"
