# 待确认的提交范围

按 ui/.trellis/workflow.md 的 Phase 3.4，在代码、验证和文档已完成后提出一次提交范围确认。尚未执行 git add/commit/push。

## 拟提交

单个工作提交：`feat: add isolated robotics telemetry viewer`

仅包含 ui/ 下非忽略的本次项目文件：

- 前端 src/、index.html、package.json/package-lock.json、TypeScript/Vite/格式配置。
- Gateway、锁定依赖、启动/安装/模型转换/验证脚本和测试。
- 自包含 G1 模型、36 STL、Unitree许可、来源和离线示例。
- 中文 README、验证报告、本提交计划。
- ui 自己的 AGENTS.md、.agents/、.codex/、.trellis/规范/计划及生成的Trellis工作流文件。

node_modules、.venv、dist、artifacts、缓存和Trellis运行时均被忽略。任务保留in_progress，真实浏览器视觉/GPU检查和后续归档尚待完成。

## 不属于本次工作的主工程改动

开始时已存在：

- README.md
- .vscode/

结束时又观察到以下未跟踪文件；本次没有写入这些路径：

- config/inputs.yaml
- config/real.yaml
- config/sim.yaml
- include/anakin_env/config.hpp
- include/anakin_env/state_machine.hpp

这些路径均排除，不修改、不暂存、不提交。提交后不推送。

追加内容：中英文界面与持久化、ui/em_run.sh、一键启动测试和依赖说明。并行工作产生的MissionOverview/Workbench及主工程修改保留；本轮仍未执行任何暂存/提交/推送。
