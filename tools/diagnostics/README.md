# tools/diagnostics — 取证脚本与原始读数

> 为什么这个目录存在：`docs/adr/ADR-003`、`ros2/README.md`、`docs/经典Bug素材录.md` 里引用了一批
> **具体数字**（7 轮 A/B 的符号翻转、毒化实验的 4 轮重复表、图缓存那三格读数）。这些数字当初是在
> 一个临时目录里跑出来的。如果脚本不入库，文档里的表就退化成"我说了算"——而这个项目从头到尾
> 卖点恰恰是"可复现证据"。所以把**产出这些数字的脚本**和**当时的原始输出**一起放进来。

## 前置条件

- 一台 Ubuntu 22.04 + ROS2 Humble 的机器（本项目实测用 VM，`g++ 11.4`）。
- 仓库在远端机器上的路径为 `~/RosProject/dmp`（各脚本里写死的就是这个；可用 `DMP_HOME` 覆盖的只有 `vm_sync_build.sh`）。
- 先构建一次：`colcon build --base-paths ros2 --packages-select dmp_msgs dmp_ros2_bridge`，
  以及 C++ 侧的 `./build_linux/{device_simulator,gateway_service}`（`vm_native_build.sh`）。
- 下表里标 **VM 侧** 的脚本要在装了 ROS 的那台机器上跑；标 **Windows 侧** 的是从本机驱动远端。

## 一览

| 脚本 | 侧 | 干什么 | 产出对应文档里的哪张表 | 性质 |
|---|---|---|---|---|
| `vm_sync_build.sh` | VM | fetch bundle（**只允许 fast-forward**）+ `colcon build` + `colcon test`，并打 `VM_HEAD_BEFORE/HEAD/BUILD_RC` | 每次"VM 构建通过"那句话的来源 | 可复用 |
| `run_vm_rounds.ps1` | Win | 从 Windows 打一条链：bundle → scp → 同步构建 → 跑指定轮次脚本 → 日志落文件 | — | 可复用（`DMP_VM_IP` 必填） |
| `vm_poison_rounds.sh` | VM | 毒化实验：第 1 轮全档（含 21s 的 worst），第 2~4 轮 mid+fast 做重复 | ADR-003 末节之二的 **4 轮重复表** | 可复用 |
| `vm_mid_rounds.sh` | VM | 连跑 2 轮 mid 档，带图诊断 | 「只有首轮干净」那个形状的复现 | 可复用 |
| `vm_ground_rounds.sh` | VM | 连跑 2 轮 fast 档（验证"现场发现"这把新尺子） | [1] 由红转绿的对照 | 可复用 |
| `vm_graph_probe.sh` | VM | **图缓存探针**：独立话题 `/dmp_probe_frame`，分别用优雅 TERM 与 SIGKILL 杀发布者，两路各读一次 | 素材录 B-40 的四格读数（TERM→0/0、SIGKILL→1/0、daemon stop→0/0） | 可复用（不碰被试话题） |
| `vm_probeone.sh` | VM | 含 worst 档的**单轮原始配方**，脚本退出后 +0/15/30s 三次读两路、全程不碰 daemon | B-40 的定因读数、B-42 的"重跑一次" | 可复用 |
| `vm_ab_probe.sh` | VM | 头阻塞 A/B，并发窗口 K 做成参数（H1 vs H2 两个假设对撞） | ADR-003 里被 7 轮 A/B 推翻的那段 | 一次性（K 扫描） |
| `vm_ab_repeat.sh` | VM | 同一脚本不改一行重复 3 轮，只提 A/B 与队列行 | 「差值符号轮次间翻转」那条判据 | 可复用 |
| `vm_diag_c8.sh` | VM | 为什么 active 期 recorder 得到 0 批、组件到底装上没有 | 素材录 B-01 / B-03 / B-07 那族"零数据" | 一次性诊断 |
| `vm_check_verdict.sh` | VM | 不重跑整机验收，只验 `bash -n` + 那段动态结论行的 python | 素材录 B-35（结论行不许写死） | 可复用 |

主验收脚本本身在上一级：`tools/vm_lifecycle_compose.sh`（[0]~[9]，含毒化 A/B、错误隔离、`on_shutdown`）。
它一共有两档，两档的断言总数不同——写进文档的那个数必须带上档位：

| 档位 | 跑哪些格 | 断言数（实跑） |
|---|---|---|
| `POISON=1`（默认全量） | [1]3 + [2]3 + [3]2 + [4]2 + [6]4 + [7]5 + [8]6 | **PASS=25 FAIL=0** |
| `POISON=0`（快档，跳过 [6][7]） | [1]3 + [2]3 + [3]2 + [4]2 + [8]6 | **PASS=16 FAIL=0** |

