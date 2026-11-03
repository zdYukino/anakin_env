# 检查
协议测试覆盖只读拒绝、字段路径、源时间和队列淘汰。集成测试必须经真实rclpy→WebSocket，使用隔离domain。ROS系统launch_testing与venv pytest插件不兼容，明确设置PYTEST_DISABLE_PLUGIN_AUTOLOAD=1，避免无关插件加载。
所有新代码限制ui/，测试不得调用控制服务或向控制Topic发布。
