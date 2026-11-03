# Read-only ROS2 gateway and stream contract

## 目标

建立可独立启动的 ROS2 只读网关和版本化批流，不改主 env。

## 前置与边界

无产品代码前置；父任务流契约为依据。

继承[父PRD](../11-03-aerospace-telemetry/prd.md)的Q1–Q8和R1–R10，全部改动限制在ui/。

## 范围

动态图发现、消息 schema、QoS 兼容订阅、所选字段数值批流、最新消息快照、连接/吞吐/丢弃诊断、缺依赖错误、清理退出。

## 验收

- [x] 受控 ROS Publisher 能被动态发现，订阅接收到嵌套数值及源/接收时间；可取消订阅。
- [x] 消息类型缺依赖仍展示 Topic；无 ROS 环境提供具体启动错误；无控制 API。
- [x] 队列/请求上限生效，慢接收者有丢弃计数；500Hz受控ROS链路记录实际吞吐。
