# 质量
当前检查为typecheck、Vitest和生产构建，没有独立lint脚本。tests/core.test.ts覆盖时间、环形buffer、降采样尖峰、格式/映射；workstation.test.tsx覆盖布局/字段/真实源错误；import-dialog.test.tsx覆盖映射确认和历史数据载入。
DOM测试mock了WebGL/Canvas或Worker边界，只能证明交互状态。实际WebGL、1366×768视觉布局和GPU帧率需要浏览器验收，不能把mock测试标记为已实测。
DEMO必须主动选择并持续标识；ROS断连不能自动回退演示。温度/电压不可用时N/A。测试和改动不触碰父env或外部模型仓库。
