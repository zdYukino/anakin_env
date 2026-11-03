# 执行计划

## 当前状态

用户已批准最终计划并授权仿真验收；代码已完成，按 inline 流程实现/检查。类型、构建、行为、真实ROS500Hz和MuJoCo LowState链路验证通过；真实浏览器视觉/GPU验证因本会话无浏览器而未完成，已明确记录。

## 顺序与依赖

- [x] Gateway：隔离项目依赖、协议、动态 schema、ROS2Adapter、只读订阅/有界批流/诊断。
- [x] Robot：依赖 Gateway 消息契约；转换 G1 资产、许可、层级/轴/名称绑定。
- [x] Workstation：依赖前两项；视觉系统、显式 DEMO、store/Canvas、各导航视图/检查器/布局交互。
- [x] Playback：依赖工作站共享时间/store；CSV/JSON Worker、回放联动、性能/浏览器验收、中文文档。
- [ ] 父任务最终评审：R1–R10、已验证结果、限制和后续路线。

父子关系不自动代表依赖；各子任务文档明确前置项。

## 验证命令计划

最终命令按 package scripts 落地，结果写回任务：

1. npm run typecheck / npm run build。
2. npm test：时间、环形缓冲、min/max、CSV/JSON 和关节映射。
3. Gateway Python：schema、订阅验证、队列界限、错误和只读协议。
4. 隔离 ROS domain 的受控500Hz发布器 → 真实Adapter → WebSocket；不调用机器人服务或发布控制Topic。
5. 浏览器：空态/错误、主动DEMO、模型/游标、Topic/字段、曲线、暂停继续采集、文件播放、布局保存恢复、1366×768。
6. 12通道×500Hz×60秒：吞吐、内存边界、绘图帧耗时和丢弃；目标频率不作为实测结果。
7. git diff --check 与最终状态，确认改动仅ui、用户现有README/.vscode保留。

## 风险与回退

消息模块需要正确source ROS/SDK，给出明确错误。性能通过实测验证，缺口先记录/优化，不静默降采样。模型/WebGL失败、无数据/时钟重置均有明确状态。

node_modules、虚拟环境、dist、缓存和测试产物不跟踪。不修改系统环境和外部仓库。独立回退 ui，无主工程迁移；不自动提交推送。完成实现和验证后依工作流单独给出提交范围供确认。

## 后续路线

P1 独立DDSAdapter：Unitree IDL/types、domain/interface、MotorState温度电压、QoS及受控DDS验收。
P2 MCAP：schema/encoding/压缩兼容表、按需读取/seek和时间一致性。
P3 其他机器人/协议、多图表类型和长期记录。

## 实际结果

完整结果、限制和复现命令见 `ui/docs/verification.md`。所有产品代码在ui/；WebGL/像素布局/GPU帧率仍待真实浏览器检查。没有Git提交/推送；提交前按workflow 3.4提出一次范围确认。

## 用户追加执行项

- [x] 添加集中式语言目录、Settings选项、主要视图/提示翻译和document.lang。
- [x] 验证双向切换、刷新恢复、不重置正在接收的数据。
- [x] 实现em_run.sh参数/依赖/增量构建/同源启动及--web。
- [x] 验证脚本help、非法参数、Web/ROS模式HTTP健康、信号退出；补依赖说明。

追加需求结果：Settings支持English/简体中文并保持采集会话；22项前端测试、6项Gateway/启动测试通过。em_run.sh验证了Web-only（无Python/ROS）和ROS2同源服务、HTTP health、退出和缓存构建。完整依赖说明见ui/docs/dependencies.md。
