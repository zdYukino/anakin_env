# 首期验证记录

日期：2026-11-03（Asia/Shanghai）。代码和项目内容全部位于 ui/；主工程已有 README.md/.vscode 改动未由本次工作修改或纳入提交。实现分支：codex/aerospace-telemetry-viewer。

## 已执行

- TypeScript strict（含未使用项检查）、16项Vitest单元与DOM交互测试、Vite生产构建均通过。
- 3项Python协议测试通过（另有上游TestClient依赖的deprecation提示）：字段/时间、队列上限和淘汰、只读操作拒绝、WebSocket订阅/批流。
- ROS2 Humble / Python 3.10.12 / Fast DDS真实集成；Node.js 24.21.0。测试子进程均已退出清理。
- 官方Unitree MJCF转换：29关节、36 STL，完整保留命名、局部位置、四元数、轴/限位及visual mesh；复制Unitree BSD 3-Clause许可与来源SHA256。

## 真实ROS2受控500Hz链路

命令：`.venv/bin/python scripts/verify_ros.py --duration 60 --ros-domain 194 --port 8767`

输入为独立rclpy JointState发布器，经真实ROS2Adapter和WebSocket接收，不是直接调用前端生成器。

| 指标 | 结果 |
|---|---:|
| 接收时间 | 60.016秒 |
| 样本 | 30,025 |
| 墙钟接收率 | 500.266 Hz |
| 源时间接收率 | 500.000 Hz |
| 数值通道 | 12 |
| 最新消息快照 | 1,759 |
| 缺值 / Gateway队列丢弃 | 0 / 0 |

## 用户授权的MuJoCo LowState链路

命令：`.venv/bin/python scripts/verify_ros.py --mujoco --duration 60 --ros-domain 193 --dds-domain 39 --port 8766`

输入为workspace中官方Unitree MuJoCo的原生LowState，经**现有env ROS2桥接**到本次Gateway。未启用UI原生DDS Adapter，不能据此宣称后续DDS插件已完成。没有启动MC或发送控制指令；检查了execution_authorized=false、sdk_publish_count=0。

| 指标 | 最新60秒结果 |
|---|---:|
| 接收时间 | 60.028秒 |
| 样本 | 25,961 |
| 墙钟接收率 | 432.465 Hz |
| 源时间接收率 | 432.216 Hz |
| 数值通道 | 12（11关节位置 + state_seq） |
| 命名关节与模型对应 | 29 / 29 |
| 缺值 / Gateway队列丢弃 | 0 / 0 |
| 上游ROS state_seq跳号 | 0 |
| 观察到的ROS上游发布率 | 432.465 Hz |

最初12关节位置测试约406.132 Hz。复测增加state_seq后可确认当前接收链路没有漏掉已发布的ROS序号；较低频率来自仿真→env输入链路的实际发布率。主env仅在新SDK generation到达时发布状态，不能将2ms调度目标当成每次运行的500Hz实测保证。物理机器人未测试。

## 数值热路径

Vitest压力输入为12通道、500Hz、70秒，验证最后60秒保留、时间淘汰和尖峰min/max。

- 12个TypedArray环形buffer分配 **6,144,000 bytes**。
- 一次运行35,000条 × 12通道写入约 **124.9 ms**；12通道10秒窗口的800像素降采样约 **3.83 ms**。
- 这些是Node中的数据管线测量，不是实际浏览器Canvas/GPU绘图帧率。

## 行为覆盖

测试覆盖有界缓冲、尖峰保留、暂停继续采集、历史快照选择、时钟重置/外来时钟隔离、CSV引号/坏行、JSON嵌套ROS时间、缺失值、命名关节/有效位、源连接失败不回退DEMO、面板添加/删除/折叠/保存恢复、字段添加/隐藏/移除、导入确认和回放载入。

文件解析在Worker，代码检查50MB和1,000,000记录上限；本次格式用例和示例文件通过，未将50MB最坏情况或任意第三方消息编码列为已验证。

