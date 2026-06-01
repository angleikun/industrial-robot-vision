# DEVLOG —— 工业机器人视觉引导系统开发日志

> 记录从初版骨架到 T3 全链路跑通的过程中，发现并修复的所有关键 bug。
> 每个 bug 都包含：现象、根因、修复位置、教训。

---

## 项目背景

- **技术栈**：Qt 6.11 + HALCON 24.11 + OpenCV 4.8 + MSVC 2022 + Windows 11
- **架构**：5 层（UI / 业务协调 / 算法 / 数据 / 通信）+ 4 线程
- **协作模式**：Claude（架构/设计/Review）+ Claude Code（自动化执行）+ 人工迭代调试

## 总览

整个开发周期识别并修复了 **21 个生产级 bug**，分布如下：

| 类别 | 数量 | 占比 |
|---|---|---|
| HALCON API 误用 | 5 | 24% |
| Qt 线程 / 信号槽 | 7 | 33% |
| C++ 通用错误（锁、单例、生命周期） | 3 | 14% |
| 协议层 / 数据库 | 2 | 10% |
| UI 交互 / 可观测性 / 编码 | 4 | 19% |

---

## Bug 修复明细（按发现时序）

### B01 — HALCON #1305: Wrong control parameter 5

**现象**：点击「创建模板」后程序崩溃，HALCON 抛出错误 `#1305: Wrong value of control parameter 5`。

**根因**：`create_shape_model` 的 `Optimization` 和 `Metric` 两个参数位置写反。HALCON C++ API 中：
```
CreateShapeModel(Template, NumLevels, AngleStart, AngleExtent,
                 AngleStep, Optimization, Metric, Contrast, MinContrast)
```
`Optimization` 是第 6 个参数（值如 `'auto' / 'point_reduction_*'`），`Metric` 是第 7 个（值如 `'use_polarity' / 'ignore_polarity'`）。原代码把值传反了，HALCON 检测到 `Optimization` 位置传了无效值就抛 #1305。

**修复**：`src/VisionProcessor.cpp` createTemplate 函数，调整参数顺序，并加注释提醒。

**教训**：HALCON 算子参数顺序在 HDevelop 文档里清楚，但 C++ binding 的方法签名容易被脑补错。**调用前查文档**比凭记忆写靠谱。

---

### B02 — HRegion 参数顺序错（3 处）

**现象**：ROI 区域和实际选择的矩形差一个对角线翻转，模板创建出来位置完全不对。

**根因**：`HRegion(Row1, Column1, Row2, Column2)` 是 (行, 列) 顺序，不是 (x, y)。原代码写成 `HRegion(y, y+h, x, x+w)`——row 给了两次、column 给了两次，得到的是一条对角线区域。

**修复**：`VisionProcessor.cpp` + `MeasurementEngine.cpp` 共 3 处，统一改为 `HRegion(y, x, y+h, x+w)`。

**教训**：HALCON 全套坐标系都是 `(row, column)`，不是图形学常用的 `(x, y)`。从 OpenCV / Qt 切到 HALCON 时，**坐标系切换是高发错误点**。

---

### B03 — QImage → HImage 步长（stride）未处理（3 处）

**现象**：模板创建有时成功有时崩溃；崩溃时图像数据看起来"歪"了。

**根因**：HALCON 的 `GenImage1` 要求像素数据**逐行紧密排列**（每行字节数 = width × bytes_per_pixel）。Qt 的 `QImage` 32-bit 格式每行末尾**有 padding**（4 字节对齐），实际步长（`bytesPerLine()`）≥ `width × 4`。直接把 `QImage::bits()` 喂给 `GenImage1`，从第二行开始数据就错位了。

**修复**：在 3 个文件（VisionProcessor, MeasurementEngine, CalibrationManager）的 `qImageToHImage` 函数里加 row-by-row 的 `memcpy`，把数据转成紧密排列的 `std::vector<uchar>` 再传给 HALCON。

**教训**：跨库的内存布局假设是 bug 高发地带。Qt 的 padding 和 HALCON 的紧密排列是两个独立的事实，必须显式适配。

---

### B04 — `loadTemplate` 递归锁死锁

**现象**：点击「加载模板」后程序卡死，CPU 占用 0%（不是死循环），日志不再输出。

