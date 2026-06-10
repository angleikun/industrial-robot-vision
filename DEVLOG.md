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

### B22 — measureAngle 实现无视入参，从单 ROI 抽线导致 HALCON #1405

**现象**：UI 测量角度时按计划点 4 个点（两条线的端点），按"执行测量"——HALCON 抛 `#1405: Invalid line` 异常，弹窗正文为空（Hotfix 1 前），日志"角度测量失败"。

**根因**：`MeasurementEngine::measureAngle` 的接口签名要求 `line1Start/End + line2Start/End` 四个点（`onPointPicked` 已经收齐 4 个点入参），但旧实现忽略入参，直接在传入的 ROI 内调 `EdgesSubPix` + `FitLineContourXld`——单 ROI 内只能抽出一条线，无法拟合"两条线之间的夹角"。结果传给 `AngleLl` 的 line2 是空 tuple，HALCON 抛 #1405。

**修复**：`src/MeasurementEngine.cpp::measureAngle` 改为：
- 不再吃 ROI，按入参 `line1Start/End` + `line2Start/End` 构造两条 XLD 直线
- 各自 `EdgesSubPix` 后用 `LengthXld` + `TupleSortIndex` 选最长 contour（防多边缘碎片污染拟合）
- 两条 contour 分别 `FitLineContourXld` → 拼 `AngleLl` → 弧度转度

**教训**（人类 review AI 代码时的具体动作）：

1. AI 留 stub 的最常见痕迹是 `/* paramName */` 这种注释掉的参数名——review 时第一个动作必须是 `grep "/\*[a-zA-Z_]+\*/"` 找候选
2. `measure_xxx` 这类多参数函数，函数签名里每个参数都应该在函数体里至少出现一次（哪怕只是 `if (param.size() < 2)` check）。AI 容易"参数声明但不用"，要**逐参数核对**
3. 单元测试用真实输入参数而非空 mock，能立刻暴露 stub——未来项目应该补单元测试覆盖率门槛

---

### B23 — measureArea 不读 ROI 像素 + 硬编码阈值

**现象**：选"面积"模式拖 ROI 测量，任何 ROI 上结果都接近 0 或一个固定数，与 ROI 内目标的实际亮度面积无关。

**根因**：`MeasurementEngine::measureArea` 完全没把 ROI 当感兴趣域用——直接 `Threshold(fullImg, 128, 255)` 对全图二值化，阈值还硬编码 128，根本不参考 ROI 内的实际灰度分布。

**修复**：`src/MeasurementEngine.cpp::measureArea`：
- 用入参 `roi` 调 `ReduceDomain` 把算子作用域限定到 ROI
- `BinaryThreshold` 用 `'max_separability'`（Otsu 自动阈值）替代硬编码 128
- `AreaCenter` 求 region 面积

**教训**（人类 review AI 代码时的具体动作）：

1. AI 容易把"硬编码魔数 + 整图处理"作为兜底实现 ship——review 必须逐个检查调用图像处理算子的函数：**是否真用了用户传入的 ROI / 是否硬编码了阈值**
2. 自适应阈值（如 Otsu `'max_separability'`）应该是工业 vision 函数的默认选择，硬编码阈值通常是开发期 placeholder
3. 实际使用的中间产物（如 Otsu 选出的具体阈值）应该暴露给用户（写进 `label`），便于调试 + 增加结果可信度
4. AI 实施完一个算法函数后，必须用与训练数据**完全不同的真实输入**测一次——stub 实现往往在"理论上合理"的输入下能跑通

---

### B24 — UI 入口 / dispatcher 对 ANGLE 和 AREA 缺少形状防御

**现象**：用户没拖 ROI 选"面积"点"执行测量"、或没点齐 4 个点切走"角度"模式——路径直接抛 HALCON 异常或弹空白对话框。

**根因**：`MainWindow::onMeasure` dispatcher 对 LENGTH/CIRCLE/DISTANCE 都有"点不够"的早返回，但 ANGLE 路径只校验"≥2 点"，没要求 4 点；AREA 路径根本没校验当前 ROI 是否有效就丢进 `measureArea`。到 `MeasurementEngine` 拿到非法入参才崩。

**修复**：`src/MainWindow.cpp::onMeasure` + `onPointPicked`：
- ANGLE 路径要求 `m_pointsNeeded = 4`；不足时不进入 `measureAngle`
- AREA 路径在 dispatcher 入口校验当前 ROI（非空且非零面积），无效时 `appendLog` + 早返回
- `onRoiSelectedForMeasure` 加 ROI 太小（<5×5）防御

**教训**：**前置校验属于"算法的一部分"，不是 UI 装饰**。AI 倾向把校验当 UI 礼貌（"给个友好提示就行"），实际上"参数形状对了才进算法"是算法稳定性的保障。dispatcher 是天然的入口校验位置——任何要求"特定形状输入"的算法都该在 dispatcher 把入参形状检查清楚。

---

### B25 — Fix 1：测量绘制层 overlay 不渲染 + clearOverlays 一刀切

