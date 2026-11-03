# 目录
- gateway/protocol.py：v1封包、Adapter Protocol、字段路径与有界Client队列，无ROS依赖。
- gateway/ros_adapter.py：唯一ROS执行器线程，图/schema/订阅命令通过Future排队。
- gateway/server.py：HTTP/WebSocket、生命周期、静态构建产物。
- scripts/gateway.sh：仅对子进程加载ROS环境；verify_ros.py只管理自己启动的进程。
例如 server 的 `await asyncio.to_thread(adapter.graph)` 不在事件循环阻塞ROS调用。禁止把rclpy引入浏览器或在WebSocket处理器直接spin。
