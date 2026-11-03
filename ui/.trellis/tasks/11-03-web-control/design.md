# 设计

现状：InputHub 只接受 config.primary 的普通输入，所有来源急停；键盘使用 termios/PTY。网页网关为只读，运行输入配置已由唯一 ENV 发布者提供。已有未提交遥测修改属于本次实现的基础，保留。

边界：仲裁在 ENV InputHub，状态权限在 RuntimeController/StateMachine；网关只转发租约化的规范化输入，React 只呈现 ENV 权限。不改 SDK 接口、MC 策略和低层执行路径。用户明确批准跨 ENV/网关/前端及启动器修改，覆盖旧遥测规范中仅 ui/、纯只读的任务约束。

输入 YAML 升级，移除键盘，增加 automatic arbitration 和 web.actions，保留共用 controller_bindings。默认自动；可显式固定为 web/gamepad/remote。当前来源持续拥有中立控制；有操作的其他来源可以请求接管。同周期冲突按 remote > gamepad > web；来源替换立即清运动，不继承旧动作。当前来源丢失按 sim/real 已有 source_loss 规则处理，不能被另一在线设备掩盖。

网页输入使用固定 std_msgs/String ROS Topic 的版本化 JSON 信封，避免更改锁定 SDK。包含运行会话、网关生成的控制会话、严格递增序号、短有效期、操作、规范化轴/按钮及可选 YAML 动作。ENV 对大小、字段、会话、时效、取值、所有权和动作权限重新验证。activate 必须中立，input 不能创建租约；失联必须重新显式 activate。只有一个网页会话持有租约。ENV 诊断流提供当前来源、状态、候选动作与可用性，独立于机器人反馈。网关只接受 control 操作，通用 publish/service/参数写入继续拒绝。

TelemetryStore 管理网页状态、心跳、输入和释放；断连、切换数据源、离开 LIVE、失焦或隐藏页面释放并清理。状态可用性读实时独立控制流，不能读历史光标。虚拟 Joy 是页面内独立拖动浮层，双指针摇杆及映射按钮按指针松开/取消归零；关闭浮层归零。网页关闭恢复 02 关节窗口。
