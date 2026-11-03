# C++ MC / Env 独立运行说明

正常入口只启动 Env，默认实机模式。MC、Env、官方模拟器是三个独立、长期运行的进程；启动顺序可以不同。缺少 MC、缺少新反馈或仿真暂时暂停时，仿真模式等待后续数据；不会因为固定运行时长而退出，也不会替用户启动、重启或重置模拟器。

## 构建与安装目录

各仓库使用原生 CMake，按 SDK → Env/MC 顺序独立构建，无需统一 colcon workspace：

```sh
/path/to/anakin_sdk/build.sh
ANAKIN_SDK_PREFIX=/path/to/anakin_sdk/install/anakin_sdk /path/to/anakin_env/build.sh --test
python3 /path/to/anakin_mc/scripts/setup_ort.py
ANAKIN_SDK_PREFIX=/path/to/anakin_sdk/install/anakin_sdk /path/to/anakin_mc/build.sh --test
```

每个包的产物位于自身 `build/包名/`、`install/包名/` 和 `log/`。默认 SDK 前缀是相邻 `../anakin_sdk/install/anakin_sdk`，也可显式指定。构建支持 `--jobs N`、`--build-type TYPE`、`--test` 和 `--` 后的 CMake 选项。

MC 使用固定官方原生 ONNX Runtime 1.20.1，初始化下载并校验到 MC 的 `.deps/`；`ONNXRUNTIME_ROOT` 可指定现有依赖。两个原有 ONNX 模型保留原目录结构，随 MC 安装到 `share/anakin_mc/assets/`，可用 `ANAKIN_LOCO_MODEL`、`ANAKIN_MIMIC_MODEL` 覆盖。模型、动作缩放、历史和过渡算法保持一致。

每个包的 `em_run.sh` 只选择本仓库的安装目录。`ANAKIN_PREFIX` 显式指定当前包的安装前缀，`ANAKIN_SDK_PREFIX` 指定 SDK；缺失或损坏的安装直接报错。Env 与 MC 的 `ANAKIN_PREFIX` 应各自设置。