**根因**：`loadTemplate` 持有 `m_modelMutex`（QMutex，非递归）后调用了 `clearTemplate()`，而 `clearTemplate` 内部又试图获取**同一把锁**——同一线程二次加锁同一把非递归 QMutex 就是死锁。

**修复**：拆出私有 `clearTemplateUnlocked()`（不加锁），`loadTemplate` 在持锁时调用它；公共 `clearTemplate()` 仍然加锁再调用 unlocked 版本。

**教训**：QMutex 默认不可重入（C++ `std::recursive_mutex` 也不是默认）。**任何一把锁内部调用的函数，如果可能尝试加同一把锁，就要拆出 unlocked 版本**。

---

### B05 — 视觉线程帧积压 + GUI 直接调 createTemplate

**现象**：点击「停止采集」后界面整体冻结，需要任务管理器强制结束。

**根因**：两个问题叠加：
1. 视觉线程没有 busy guard，每帧都 enqueue 处理请求，相机线程发得比处理快 → 积压上千帧
2. `createTemplate` 是 GUI 线程通过 `Direct Connection` 调用的，进入视觉线程的事件循环时阻塞在 mutex 上

**修复**：
- 加 `std::atomic<bool> m_busy` 帧丢弃保护（RAII `BusyGuard`），处理中的帧若收到新帧直接丢
- 改为 `Q_INVOKABLE` + `Qt::BlockingQueuedConnection`，GUI 线程显式等待视觉线程处理完
- `m_modelValid` 改为 atomic

**教训**：Qt 跨线程交互**永远不要用 Direct Connection**。线程间共享状态要么用 atomic，要么用 BlockingQueuedConnection 显式同步。

---

### B06 — `mapHalconError` 把 #1301 误判为 NO_LICENSE

**现象**：实际是参数错误的 HALCON 异常，被识别成 License 失效，触发降级模式，所有视觉功能被禁用。

**根因**：原代码用 `error >= 5000` 粗暴判定为 License 错误。实际上 HALCON License 错误码范围是 **5200–5399**，#1301 是参数错误（`> 1000` 系列），完全不在 License 范围内。

**修复**：`mapHalconError` 严格限制 License 错误码为 5200-5399 区间。

**教训**：错误码的归类必须查官方文档，**不要用"差不多"的范围**。错误判断错了，连锁反应（自动降级、UI 禁用）会让症状变得很难诊断。

---

### B07 — License 检查成功路径从不启用「创建模板」按钮

**现象**：License 实际是有效的（HDevelop 里能用），但程序里「创建模板」按钮一直灰着。

**根因**：按钮初始化时 `setEnabled(false)`，只在 `onLicenseStatusChanged(true)` 里才会重新启用。而 `checkHalconLicense()` 在 License 有效路径**直接返回**，没有调用 `onLicenseStatusChanged`。

**修复**：`checkHalconLicense()` 成功路径显式调用 `onLicenseStatusChanged(true)`。

**教训**：UI 状态机要有**单一信号源**。"成功就什么都不做"听起来很自然，但留下了"按钮永远启用不了"的死路。

---

### B08 — 匹配得分阈值滑块是死的

**现象**：界面里的「匹配得分阈值」spinbox 转来转去，匹配行为完全不变。

**根因**：`onMatchScoreChanged` 槽函数存在但根本没连接到 spinbox 的 `valueChanged` 信号，spinbox 的值从未传给 `setMatchConfig`。

**修复**：`applyMatchConfig()` 把 spinbox 当前值打包成 `MatchConfig` 推给 `VisionProcessor`，并把 spinbox 的 `valueChanged` 连接到 `applyMatchConfig`。

**教训**：UI 控件创建出来后**必须验证一遍信号槽**——光看代码读不出来"漏连了"，要靠运行时操作每个控件确认它真的有效。

---

### B09 — Logger 单例非线程安全

**现象**：偶发的日志条目丢失，多线程同时写日志时偶尔看到字符串拼接错乱。

**根因**：经典 `if (!s_instance) s_instance = new Logger()` 模式，多线程同时调用 `instance()` 可能创建出多个实例。

