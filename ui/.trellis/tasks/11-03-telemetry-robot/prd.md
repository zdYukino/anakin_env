# G1 model and named joint binding

## 目标

以自包含 G1 模型和真实命名关节数据提供机器人状态视窗。

## 前置与边界

先完成 telemetry-gateway 的快照/时间契约；界面挂载在 workstation 集成。

继承[父PRD](../11-03-aerospace-telemetry/prd.md)的Q1–Q8和R1–R10，全部改动限制在ui/。

## 范围

MJCF 运动树和网格转换、Unitree 许可和来源、Three.js 可替换加载器、相机交互、关节名称/轴/限位绑定、缺失字段状态。

## 验收

- [x] 生成36个引用网格及可追溯运动树；浏览器运行不依赖外部模型目录。
- [ ] 指定已知关节转动符合运动树与轴，名称不匹配时明确报告；温度电压缺失为N/A。
- [ ] 旋转、缩放、重置有效；资源释放、加载失败和 WebGL 错误可见。
