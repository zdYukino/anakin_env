# 状态与时间
TelemetryStore是数据源、连接、所选通道、有界buffer、快照与共享显示光标的所有者；App持有导航/布局/弹窗等UI状态。packet是网关未知JSON的唯一解码入口。source切换递增session，旧WS和异步DEMO不得污染新源。
RingBuffer每通道32,000样本/60秒，file可为Infinity保留文件样本。Canvas按像素min/max降采样并缓存稳定视窗的结果。
暂停继续采集；历史模型和Inspector使用store.at(topic,cursor)。已知Topic时钟重置清新段；新Topic的外来时钟域拒绝混用。有效位与非有限值不能转成真实0。
localStorage布局v1保存顺序/可见/折叠/列宽/行高及通道。损坏配置回默认，恢复通道限当前源的可用Topic。

语言由core/i18n.ts和locales/zh-CN.json独立维护，key=aerospace.locale.v1，值en/zh-CN。useI18n订阅语言变化；document.lang为en/zh-Hans。只在显示边界翻译，路由/Topic/字段/单位和原始诊断数据不改。切语言不重连、不清buffer。MissionOverview和Workbench共用该目录。
