# 组件
组件props使用显式TypeScript类型，数据类型用import type。Panel只在传入handler时显示操作按钮；不能留下点击无效的控制。面板状态由App布局统一管理。
Chart使用Canvas，RobotViewer使用Three.js。ResizeObserver、RAF、事件监听、GPU geometry/material必须在effect清理。镜头轨道拖动结束不能误判为关节点击。
默认暗色tokens、细边框、低圆角；读数用IBM Plex Mono/tabular数字，单位分离。连接/DEMO/历史状态必须有文字。
