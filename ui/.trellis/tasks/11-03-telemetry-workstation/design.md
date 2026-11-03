# 技术设计

共同架构与协议以[父设计](../11-03-aerospace-telemetry/design.md)为准。

React/TypeScript UI、低频快照store、Canvas绘图热路径分离。Typed ring buffer 和 min/max 降采样。共享displayTime驱动模型、检查器和曲线。版本化localStorage，所有状态有文字和颜色。Canvas与WebGL各自清理资源，样式统一tokens。

边界与回退：只改ui，独立依赖；不改机器人运行参数和env。缺能力明确报告，不把DEMO当真实数据。