另一套独立验收也在上一级：`tools/vm_downstream_bag.sh`（下游联动 e1，让生态组件 rosbag2 存/取/回放 `/dmp/frames`）。
它是**另一套 12 条**，与上面那个 25/16 不共用、不相加，写文档时不要当成同一轮的读数：

| 脚本 | 格 | 断言数（实跑） |
|---|---|---|
| `tools/vm_downstream_bag.sh` | [1]阳性对照 + [2][3]录/取逐字段 + [4]不给 override + [4b]静音前置 + [5]回放回比 + [9b]收尾 | **PASS=12 FAIL=0 SKIP=1**（`diagnostic_aggregator` 那格因无 sudo 显式 SKIP） |

## 最短复跑路径

```bash
# 远端机器上, 仓库根目录:
bash tools/vm_lifecycle_compose.sh                      # 全量(含 [6] 毒化与 [7] 隔离, 约 12 分钟)
POISON=0 bash tools/vm_lifecycle_compose.sh             # 只要状态机+on_shutdown 断言(约 3 分钟, 16 条)
POISON=1 POISON_LEVELS="mid" LOG=/tmp/dmp_mid bash tools/vm_lifecycle_compose.sh
```

跑之前先确认远端 `HEAD` 就是本地 `HEAD`（断言数是要写进文档的数字，测错版本等于没测——素材录 B-30/B-43）；
输出请重定向到 **`$LOG` 目录之外**再 scp 取回，用 `Tee-Object` 抓 ssh 输出会被控制台编码二次解码成乱码
（`run_vm_rounds.ps1` 已经改成远端落文件 + scp 取回，并且同时印 `bytes=` 与 `chars=`——中文一字三字节，
把字符数当字节数写进提交信息就是又一次自欺）；取回后若日志里**一行 `PASS=\d+` 都没有**，驱动脚本直接 ABORT——
中途就退的“跑了”不能记成“跑过了”（这条守卫自己第一次负向测试没拦住，样本里一句描述文字含 token 就被哄过去了，见素材录 B-48）。

## 原始读数（`evidence/`）

| 文件 | 内容 |
|---|---|
| `c8_abrepeat.txt` | 7 轮 A/B 的每轮 mt/st 两行与队列行（符号翻转那条判据的原始依据） |
| `c9_runs.txt` | 毒化实验 4 轮（含修尺子后的重跑），每轮的表格 + `PASS=/FAIL=` |
| `c9_ground.txt` / `c9_mid.txt` | fast×2 与 mid×2 轮，含 `图诊断` 两路读数行 |
| `c9_probeone.txt` | 定因那一次：单轮（mid+worst）跑完后的 `+0s/+15s/+30s` 三行两路读数 |
| `c11_full_poison1.txt` | `POISON=1` 整轮（远端 `HEAD=c14f735`）：[6] 八格表 + [7] 五条 + [8] 六条，`PASS=25 FAIL=0` |
| `c11_shutdown_poison0.txt` | `POISON=0` 快档：[8] 六条与状态机十条，`PASS=16 FAIL=0`（[6][7] 按设计跳过，日志里有那行“跳过”声明） |
| `e1_bag_run1_fail.txt` | 下游联动的**红的那一轮**（bytes=2487）：`PASS=6 FAIL=2 SKIP=1 PRED_OVERTURNED=1`，三条原始读数都在——预设 0 / 实测 141 批、`kill -INT` 后仍收 64 帧、回放回比 200 帧找不到 |
| `e1_bag_run2_green.txt` | 改完之后的全绿轮（远端 `HEAD=0692751`，bytes=2522）：录 231 批 / 不给 override 144 批 / live 160–bag 308 逐字段 0 差异 / `deactivate` 后 3s 0 帧 / 回放 200 帧 0 找不到 / [9b] 0/0，`PASS=12 FAIL=0 SKIP=1` |

这些是**当时那一次**的输出原文（GBK/UTF-8 混排，控制台里可能显示成乱码，字节是对的），不是重新生成的摘要。
复跑会得到新数字——尤其是绝对毫秒数会随机器变化；文档里能站住的是**同格配对比值**与符号分布，
不是单次绝对值（素材录 B-36）。

## 已知不完美

- 标"一次性"的三个脚本依赖当时的 `/tmp` 现场（例如 `vm_diag_c8.sh` 假设前一轮日志还在），直接复跑可能只能拿到部分输出。它们留在仓库里的价值是**方法与踩坑点**，不是可重复的结论。
- `vm_sync_build.sh` 里"HEAD 有没有真的前移"用的守卫是检查某个具体新文件存在，不是比较 commit——这是刻意的（已同步时比较 HEAD 会误报，见素材录 B-30）；代价是那个文件日后若改名，守卫要跟着改。
- 远端地址一律不写死：`run_vm_rounds.ps1` 从环境变量 `DMP_VM_IP` 取，没设就直接退出。
