# 检查
协议测试覆盖只读拒绝、字段路径、源时间和队列淘汰。集成测试必须经真实rclpy→WebSocket，使用隔离domain。ROS系统launch_testing与venv pytest插件不兼容，明确设置PYTEST_DISABLE_PLUGIN_AUTOLOAD=1，避免无关插件加载。
遥测任务默认限制 ui/。用户明确批准的网页控制任务可跨 ENV/启动器；真实控制链路测试必须用隔离 domain、lo，并关闭所有 LowCmd 输出，禁止对运行中的机器人会话发送验证输入。
