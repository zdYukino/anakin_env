# 目录
App.tsx组合导航和工作区。components/包含Panel、Chart、RobotViewer、Inspector、TopicBrowser、Timeline和ImportDialog。core/拥有types、buffer、telemetry、demo、importer/Worker、robot、layout和format等数据逻辑。styles.css是统一视觉系统。
例如数值路径解析归types.numberAt和flatten，JointState读取归robot.findJoints；视图不重复解析不同版本的网关封包。
