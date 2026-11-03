#!/usr/bin/env bash
# Independent telemetry workstation launcher. Never starts the robot/env/simulator.
set -euo pipefail
ui_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
caller_dir=$PWD
cd "$ui_root"
web_only=false
prepare_only=false
install_allowed=true
force_build=false
launch_host=${TELEMETRY_HOST:-127.0.0.1}
launch_port=${TELEMETRY_PORT:-8765}
ros_setup=${TELEMETRY_ROS_SETUP:-}
ros_domain=${ROS_DOMAIN_ID:-87}
fail() { printf '启动失败 / Startup failed: %s\n' "$*" >&2; exit 2; }
usage() {
  cat <<'HELP'
Aerospace Robotics Telemetry Viewer
用法 / Usage: ./ui/em_run.sh [options]

前置：Node.js ^22.22.2 / ^24.15.0 / >=26，npm；Gateway另需Python 3.10。

默认：准备依赖、按需构建、启动 Web + 只读 ROS2 Gateway。
Default: prepare locked dependencies, build when needed, serve Web + ROS2 gateway.

  --web              仅 Web（DEMO / CSV / JSON），无需 Python / ROS2
  --host HOST        绑定地址（默认 127.0.0.1；局域网可用 0.0.0.0）
  --port PORT        HTTP / WebSocket 端口（默认 8765）
  --ros-setup FILE   加载指定 ROS 消息工作区 setup.bash / local_setup.bash
  --domain ID        ROS_DOMAIN_ID（默认 87）
  --no-install       缺少锁定依赖时退出，不自动安装
  --rebuild          强制重新构建前端
  --prepare          仅安装/构建，不启动服务
  -h, --help         显示本帮助

示例：./ui/em_run.sh
      ./ui/em_run.sh --web
      ./ui/em_run.sh --host 0.0.0.0 --port 8765 --domain 87

仅启动本平台，不启动主 env、机器人或 MuJoCo。Ctrl+C 退出。
HELP
}
need_value() { [[ $# -ge 2 && -n $2 ]] || fail "$1 需要一个值 / requires a value"; }
while (($#)); do
  case "$1" in
    --web) web_only=true; shift ;;
    --prepare) prepare_only=true; shift ;;
    --no-install) install_allowed=false; shift ;;
    --rebuild) force_build=true; shift ;;
    --host) need_value "$@"; launch_host=$2; shift 2 ;;
    --port) need_value "$@"; launch_port=$2; shift 2 ;;
    --ros-setup) need_value "$@"; ros_setup=$2; shift 2 ;;
    --domain) need_value "$@"; ros_domain=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) fail "未知选项 / unknown option: $1 (use --help)" ;;
  esac
