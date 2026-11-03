# Gateway 开发规范
## Pre-Development Checklist
先读当前任务 PRD/design/implement，再读 [目录](directory-structure.md)、[协议](telemetry-contract.md)、[错误](error-handling.md)、[日志](logging-guidelines.md)。
## Quality Check
运行 `PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 .venv/bin/python -m pytest tests/test_gateway.py -q`。改流契约时同时验证前端 `TelemetryStore.packet`。改变ROS执行器/订阅热路径时使用独立domain运行 `scripts/verify_ros.py`。不得以合成目标频率替代接收实测值。
