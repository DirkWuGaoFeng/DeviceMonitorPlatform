# 经典 Bug 素材录（面试可讲清单）

> 用途：这不是 changelog，是**面试弹药**。每条都写清"当时我以为是什么"，因为面试官真正在问的是
> 你的排查路径，不是结论。编号越小越值得主讲。
> 追加约定：解决一个非平凡问题就加一条，**当天加**，一周后就编不出来了（细节会失真）。
> 结构模板：现象 → 误判路径 → 根因 → 修复 → 可讲点 → 证据/复现。

---

## 0. 索引（30 秒回忆用）

| # | 一句话 | 类型 | 主讲价值 |
|---|---|---|---|
| B-01 | `select(0,...)` 在 Windows 正常、Linux 上"连接成功但零字节" | 跨平台语义 | ★★★ |
| B-02 | `--verify` 用 `COUNT(*)` 绝对值判等，持久库第二次跑必挂 | 自检设计 | ★★★ |
| B-03 | `timeout` 默认 SIGTERM 杀进程不 flush，取证手段自己造出"0 条" | 方法论 | ★★★ |
| B-04 | `qos.get_rmw_qos_profile().depth = 100` 改的是副本，编译过但静默失效 | 静默 no-op | ★★★ |
| B-05 | `LANGUAGES CXX` 让 CMake 静默不编译 `.c`，链接期才炸 | 构建系统 | ★★ |
| B-06 | rosidl 生成的 C 目标要求 `LANGUAGES C CXX`（同族错误在别的工具链重现） | 构建系统 | ★★ |
| B-07 | DDS QoS 不兼容方向与我记的规则相反，实测四格推翻假设 | 认知偏差 | ★★★ |
| B-08 | `pkill -f bridge_node` 的模式串出现在自己命令行里，杀掉了自己所在 shell | 取证陷阱 | ★★ |
| B-09 | `ament_target_dependencies` 走 plain 签名，与 keyword `target_link_libraries` 混用被拒 | 构建系统 | ★ |
| B-10 | 测试写死相对路径，ctest 工作目录不是仓库根 → `fopen` 返回 nullptr → 段错误 | 测试设计 | ★★ |
| B-11 | `NodeOptions::node_name()` 在 Humble 上不存在；改名靠 `__node` 重映射 | API 凭印象 | ★ |
| B-12 | `diagnostic_msgs::msg::KeyValuePair` 应为 `KeyValue` | API 凭印象 | ★ |
| B-13 | `package.xml` 是 XML，描述里写 `<->` 直接解析失败 | 低级但高频 | ★ |
| B-14 | `set -u` 与 ament `setup.bash` 不兼容，脚本"无声死在环境加载" | 脚本 | ★ |
| B-15 | ROS2 服务名带 device_id、话题不带；名字写错只停在 "waiting for service..." | 接口命名 | ★★ |
| B-16 | 探测一个根本没发布的键得到 `?`，看着像故障，实际是验收脚本自己的错 | 方法论 | ★ |
| B-17 | Qt 主窗口 `status_` 未初始化即使用 → 启动即 0xC0000005 | 生命周期 | ★ |
| B-18 | 串口 `CreateFileA err=5` 是被残留进程占用；`err=161` 是 `\\.\COM4` 前缀写错 | 平台细节 | ★ |
| B-19 | MinGW exe 启动即退 `0xC000007F`：PATH 里旧 `libstdc++-6.dll` 抢先 | 部署 | ★ |
| B-20 | 网关 `parseCommand` 统一转小写，命令词比较必须按小写写 | 协议实现 | ★ |
| B-21 | Ubuntu apt 版 gRPC 无 CMakeConfig，必须走 pkg-config 探测 | 依赖集成 | ★ |
| B-22 | `repeated` 枚举在 C++ 是 `RepeatedField<int>`，构造 `vector<Enum>` 触发 static_assert | 语言绑定 | ★ |
| B-23 | `git bundle verify` 在非仓库目录跑 → 假阴性，clone 成功才是有效证明 | 取证陷阱 | ★ |
| B-24 | CI 那个 exit 1 我一口咬定是自检逻辑——实际失败在**构建阶段**，后两步根本 skipped | 认知偏差 | ★★★ |
| B-25 | CI 装 Qt 的 `modules:` 漏了一个，而本地全量安装永远复现不了 | 构建/环境差异 | ★★★ |

