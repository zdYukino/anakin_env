# Aerospace Robotics Telemetry Viewer

面向机器人开发和实验调试的遥测工作站，支持显式激活的网页控制。所有依赖、模型、Gateway、测试和 Trellis 计划都在本目录，与主 env 的构建和启动相互独立。

首期已实现 ROS2 动态发现与订阅、G1 29 DOF 状态模型、实时 Canvas 曲线、字段检查器、可保存布局和 CSV/JSON 本地回放。原生 DDS Adapter 与 MCAP 完整支持在后续计划中，界面不会把它们显示为已支持。

## 一键启动

在主工程根目录执行：

```bash
./ui/em_run.sh
```

脚本检查锁定依赖，缺失时安装到 ui/node_modules 和 ui/.venv，源文件变化时构建，然后启动同源 Web + ROS2 遥测/网页控制 Gateway。默认打开 **http://127.0.0.1:8765/**，Ctrl+C退出。当前workspace的SDK消息环境会自动探测；服务就绪后在界面选择“连接数据源”。脚本不启动主env、机器人或仿真。

```bash
./ui/em_run.sh --web                         # 仅演示/CSV/JSON，无需Python或ROS2
./ui/em_run.sh --host 0.0.0.0 --port 8765     # 可信局域网浏览器访问
./ui/em_run.sh --domain 87 --ros-setup /path/to/install/setup.bash
./ui/em_run.sh --no-install                  # 禁止自动安装，缺依赖时给出错误
./ui/em_run.sh --prepare                     # 只准备依赖和构建
./ui/em_run.sh --help
```

支持从任意工作目录通过脚本路径启动；显式相对ROS setup路径按调用目录解析。首次准备Gateway需要uv；已有锁定依赖后不要求uv参与每次启动。

## 界面语言

进入 **Settings → Language → 简体中文**，即可切换。中文下为 **设置 → 界面语言**，可随时切回English。选择即时生效并保存在当前浏览器，刷新恢复；切换不重连数据源、不清空曲线。Topic、消息类型、字段路径、单位及协议枚举保持原始工程含义。依赖清单见 [docs/dependencies.md](docs/dependencies.md)。

## 快速预览

完整依赖支持 Node.js `^22.22.2 || ^24.15.0 || >=26.0.0`（当前验证为24.21.0）。浏览器无需 ROS2、DDS 或 Python。

```bash
cd /home/zdyukino/workspace/anakin_env/ui
npm ci
npm run dev
```

打开 **http://127.0.0.1:5173/**，点击 **Connect source → Explore demo workspace** 查看演示。DEMO 必须主动选择，连接失败不会自动变成演示数据。

DEMO 使用可重复的合成信号，包含500 Hz样本、G1关节姿态、IMU和里程计。界面始终显示 `DEMO ACTIVE / SIMULATED DATA`。未连接时模型显示参考零位和 `AWAITING JOINT DATA`。

## 接入真实 ROS2

Gateway 主机需要 ROS2 Humble、Python 3.10、rclpy，以及目标 Topic 所使用的消息包。系统 ROS 包通过独立虚拟环境的 system-site-packages 读取，其他依赖安装在 ui/.venv，不修改主工程。

```bash
uv venv .venv --python /usr/bin/python3 --system-site-packages
uv pip sync --python .venv/bin/python gateway/requirements.txt
npm run build

TELEMETRY_ROS_SETUP=/home/zdyukino/workspace/anakin_cpp_ws/install/anakin_sdk/share/anakin_sdk/local_setup.bash \
ROS_DOMAIN_ID=87 ./scripts/gateway.sh
```

直接打开 **http://127.0.0.1:8765/**，选择 **Connect source → Connect gateway**。同源的 WebSocket 地址为 `ws://127.0.0.1:8765/ws`。在 Vite 开发界面中使用 `ws://127.0.0.1:5173/ws`，开发代理会转发到8765。

`TELEMETRY_ROS_SETUP` 可指向自己的 workspace/setup.bash 或消息包 local_setup.bash。未加载消息包时仍能看到 Topic 名称和类型，schema/订阅会返回具体加载错误。`/api/health` 可检查 Gateway 是否初始化。

局域网浏览器使用同一 Gateway 托管的构建产物：

```bash
TELEMETRY_HOST=0.0.0.0 TELEMETRY_PORT=8765 \
TELEMETRY_ROS_SETUP=/path/to/your/install/setup.bash \
ROS_DOMAIN_ID=87 ./scripts/gateway.sh
```

浏览器打开 `http://<gateway主机IP>:8765/`，连接地址改为 `ws://<gateway主机IP>:8765/ws`。Gateway 默认使用 ROS_LOCALHOST_ONLY=1，适合与 ROS 节点在同机的情形；ROS 网络跨主机时设置 `TELEMETRY_ROS_LOCALHOST_ONLY=0`，并按网络配置 domain 和 RMW。无需在浏览器设备安装 ROS。该版本面向已确认的可信局域网和单操作员，不包含账号系统。

