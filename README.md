# anakin_env

Anakin 的原生 C++ 执行控制模块，作为独立的 ROS 2 Humble 包构建。它通过 `anakin_sdk` 定义的接口与 `anakin_mc` 交换状态、控制目标和推理状态。

## 构建

先安装 [anakin_sdk.revision](anakin_sdk.revision) 中锁定的 SDK 版本，再指定其安装前缀进行构建：

```sh
ANAKIN_SDK_PREFIX=/path/to/sdk/install/anakin_sdk ./build.sh
```

也可以将三个独立仓库链接到 `../anakin_cpp_ws/src`，构建共享工作区。构建需要 ROS 2 Humble，并会检查已安装的 SDK 版本是否与锁定版本一致。

## 运行

```sh
./em_run.sh                         # 默认 real：只启动原生 Env
./em_run.sh --real --input remote   # 实机；先配置明确的机器人网卡
./em_run.sh --sim --input keyboard  # 显式仿真；MC、官方模拟器分别启动
```

完整操作步骤、端点、配置覆盖、键位、状态/输出表和用户验收边界见[独立运行说明](docs/standalone-operation.md)。旧 `--loopback-sim` 已移除并明确拒绝。正常入口不启动 MC、模拟器或测试夹具，不自动 reset；MC/Env 持续等待所需的新数据。

启动脚本优先使用本仓库的独立安装目录，否则使用共享工作区的安装目录。可通过 `ANAKIN_PREFIX` 指定安装目录，通过 `ANAKIN_SDK_PREFIX` 指定 SDK。启动前检查选定 ELF、本地包标记、安装 YAML 和 SDK 构建记录；配置与该 ELF 的前缀一致，不从源码或其他 overlay 回退。ROS 默认 domain 87，仅在本机发现节点；仿真 SDK 为 `lo` / domain 31，实机使用配置中的显式网卡 / domain 0。脚本直接运行 C++ 二进制文件，不安装依赖或启动 Python。

## 架构

Env 使用一个 YAML 状态引擎、共用输入映射和共同 SDK PD 编码路径；sim/real 通过配置与后端选择区别路由、守卫和权限。MC 独立计算原模型/过渡的最终参考，Env 在当前事务、目标新鲜度、输出和权限检查后写入 SDK。SDK 发布成功不代表执行器已执行命令。

```mermaid
flowchart LR
    Launch[em_run.sh 默认 real / 显式 sim] --> Native[原生 Env]
    Inputs[键盘 / SDL / Unitree 共用映射] --> Native
    Config[选定安装内 YAML] --> Native
    Native <-->|ROS 状态 / 事务 / 目标| MC[独立 MC]
    Native <-->|HG LowState / LowCmd| Backend[官方模拟器或 G1]
```

代码依据：[入口](em_run.sh)、[配置/状态机](src/state_machine.cpp)、[生产控制器](src/runtime.cpp)、[原生节点](src/native_node.cpp)、[传输](src/transport.cpp)、[异步实机权限](src/authority.cpp)、[安装规则](CMakeLists.txt)。显式无 SDK 的 ROS 夹具保留在 [integration_run.sh](scripts/integration_run.sh) 的测试路由中。

[原生检查点](docs/native-checkpoint.md)与[后续审查记录](docs/review-fixes-20261027.md)保留历史证据。当前软件检查不代替用户实际仿真或硬件验收，尚未声称策略稳定性、物理停止或设备接管已通过硬件测试。