[AnakinC](https://github.com/zdYukino/AnakinC) 通过 submodule 固定三仓版本，其 `boostup.sh`、`build.sh`、`em_run.sh` 分别提供一键初始化、独立编译编排和 ENV + MC 联合启动。父仓启动器默认 sim，保留 Env 前台交互并在退出时清理 MC；本仓单独入口仍默认 real。官方模拟器单独启动。

启动器检查选定 ELF、其本地 ament 包标记、安装资源和 SDK 构建记录。配置默认只来自该 ELF 所在前缀的 `share/anakin_env/config/`，不从当前目录、源码或其他 overlay 补齐。启动器只执行已安装 C++ ELF，不构建、不安装依赖、不需要 Python 运行时。

SDK 构建元数据记录 Git HEAD；若 SDK 有未提交源代码差异，不能把 HEAD 当作这份差异的新版本，应使用同一源树成套重建 SDK 和两个消费者。发布已提交的 SDK 变更时，先提交 SDK，再把两个消费者的锁更新为实际提交版本并成套重建。具体软件检查使用的前缀、revision 和未提交状态由对应任务证据记录。

## 三个终端分别启动

以下命令供用户实际操作；软件检查不会自动执行它们。

终端一启动 MC：

```sh
cd /home/zdyukino/workspace/anakin_mc
./em_run.sh
```

终端二启动 Env，仿真必须显式写 `--sim`：

```sh
cd /home/zdyukino/workspace/anakin_env
./em_run.sh --sim --input keyboard
```

终端三由用户启动官方模拟器，使用相同 SDK 端点与原始 29 自由度场景：

```sh
cd /home/zdyukino/workspace/unitree_mujoco/simulate/build
./unitree_mujoco -r g1 -i 31 -n lo -s /home/zdyukino/workspace/unitree_mujoco/unitree_robots/g1/scene_29dof.xml
```

正常流程不要求自动 reset、重生或服务握手。需要重置或摆正仿真机器人时，由用户在官方模拟器内手动操作。SDK tick 回退只增加诊断，Env 的状态序号继续单调递增；不会清空 MC 模型、时间线或历史。重复 tick 不算新的物理反馈。仿真允许倒地姿态直接请求六个业务模式；这不保证某个姿态下机器人能站起或策略稳定。

实机用 `./em_run.sh` 或显式 `./em_run.sh --real`。先复制选定安装内的 `real.yaml`，把 `transport.interface` 改成明确的机器人网卡名称，再通过 `--config` 指定。程序不猜测网卡：

```sh
./em_run.sh --real --input remote --config /absolute/path/robot-real.yaml
```

| 模式 | SDK 网卡 / domain | ROS domain | 可选里程计 / 坐标系 |
| --- | --- | --- | --- |
| sim 默认 | `lo` / `31` | `87` | `rt/sportmodestate`，`unitree_sim_world` → `unitree_sim_base` |
| real 默认 | 部署配置的显式网卡 / `0` | `87` | `rt/odommodestate`，`unitree_odom` → `unitree_base` |

两者均使用 G1 HG `rt/lowstate` / `rt/lowcmd`、machine 5、PR 模式和 canonical29 顺序。实机里程计需固件确实提供有效新样本；LowState 本身没有基座位置估计，用户指定站立高度不是测量高度。MC 的 `ROS_DOMAIN_ID` 需与所选 Env YAML 的 `transport.ros_domain` 一致；例如改成 89 时，MC 启动前设置 `ROS_DOMAIN_ID=89`。SDK domain 与 ROS domain 是两套独立通信编号。

## YAML 与输入

安装中包含 `sim.yaml`、`real.yaml`、共用 `inputs.yaml`。通过 `--config PATH` 替换所选模式配置，`--input-config PATH` 替换输入配置，`--input keyboard|gamepad|remote` 覆盖普通控制主来源；覆盖文件的 `mode` 必须与 `--sim` / `--real` 一致。路径可以含空格，应正确引用。启动时一次加载，无热重载；错误或未知字段会报出文件和字段并停止启动。

```sh
./em_run.sh --sim --input gamepad --input-config /absolute/path/inputs.yaml
./em_run.sh --sim --input remote --config /absolute/path/sim.yaml --input-config /absolute/path/inputs.yaml
```

共用输入配置的 `primary` 默认 keyboard；`enabled_sources` 默认包含 keyboard、gamepad、remote。普通模式/速度输入只采纳主来源，所有启用来源的紧急停止都会进入同一个停止条件。选择 remote 时，默认从 HG LowState 的无线遥控负载读取；显式测试可以把 `remote.channel` 改成 `wireless_topic`，读取 `rt/wirelesscontroller`。两者使用同一逻辑映射。

键盘需要前台 TTY，SSH 分配 TTY 后可用；终端不能知道物理按键松开，WASD/QE 速度按 `keyboard.command_timeout_ms`（默认 250 ms）失效，持续移动依赖按键重复。正常退出会恢复终端属性。指定 gamepad/remote 为主来源时，不强制要求可选键盘拥有 TTY。SDL GameController 支持热插拔，静止中位仍按连接状态采样；左摇杆向前统一为逻辑正值。

| 操作 | 键盘 | 游戏手柄 / Unitree 遥控器 |
| --- | --- | --- |
| Disarmed | `1` | L1+R1+Down |
| 紧急停止 / Damping | `2` 或 Space | A 或 B，任意肩键组合，持续有效 |
| Align | `3` | L1+R1+X |
| Hold | `4` | L1+R1+Up |
| ASAP Loco | `5` | **R1+Down** |
| ASAP CR7 | `6` | R1+Up |
| 清除 Fault | `C` | L1+R1+Y |
| 移动开关 | `G` | 无肩键+Left |
| 速度 | WASD / QE | 左摇杆前后/左右、右摇杆转向 |
| 期望高度 | Up / Down | 无肩键+Up / Down |

普通组合精确匹配肩键；同时按 L1+R1 不会冒充“仅 R1”。Unitree 原始 R1 在 bit0、L1 在 bit1，由适配器转换为统一逻辑位。启动或重新连接时已按住的普通按键不会伪造新按下沿，先松开再按；A/B 停止仍优先。仿真主来源丢失默认清零移动并保留模式与策略进程，后续新反馈继续工作；实机默认锁定 Fault，需显式清除后重新走合法授权链。

## 模式、输出与实机顺序

| 状态 | ROS 状态输出（默认） | LowCmd（默认） |
| --- | --- | --- |
| Disarmed | 开 | 关；先撤销本地许可，必要时释放已有命令 |
| Damping | 开 | 已有权限时发送阻尼 |
| Align | 开 | 从新鲜实测姿态插值至固定站立姿态 |
| Hold | 开 | 固定站立 PD |
| ASAP Loco | 开 | 原模型最终 PD 参考 |
| ASAP CR7 | 开 | 原模型最终 PD 参考 |
| Preparing（内部） | 开 | 从 Hold 保持固定站立，其他来源阻尼；收到新事务目标后使用匹配目标 |
| Fault（内部） | 开 | 释放后关闭，等待显式清除 |

这些开关、命令来源、进入/退出动作、守卫和普通自动转换由对应 YAML 的 `states`、`manual_transitions`、`automatic_transitions`、`system_transitions` 定义。关闭输出时先撤销目标和许可；只有拥有权限/此前实际发送过命令时才发送全零、禁用的释放包。实机只读启动和未拥有权限的退出不会先写一个零增益包。

仿真在六个业务状态之间允许直接请求。Align 完成自动进入 Hold；CR7 的 `motion_done` 自动申请新的 Loco 事务。普通手动请求优先于普通自动事件，停止/故障优先级最高；Fault 必须先清除。

实机合法主链为 `Disarmed → Align → Hold → ASAP Loco ↔ ASAP CR7`；Align 完成后自动到 Hold。活动状态的停止/Disarmed 请求经过 Damping 后进入 Disarmed。初始 Disarmed 只接收/发布有效状态，不自动争抢权限；按 `3` 或 L1+R1+X 才启动异步 CheckMode / 必要的 ReleaseMode / 再 CheckMode。RPC 具备单次超时和总期限，不阻塞 2 ms 控制锁。取消立即撤销本地许可，迟到的成功不能启动旧对齐；程序不会自动 SelectMode 或恢复出厂运动服务。失败后清除 Fault 回到 Disarmed，再以新的 Align 请求重新检查。

Preparing 会允许 MC 在匹配的新反馈上预热；该消息的 `execution_authorized` 是推理许可，电机写入另外检查当前事务、新鲜目标、输入停止条件、YAML 输出及后端权限。完成准备还需要当前 TARGET_ACTIVE 的 SDK writer 回执。接收、校验、SDK Write 成功都不等同于执行器实际执行。

仿真与实机共用 `g1_sdk_pd_v1` / `PD_REFERENCE` 和同一个 HG q/dq/Kp/Kd/tau、模式、CRC 编码路径。保留原模型、动作缩放和按实际产出模型选取的增益；不把仿真参考投影到机械角度界限，不新增仿真补偿。实机新鲜度、姿态和可用实测高度等守卫仍由实机 YAML 决定。

## 软件检查与用户验收

软件检查可在不启动 ROS/DDS 节点和模拟器的情况下执行：

```sh
ctest --test-dir /home/zdyukino/workspace/anakin_env/build/anakin_env -L offline --output-on-failure --no-tests=error
ctest --test-dir /home/zdyukino/workspace/anakin_mc/build/anakin_mc -L offline --output-on-failure --no-tests=error
```

已注册测试包含配置/安装路径、状态机、PTY 键盘、注入 SDL/记录遥控、生产控制器和编码器、假的异步权限/写入生命周期、MC 等待门控、历史合约及 stub ELF 启动捕获。它们不运行真实 SDK 客户端、Env/MC 节点、模拟器、probe、runner 或 reset，不证明仿真稳定性或硬件可靠性。

旧的有限集成夹具仅在显式 `scripts/integration_run.sh --offline-test [--demo-switch]` 中运行；它是带 `OFFLINE_FIXTURE` 标签的无物理、无 SDK 通道 ROS 夹具，不是正常 Env。显式 `--sim-test` 是专用的原生仿真测试工具，可能启动其自有模拟器/MC/Env/probe，测试工具自己拥有 `--duration`、证据和进程清理；这些参数不再传给正常 Env。工具仅在用户主动选择时运行，不属于默认软件检查。其 `--reset-at-start` 是显式测试选项，默认关闭；正常三终端流程中只由用户在模拟器里手动 reset。

用户实际仿真/实机验收应检查所需姿态/策略转换、停止、来源丢失、网卡/固件/里程计、接管和命令效果。没有强加固定 30 秒或三次 reset 的完成条件。本次交付的软件构建/离线检查与尚未执行的仿真、硬件验收分别记录；历史 checkpoint 文档保留历史结果，不代表当前启动方式或当前验收结论。