首条数据链路为 `/anakin/state` (`anakin_sdk/msg/RobotStateFrame`)；同时可动态浏览其他已安装类型的 ROS Topic。Gateway 协议只允许发现、schema、订阅/取消和ping，没有发布控制命令、调用机器人服务或写参数的操作。

## 工作区操作

- **Overview**：中央为可拖拽旋转、滚轮缩放的 G1 模型，左侧显示主要状态，右侧按名称显示关节角度（度），可按上下肢筛选并选择关节添加曲线。中央仪表可隐藏；未连接、演示、历史帧和停止更新的最后一帧均有标记。查看其他 Topic 时仍保留对已发现的 `/anakin/state` 的订阅。
- **Workbench**：关节曲线、IMU 曲线、系统状态和数据源四个面板，不再重复展示总览中的机器人模型。拖动面板标题上的握柄交换位置，拖动列分隔条调宽；标题按钮折叠、最大化、移除面板，`Add view` 恢复。旧布局自动移除机器人面板，其他布局偏好和通道保留。
- **Topics / Inspector**：选择Topic查看endpoint、QoS、频率、带宽、源/接收时间及字段结构；数值字段旁的 `+` 添加通道，最多12条。消息尚未到达时可先查看静态schema。频率/带宽是已订阅Topic的实测值，不对未订阅流伪造频率。
- **Telemetry**：叠加、隐藏或移除通道，编辑单位；Y轴自动范围可以锁定。图表点击选择数据点并同步历史时间。图表与底部控制器共享范围/光标。
- **Controllers / 遥控器状态**：分别显示 Joy（SDL 游戏手柄）和 Unitree 遥控器的四轴、按键、主输入、输入年龄及在线/禁用/断开/超时状态。侧栏“状态切换方式”显示实际组合键、精确肩键条件和按下/按住/松开触发方式，并提供键盘与运动轴映射。侧栏来自运行节点加载的 YAML 快照，包含 `--input` 覆盖，不读取 UI 主机的默认文件，也不写死快捷键。机器人模型与关节角度保留在总览。
- **Playback**：打开/拖入记录，确认时间列/单位和分析字段后载入；用底部播放、速度、跳转和时间范围回看。支持下载示例CSV；`public/samples/g1-motion.json`可验证带命名关节的姿态回放。
- **Diagnostics**：连接事件、错误、缺值/排序提示、接收率、Gateway队列丢弃、数值buffer分配和Canvas帧耗时。
- **Settings**：英文/简体中文、工作区名、数值精度（0–8位）、科学计数法、Gateway地址与订阅可靠性。

实时选中通道保留60秒/每通道最多32,000样本。采集与约30 FPS绘图分开，曲线以每像素min/max保持尖峰。暂停显示继续采集，历史超过buffer会显示 `HISTORY EXPIRED`；`LIVE` 返回最新位置。历史模式的模型和检查器取同一时间之前最近的快照。

源时间优先；无有效header时间时明确标记接收时间。源时钟重置开启新段；不同时钟域不会静默叠加。跨主机未同步的源/接收时间差不作为精确网络延迟。缺失或无效数据使用 `— / N/A`，不合成温度、电压。

遥控状态由新版 env 独立发布 `/anakin/input_state`，无需先收到机器人反馈。Gateway 从该 Topic 的唯一发布节点读取只读参数 `input_config_yaml`、`input_config_path`、`mode_config_path` 和 `input_timeout_ms`。运行配置文件修改后，需要重启 env 才会生效；重连/切源会清除原配置，旧版 env 或配置读取失败时明确显示不可用。首次使用此功能需更新并重启 env 与 UI Gateway；不会由 UI 自动重启控制进程。历史轴/按键随共享时间轴回看，侧栏始终注明当前运行配置。

Xbox 显示 LB/RB 肩键及独立 LT/RT 模拟扳机；侧栏用 LB/L1、RB/R1 标明两种控制器的对应关系。设备详情可检查 GUID 与已应用的 SDL 映射。`gamepad.mappings` 从实际 YAML 加载，默认包含 Series X 蓝牙 5.9 的修正，避免旧 SDL 通用映射混淆右摇杆和扳机。

底部导航右侧常驻最新事件，点击可打开系统诊断的完整日志。运行节点报告输入来源、目标、当前状态、结果和原因，前端按运行会话及事件序号去重；按住急停不会每个控制周期刷一条日志。策略切换在 Preparing 时显示“进行中”，运行状态达到目标才显示“成功”，故障或被拒绝时显示“失败”，被新请求打断时显示“已取消”。成功指本地状态机确认；底部条还会显示连接等原有事件。

## 网页控制