**修复**：改用 C++11 magic static：
```cpp
Logger* Logger::instance() {
    static Logger inst;
    return &inst;
}
```
编译器保证线程安全初始化，无需 double-check locking。

**教训**：C++11 后**所有单例都该用 magic static**。手写 double-check locking 容易写错（需要 memory barrier），不如交给标准。

---

### B10 — 重连导致 GUI 周期性冻结 1 秒

**现象**：机器人模拟器没启动时，主程序界面每秒卡顿一次，鼠标点击没响应。

**根因**：`onReconnectTimer` 每秒触发重连，使用 `Qt::BlockingQueuedConnection` 调用 worker 线程的 `waitForConnected(1000ms)`——GUI 线程阻塞**最长 1 秒**等 worker 完成。

**修复**：改为 `Qt::QueuedConnection`（fire-and-forget），靠 worker 的 `connected/disconnected` 信号驱动状态机，GUI 线程完全不阻塞。

**教训**：`BlockingQueuedConnection` 是反模式陷阱——AI 写代码常默认用它，看起来"简单同步"，实际把异步操作变成隐式阻塞。**周期性任务里出现 Blocking 几乎一定是 bug**。

---

### B11 — `saveGrabRecord` 外键约束失败

**现象**：发送抓取位姿后日志报 `FOREIGN KEY constraint failed: Unable to fetch row`。

**根因**：`GrabRecord.detId` 默认值是 `-1`，SQL `INSERT` 直接绑了这个值。但表结构定义了 `det_id REFERENCES detections(id)`，SQLite 检查到 `detections.id = -1` 不存在 → 抛 FK 错误。

**修复**：`saveGrabRecord` 里判断 `detId < 0` 时绑定 `QVariant()`（SQL NULL）而非数值；外键约束允许 NULL。

**教训**：SQL FK 是"严格类型"——`-1` 不是"无关联"，是"指向不存在的 id"。**无关联要用 NULL**，编程语言里的 `-1` 哨兵在 SQL 里没意义。

---

### B12 — `qInstallMessageHandler` 未安装，HALCON 错误丢失

**现象**：HALCON 抛异常（如 #1305）时，GUI 日志窗口什么都没显示，只在终端崩溃前一闪而过。

**根因**：代码里有大量 `qWarning() << "HALCON error: ..."`，但 `main.cpp` 从未调用 `qInstallMessageHandler`，所以这些消息走 Qt 默认 handler（stderr），而 GUI 程序的 stderr 是关闭的——**全部沉默丢弃**。

**修复**：`main.cpp` 安装自定义 handler，把 `QtDebugMsg / QtWarningMsg / QtCriticalMsg` 路由到 `Logger`，写进日志文件。

**教训**：**可观测性是基础设施，不是奢侈品**。如果错误没有被记录，那它"没发生过"——后续调试基本靠猜。这个 bug 修复后所有其他 bug 的诊断难度都下降了一档。

---

### B13 — ImageView 左键松开重复发射信号

**现象**：测量模式下偶尔触发不该触发的 ROI 重建。

**根因**：`mouseReleaseEvent` 先检查"是不是 ROI 拖拽完成"，再检查"是不是点击拾取点"——两个条件可能**同时为真**（用户拖了几像素），导致 `roiSelected` 和 `pointPicked` 同时发射。

**修复**：ROI 有效完成后 `return`，跳过 `pointPicked` 判定。

**教训**：UI 事件路由的多分支逻辑要明确**互斥性**——用 `if / else if` 而非两个独立 `if`，或在第一个分支末尾显式 `return`。

---

### B14 — "机器人状态: 就绪" 日志每秒刷屏

**现象**：连接机器人后，日志窗口每秒钟弹一条 `机器人状态: 就绪`，几秒就把别的信息淹没。

**根因**：心跳响应每秒触发 `statusChanged(READY)`，对应的 slot 不管状态有没有变都 `appendLog`。

**修复**：用 `m_lastRobotStatus` 缓存上次状态，仅在跳变时记日志；按钮启用逻辑仍然每帧更新。

**教训**：状态日志要记录**事件**（"X 变成了 Y"），不是**采样**（"现在是 Y"）。前者有信息量，后者是噪声。

---

### B15 — 重连 "连接丢失" 日志会刷两条

