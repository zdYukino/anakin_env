# Aerospace Robotics Telemetry Viewer

## 目标

面向机器人开发、实验测试和算法调试的浏览器遥测工作站。通过独立只读 Gateway 接入真实 ROS2，联动观察机器人姿态、消息字段和同步曲线，导入离线记录分析。视觉服务于工程数据读取。

## 已确认的产品决定

| 决定 | 用户确认 |
|---|---|
| Q1 首期 | 可运行 ROS2 MVP；原生 DDS Adapter 和 MCAP 完整支持进入后续阶段 |
| Q2 权限 | 只读；显示控制指令 Topic，不发布命令、不修改参数 |
| Q3 部署 | 单操作员，本机/可信局域网；Gateway 在 ROS 主机，浏览器可跨设备；无账号、多租户、公网托管 |
| Q4 语言 | 英文/简体中文可切换，中文文档（用户后续明确追加中文选项） |
| Q5 机器人 | G1 29 DOF，首条链路 /anakin/state；将指定目录的 MJCF/STL 转为 ui 内自包含浏览器模型，按 JointState 关节名称绑定 |
| Q6 规模 | 1 台机器人、约 20 Topic、选中数值字段最高 500 Hz、12 曲线、60 秒缓冲、约 30 FPS 绘图、CSV/JSON ≤50 MB、1366×768 起适配 |
| Q7 时间 | 源时间用于曲线/回放，接收时间用于诊断；共享时间光标；暂停显示继续采集；历史与 LIVE 明确区分 |
| Q8 演示 | 主动选择的独立 DEMO，可重复数据；连接失败不自动切换 DEMO |

## 仓库事实与约束

- 全部新内容在仓库 ui/；不修改主工程 CMake、ROS 节点、启动脚本及已有 README/.vscode 改动，无嵌套 Git 仓库。
- ROS2 Humble；en_run.sh 默认 domain 87、localhost only、Fast DDS。Gateway 需要匹配 ROS domain 并加载 SDK 消息工作区。
- /anakin/state 类型 anakin_sdk/msg/RobotStateFrame，含 JointState、IMU、Odometry、诊断等；best effort / volatile / depth 1。另有 /anakin/control_target 和 /anakin/inference_status。
- 模型源为 /home/zdyukino/workspace/unitree_mujoco/unitree_robots/g1/g1_29dof.xml 和 STL，无 URDF；保留 Unitree BSD 3-Clause，运行不依赖外部目录。
- 当前 ROS 消息无电机温度/电压；显示 N/A。未知关节不猜测映射，不直接用并联 A/B 电机位置驱动 Pitch/Roll。
- 浏览器不依赖 ROS/DDS；本机有 rclpy，anakin_sdk Python 消息需要 source 对应工作区。

## 首期需求与验收

| ID | 需求 | 验收条件 |
|---|---|---|
| R1 | 独立启动 | ui 自有依赖、构建和启动文档；Web 单独启动也可用 DEMO/文件；跨设备浏览器不安装 ROS |
| R2 | 真实 ROS2 | 动态 Topic/类型/schema、endpoint 数和可用 QoS；可选择 Topic/数值字段；缺消息类型有具体错误；无控制发布/服务调用入口 |
| R3 | 真实状态 | 文本+颜色区分 DISCONNECTED/CONNECTING/CONNECTED/DEGRADED/ERROR，显示实测频率/带宽/最近时间/丢弃；DEMO 独立标记 |
| R4 | 机器人视窗 | 默认中央左侧较小 G1 视窗、右侧数据主区；旋转/缩放/重置；按命名关节联动；缺数据/加载失败可见；可检查关节数值 |
| R5 | 曲线分析 | 12 数值通道，叠加/隐藏/移除，单位/精度设置；同步游标/时间范围；选点显示时间/字段/值/单位；字段树和值检查器 |
| R6 | 工作站布局 | 顶栏、折叠导航、Overview/Topics/Telemetry/Robotics/Playback/Diagnostics/Settings、可折叠检查器、底部时间轴；面板重排/调宽/折叠/最大化/添加/删除；保存恢复布局 |
| R7 | 时间控制 | LIVE/PAUSED/HISTORY/PLAYBACK 明确区分；暂停继续采集；播放/暂停、速度、跳转、范围缩放、共享光标和返回 LIVE；源时钟重置/缺失有明确提示 |
| R8 | 本地回放 | CSV 时间列+数值列；JSON 带时间戳的记录；导入 ≤50 MB，明确时间映射/单位；坏文件/缺时间/非有限数值有可理解反馈；文件不上传 |
| R9 | 性能验证 | 受控合成数据和 ROS 测试验证 500 Hz/12 通道/60 秒；采集与约 30 FPS 绘图分离；有界缓冲；记录吞吐/帧耗时/丢弃；区分合成、ROS 与物理机器人验证 |
| R10 | 工程视觉 | #080A0D 深色、细边框、低圆角、严格网格，等宽 tabular 数字和独立单位，强调色克制；1366×768 主要功能可用 |

## 首期之外

原生 DDS Adapter、MCAP 完整导入、控制命令、参数修改、账号/多租户/公网、多机器人同时监控；未对物理机器人或任意第三方自定义消息宣称验收。

## 任务树

父任务管理需求、设计、执行依赖和最终集成评审。按顺序实现：

1. 11-03-telemetry-gateway：ROS2 Gateway、流协议、诊断和受控测试。
2. 11-03-telemetry-robot：G1 资产、许可、运动学和名称绑定。
3. 11-03-telemetry-workstation：完整工作站、DEMO、数据管线、曲线和布局。
4. 11-03-telemetry-playback：CSV/JSON、统一回放、集成/性能验收、中文文档。

2026-11-03 用户明确回复“是”批准最终计划并开始开发；随后授权workspace内Unitree MuJoCo LowState验收。首期代码和自动化验证已完成，真实浏览器视觉验收未完成，结果见 ui/docs/verification.md。当前等待工作流要求的提交范围确认。

## 2026-11-03 用户追加需求

- Settings提供English/简体中文语言选项，即时生效、localStorage持久化；默认保留English，不改变数据源/缓冲。Topic/消息字段/单位及协议枚举保持工程语义。
- 新增ui/em_run.sh：从任意工作目录一键准备项目依赖、按需构建、同源启动Web/Gateway；支持--web无ROS预览、host/port、ROS环境参数；不启动主env/机器人/仿真。
- 提供项目当前锁定依赖和浏览器/前端开发/Gateway/仿真验收所需依赖说明。
