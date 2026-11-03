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

[AnakinC](https://github.com/zdYukino/AnakinC) 通过 submodule 固定三仓版本，其 `boostup.sh`、`build.sh`、`em_run.sh` 分别提供一键初始化、独立编译编排和 ENV + MC 联合启动。父仓启动器默认 sim，Env 无需终端输入并在退出时清理 MC；本仓单独入口仍默认 real。官方模拟器单独启动。

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
./em_run.sh --sim --input auto
```

需要在回环仿真中执行实机状态机和检查时，改用：

```sh
./em_run.sh --sim --real-checks --input auto
```

该选项从当前安装的 `real.yaml` 加载状态、转换、守卫和时序，从 `sim.yaml` 保留 `lo`、非零 SDK domain 和仿真里程计。要指定不同的实机规则，追加 `--real-config /absolute/path/robot-real.yaml`；`--config` 仍选择仿真传输配置。实机权限获取的异步状态流程照常执行，但权限服务是本地模拟替身，不发起实际 MotionSwitcher RPC。模式日志和只读 ROS 参数 `real_checks` / `real_checks_config_path` 标明这一选择。

真实模式发起新的受控状态请求时要求新鲜的主输入；网页控制需先激活租约，手柄/遥控器需在线。主输入丢失时清零运动指令并保留当前状态和已授权输出，不因丢失本身进入 Fault；坏反馈和运行中仿真 reset 造成的 SDK tick 回退等其他故障仍按 real 规则处理。该选项只能使用 `lo` 和非零 SDK domain，验证结果不代表真实权限服务或机器人已通过验收。

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
| sim + real-checks | `lo` / `31` | `87` | 同 sim；状态和守卫来自 real 配置 |
| real 默认 | 部署配置的显式网卡 / `0` | `87` | `rt/odommodestate`，`unitree_odom` → `unitree_base` |

两者均使用 G1 HG `rt/lowstate` / `rt/lowcmd`、machine 5、PR 模式和 canonical29 顺序。实机里程计需固件确实提供有效新样本；LowState 本身没有基座位置估计，用户指定站立高度不是测量高度。MC 的 `ROS_DOMAIN_ID` 需与所选 Env YAML 的 `transport.ros_domain` 一致；例如改成 89 时，MC 启动前设置 `ROS_DOMAIN_ID=89`。SDK domain 与 ROS domain 是两套独立通信编号。

## YAML 与输入

安装中包含 `sim.yaml`、`real.yaml`、共用 `inputs.yaml`。通过 `--config PATH` 替换所选模式配置，`--input-config PATH` 替换输入配置，`--input auto|web|gamepad|remote` 覆盖普通控制主来源；覆盖文件的 `mode` 必须与 `--sim` / `--real` 一致。路径可以含空格，应正确引用。启动时一次加载，无热重载；错误或未知字段会报出文件和字段并停止启动。

```sh
./em_run.sh --sim --input gamepad --input-config /absolute/path/inputs.yaml
./em_run.sh --sim --input remote --config /absolute/path/sim.yaml --input-config /absolute/path/inputs.yaml
```

输入配置使用 `schema_version: 2`，`primary: auto` 默认在 `web`、`gamepad`、`remote` 中自动仲裁。中立设备心跳不会夺取控制；实际摇杆或动作请求才参与争用，同一周期优先级为 Unitree 遥控 > 实体 Joy > 网页。无新操作时保留当前来源；来源切换清除运动开关，需显式重新启用移动。所有启用来源的急停始终有效。当前来源丢失不会被另一个在线来源掩盖，仿真和实机均清零运动并保留当前状态。`--input web|gamepad|remote` 可显式固定来源，`--input auto` 恢复自动模式。

终端键盘及 `keyboard`/`keyboard_bindings` 配置已删除，不要求 TTY。旧 schema 1 输入 YAML 需迁移为 schema 2：删除键盘字段，使用 `web.actions` 列出网页候选动作。SDL GameController 支持热插拔，中立仍持续采样；左摇杆向前统一为逻辑正值。Unitree 默认读取 HG LowState 无线遥控负载，`remote.channel: wireless_topic` 可选择独立无线 Topic。

网页连接 ROS 数据源后，右上方“网页控制”显式获取短租约，关闭、断连、页面隐藏/失焦或进入历史视图会释放输入；重连后必须重新激活。总览右侧 02 切换为 YAML 状态指令；ENV 根据当前状态机和入口守卫输出亮/灰可用性，并在执行时重新检查。“打开虚拟 Joy”显示独立可拖动浮层，摇杆松开回中；LB/L1、RB/R1 可点击保持组合，按钮使用同一 controller_bindings。关闭浮层归零，关闭网页控制恢复关节列表。多个浏览器只能有一个持有网页租约。

Xbox 的 LB/RB 对应逻辑 L1/R1，LT/RT 独立采样为 0..1 模拟扳机。`gamepad.mappings` 可选，填写按精确 32 位小写 GUID 匹配的 SDL 映射字符串；缺省时保留 SDL 自带映射。仓库配置包含已实测的 Xbox Series X 蓝牙 `050000005e040000130b000009050000` 修正：右摇杆 a2/a3、LT a5、RT a4，LB/RB b6/b7。旧 SDL 的通用 Xbox 回退可能与此设备的蓝牙布局不符；[SDL 上游也已移除此类通用回退](https://github.com/libsdl-org/SDL/commit/9567989eb3ce9c858f0fe76806c5ccad69da89ba)。映射随启动 YAML 加载并导出到 UI，其他 GUID 不受这条配置影响。

可用 `build/anakin_env/anakin_env_input_test inspect config` 只读检查正式输入适配器的设备、GUID、实际 SDL 映射、四轴、LT/RT 与 LB/RB。此检查不初始化 ROS/SDK/控制器。自定义 `--input-config` 时应在该文件中同步所需 `gamepad.mappings`，重启 env 生效。

| 操作 | 网页 / 虚拟 Joy | 游戏手柄 / Unitree 遥控器 |
| --- | --- | --- |
| Disarmed | 状态按钮 | L1+R1+Down |
| 紧急停止 | 急停按钮，或虚拟 A/B | A 或 B，任意肩键组合，持续有效 |
| Align | 状态按钮 | L1+R1+X |
| Hold | 状态按钮 | L1+R1+Up |
| ASAP Loco | 状态按钮 | R1+Down |
| ASAP CR7 | 状态按钮 | R1+Up |
| 清除 Fault | 状态按钮 | L1+R1+Y |
| 移动开关 | 运动按钮，或虚拟无肩键+Left | 无肩键+Left |
| 速度 | 虚拟双摇杆 | 左摇杆前后/左右、右摇杆转向 |
| 期望高度 | 高度按钮，或虚拟无肩键+Up/Down | 无肩键+Up/Down |

普通组合精确匹配肩键；同时按 L1+R1 不会冒充“仅 R1”。Unitree 原始 R1 在 bit0、L1 在 bit1，由适配器转换为统一逻辑位。启动或重新连接时已按住的普通按键不会伪造新按下沿，先松开再按；A/B 停止仍优先。输入断开后保留当前模式、策略和已授权输出，清零速度与移动开关；重连后需要重新启用移动。旧自定义配置中的 `source_loss: fault` 只为兼容而接受，启动时会提示该值已不生效，断连行为与 `zero_motion_keep_mode` 一致。

## 模式、输出与实机顺序

| 状态 | ROS 状态输出（默认） | LowCmd（默认） |
| --- | --- | --- |
| Disarmed | 开 | 关；先撤销本地许可，必要时释放已有命令 |
| Damping | 开 | 已有权限时发送阻尼 |
| Align | 开 | 从新鲜实测姿态插值至固定站立姿态 |
| Hold | 开 | 固定站立 PD |
| ASAP Loco | 开 | 原模型最终 PD 参考 |
| ASAP CR7 | 开 | 原模型最终 PD 参考 |
| Preparing（内部） | 开 | 从 Hold 保持固定站立；策略切换时短暂沿用仍有效的上次已写目标，其余情况阻尼；收到新事务目标后使用匹配目标 |
| Fault（内部） | 开 | 释放后关闭，等待显式清除 |

这些开关、命令来源、进入/退出动作、守卫和普通自动转换由对应 YAML 的 `states`、`manual_transitions`、`automatic_transitions`、`system_transitions` 定义。关闭输出时先撤销目标和许可；只有拥有权限/此前实际发送过命令时才发送全零、禁用的释放包。实机只读启动和未拥有权限的退出不会先写一个零增益包。

仿真在六个业务状态之间允许直接请求。Align 完成自动进入 Hold；CR7 的 `motion_done` 自动申请新的 Loco 事务。普通手动请求优先于普通自动事件，停止/故障优先级最高；Fault 必须先清除。

MC 策略间请求使用统一的直接切换，新策略在下一有效状态帧输出；后台历史准备不阻塞切换。新目标到达前，仅可沿用上次实际写入、尚未过期的旧策略目标，旧目标不会冒充新事务回执。CR7 自动返回 Loco 时会屏蔽此前保持的移动开关和速度指令；进入 Loco 后先将摇杆回中，再次推动才恢复运动。Loco 未由策略控制的关节使用 G1 基础 PD 返回 Hold 站姿；其他策略未控制的关节保持切换时的实测姿态。站姿目标不能保证机体在世界坐标中完全不移动。

实机合法主链为 `Disarmed → Align → Hold → ASAP Loco ↔ ASAP CR7`；Align 完成后自动到 Hold。活动状态的停止/Disarmed 请求经过 Damping 后进入 Disarmed。初始 Disarmed 只接收/发布有效状态，不自动争抢权限；点击网页 Align 或按 L1+R1+X 才启动异步 CheckMode / 必要的 ReleaseMode / 再 CheckMode。RPC 具备单次超时和总期限，不阻塞 2 ms 控制锁。取消立即撤销本地许可，迟到的成功不能启动旧对齐；程序不会自动 SelectMode 或恢复出厂运动服务。失败后清除 Fault 回到 Disarmed，再以新的 Align 请求重新检查。

默认 `real.yaml` 的 `allow_fallen: true` 和 `enforce_measured_joint_limits: false` 不以基座倾角、基座高度或实测关节角限制对齐、状态切换及运行输出；`max_tilt_radians` 和 `min_base_height` 在此配置下不生效。反馈完整性、新鲜度、输入、权限和故障处理仍按 real 配置执行。自定义 real 配置可重新开启现有关节限位或姿态检查。该配置同样用于 `--sim --real-checks`；允许从倒地姿态发出对齐命令不代表机器人能够自行站起。

Preparing 会允许 MC 在匹配的新反馈上预热；该消息的 `execution_authorized` 是推理许可，电机写入另外检查当前事务、新鲜目标、输入停止条件、YAML 输出及后端权限。完成准备还需要当前 TARGET_ACTIVE 的 SDK writer 回执。接收、校验、SDK Write 成功都不等同于执行器实际执行。

仿真与实机共用 `g1_sdk_pd_v1` / `PD_REFERENCE` 和同一个 HG q/dq/Kp/Kd/tau、模式、CRC 编码路径。保留原模型、动作缩放和按实际产出模型选取的增益；不把仿真参考投影到机械角度界限，不新增仿真补偿。实机新鲜度、姿态和可用实测高度等守卫仍由实机 YAML 决定。

## 软件检查与用户验收

软件检查可在不启动 ROS/DDS 节点和模拟器的情况下执行：

```sh
ctest --test-dir /home/zdyukino/workspace/anakin_env/build/anakin_env -L offline --output-on-failure --no-tests=error
ctest --test-dir /home/zdyukino/workspace/anakin_mc/build/anakin_mc -L offline --output-on-failure --no-tests=error
```

已注册测试包含配置/安装路径、状态机、网页租约/仲裁、注入 SDL/记录遥控、生产控制器和编码器、假的异步权限/写入生命周期、MC 等待门控、历史合约及 stub ELF 启动捕获。它们不运行真实 SDK 客户端、Env/MC 节点、模拟器、probe、runner 或 reset，不证明仿真稳定性或硬件可靠性。

旧的有限集成夹具仅在显式 `scripts/integration_run.sh --offline-test [--demo-switch]` 中运行；它是带 `OFFLINE_FIXTURE` 标签的无物理、无 SDK 通道 ROS 夹具，不是正常 Env。显式 `--sim-test` 是专用的原生仿真测试工具，可能启动其自有模拟器/MC/Env/probe，测试工具自己拥有 `--duration`、证据和进程清理；这些参数不再传给正常 Env。工具仅在用户主动选择时运行，不属于默认软件检查。其 `--reset-at-start` 是显式测试选项，默认关闭；正常三终端流程中只由用户在模拟器里手动 reset。

用户实际仿真/实机验收应检查所需姿态/策略转换、停止、来源丢失、网卡/固件/里程计、接管和命令效果。没有强加固定 30 秒或三次 reset 的完成条件。本次交付的软件构建/离线检查与尚未执行的仿真、硬件验收分别记录；历史 checkpoint 文档保留历史结果，不代表当前启动方式或当前验收结论。