**现象**：模拟器关掉后，"机器人连接丢失，开始自动重连…" 在 80ms 内出现两次。

**根因**：Qt `QTcpSocket` 在断连时**同时**触发 `disconnected` 和 `errorOccurred` 两个信号，两个 slot 都会发 `connectionLost`。`m_state` 检查阻止了二次 `setState(DISCONNECTED)`，但日志记录在状态检查之外，仍然记录了两次。

**修复**：MainWindow 的 connectionLost handler 加 500ms 时间戳去重。

**教训**：Qt socket 的断连信号是**幂等多次发射**模式，应用层去重是必要的。

---

### B16 — 检测结果间歇性丢失，点"发送抓取位姿"经常没目标

**现象**：明明检测稳定（状态栏显示绿色坐标），点发送按钮却经常弹"无目标"对话框。

**根因**：`m_lastDetections` 每帧覆盖（包括空结果）。匹配是间歇性的（一帧匹配上、下一帧匹配失败），点击时 50% 概率刚好落在空帧上。

**修复**：另存 `m_lastValidMatch` + 时间戳，发送时优先用当前帧，没有则回退到 1500ms 内的最近有效检测，日志明确提示"使用 X ms 前缓存的检测结果"。

**教训**：用户操作的时间精度是**几百毫秒级**，不能要求点击落在某一帧上。状态机里区分**"当前"**和**"最近有效"**很重要。

---

### B17 — 模板匹配不抗尺度变化，瓶子挪一点就丢

**现象**：手持瓶子挪几厘米，匹配 score 从 0.95 暴跌到 0.5 以下，目标丢失。

**根因**：使用 `create_shape_model`，**不带尺度容差**。手持距离稍变（25cm → 35cm）画面缩放比变成 0.71，模型搜索范围内找不到匹配。

**修复**：升级到 `create_scaled_shape_model` + `find_scaled_shape_model`，默认尺度范围 0.7–1.5（覆盖距离变化的 50%–150%）。

**教训**：**算子选型要看应用场景**。固定工作距离的工业场景用 `create_shape_model` 够了；手持/可变距离场景必须用 `create_scaled_shape_model`。这是工程判断，不是 AI 默认能给出来的。

---

### B18 — PyBullet GUI 窗口关闭后崩溃丢数据

**现象**：手动关闭 PyBullet 3D 窗口后，仿真器抛 `pybullet.error: Not connected to physics server`，已经完成的抓取数据**没有写进报告**。

**根因**：`p.stepSimulation()` 在物理服务器断开后会抛异常，原代码没捕获；`_write_report()` 只在 `KeyboardInterrupt` 路径调用，崩溃路径直接跳过。

**修复**：`stepSimulation()` 加 `except p.error`，转为优雅退出；`_write_report()` 挪到 `finally` 块，**任何退出路径都强制写报告**。

**教训**：`finally` 是给"必须做的清理"用的，不是 `except` 的兄弟分支。**任何用户产生的数据**（统计、配置、缓存）都该在 `finally` 里落盘。

---

### B19 — 模拟器 Unicode emoji 导致 handle_client 线程崩溃

**现象**：T3 抓取测试中，点击「发送抓取位姿」几次后连接断开，按钮永久失效。

**根因**：`t3_robot_simulator.py` / `pybullet_robot_simulator.py` 的 print 语句中包含 `θ`（希腊字母 U+03B8）、`✅`/`❌` 等 emoji。Windows 控制台默认编码 GBK 无法编码这些字符 → `UnicodeEncodeError` → `handle_client` 线程崩溃 → `conn.close()` → 主程序检测断线 → 5 次重连失败 → FAULT → 按钮永久禁用。

**修复**：所有模拟器脚本中的 Unicode 特殊字符替换为 ASCII (`θ`→`a`, `✅`→`[OK]`, `❌`→`[FAIL]`, `⚠️`→`[!!]`, `📊`→`[=]`, box-drawing → `+`/`|`/`=`)。共修复 4 个脚本（`robot_simulator.py`, `t3_robot_simulator.py`, `t4_stability_monitor.py`, `t5_pressure_test.py`, `pybullet_robot_simulator.py`）。

