# 当前依赖

版本从本项目package.json、package-lock.json和gateway/requirements.txt核对。语言选项使用项目自己的集中翻译目录，无新增国际化运行库。

## 运行位置与前置

| 使用场景 | 需要 |
|---|---|
| 浏览器访问 | 现代浏览器，支持WebGL；中文使用浏览器所在系统的CJK字体回退 |
| 一键启动/前端构建 | Node.js `^22.22.2 || ^24.15.0 || >=26.0.0`、npm；当前验证24.21.0 / npm11.19.0 |
| 只读ROS2 Gateway | Python3.10、ROS2 Humble、rclpy、rosidl_runtime_py/rosidl_parser和所订阅消息包；首次安装项目Python依赖需要uv |
| 现有 /anakin/state | 已构建的anakin_sdk消息包；脚本探测相邻workspace或通过--ros-setup指定环境 |
| MuJoCo验收（可选） | 已构建的unitree_mujoco、现有env、SDK和Xvfb，仅验证脚本使用 |

Node版本范围同时满足当前Vite/Vitest/jsdom。Vite自身的最低版本限制较低，但完整开发测试依赖中的jsdom30.1.3要求 `^22.22.2 || ^24.15.0 || >=26.0.0`，因此统一采用该范围。

仅演示和本地文件时使用 `./ui/em_run.sh --web`，不需要Python或ROS；浏览器设备不安装ROS/DDS。预先构建的dist还可由现有 `scripts/gateway.sh` 托管，该服务进程本身不依赖Node。

## 前端运行依赖（锁定）

| 依赖 | 版本 | 用途 |
|---|---|---|
| react / react-dom | 19.3.1 | 界面与状态订阅 |
| three | 0.187.0 | G1网格、运动学和3D视图 |
| lucide-react | 1.66.0 | 图标 |
| @fontsource/inter | 5.3.0 | 本地UI字体 |
| @fontsource/ibm-plex-mono | 5.3.0 | 本地等宽数字字体 |

## 前端开发/测试依赖（锁定）

TypeScript7.0.2、Vite8.3.5、@vitejs/plugin-react6.2.1、Vitest5.1.0、jsdom30.1.3、@testing-library/react16.3.3、@testing-library/user-event14.6.8、Prettier3.9.10，以及React/Three/Node的类型定义。精确完整列表在package.json；传递依赖在package-lock.json。

## Gateway依赖（锁定）

| 依赖 | 版本 | 用途 |
|---|---|---|
| FastAPI | 0.162.0 | HTTP/WebSocket接口 |
| Uvicorn | 0.54.0 | ASGI服务 |
| websockets | 16.1.1 | WebSocket传输及验收客户端 |
| httpx | 0.28.1 | API测试 |
| pytest | 9.2.0 | 测试 |

Pydantic、Starlette、AnyIO、uvloop、httptools等传递依赖也固定于gateway/requirements.txt。rclpy、ROS消息类型由系统ROS/SDK提供，不通过pip安装进浏览器或替换主工程环境。

一键脚本使用npm ci和uv pip sync安装锁定依赖，不自动安装系统ROS、Node或MuJoCo。所有项目安装产物均位于ui/，node_modules/.venv/dist/artifacts不提交。
