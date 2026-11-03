# 前端开发规范
## Pre-Development Checklist
先读当前任务，再读[目录](directory-structure.md)、[状态](state-management.md)、[类型](type-safety.md)、[组件](component-guidelines.md)及[hooks](hook-guidelines.md)。源/时间/文件改动需读[跨层契约](../backend/telemetry-contract.md)。
## Quality Check
npm run typecheck、npm test、npm run build。用可用浏览器检查布局与WebGL；不可用时明确未验证，DOM测试不能替代视觉/真实Canvas/GPU验收。数值缓冲和降采样测试不等于实际浏览器30FPS证明。
