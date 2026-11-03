# Offline playback and integrated acceptance

## 目标

完成CSV/JSON本地分析与统一回放，验证首期端到端交付。

## 前置与边界

依赖 workstation 的store、共享时间和所有面板；最终验收依赖其余全部子任务。

继承[父PRD](../11-03-aerospace-telemetry/prd.md)的Q1–Q8和R1–R10，全部改动限制在ui/。

## 范围

≤50MB文件验证、Worker解析、时间列/单位映射、数值字段发现、速度/跳转/范围、历史模型/检查器联动、中文文档、性能/浏览器/真实ROS验证报告。

## 验收

- [ ] CSV/JSON示例可导入、识别数值和时间，播放/暂停/跳转/速度/缩放有效；文件不上传。
- [ ] 坏行、错误时间、非有限数值、超限与MCAP给明确反馈，主线程可响应。
- [ ] 源时间/接收时间、LIVE/PAUSED/HISTORY/PLAYBACK遵守统一语义；历史点和姿态一致。
- [ ] 类型/构建/针对性测试通过，500Hz×12通道×60秒记录实测；1366×768、DEMO、ROS受控链路及用户改动保留有证据。
