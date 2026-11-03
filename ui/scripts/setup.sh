#!/usr/bin/env bash
set -euo pipefail
ui_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
exec "$ui_root/em_run.sh" --prepare "$@"
