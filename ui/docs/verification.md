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