连接实时 ROS2 数据后，右上“网页控制”可激活/关闭输入。激活后总览 02 面板列出 ENV 当前加载 YAML 的动作；当前状态与入口守卫允许的动作亮起，其余灰色。实机模式的状态流转由 ENV 决定，界面不维护另一套转换表。可打开独立可拖动的虚拟 Joy，双摇杆松开回中，肩键可点击保持组合，关闭浮层会归零。

ENV 自动在 Unitree 遥控、实体 Joy、网页之间按实际操作仲裁；中立心跳不会抢占，同周期冲突优先 Unitree > Joy > 网页。当前控制来源在指令面板显示。每次接管清除运动开关；所有已启用来源的急停始终有效。网页租约仅允许一个浏览器，断连、失焦、隐藏或历史回放释放，重新连接需显式激活。普通 publish/service/参数写入继续拒绝；网关只转发专用网页输入到 ENV，浏览器页面需与网关同源。

终端键盘控制已完全删除。ENV 和 Gateway 更新后需重启加载新接口；正常域 87 的现有运行进程不会被开发验证自动重启。

## 离线格式

文件完全在浏览器本地处理，Worker解析，不上传Gateway。上限50 MB、1,000,000条记录、同时12条选中曲线。选择时间单位时可用秒/elapsed、毫秒、纳秒、ISO 8601或ROS sec+nanosec。纳秒大整数建议以字符串保存在JSON中。

CSV为唯一列名、时间列和数值列，支持逗号/分号、带引号的逗号/换行及双引号转义：

```csv
elapsed,left_hip_position,left_knee_position
0.000,-0.120,0.320
0.002,-0.119,0.322
```

JSON支持记录数组，或 `{ "messages": [...] }`；每项可以是平坦数值行，也可以是带topic/data的消息。含原始命名关节结构时会驱动模型：

```json
[
  {"timestamp":0.0,"topic":"/anakin/state","data":{"joints":{"name":["left_knee_joint"],"position":[0.32]}}},
  {"timestamp":0.1,"topic":"/anakin/state","data":{"joints":{"name":["left_knee_joint"],"position":[0.35]}}}
]
```

缺失/非法时间会阻止导入；逆序时间排序并提示，重复时间按原顺序保留，非数值字段不画曲线。快照最多约30Hz，数值通道保留文件内选中字段的全部有效样本。若要分析导入时未选中的字段，重新打开文件并选择该字段；不会用低频快照冒充其全速数据。仅有数值列且无JointState名称的CSV不会被猜测为机器人姿态。

## 验证

```bash
npm run typecheck
npm test
npm run build
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/python -m pytest tests/test_gateway.py -q

# 源 ROS/SDK 环境后，独立运行 env（无机器人反馈/禁用 LowCmd 输出），验证输入和生效 YAML
.venv/bin/python scripts/verify_inputs.py

# 隔离 env + 网页租约，禁用所有 LowCmd 输出；验证状态指令/灰色动作拒绝，可选监视 Xbox
.venv/bin/python scripts/verify_inputs.py --events --gamepad --preview-seconds 30

# 真实rclpy受控发布器 → Gateway → WebSocket，独立ROS domain
.venv/bin/python scripts/verify_ros.py --duration 60 --ros-domain 194 --port 8767

# 经用户授权，启动workspace内官方Unitree MuJoCo和现有env；不启动MC、不发送控制
.venv/bin/python scripts/verify_ros.py --mujoco --duration 60 --ros-domain 193 --dds-domain 39 --port 8766
```

MuJoCo验证需要旁边的 `unitree_mujoco` 已构建、现有 env 已安装、SDK消息包和 Xvfb（脚本也支持workspace内现成Xvfb）。测试只启动自己拥有的进程并清理；不修改外部仓库，不与当前87域会话混用。env保持DISARMED，核验 `execution_authorized=false` 和 `sdk_publish_count=0`。

ROS系统自带的launch_testing插件与虚拟环境pytest版本不匹配，因此测试明确禁用自动插件加载。验证结果写入不跟踪的 `artifacts/`；提交的验证摘要见 [docs/verification.md](docs/verification.md)。受控ROS源已验证约500Hz；MuJoCo经现有env的实际发布率较低，报告单独列出，不当成Gateway满速证明。

## 架构和计划

`gateway/protocol.py`定义只读适配器边界和有界队列，`gateway/ros_adapter.py`独占ROS执行器，`gateway/server.py`提供HTTP/WebSocket。前端数据源/时间/缓冲在`src/core/`，工作站组件在`src/components/`。模型资产及Unitree BSD 3-Clause许可在`public/models/g1/`，转换脚本为`scripts/convert_model.py`。

Trellis需求、技术设计和四个子任务在`.trellis/tasks/11-03-aerospace-telemetry/`。后续按相同Adapter接口扩展原生DDS，再扩展MCAP的schema/encoding/压缩和seek，继续保持浏览器不参与DDS网络。