---

## 1. 跨平台语义差异（这类最值钱：现象和根因隔着两个操作系统）

### B-01 ★ 设备模拟器在 Linux 上"连接正常、一个字节都不传"

- **现象**：VM 里跑 `device_simulator` + 桥，端到端第一次全是零数据。控制面完全正常——`/dmp/rules`
  有默认阈值、`set_rule` 返回 accepted、非法区间被拒；但 `STATS ok=0`、`/dmp/frames` 空、`decoded_ok=0`。
  TCP 连接看着是"建立成功"的。
- **误判路径**：我先怀疑桥的解析逻辑、又怀疑 QoS 不匹配、再怀疑 VM 与 Windows 的时钟/缓冲。
  这三个方向都"能解释零数据"，所以都排不掉——这是典型的**多个假设同时成立**的困局。
- **根因**：`select()` 的首参 `nfds` 语义在两平台不同。Windows 忽略它；POSIX 要求它是 `max_fd + 1`。
  代码写的是 `select(0, ...)`，在 Linux 上等于**一个描述符都不检查**，`accept()` 永远不会被调用。
  而三次握手由内核 listen backlog 自己完成了，所以客户端 connect 成功、发送成功、就是没有任何数据。
- **修复**：把差异显式写开，而不是"找个两边都行的写法"：
  ```cpp
  #ifdef _WIN32
      const int nfds = 0;          // Windows 忽略首参
  #else
      const int nfds = listener + 1;  // POSIX: max_fd + 1
  #endif
  ```
  提交 `8fb6e66`，并把症状注释写进代码，防止后人看到 `+1` 手贱删掉。
- **可讲点**：① "连接建立"不等于"服务在响应"，backlog 会替你骗过 telnet；
  ② 全仓 grep 同类调用才是真修复——我当时 grep 了所有 `select(`，确认只有模拟器这一处写错
  （`gateway_client` 用的是 `fd+1`，`gateway_service` 自己维护 `nfds`），
  **修一处容易，证明只剩一处才是工程**。

### B-05 / B-06 同一族错误在不同构建系统里重现

- **现象**：Windows 侧长期全绿；Linux 首次链接报 `undefined reference to dmp_crc16_ccitt`。
  后来 ROS2 包 `dmp_msgs` 又独立报 `Cannot determine link language for target dmp_msgs__rosidl_generator_c`。
- **根因**：`project(... LANGUAGES CXX)` 时，CMake 把 `.c` 列进源文件列表**既不报错也不编译**；
  而 Windows 侧我手工用 `g++` 编，`.c` 被当 C++ 处理，侥幸通过。
  ROS2 那次是同一机制的第二幕：rosidl 为每个 msg 生成纯 `.c` 的 `*_rosidl_generator_c` 目标，
  不启用 C 编译器就链接不起来。
- **修复**：`LANGUAGES C CXX`；并且**验证生成物存在**而不是验证"配置成功"——
  `find build/... -name '*.c' | wc -l` 当时是 `0`，这一条就是铁证。
- **可讲点**：单工具链上跑绿的"跨平台代码"，其实是单平台代码。证"编译通过"不如证"产物存在"。

### B-10 单元测试在 CI 里段错误，本地怎么跑都正常

- **根因**：测试写死 `build/_storage_test.csv`，而 `ctest` 的 cwd 是构建目录不是仓库根 →
  父目录不存在 → `fopen` 返回 `nullptr` → 紧接着 `fprintf(fp, ...)` 未检查句柄 → UB。
- **修复**：产物落在当前 cwd（不带子目录），并补 `CHECK(fp != nullptr)`、容器 `if (!v.empty())` 守卫。
- **可讲点**：测试的**环境契约**要写进代码；另外"崩溃在断言之前"往往说明有 unchecked handle。

### B-18 / B-19 / B-20 Windows 侧三个静默故障

- `CreateFileA` 返回 `err=5`(ACCESS_DENIED) 九成是 `monitor_serial`/`qt_monitor` 残留进程占着 COM 口，
  不是驱动问题；`err=161`(BAD_PATH) 才是路径写法——C++ 字面量要 `"\\\\.\\" "COM4"`，少一个反斜杠就打不开。