done
[[ $launch_port =~ ^[0-9]{1,5}$ ]] || fail '端口应为 1–65535 / port must be 1–65535'
launch_port=$((10#$launch_port))
((launch_port >= 1 && launch_port <= 65535)) || fail '端口应为 1–65535 / port must be 1–65535'
if ! $web_only; then
[[ $ros_domain =~ ^[0-9]{1,3}$ ]] || fail 'ROS domain 应为 0–232 / must be 0–232'
ros_domain=$((10#$ros_domain))
((ros_domain <= 232)) || fail 'ROS domain 应为 0–232 / must be 0–232'
[[ -n $launch_host ]] || fail '绑定地址不能为空 / host cannot be empty'
if [[ -n $ros_setup ]]; then
  [[ $ros_setup == /* ]] || ros_setup="$caller_dir/$ros_setup"
  [[ -r $ros_setup ]] || fail "无法读取 ROS setup / cannot read: $ros_setup"
  ros_setup=$(realpath -- "$ros_setup")
else
  sdk_workspace=${ANAKIN_WS:-"$ui_root/../../anakin_cpp_ws"}
  [[ $sdk_workspace == /* ]] || sdk_workspace="$caller_dir/$sdk_workspace"
  candidate="$sdk_workspace/install/anakin_sdk/share/anakin_sdk/local_setup.bash"
  if [[ -r $candidate ]]; then ros_setup=$(realpath -- "$candidate"); fi
fi
fi
command -v node >/dev/null || fail '需要 Node.js 24.15+ (24.x)、22.22.2+ (22.x) 或 >=26 / compatible Node.js required'
command -v npm >/dev/null || fail '需要 npm / npm is required'
node -e 'const [major,minor,patch]=process.versions.node.split(".").map(Number);process.exit((major===22&&(minor>22||(minor===22&&patch>=2)))||(major===24&&minor>=15)||major>=26?0:1)' || fail '需要 Node.js 24.15+ (24.x)、22.22.2+ (22.x) 或 >=26 / compatible Node.js required'

# Check lockfile contents rather than reinstalling everything on every launch.
if ! node --input-type=module - <<'JS'
import fs from 'node:fs';
try {
  const lock=JSON.parse(fs.readFileSync('package-lock.json','utf8'));
  const pkg=JSON.parse(fs.readFileSync('package.json','utf8'));
  for (const group of ['dependencies','devDependencies']) {
    const declared=pkg[group]||{}, resolved=lock.packages[''][group]||{};
    if(Object.keys(declared).length!==Object.keys(resolved).length||Object.entries(declared).some(([name,version])=>resolved[name]!==version))process.exit(1);
  }
  for(const [path,entry] of Object.entries(lock.packages)) {
    if(!path)continue;
    const installed=path+'/package.json';
    if(!fs.existsSync(installed)){if(entry.optional)continue;process.exit(1);}
    if(JSON.parse(fs.readFileSync(installed,'utf8')).version!==entry.version)process.exit(1);
  }
} catch { process.exit(1); }
JS
then
  $install_allowed || fail '前端依赖未就绪；去掉 --no-install 自动安装 / frontend dependencies missing'
  printf '[1/3] 安装锁定的前端依赖 / Installing frontend dependencies…\n'
  npm ci --no-audit --no-fund
else
  printf '[1/3] 前端依赖已就绪 / Frontend dependencies ready\n'
fi

if ! $web_only; then
  python_bin=${TELEMETRY_PYTHON:-/usr/bin/python3}
  command -v "$python_bin" >/dev/null || fail '需要 Python 3.10；仅前端请用 --web / Python 3.10 required, or use --web'
  "$python_bin" -c 'import sys;sys.exit(0 if sys.version_info[:2]==(3,10) else 1)' || fail 'ROS2 Humble Gateway 需要 Python 3.10；仅前端请用 --web'
  if [[ ! -x .venv/bin/python ]]; then
    $install_allowed || fail '缺少 ui/.venv；去掉 --no-install 自动创建 / virtual environment missing'
    command -v uv >/dev/null || fail '首次准备 Gateway 需要 uv / uv is required to prepare the gateway'
    uv venv .venv --python "$python_bin" --system-site-packages
  fi
  .venv/bin/python -c 'import sys;sys.exit(0 if sys.version_info[:2]==(3,10) else 1)' || fail '现有 ui/.venv 不是 Python 3.10，请重新准备该环境 / existing venv must use Python 3.10'
  if ! .venv/bin/python - <<'PY'
import importlib.metadata
import sys
from pathlib import Path
if sys.version_info[:2] != (3, 10):
    sys.exit(1)
try:
    for line in Path('gateway/requirements.txt').read_text().splitlines():
        if not line or line.startswith((' ', '#')):
            continue
        name, version = line.split('==', 1)
        if importlib.metadata.version(name) != version:
            sys.exit(1)
    import fastapi, uvicorn, websockets
except (ImportError, importlib.metadata.PackageNotFoundError, ValueError):
    sys.exit(1)
PY
  then
    $install_allowed || fail 'Gateway 依赖未就绪；去掉 --no-install 自动安装 / gateway dependencies missing'
    command -v uv >/dev/null || fail '安装 Gateway 依赖需要 uv / uv is required'
    uv pip sync --python .venv/bin/python gateway/requirements.txt
  fi
  printf '[2/3] Gateway 依赖已就绪 / Gateway dependencies ready\n'
else
  printf '[2/3] 仅 Web 模式 / Web-only mode (no ROS2 / Python gateway)\n'
fi

# Hash actual build inputs, including model assets, so stale dist is never silently served.
build_fingerprint=$(node --input-type=module - <<'JS'
import fs from 'node:fs';import path from 'node:path';import crypto from 'node:crypto';
const hash=crypto.createHash('sha256');hash.update(process.versions.node);
function walk(file){const stat=fs.statSync(file);if(stat.isDirectory()){fs.readdirSync(file).sort().forEach(name=>walk(path.join(file,name)));}else{hash.update(file);hash.update(fs.readFileSync(file));}}
['src','public','index.html','package.json','package-lock.json','tsconfig.json','vite.config.ts'].forEach(walk);
process.stdout.write(hash.digest('hex'));
JS
)
saved_fingerprint=''
if [[ -r dist/.aerospace-build.sha256 ]]; then read -r saved_fingerprint < dist/.aerospace-build.sha256 || true; fi
if $force_build || [[ ! -f dist/index.html || $build_fingerprint != "$saved_fingerprint" ]]; then
  printf '[3/3] 构建前端 / Building frontend…\n'
  npm run build
  printf '%s\n' "$build_fingerprint" > dist/.aerospace-build.sha256
else
  printf '[3/3] 复用当前构建 / Reusing current build\n'
fi
if $prepare_only; then printf '\n准备完成 / Ready. 启动 / Run: ./ui/em_run.sh\n'; exit 0; fi
url_host=$launch_host
[[ $url_host != 0.0.0.0 ]] || url_host=127.0.0.1
if [[ $url_host == *:* ]]; then url_host="[$url_host]"; fi
printf '\nAerospace Robotics Telemetry Viewer\n浏览器打开 / Open: http://%s:%s/\n退出 / Stop: Ctrl+C\n\n' "$url_host" "$launch_port"
if $web_only; then
  exec node node_modules/vite/bin/vite.js preview --host "$launch_host" --port "$launch_port" --strictPort
fi
export TELEMETRY_HOST=$launch_host TELEMETRY_PORT=$launch_port ROS_DOMAIN_ID=$ros_domain
if [[ -n $ros_setup ]]; then export TELEMETRY_ROS_SETUP=$ros_setup; printf 'ROS messages: %s\n' "$ros_setup"; fi
exec "$ui_root/scripts/gateway.sh"
