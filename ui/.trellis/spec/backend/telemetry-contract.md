# 遥测跨层契约
## 1. Scope / Trigger
ROS2 Adapter与浏览器的版本化WebSocket边界。
## 2. Signatures
GET /api/health；WS /ws。Adapter.graph()/schema(topic)/configure(client,requests)/detach(client)/close()。
## 3. Contracts
请求必须v:1，kind为subscribe/schema/refresh/ping/unsubscribe/control。subscribe.subscriptions每项topic、fields、qos；最多20Topic、12数值字段。QoS为auto/best_effort/reliable。
服务hello/topics/schema/batch/status/error；batch含samples、snapshots、dropped、received。sample含topic/t/seq/source_ns/receive_ns/clock/values，snapshot含topic/t/data及时间信息。纳秒为十进制字符串；t为绘图用秒。
`input_config` 是独立任务每秒发送的只读运行元数据，避免参数读取超时阻塞实时批流。Adapter.input_config() 只向 `/anakin/input_state` 的唯一发布者读取 GetParameters；不得回退本地 YAML 或固定映射。成功字段 available/node/path/mode_path/input_timeout_ms/config；失败 available=false/message，前端清除旧配置。输入配置 schema_version=2，primary 为 auto/web/gamepad/remote，web.actions 为网页候选动作；不再接受终端键盘配置。env 的 DiagnosticArray 输入流独立于机器人反馈，按 source 分别报告 schema_version=1、enabled/primary/connected/online、单调采集 age_ms、axis.*、button.*；心跳不得刷新原始采集年龄。
环境：TELEMETRY_HOST默认127.0.0.1、PORT默认8765；ROS_DOMAIN_ID默认87，ROS_LOCALHOST_ONLY默认1，TELEMETRY_ROS_SETUP可加载自定义消息环境。
Client队列max5000，溢出淘汰最旧并计数；约30Hz批量发送，原始最新快照独立节流约30Hz。
输入诊断可含 `trigger.LT/RT`（0..1，不从 right_y 推断）、device_name/device_guid/sdl_mapping。`gamepad.mappings` 在生效 YAML 中保留，精确 GUID 匹配；UI 的 Xbox LB/RB 只重标逻辑 L1/R1，不改变状态组合语义。`anakin_env/switch_event` 的 hardware_id 为运行会话，KeyValue 含 schema_version=1/sequence/time_ms/source/from/target/current/result/detail；最多保留32条。result 为 pending/success/failed/cancelled，Preparing 不得冒充成功，异步结束保留原请求来源，紧急停止来源使用 InputHub 的实际触发来源。前端按会话+序号去重，同步 Diagnostics 与底部 latest event；仅实时源生成运行事件日志。
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

ui/em_run.sh默认8765同源启动前端/Gateway，支持--web/--host/--port/--domain/--ros-setup/--prepare/--no-install/--rebuild。--web不要求Python/ROS。Node范围与package.json及jsdom引擎一致；项目依赖按锁定版本校验，构建按src/public/config内容指纹更新，不自动启动主env或仿真。tests/test_launcher.py验证参数、缓存、两种HTTP模式、health及信号退出。


## 网页控制契约（2026-11-03）

1. 范围：网页显式租约化输入；ENV InputHub 仲裁，RuntimeController/StateMachine 决定动作权限和实际执行。MC/SDK 低层执行路径保持共用。
2. 接口：Adapter.control(client, request)/control_status(client)/release_control(client)。WS kind=control，operation=activate/release/input；activate/release 不能携带输入。input 必须 axes[4]（有限数 -1..1）、buttons[10]（bool）、shoulders（整数 0..3），可选 action。不接受浏览器提供 Topic、会话、序号或环境标识。浏览器 Origin 必须与网关同源；普通 publish/service/参数写入仍拒绝。
3. 运行状态：独立 10Hz control_state，含 available/enabled/owned/pending/leased/environment/active_source/mode/state/requested_state/moving/height/timeout_ms/actions/message/sdk_publish_count。actions 来自 ENV 当前加载的 web.actions 及实际状态机守卫，不从历史遥测或前端表推断。health/hello 的 read_only=false、control_protocol=1。
4. 写入：固定 /anakin/web_input std_msgs/String JSON v1；网关生成环境绑定会话、递增 sequence、sent_ms。可靠 QoS depth=32 保留短按下/松开沿，ENV 拒绝超过输入超时的排队数据和回放。activate 必须中立且无其他存活租约；input 不能创建/恢复租约。ENV 仅接受配置中的动作并重新检查入口规则。
5. 错误：非法字段/会话/数值/无效动作/重复租约 → CONTROL_REJECTED。运行状态过期、原租约无心跳、ENV 重启或激活未获确认 → 释放；ASGI handler 取消时也须完成 release_control。用户需显式重新激活。
6. 仲裁：schema 2 的 primary=auto 按实际动作/已回中的摇杆操作争用，同周期 remote > gamepad > web；中立心跳保留现有来源。当前来源失联仍触发 source_loss，不能由其他在线设备掩盖；接管清除运动开关。所有启用来源急停有效。
7. 验证：tests/test_control.py 校验字段/唯一租约/时效/激活确认/同源/断连清理，tests/web-control.test.tsx 验证显式激活、状态权限、浮层和释放。scripts/verify_inputs.py --events 使用独立 ROS 196、SDK 43、lo，所有 LowCmd 输出关闭且配置撤权动作；必须观测 sdk_publish_count=0。不能据此声称实际 G1 硬件动作已验收。