- MinGW 编出的 exe 在装有旧 MinGW 运行时的机器上启动即退，`0xC000007F`(ERROR_PROC_NOT_FOUND)：
  PATH 里旧版 `libstdc++-6.dll` 抢先加载。修复是把三个 DLL 拷到 exe 同目录（exe 旁优先于 PATH）。
- 网关 `parseCommand` 把命令统一转小写，所以新增命令词必须按小写比较；
  正解是给 `CmdKind` 加强类型枚举分支，而不是拿字符串比大写。

---

### B-25 ★ CI 装 Qt 的 `modules:` 漏了一个——本地全量安装，所以永远复现不了

- **现象**：Windows 本地用 Qt 官方安装的 6.8.1 mingw_64 编 `qt_monitor` 全绿（Core/Gui/Widgets/
  Charts/Sql/Network/SerialPort 全部链接成功）；Linux CI 的 `Build qt_monitor` 腿则**每次都在 configure
  阶段挂**，而 CI 状态徽章是绿的（该腿 `continue-on-error: true`），只留一句干巴巴的 `exit code 1`。这个状态持续了好几轮。
- **误判路径**：先归因于“缺系统图形库”（于是加了 apt 那一步）、再归因于“`--verify` 非幂等”（B-02/B-24）。
  两个方向都不是。**注意这三个假设全部仍能在“本地绿、远端红”下成立——这正是排不掉的原因。**
- **拿证据**：把失败腿的 `CMake Error` grep 成 `::error::` 注解（匿名可读），**第一次跑就拿到原话**：
  `CMake Error at CMakeLists.txt:44 (find_package)` —— 第 44 行正是
  `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Sql Network SerialPort)`。
- **根因**：`install-qt-action` 只装 `modules:` 里点名的模块（Charts / SerialPort 是 Qt 独立仓库的模块，
  **不随 qtbase 打包**），而我那里只写了 `qtcharts`。本地是安装向导里的“全选”，所以两边清单不同构。
- **修复**：`modules: qtcharts qtserialport`；并把注解再推进一步——configure 失败时额外报一行
  `缺组件config: Qt6Xxx`（拿 find_package 的组件名单去 `find -name 'Qt6*Config.cmake'` 里逐个对），
  以及时把“假设”和“事实”分开：即使下次仍挂，那一行会直接指出哪个组件没落地。
- **可讲点**：① **本地装了全部依赖，就永远复现不了“白名单安装”的 CI**——依赖清单得有两个独立的声明面，
  面与面不同构就是 bug；真正稳的做法是用同一个源生成两处（组件列表 → modules 参数）。
  ② `continue-on-error` 的软失败腿必须自带**可报警的面**，否则它红了几周也没人看；
  ③ 拿不到日志就去拿能拿到的面（注解/步骤结论）——这一条直接把周期从“几轮猜”缩到“一轮定”。

---

## 2. 静默 no-op 与"凭印象写 API"（编译器能救的都不算事故，编译器救不了的才是）

### B-04 ★ 改 QoS 队列深度，编译通过、运行不报错、深度没变

- **现象**：写 `qos.get_rmw_qos_profile().depth = 100;` 想让可靠队列变深，跑起来毫无变化。
- **根因**：`get_rmw_qos_profile()` **返回副本**，改完即丢。队列深度只能通过
  `rclcpp::QoS(rclcpp::KeepLast(200))` 设定。
- **可讲点**：最危险的一类 bug 是"返回值是副本，你却在其上赋值"。C++ 里这一族还包括
  `map[k] = v` 对 `operator[]` 返回 const 引用的误解、`std::optional` 拷贝后改原件……
  识别信号：**代码读起来像命令句，但它没有可观察副作用**。

### B-11 / B-12 / B-13 三条"记不清就写"的成本

- `rclcpp::NodeOptions` 在 Humble 上**没有** `node_name()`；改名机制是 launch_ros 的 `name=` 下发
  `__node` 重映射，由 `rcl_init` 覆盖构造器里的字面量名字。
- `diagnostic_msgs::msg::KeyValue`，不是 `KeyValuePair`。
- `package.xml` 是真 XML：`<description>` 里写 `A <-> B` 会让 ament 解析失败，
  报错只给 `not well-formed (invalid token): line 7, column 10`，跟"内容"看起来无关。