**现象**：检测和测量结果落进 `m_lineMeasurements` / `m_circleMeasurements` / `m_angleMeasurements` 这些容器，但 ImageView 上**什么也看不见**；切换检测→测量时旧检测 overlay 也擦不掉，靠"全清"一刀切就连标定 corner 也带走。

**根因**：两件事：
1. `ImageView::drawOverlays` 完全没遍历 `m_lineMeasurements / m_circleMeasurements / m_angleMeasurements`——add API 在写、绘制路径里没读
2. 只有一个 `clearOverlays()` 把所有容器（detection / measurement / line / circle / angle / calib）一起清空，无法按类型清理

**修复**：
- `src/ImageView.cpp::drawOverlays` 加 3 个新 section（line / circle / angle）按各自几何画线 + 端点 cross + label
- `include/ImageView.h` + `src/ImageView.cpp` 拆 `clearDetectionOverlays` / `clearMeasurementOverlays` / `clearCalibrationOverlays` / `clearAllOverlays` 四个 API；旧 `clearOverlays` 留为 `clearAllOverlays` 的兼容别名
- `MainWindow::onMatchResult` 改用 `clearDetectionOverlays`；`applyMeasurementOverlay` 入口调 `clearMeasurementOverlays` 实现"只显示最新一次"

**教训**：**add API 和 draw 路径是一对**——加了存储就该有渲染，AI 容易"写了 setter 就以为通了"。Review 时凡是新增 `add*Overlay` API 必须同步检查 `paintEvent` / `draw*` 是否有对应分支。clear 函数的颗粒度直接决定 UX 自由度，"一刀切"是必然演化成 bug 的设计。

---

### B26 — Hotfix 1：HALCON 异常路径未填 `r.label`，UI 弹空白 QMessageBox

**现象**：测量黑色 ROI（无有效边缘）时，`QMessageBox` 弹出标题"测量失败"但**正文完全空白**，用户不知道为什么失败。

**根因**：`MeasurementEngine::measureLength / measureCircle / measureDistance` 的 `catch (HException &e)` 块只 `qWarning() << e.ErrorMessage().Text()`，**没设 `r.valid = false` 也没设 `r.label`**。`MeasureResult` 的 `valid` 默认 `false`、`label` 默认空——`valid==false` 走失败分支，但 `label==""` 导致 `QMessageBox::critical(this, "测量失败", r.label)` 正文为空。

**修复**：3 个 catch 块各加 2 行：
```cpp
r.valid = false;
r.label = QString("XX测量失败: ") + e.ErrorMessage().Text();
```

**教训**：**异常路径的状态一致性必须显式构造，不能依赖默认值**。AI 写 catch 块倾向"记个 log 就 return"——但 result struct 的"失败态"该是什么样**必须明确写**。这次通过追溯 `QMessageBox` 入口看 `r.label` 的赋值路径，发现 3 处同款漏赋——**同一类 catch 块漏同一类字段是结构性疏忽，不是孤立失误**，应当一次扫齐。

---

### B27 — Fix 3：DISTANCE / ANGLE 点击拾取阶段无视觉反馈

**现象**：DISTANCE 模式点了第一个点，画面**什么都没变**；ANGLE 模式连点 4 个点之间用户不知道点了几个、点在哪里——直到收齐输入算完才一次性出 overlay。点击中途是盲操作。

**根因**：`onPointPicked` 把点存进 `m_collectedPoints` 就 return，没有让 ImageView 在画面上回显"用户刚才点的位置"。绘制层只有"测量完成的 overlay"通道，没有"输入收集中的瞬时 marker"通道。

**修复**：
- `include/ImageView.h` 加 `addClickedPointMarker(pos, color, label)` + `clearClickedPointMarkers()` + 私有 `ClickedPointMarker` 容器
- `src/ImageView.cpp::drawOverlays` 加 section 3e 画 cross + label（瞬时层）
- `clearAllOverlays()` 联动清 `m_clickedPoints`
- `src/MainWindow.cpp::onPointPicked` 用 switch 按 `m_pendingMeasureType` + 点序号派生 marker 颜色（DISTANCE 全青色、ANGLE 1-2 青 / 3-4 品红）和 label（"距离点1/2"、"线1点1/2"、"线2点1/2"），收齐输入或失败时清 marker

**教训**：**本次最重要的 AI 协作教训不在代码，而在 scope**——原计划 Fix 3 是 5 行 statusBar 同步，AI 看了完整的 `09_ux_fix_plan.md` 之后"重新解读"为 70 行 marker 可视化。结果用户接受了，但明确立规：**任何对 Fix 范围的重新解读必须在动代码前先问**。AI 不能因"方向更好"就静默扩大 scope；用户的"上下文 + 时间预算"判断优先。后续所有 Fix 的 scope 讨论文档（如 `12_fix2_scope.md`）都源于这一教训。

---

### B28 — Fix 2：执行测量按钮无 checked 视觉态 + combo 无反馈，状态机泄漏

**现象**：用户选"角度"按"执行测量"——按钮按下立刻弹回，看起来"什么也没发生"；改 combo 选个测量类型，UI 也没任何反馈，用户不知道选了没。即使测量进行中，也没有"现在处于测量态"的全局指示。

