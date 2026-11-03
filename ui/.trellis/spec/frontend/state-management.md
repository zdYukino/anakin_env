# 状态与时间
TelemetryStore是数据源、连接、所选通道、有界buffer、快照与共享显示光标的所有者；App持有导航/布局/弹窗等UI状态。packet是网关未知JSON的唯一解码入口。source切换递增session，旧WS和异步DEMO不得污染新源。
RingBuffer每通道32,000样本/60秒，file可为Infinity保留文件样本。Canvas按像素min/max降采样并缓存稳定视窗的结果。
暂停继续采集；历史模型和Inspector使用store.at(topic,cursor)。已知Topic时钟重置清新段；新Topic的外来时钟域拒绝混用。有效位与非有限值不能转成真实0。
localStorage布局v1保存顺序/可见/折叠/列宽/行高及通道。损坏配置回默认，恢复通道限当前源的可用Topic。
工作台已删除 robot 面板；载入旧布局时过滤 robot，保留其他顺序/隐藏/折叠偏好，列宽使用 columnWidth。Controllers 替代 Robotics，始终保留 `/anakin/input_state` 订阅；新发现输入 Topic 时补订阅。输入读取与配置解码集中在 core/inputs.ts，运行配置仅来自 input_config 封包；断连/切源/元数据失败清除配置。在线状态还需校验自身快照到达时间，历史数据不得标作当前在线。
Xbox 的 LT/RT 由独立 trigger.* 数据读取，LB/RB 是显示层对逻辑 L1/R1 的别名；右摇杆 SDL Y 的图形朝向在视图校正，控制轴数值不变。core/events.ts 校验运行切换事件，TelemetryStore 按会话+序号去重写入同一有界事件日志，latestEvent 驱动底部 EventIndicator；pending 不得显示为 success，底部状态须在窄屏仍可见，点击只打开 Diagnostics。详情显示当前状态及原因，不根据按键高亮推测切换成功。

语言由core/i18n.ts和locales/zh-CN.json独立维护，key=aerospace.locale.v1，值en/zh-CN。useI18n订阅语言变化；document.lang为en/zh-Hans。只在显示边界翻译，路由/Topic/字段/单位和原始诊断数据不改。切语言不重连、不清buffer。MissionOverview和Workbench共用该目录。

网页输入由 TelemetryStore 统一管理：control_state 在 packet 边界经 core/control.ts 校验，controller 配置 schema 为 2；动作名称允许 asap_cr7 等含数字标识。webRequested 与 ENV owned/pending 分开，必须等待激活确认；激活前已在途的 periodic status 不能立即撤销请求，但等待有明确超时。control_state 独立于历史光标且过期即释放；断连/切源/离开 LIVE/页面隐藏/失焦释放，恢复后不能自动激活。50ms 心跳不重复一次性 action。虚拟 Joy 规范化输入复用物理控制器映射，双指针摇杆独立回中，关闭浮层归零；不得以按键高亮推测机器人动作成功。