- **可讲点**：这三条都是**上一轮我口头"确信"的结论**。教训是把猜测留在注释里的代价——
  我在注释里写过一条方向错误的 QoS 结论，编译器把它抓出来了，我在提交里同时把注释也改了，
  **错误注释比没有注释更贵**。

### B-17 Qt 主窗口启动即崩

- `status_`（QLabel*）未初始化就被 `setStatus()` 解引用 → `0xC0000005`。
- 修复：构造期先 `status_ = new QLabel("未连接")`。
- **可讲点**：这类崩溃的通用防法是"成员指针默认 nullptr + 使用前 assert"，
  而在 `--verify` 无头自检里它被抓出来了——**能写进 CI 的自检就不要依赖人手工点界面**。

---

## 3. 取证手段自己造假（我个人认为最有面试价值的一组）

### B-03 ★ 我的"0 条数据"是计时方法造出来的

- **现象**：QoS 实验里订阅端打印 0 条，我一度认定"配置没生效"。
- **根因**：`timeout` 默认发 **SIGTERM**，被杀的 python/ros2 进程**不 flush 块缓冲**，
  缓冲区里的数据整块丢失 → 看起来一条都没收到。
- **修复**：一律用 `timeout -s INT`（SIGINT 走优雅退出与 flush），并把这条写进脚本注释：
  ```bash
  # 计时口径: 用 SIGINT 而非默认 SIGTERM —— 被 SIGTERM 杀掉的 python 进程不 flush,
  # 块缓冲里的数据会整体丢失, 于是"0 条"可能是取证手段自己造成的假象。
  ```
  同类：读网关多行应答时 `timeout 2 head -3 <&3` 也会丢（stdout 是管道时 head 按块缓冲），
  换成 python 显式 `recv` 循环。
- **可讲点**：**先怀疑测量方法，再怀疑被测系统**。这一条让我把 B-07 的结论纠正了。

### B-07 ★ DDS QoS 不兼容方向，我记的是反的

- **我的假设**：发布 RELIABLE + 订阅 best_effort 会"连得上收不到"。
- **实测四格**（`tools/vm_qos_mismatch.sh`，每格独立计时取数）：

  | 发布端 | 订阅端 | 实测条数 |
  |---|---|---|
  | best_effort | reliable | **0** |
  | best_effort | best_effort | 128 |
  | reliable | best_effort | **136**（我以为会是 0） |
  | reliable | reliable | 132 |

- **正确规则**：reliability 匹配是"发布端能力 ⊇ 订阅端要求"。
  真正静默无数据的方向是**发布 best_effort、订阅端要求 reliable**。
  我还用 `ros2 topic info --verbose` 证伪了"配置没生效"这个替代解释，避免把两个错误混成一个。
- **可讲点**：讲这条时要突出**我没有停在"实测 136 条与预期不符"就下结论**，
  而是把它写成四格矩阵 + 用 `topic info` 排除配置未生效这个竞争假设。

### B-08 `pkill -f` 把我的远程 shell 自己杀了

- **现象**：`ssh host "pkill -f bridge_node; bash acceptance.sh"` —— 脚本一行都没执行。
- **根因**：`pkill -f` 匹配整条命令行；`bridge_node` 这个字符串出现在**我自己这条远程命令的命令行里**，
  pkill 只排除自己，不排除父 shell，于是把承载命令的 bash 干掉了。
- **修复**：`pkill -f '[b]ridge_node'`（括号法：正则匹配目标，但字面量不再匹配自身）。
- **可讲点**：任何"按模式找进程并杀"的自动化都有这个自伤面，脚本里杀进程要么用 PID 文件，要么用括号法。

### B-14 / B-16 / B-23 另三条"工具骗我"

- `set -u`（或 `-euo pipefail`）与 ament `setup.bash` 不兼容：环境脚本内部引用未定义变量，
  脚本死在 `source` 那一行，**现象是"环境没加载"而不是报错行**。→ ROS 脚本里 source 段用 `set +u`。
- 验收脚本去 grep 一个桥根本没发布的键（`dropped`），得到 `?`，看着像链路故障。
  → 探测字段必须先确认发布方真的发了；桥的 `/diagnostics` 只有 `decoded_ok`/`crc_err`。
