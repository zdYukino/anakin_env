# 技术设计

共同架构与协议以[父设计](../11-03-aerospace-telemetry/design.md)为准。

FastAPI/Uvicorn WebSocket 与 rclpy 执行器线程隔离。Adapter 不依赖 HTTP。使用动态 get_message 和字段自省，验证 Topic/字段/消息大小，最新 raw 快照与批内数值样本分离。默认只绑定 loopback，构建产物可同源提供。

边界与回退：只改ui，独立依赖；不改机器人运行参数和env。缺能力明确报告，不把DEMO当真实数据。
