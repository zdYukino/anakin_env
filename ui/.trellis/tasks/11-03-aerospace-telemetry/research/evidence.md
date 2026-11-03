# 本地调查证据

2026-11-03，只读调查。未启动机器人/ROS节点/DDS通道。

- env 无 .codegraph，ui 初始为空；用户 README.md 和 .vscode/ 改动保留。
- Trellis 0.6.17 在ui初始化；native workflow，codex inline，session_auto_commit false。
- env 接口：en_run.sh、src/node.cpp、src/native_node.cpp。
- SDK消息：/home/zdyukino/workspace/anakin_cpp_ws/install/anakin_sdk/share/anakin_sdk/msg/RobotStateFrame.msg。
- 模型：/home/zdyukino/workspace/unitree_mujoco/unitree_robots/g1/g1_29dof.xml，36个引用STL共18,538,724字节；许可同仓库LICENSE。
- 关节索引：unitree_robots/g1/g1_joint_index_dds.md；并联A/B不能直接当关节角。
- rclpy/rosidl_runtime_py已装，anakin_sdk需加载工作区；FastAPI/websockets/mcap未在系统Python中发现。
- env 2ms调度只说明目标约500Hz，不是当前链路吞吐实测。
- 访谈Q1–Q8用户均明确接受推荐，结论见父PRD；下一步为最终计划确认。
