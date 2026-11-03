# 遥测跨层契约
## 1. Scope / Trigger
ROS2 Adapter与浏览器的版本化WebSocket边界。
## 2. Signatures
GET /api/health；WS /ws。Adapter.graph()/schema(topic)/configure(client,requests)/detach(client)/close()。
## 3. Contracts
请求必须v:1，kind为subscribe/schema/refresh/ping/unsubscribe。subscribe.subscriptions每项topic、fields、qos；最多20Topic、12数值字段。QoS为auto/best_effort/reliable。
服务hello/topics/schema/batch/status/error；batch含samples、snapshots、dropped、received。sample含topic/t/seq/source_ns/receive_ns/clock/values，snapshot含topic/t/data及时间信息。纳秒为十进制字符串；t为绘图用秒。
环境：TELEMETRY_HOST默认127.0.0.1、PORT默认8765；ROS_DOMAIN_ID默认87，ROS_LOCALHOST_ONLY默认1，TELEMETRY_ROS_SETUP可加载自定义消息环境。
Client队列max5000，溢出淘汰最旧并计数；约30Hz批量发送，原始最新快照独立节流约30Hz。
## 4. Validation & Error Matrix
未知操作/错误版本/字段路径/超过上限→REQUEST_REJECTED；ROS未加载→ROS_UNAVAILABLE；动态类型缺依赖→具体REQUEST_REJECTED；慢连接发送超时→关闭并释放订阅。
## 5. Good / Base / Bad
Good：500Hz×12通道完整传递采样时间；Base：schema可在首条数据之前显示；Bad：publish/service/参数写入操作一律拒绝。
## 6. Tests Required
tests/test_gateway.py断言只读拒绝与缓冲淘汰；scripts/verify_ros.py验证真实发现/schema/批流，MuJoCo需29命名关节匹配、SDK写次数0、源序号连续。
## 7. Wrong vs Correct
错误：每条ROS消息await websocket.send_json，或把当前浏览器刷新时间当源时间。
正确：ROS回调写有界Client，异步批流保留source_ns和receive_ns，前端单一packet解码后交给共享store。

## 一键启动入口

ui/em_run.sh默认8765同源启动前端/Gateway，支持--web/--host/--port/--domain/--ros-setup/--prepare/--no-install/--rebuild。--web不要求Python/ROS。Node范围与package.json及jsdom引擎一致；项目依赖按锁定版本校验，构建按src/public/config内容指纹更新，不自动启动主env或仿真。tests/test_launcher.py验证参数、缓存、两种HTTP模式、只读health和信号退出。