**根因**：
- `m_btnMeasure` 不是 `setCheckable(true)`，clicked 后立刻 release，无法表达"测量进行中"
- combo `currentIndexChanged` 没连任何 slot
- 没有 status bar 通道反映测量状态机的当前位置

**修复**：`include/MainWindow.h` + `src/MainWindow.cpp`：
- `m_btnMeasure` 提升为成员 + `setCheckable(true)`，clicked(bool) lambda 分发 true→onMeasure / false→cancelMeasurement
- `setupStatusBar` 加 `m_statusMeasure` label（"测量: 空闲" / "测量: 已选 X..." / "测量中: X..."）
- combo `currentIndexChanged` 连 lambda 更新 statusMeasure 文字（按钮 checked 时抑制以防"测量中切类型"造成显示错乱）
- 新增 `cancelMeasurement()` helper（清 marker + 清 collectedPoints + 切回 ROI 模式 + log + reset）
- 新增 `resetMeasurementState()` helper（`setChecked(false)` + 状态文字"空闲"）
- **关键**：reset 调用点覆盖**全部 10 条测量结束路径**——success / failure / 早返回 / cancel 全闭环，不只是"成功分支"

**教训**：**状态机正确性 = 覆盖所有结束路径**。如果只有"成功分支"调 reset，UI 一次失败就泄漏"按钮 checked + 状态文字'测量中'"——下次用户点按钮以为是 cancel，实际是 onMeasure 再次启动。AI 写状态机倾向关注 happy path，failure path 经常被遗漏；本次把"各结束路径（成功 / 失败 / 早返回 / cancel）"列入 spec 章节标题，实现时按章节精神扩展覆盖到全部 10 条路径，并在交付时**透明汇报"扩展 6 条字面 spec 没列的路径"**——AI 的扩展解读可以做，但必须报。

**补充教训**（关于 AI 协作纪律）：

- 本次 reset 4 处调用点是 AI（Claude Code）在实施 Fix 2 时主动扩展原 spec、额外添加的覆盖路径。原 spec 只列出 3 处主要 reset 点，AI 补充的 4 处全部来自"对状态机正确性的完整化"动机
- 这是 AI 协作的**正面 case**：AI 在实施时基于代码语境主动识别出 spec 漏洞并补全
- 但 AI 没有提前告知（违反了"扩展 spec 前 ask"的协作纪律），事后才透明汇报。这次接受是因为方向正确，但**下次类似情况应该 review 前先 ask**
- **硬约束**：AI 协作的纪律性约定不能因为"扩展方向正确"而放松

---

### B29 — 未初始化 enum/struct 成员（Cppcheck 静态扫描发现）

**现象**：
v1.2 修复主线完成后做静态扫描（Cppcheck 2.21.0 + grep 模式扫描）。Cppcheck 报告 `MeasurementEngine.cpp:75` 在 DISTANCE 早返回路径构造的 `MeasureResult` 含未初始化 enum 字段 `type`。同类问题在 `ImageView::DetectionItem` 重复 3 次（`angle` / `score` / `valid`）。

**根因**：
`struct MeasureResult { MeasureType type; ... }` 缺默认初始化器。人工 review 12 次未发现 —— 因为成功路径和异常 catch 路径都显式设了 `r.type`，只有"早返回"路径漏了。运行时表现为偶然正常（栈上垃圾值碰巧落在合法 enum 范围内），但理论上是未定义行为。

**修复**：
所有相关 struct 成员加默认初始化器（C++11 in-class member initializer）：
- `MeasureType type = MeasureType::LENGTH;`
- `double angle = 0.0;`
- `double score = 0.0;`
- `bool   valid = false;`

并顺手处理了 `CalibrationManager` 两处空 catch（B26 同类）：析构函数边界保留 `catch(...)` 但补 qWarning + 注释说明"故意吞"；`clearCalibImages()` 改为完整 chained catch（HException / std::exception / ...）。

**教训**：
- 人工 review 找不出"碰巧能跑"的未定义行为。Cppcheck 等静态分析工具的不可替代价值在于**数据流分析**，能识别"未初始化但被消费"这种模式
- C++ struct 应该**所有成员都给默认初始化器**，不要假设默认构造会处理 —— 对 POD 类型（enum / 原生数值 / bool）默认构造不会零初始化
- AI 协作中的 review 报告再多也无法覆盖工具能覆盖的盲点。正确做法：**人工 review + grep 模式 + 静态分析工具 三层叠加**，各自的强项不重合：
  1. 人工 review 找语义层 bug（逻辑、流程、UX）
  2. grep 模式找模式化反模式（空 catch、裸指针、缺 emit）
  3. 静态分析找数据流盲点（未初始化、UB、shadow）
- 人类 review 此类提交时的具体动作：每个 `struct` 定义都 grep 一遍 `[a-zA-Z_]+;$` 找无初始化的成员声明；任何 enum 字段必须有 `= EnumName::SomeValue` 默认值，因为 enum 不像 int 默认 0

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
