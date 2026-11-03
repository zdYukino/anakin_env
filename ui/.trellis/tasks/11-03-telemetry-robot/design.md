# 技术设计

共同架构与协议以[父设计](../11-03-aerospace-telemetry/design.md)为准。

转换脚本保留局部 pos/quat、joint axis/range 和 visual 属性，不展开成失去关节层级的静态模型。Three.js group 树中关节旋转叠加于初始变换。G1映射是模型配置，运行按 JointState.name，不直接使用未知电机序号。

边界与回退：只改ui，独立依赖；不改机器人运行参数和env。缺能力明确报告，不把DEMO当真实数据。