**教训**：Windows 控制台与 UTF-8 的兼容性是跨平台 Python 脚本的经典陷阱。**所有 console print 只使用 ASCII 字符**；需要特殊符号时检查 `sys.stdout.encoding`，或设置 `PYTHONIOENCODING=utf-8`。

---

### B20 — QTextEdit 日志无限增长（log_max_lines 从未生效）

**现象**：T4 稳定性测试中，程序运行 10 分钟内存从 74 MB 暴涨至 159 MB（+85 MB），远超 50 MB 验收标准。

**根因**：`settings.json` 定义了 `ui.log_max_lines: 5000`，但 `appendLog()` 中**从未读取或执行此限制**。`m_logView->append(...)` 直接追加到 QTextDocument，文本缓冲区无限增长。每条约 80 字节的日志 × 数千条 = 数百 MB 的 QTextBlock 对象树。

**修复**：
- `MainWindow.h` 新增 `m_logMaxLines` 成员，`loadSettings()` 读取 `ui.log_max_lines`
- `appendLog()` 末尾检查 `blockCount() > m_logMaxLines`，超限时用 `QTextCursor` 从文档头部逐行删除

**教训**：配置文件中的限制参数**必须有对应的代码强制执行**——配置文件不会自动限制任何东西。UI 控件的内部数据结构（QTextDocument, QListView model, QTreeWidget）在长时间运行中都是潜在的内存泄漏源。

---

### B21 — `queryToolPose()` 主线程死锁（每次发送 UI 冻结 1 秒）

**现象**：点击「发送抓取位姿」后 UI 冻结约 1 秒，连续发送 50 次累计冻结 50 秒。

**根因**：`RobotClient::queryToolPose()` 使用 `Qt::QueuedConnection` 将发送请求投递到 worker 线程，然后**主线程**调用 `QWaitCondition::wait(1000ms)` 阻塞等待响应。但响应帧（0x84）需要经过 worker 线程解析 → `frameReceived` 信号 → QueuedConnection 投递 → 主线程事件循环处理 → `onDataReceived` → 唤醒条件变量。**主线程被 wait() 阻塞，事件循环无法运行，响应永远无法到达** → 每次都超时 1000ms。

**修复思路**（本次未改，记录待办）：
- 方案 A：去掉 `queryToolPose()` 的同步等待，改为异步回调（信号驱动）
- 方案 B：使用 `QEventLoop::exec()` 代替 `QWaitCondition::wait()`（QEventLoop 会处理事件）
- 当前绕过：在测试标定模式下 `pixelToWorld` 不需要 `base_H_tool` 参数，空 pose 不影响结果

**教训**：**等待跨线程响应时，等待侧的事件循环必须能运行**。`QWaitCondition` 只是 pthread/lock 原语，不理解 Qt 事件循环。跨线程同步在 Qt 中应优先使用信号槽异步模式，避免阻塞主线程。

| 测试 | 验收标准 | 实测 | 结果 |
|---|---|---|---|
| T2 定位精度 | std < 0.05mm | std_x = 0.0004mm, std_y = 0.0009mm | ✅ |
| T3 抓取成功率 | ≥ 95% (50 次) | 100% (53 次真实, PyBullet 升级版 100% (12 次)) | ✅ |
| T4 8 小时稳定性 | 内存增长 < 50MB | 1h40m: 内存 -7MB（稳定） | ⏳ 进行中 |
| T5 通信压力 | 1000 帧 CRC 全通过 | 1000/1000, 平均延迟 0.13ms | ✅ |
| T1 标定精度 | 重投影 < 0.5px | 未实测（需打印标定板） | ❌ 缺 |

---

## 流程反思

回头看，**14+ 个 bug 中至少 70% 可以通过"初始 prompt 更严格"避免**：

- 给 HALCON API 文档片段当上下文 → B01/B02/B06 不会发生
- 明确指定线程模型 → B05/B10 不会发生
- 要求 RAII + 明确锁层次 → B04 不会发生
- 要求安装 messageHandler 作为基础设施 → B12 不会发生

下一个项目的核心改进：**先写 CONSTRAINTS 文档把已知陷阱列清楚，再让 Claude Code 写代码**。

详细的"AI 编程陷阱清单"维护在 `D:\dev_notes\AI_CODING_TRAPS.md`。

---

*最后更新：2026-05-30*