- `git bundle verify` 在非仓库目录跑会报 fail（它需要参照仓库）→ 假阴性；
  同一个脚本里 `git clone` 成功本身就是 bundle 有效的证明。
- 附送 PowerShell 侧的同类坑：`-DCMAKE_MAKE_PROGRAM=$var` 不加引号不展开；
  `(cd x; cmd)` 语句内括号 ParserError；`原生命令 | Select-Object -First N` 会掐断原生命令；
  向 bash 传脚本必须先剥 CR 且无 BOM。

---

## 4. 接口与协议设计（不是 bug，是"为什么这样定"）

### B-15 服务名带设备段，话题不带

- `/dmp/frames`（话题按 `topic_prefix`）vs `/dmp/bed01/set_rule`（服务按 `prefix/device_id`，因为多设备=多实例）。
  我的验收脚本把服务写成 `/dmp/set_rule`，结果只有一行 `waiting for service to become available...`，
  **没有任何错误信息**，卡满超时。
- **可讲点**：ROS2 服务发现失败不报错是这个框架的已知体验；
  对策是把"服务是否存在"做成脚本里的显式断言（`ros2 service call` 前先 `ros2 service list | grep`）。

### 下行只写判定层（ADR-002）

- 桥的写操作只允许改**告警判定阈值**，不碰蜂鸣/停机/测量链路。理由：这样任何 ROS 侧误操作的最大后果
  是"误告警/漏告警"，而不是"给出错误测量值"。判据用断言钉住：写路径断言 `stat().ok` 不变。
- **可讲点**：安全边界不写在文档里，写成**可执行断言**——这是我在医疗器械语境下最愿意讲的一条。

---

## 5. 自检与断言设计缺陷（这类最容易被当成“环境问题”）

### B-02 ★ `--verify` 只有在空库时才会通过（自检脚本自己不可重跑）

- **现象**：本地跑 `qt_monitor.exe --verify` 得
  `VERIFY ok=50 crcErr=0 drained=50 db_rows=358 chart_pts=50 N=50 -> FAIL`。
  前三项全对，只有 `db_rows` 不对。
- **误判路径**：我立刻把它当成“CI 那条腿 exit 1 的根因”，还先把结论写进了 CI 注释——
  后来逐步核实发现错了（见 B-24）。**两个都是真 bug，但不是同一个**。
- **根因**：断言写的是 `rows == N`，而 `rows` 来自 `SELECT COUNT(*) FROM samples`；
  `device_history.db` 是**跨次运行的持久文件**（旧行还在里面）。所这个自检从设计上就只在空库时成立。
- **修复**：改成取增量，并把“开库失败”与“增量不符”分开可观察：
  ```cpp
  const int rowsBefore = dbSink_.isOpen() ? dbSink_.rowCount() : 0;
  ...
  bool pass = (st.ok == N && drained == N && rows >= 0 && newRows == N && pts == N && st.crcErr == 0);
  // 输出变成: db_rows=408(+50) ... db=open -> PASS
  ```
  本地连跑 3 次全 PASS，绝对行数在涨（408/458/508）但增量恒为 +50。
- **可讲点**：自检/回归脚本必须做到**无环境前置 + 可重跑**。拿“全局绝对量”做断言的测试，
  第一次跑绿只说明“环境恰好干净”。同一族写法：用全局内存占用断言泄漏、用 `ls` 条数断言新增文件、
  用表总行数断言本批写入。

### B-24 ★ CI 的 `exit code 1` 不等于“自检判 FAIL”：我先猜了根因，证据扇了我一耳光

- **现象**：徽章是绿的（Qt 腿 `continue-on-error`），但注解里永远挂一句
  `Linux · Qt6 上位机 (--verify 无头自检) Process completed with exit code 1.`——就这一句，没有步骤名。
- **我的错误做法**：本地复现 `--verify` FAIL（B-02）后，我直接把它当成 CI 的根因，还写进了 ci.yml 注释。
  这一步我**没有拿任何 CI 侧证据**就下了结论。