## 尚未验证

当前会话浏览器库存为空，创建iab返回 `Browser is not available: iab`。本地Vite预览HTTP请求成功，生产构建通过；**未完成真实浏览器截图、WebGL外观、1366×768像素布局、GPU约30FPS和浏览器内大文件性能验收**。DOM测试使用模拟renderer/Worker边界，不能替代这些检查。

已提供的界面和响应式样式以1366×768为目标；上述项目仍应在实际浏览器人工检查。原生DDS Adapter、MCAP、多机器人和控制命令均属于首期之外。

原始机器相关结果位于不跟踪的artifacts/：mujoco/result.json、ros-synthetic/result.json、buffer-performance.json。可按上列命令复现。

生产构建另有Three.js压缩后约567KB的chunk大小提示，构建成功；该依赖按单独chunk加载。没有将该提示当作运行错误，也没有通过扩大阈值掩盖。

结束检查时另观察到主工程新出现的未跟踪config/{inputs,real,sim}.yaml及include/anakin_env/{config,state_machine}.hpp；这些不属于本次ui实现，未被修改或暂存。提交范围见docs/commit-plan.md。

## 追加：简体中文与一键启动

2026-11-03 按用户追加要求，Settings加入English/简体中文，localStorage保存选择并设置document.lang；协议字段保持原样。测试覆盖跨主要视图即时切换、双向切换、重挂载保持、已收数据/会话不变、参数化提示和原始标识保留。新增视图沿用当前并行更新的MissionOverview/Workbench结构。

本轮类型检查、22项前端测试、格式检查、生产构建通过；6项Python协议/启动检查通过。启动脚本从/tmp通过绝对路径运行，验证无Python/ROS的--web、ROS2独立195域的同源Web及read_only=true健康接口、当前SDK消息环境探测、缓存构建不重写产物、非法参数和服务进程退出。测试进程已清理。

Node依赖范围按当前jsdom引擎收紧为^22.22.2 / ^24.15.0 / >=26，未变更锁定包版本。完整依赖见docs/dependencies.md。浏览器视觉/GPU验证限制仍与上文相同。

## 追加：遥控器状态与生效 YAML

2026-11-03，Controllers 替代重复的 Robotics 页面；工作台移除 robot 面板并迁移旧布局。新增独立输入诊断流，侧栏通过发布节点的只读参数读取生效 YAML（含 CLI 主输入覆盖），不回退默认文件。遥测在 ROS 低频定时器发布，避免把序列化加入 500 Hz 控制线程；参数读取也与 WebSocket 实时批流分开。

验证：前端 28 项测试、Gateway 4 项协议测试、env 22 项 offline CTest、类型检查和生产构建通过。SDL 测试在沙箱内异常、TestClient 在线程启动处阻塞；同一组测试在本机运行全部通过。

`scripts/verify_inputs.py` 在独立 ROS 196 / DDS 43 loopback 域启动实际 native env 和只读 Gateway，无机器人反馈、无 MC/控制发布。最终接收 30 个双源状态快照，实测 29.41 Hz；核验自定义 R1+Start 松开映射、数字键字符串、`--input remote` 覆盖文件中的 keyboard，以及磁盘 YAML 后续修改不会冒充运行配置。测试进程自动清理。

本轮使用 Edge 完成中文 1366×768 遥控器/工作台视觉检查及约 390×844 窄屏侧栏检查；工作台无机器人面板，两路状态与运行配置组合键均可见。这补充了上述早期记录中的浏览器不可用限制；未据此声称物理手柄/遥控器接入或 GPU 帧率已验收。

## 追加：Xbox 映射与底部事件条

