# 技术设计

## 边界与目录

ui/ 独立包含 src/、gateway/、public/models/、scripts/、tests/、中文 README 和依赖/构建配置，不修改 env 和外部模型仓库。

前端 React + TypeScript + Vite；Three.js 模型网格和 Canvas 高频曲线；克制的自定义组件系统。安装时锁定依赖。Gateway 用 Python 3.10、FastAPI/Uvicorn WebSocket、rclpy；虚拟环境读取系统 ROS 包，其他依赖独立安装；启动时显式加载 ROS/SDK 环境。默认回环绑定，局域网访问通过参数开启。

## 数据流与适配器

ROS2 → ROS2Adapter → schema/时间规范化 → 有界批流 → WebSocket → 数值 buffer → 同步曲线/模型/检查器。

Adapter 提供图发现、schema、订阅、取消和关闭，未来 DDS 实现同一边界。DEMO、文件是独立前端数据源；切换源清空会话缓冲并丢弃过期连接回调。浏览器无 ROS/DDS 依赖。

## 流契约 v1

HTTP health、WebSocket /ws；构建后的静态文件可由 Gateway 同源服务，开发 Vite 代理 /api 与 /ws。

客户端 subscribe 指定 Topic/字段，可 unsubscribe、刷新和 ping；最多约20 Topic/12数值通道，明确拒绝非法/超限请求。协议无 publish/service/参数写操作。

服务事件 hello/topics/schema/batch/status/error，带版本和类型；错误有代码及可显示原因。Topic 包含名称、类型、endpoint 数、QoS、schema 可用性、实测频率/字节率/最近接收时间。

样本保留 Topic、序列、源时间、接收时间、时钟来源和数值路径。纳秒用字符串防 JS 精度丢失，绘图以会话相对秒表示。最新完整消息用于检查器/机器人并节流；选中数值样本按约30Hz批量发送，保留批内采样时间。每客户端队列有界，慢客户端淘汰旧数据并报告丢弃。

rclpy 动态消息类与类型自省支持嵌套消息/数组，schema 不依赖首条消息。消息包未装仍显示 Topic 和具体加载错误。订阅 QoS 与发现的 publisher 兼容，并提供只读订阅设置。JointState 按 name 绑定，不假设所有机器人数组顺序相同。

## 时间与绘图

源时间优先，无有效源时间标记 RECEIVE TIME。不同 clock-domain 不静默叠加，时钟重置开始新段并提示。未同步跨主机时钟不把差值标为精确网络延迟。

选中通道有界环形缓冲保留60秒；按像素 min/max 降采样避免抹掉尖峰。React 只接低频 UI 快照，采集热路径位于 store/Canvas。暂停固定显示光标且继续采集，历史过期明确提示。模型、检查器和曲线使用同一显示时间，避免历史图和当前姿态混杂。

## 模型资产

脚本从 MJCF 保留 body 树、局部位置/四元数、joint 轴/限位、visual mesh/material，生成浏览器运动树 JSON 并复制 STL。附许可、来源和重建命令，运行不依赖源目录。

模型加载器与 G1 映射分离，可后续扩展 URDF。只应用名称匹配且语义明确的关节角；未知/缺失值标注未映射，不猜测并联电机 A/B。温度电压缺失显示 N/A。

## 视觉与交互

深色严格网格，顶部工作区/数据源/连接/速率/时间/设置；折叠导航；默认左模型/姿态摘要、右曲线/通道表及检查器；底部同步时间控制。

面板拖动重排、分隔条调宽、折叠/移除/添加/最大化；布局、通道和精度以版本化 localStorage 保存，可恢复/重置，坏配置回默认。小屏折叠导航和检查器，主要数据区可操作。

## 离线

本地 CSV 时间列+数值列，JSON 时间戳消息记录数组或数值行；显式选择/确认时间字段与单位，支持 elapsed、秒/毫秒/纳秒/ISO。文本不能绘图时保留检查能力；非法时间/非有限数值/重复逆序有明确策略和反馈。读取前检查50MB，Worker解析避免冻结主线程。

全局光标控制回放图表/模型/检查器，支持速度、跳转、范围缩放。MCAP 显示后续阶段说明，不声称可用。

## 验证与回退

类型检查/构建；针对时间、buffer、降采样、解析、运动学和协议的测试；Gateway 缺 ROS 错误路径及真实 rclpy 受控发布器；浏览器验证 DEMO/离线/布局/1366×768。记录测试输入、机器、吞吐、帧耗时、丢弃，区分合成与物理机器人实测。可独立移除/回退 ui/，退出清理节点和资源。

## 官方参考

- [Vite](https://vite.dev/guide/)
- [STLLoader](https://threejs.org/docs/pages/STLLoader.html)
- [OrbitControls](https://threejs.org/docs/pages/OrbitControls)
- [FastAPI WebSockets](https://fastapi.tiangolo.com/advanced/websockets/)
- ROS2 Humble 官方站点访问被站点防护阻挡，实施以本地 rclpy/rosidl API 自省和受控集成验证为依据。

## 追加实现边界

语言由src/core/i18n.ts集中管理，独立于TelemetryStore；所有主要视图/控件/可访问名称在渲染边界翻译，日志和错误保持原始记录、显示时翻译已知提示，协议数据不翻译。语言切换不重建数据源。字体采用本机CJK回退，无新增网络字体或运行依赖。

ui/em_run.sh作为项目启动入口，缺依赖时在ui/node_modules及.venv安装锁定版本；按内容指纹构建，默认由现有Gateway同源托管dist。--web仅启动静态预览。前置ROS系统包不自动安装，消息环境优先显式配置，其次探测相邻SDK安装环境。