- **真实事实**（用公开页面/API 逐步拿到的 conclusion）：

  | 步骤 | 结论 |
  |---|---|
  | install-qt-action / Install Qt6 headless deps / Tool versions | success |
  | **Build qt_monitor** | **failure** ← 就是这里 |
  | Link check | **skipped** |
  | Headless verify | **skipped** |

  即 exit 1 发生在 **CMake configure/编译阶段**，`--verify` 根本没被执行到。
- **为什么拿不到日志**：Actions 日志正文**需登录**；匿名访问时折叠条能点开但内容区为空，
  REST 日志端点返 403（`Must have admin rights to Repository`）。唯一匿名可读的面是**注解（annotations）**。
- **对策（已写进 ci.yml）**：失败时把根因 grep 出来用 `::error::` 当注解报出去 ——
  注解会出现在 Summary 页且**匿名可读**，不需要任何人交凭据：
  ```bash
  { cmake -S . -B build_qt ...; echo "CFG_RC=$?"; } > cfg.log 2>&1
  grep -q 'CFG_RC=0' cfg.log || echo "::error::configure 失败(本行匿名可读): $(grep -E -A1 'CMake Error' cfg.log | tr '\n' '|')"
  ```
- **后续（一次 CI 就兑现）**：下一次跑（Run #4）的 Summary 页匿名就能看到新注解原话：
  `configure 失败(本行匿名可读): CMake Error at CMakeLists.txt:44 (find_package):|` —— 根因见 B-25。
  经验是：**可观测性投入的兑现周期可以短到一轮**，所以它值得和修复放在同一个提交里做，而不是“下次再说”。
- **可讲点**：① 先定住“**挂在哪一步**”，再谈根因；步骤结论在公开 API 里就能拿到，不需要日志权限；
  ② 本地能复现的 bug 不等于远端那个 bug，**同一个症状族里可能有两个独立缺陷**；
  ③ 可观测性要设计在**自己能读的那个面上**（注解 > 步骤输出 > 日志）；
  ④ 我把写错的注释当场改回“待下一次注解锁定”，而不是留着好看。

---

## 6. 这一轮链路的完整时间线（可作为"我如何调试"的样板回答）

1. 端到端零字节 → 不猜，grep 全仓 `select(` → 定位 `select(0,...)`（B-01）。
2. 修完有数据：`STATS ok=396 crc_err=0 dropped=0`、`/dmp/frames` 出真帧。
3. QoS 实验"0 条" → 怀疑测量方法 → `timeout -s INT` 重做四格矩阵（B-03）→ 发现自己的规则记反（B-07）→
   改代码注释 + 改 launch 注释 + 写进 `ros2/README.md` 复盘。
4. 桥首次真编译抓出 4 个错（B-11/B-12/B-04 + 一个未声明成员函数）。
5. CI 徽章绿但注解里挂一个 `exit code 1` → 本地复现 `--verify` 的 `db_rows=358 -> FAIL` → 修成增量计数
   → 连跑 3 次 PASS（B-02）。但我当时把这句当成了 CI 的根因并写进注释 —— 后续逐步核实发现 CI 其实挂在
   `Build qt_monitor`，后两步 skipped（B-24）。
6. 把根因改成匿名可读面（`::error::` 注解）→ **下一轮 CI 就拿到** `CMake Error at CMakeLists.txt:44
   (find_package)` → 发现装 Qt 的 `modules:` 漏了 `qtserialport`（B-25）。
7. 跨机真板验收：裸 TCP 先证网关在出帧 → 再起桥 → `/dmp/frames` 33~35 条真帧、
   `/diagnostics decoded_ok=89 crc_err=0`、`set_rule` 后**网关侧独立回读** `HR low=40.000 high=105.000`、
   倒置区间 `accepted=False reason='need low < high'`、`selftest healthy=True ok=3034`。
   脚本最后自动把 HR 阈值恢复默认（验收脚本必须还原现场）。

---

## 7. 追加模板

```markdown
### B-XX ★? <一句话标题>
- **现象**：用户/日志看到的原样症状，包含"看起来正常"的部分。
- **误判路径**：我先后怀疑过什么，为什么每个都能解释症状。
- **根因**：机制层面一句话。
- **修复**：代码/配置怎么改，提交号。
- **可讲点**：脱离本项目仍然成立的那条原理。
- **证据**：命令与实测数字（可复现）。
```