检测到实机 Xbox Series X Controller，蓝牙 GUID `050000005e040000130b000009050000`。旧 SDL 通用映射将右摇杆与扳机混淆，部分面键/肩键序号也不符。修正配置保存在 `inputs.yaml` 的 `gamepad.mappings`，仅匹配此 GUID；正式适配器只读采样确认右摇杆回到正常中位，LT/RT 分别为0。完整四轴、独立 LT/RT、LB/RB 及 X 的映射使用 SDL 虚拟设备回归；没有将中位采样当作全部实体按键/行程验收。

前端29项测试和 env 23项 offline CTest通过；新增事件测试覆盖输入来源、异步进行中/确认成功/故障失败、非主输入急停归因、按住不刷屏和有界序号。`verify_inputs.py --events --gamepad` 用独立 ROS198/DDS46 的无机器人反馈 native env、PTY 和实际只读 Xbox 验证 Gateway WebSocket；实测29.41Hz，核验键盘发起的 disarmed成功、asap_loco进行中、hold拒绝、damping成功及被中断请求取消。没有 MC/机器人控制发布者或模拟器。

浏览器核验中文1366×768遥控器面板、真实 Xbox 四轴/模拟扳机、侧栏运行映射、底部“键盘→保持站姿·失败”，点击底部条可见完整来源/目标/结果和原因；390×844下结果仍在右侧显示。独立预览意外终止时发现两个测试子进程残留，已按具体 PID 清理；验证脚本增加 SIGTERM 清理和启动前端口占用检查，避免复用已有服务。


## 网页控制与自动仲裁验收（2026-11-03，最终版本）

本次已获用户明确批准：跨 ENV/网关/前端/启动器移除终端键盘，改为 Unitree、实体 Joy、网页自动仲裁。保留任务开始前所有未提交工作；未自动提交混合工作区。

- ENV 独立构建安装完成，C++ offline 24/24 通过，包含三路仲裁、非主来源急停、失联/重连/回放、网页租约及实机入口动作可用性。SDL 虚拟设备测试在默认沙箱中受设备访问限制崩溃，使用同一产物在沙箱外完成整套 24/24；没有 ROS/机器人动作。
- Vitest 34/34，包含网页显式激活、旧 periodic status 与激活确认交错、心跳超时、数据源/历史切换释放、虚拟 Joy 按键/肩键/摇杆回中、灰色指令及关闭面板恢复。TypeScript strict 与 Vite 生产构建通过。
- Python 网关/控制/启动器 24/24；普通 ROS 写入拒绝、同源 Origin、独占租约、过期、乱序、重复激活、ASGI 取消中的断连释放均覆盖。TestClient 依赖本机 asyncio socket，沙箱外运行。上游 Starlette TestClient 弃用提示仍存在。
- 父仓 tests/orchestration.py 通过：自动模式无 TTY 启动、keyboard 参数拒绝、domain/参数传递、退出与信号清理。
- 最终真实链路：scripts/verify_inputs.py --events，独立 ROS domain 196、SDK domain 43、网卡 lo；所有测试状态关闭 LowCmd 输出并配置撤权动作。收到 37 个输入快照，29.41 Hz，sdk_publish_count=0。已确认 web 来源 disarmed 成功、asap_loco Preparing 进行中、急停 damping 成功；被 YAML 移除的 Hold 动作由 ENV 报不可用且网关拒绝；修改磁盘文件没有改变已加载配置。
- Edge 中文实际页面（1474×748）核验网页开关、02 指令面板、亮/灰状态、可独立悬浮的虚拟 Joy。虚拟 Up 将期望高度 0.78 改为 0.83，RB/R1+Up 通过共用映射产生 ASAP CR7 Preparing 事件。最终构建再次验证 Hold disabled、虚拟 Joy 可见；关闭网页控制后关节窗口恢复且浮层消失。WebGL 模型正常显示；未测最坏情况 GPU 帧率和实机动作。

正常 domain 87／端口 8765 的原会话未重启或发送验证输入。需要重启 ENV 和 UI Gateway 才能加载本次接口；旧版 Gateway 不具备控制协议时按钮保持不可用。
