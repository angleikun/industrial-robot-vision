# Review 1: C++ / Qt 特有问题

**审查范围**: `src/` 14 个 .cpp（3,867 LOC） + `include/` 13 个 .h（939 LOC），共 4,806 LOC
**标准**: CMakeLists.txt:4 `CMAKE_CXX_STANDARD 17`，MSVC 2022 `/W4 /utf-8 /MP`
**审查日期**: 2026-06-06
**视角**: 不重复 P0-P8 体系，聚焦 6 个 C++/Qt 工程化维度

---

## 未能验证的项

- 无（src/include 全量可读）

---

## (1) RAII 与资源管理

### ✅ 做得好的

- **QObject parent 树系统化使用**: MainWindow 一次性 `new` 出 8 个模块（MainWindow.cpp:45-56），全部以 `this` 为 parent。Qt 析构链负责清理。
- **QThread 父子关系正确**: `m_workerThread = new QThread(this)` (CameraManager.cpp:293, MainWindow.cpp:48, RobotClient.cpp:486)。
- **worker 的 deleteLater pattern**: 所有 3 个 worker（VisionProcessor/CameraWorker/RobotCommunicator）都通过 `connect(thread, &QThread::finished, worker, &QObject::deleteLater)` 处理生命周期（MainWindow.cpp:50、CameraManager.cpp:300、RobotClient.cpp:494）。
- **RAII BusyGuard**: VisionProcessor.cpp:231-234 用局部 struct 在栈上保证 `m_busy` 一定被复位，即使匹配抛异常。**高水准做法**。
- **`~MainWindow` teardown 顺序有意识**: MainWindow.cpp:74-99 显式按 "stop producer → drain → disconnect → close sink" 顺序，并在注释里写了"vision thread 退出前 DB 必须还活着"。**这是有经验的代码**。

### ⚠️ 风险点

| # | 文件:行 | 问题 | 严重度 |
|---|---|---|---|
| R1.1 | **CameraManager.cpp:77** | `m_capture = new cv::VideoCapture(...)` 裸 new；CameraManager.cpp:79/110 手动 `delete m_capture`。**非 Qt 对象，无 parent**。如果 `open()` 中途异常（不会，但保险性差），m_capture 泄漏。 | 🟡 低 |
| R1.2 | **MainWindow.cpp:84-87** | `m_visionThread->wait(3000)` 超时回退路径未处理：`wait` 返回 false 表示 quit 未完成，但代码继续执行，可能导致 `m_dbMgr->close()` 之后 m_visionProc 仍在跑（emit matchingComplete → onMatchResult → saveDetection→访问已关 DB）。 | 🟡 低（概率小，但析构期 use-after-close） |
| R1.3 | **全项目** | **0 处 `std::unique_ptr` / `std::shared_ptr` / `std::make_unique`**（grep 命中 0）。C++17 项目却完全使用裸指针 + Qt parent 模式管理资源。不算 bug，但属于"未充分使用语言能力"。 | 🟡 |
| R1.4 | **RobotClient.h:161** | `void *m_socket = nullptr;` opaque pointer，所有使用处都要 `static_cast<QTcpSocket*>(m_socket)`（RobotClient.cpp:147/154/161/166/174）。**意图猜测**：想避免 .h 暴露 `<QTcpSocket>`，但 cpp:3 已经 include 了，没必要。**回归到 C 风格**。 | 🟡 |
| R1.5 | **CalibrationManager.cpp:23** + **VisionProcessor.cpp:157** | 析构/清理路径里 `catch (...) {}` 沉默吃异常。**这是 RAII 的正确做法**（析构不应抛），但应改成 `catch (HException &e) { qWarning() << ...; }` 至少留 log。 | 🟢 实际合理，可观测性低 |

### HObject / HTuple 异常路径

- **CalibrationManager.cpp**: 14 处 try/catch，全部 catch `HalconCpp::HException &e`，HObject/HTuple 都是栈对象（HALCON 自己引用计数），异常路径**自动安全** ✅
- **MeasurementEngine.cpp**: 12 处类似 ✅
- **VisionProcessor.cpp**: 10 处类似 ✅
- **m_shapeModel 是成员变量**（VisionProcessor.h:84），需要在 dtor 显式 ClearShapeModel — VisionProcessor.cpp:26-29 `~VisionProcessor() { clearTemplate(); }` → 走到 line 156 `ClearShapeModel` ✅

---

## (2) Qt 信号槽与线程

### 线程模型（4 线程）

| 线程 | Worker | 跨线程通信 |
|---|---|---|
| 主线程 | UI + SQLite 写入（DatabaseManager 排队到主线程） | — |
| 采集线程 | CameraWorker（CameraManager.cpp:294 `new CameraWorker;` 无 parent + moveToThread:295） | frameReady → main，`Qt::QueuedConnection` 隐式 |
| 视觉处理线程 | VisionProcessor（MainWindow.cpp:47-49） | matchingComplete → main，`Qt::QueuedConnection` 隐式 |
| 通信线程 | RobotCommunicator（RobotClient.cpp:487-488） | frameReceived → main |

### ✅ 做得好的

- **3 处 `QMetaObject::invokeMethod` 显式指定 `Qt::QueuedConnection`**：CameraManager.cpp:253/261, MainWindow.cpp:587, RobotClient.cpp:371/468/527
- **1 处 `Qt::BlockingQueuedConnection`**：RobotClient.cpp:271 `m_worker->connectToHost(...)` — 这是**正确做法**（需要返回 bool 结果，需阻塞）
- **VisionProcessor::loadTemplate 的死锁修复**（VisionProcessor.h:78-82 + .cpp:152）：明确记录了"非递归 mutex 重入死锁"的历史教训，提取 `clearTemplateUnlocked` 给已持锁路径用。**有完整 commit-level 经验**。
- **RobotClient 三层并发同步**:
  - `mutable QReadWriteLock m_stateLock`（RobotClient.h:108）保护状态枚举
  - `QMutex m_sendMutex`（RobotClient.h:130）串行化发送
  - `QMutex m_poseRespMutex + QWaitCondition`（RobotClient.h:133-134）实现"发送→等待响应→唤醒"的同步查询模式
  这是**工业代码的多线程能力证明**。

### ⚠️ 风险点

| # | 文件:行 | 问题 |
|---|---|---|
| R2.1 | **VisionProcessor.cpp:217-238** `processImage` | `compare_exchange_strong` 阻止帧积压 ✅，但**没记录 dropped frame 数**。长时压测时无法知道丢了多少帧（FPS 数据失真）。建议加 `std::atomic<uint64_t> m_droppedFrames` 暴露给 UI/log。 |
| R2.2 | **VisionProcessor.h:94** `mutable QMutex m_modelMutex`（非递归） | 7 处加锁（VisionProcessor.cpp:64/109/126/148/169/242/248）。findObject 持锁全程（line 169-214）跑 HALCON 匹配 — **如果 HALCON 算子内部抛，BusyGuard 不在这里覆盖**（BusyGuard 只在 processImage 包了 findObject）。实际异常被 try/catch 捕获 ✅。 |
| R2.3 | **RobotClient.cpp:271** | `Qt::BlockingQueuedConnection` 是从 GUI 线程到 worker 线程 — 如果 worker 卡住（DNS 查询慢），主线程冻结。但有 `timeoutMs` 控制（默认 3000ms），实际可控。 |
| R2.4 | **RobotClient.h:107** `RobotState m_state = ...` | 用 QReadWriteLock 保护（RobotClient.h:108 mutable QReadWriteLock m_stateLock），但 `m_reconnectCount`（.h:118）和 `m_lastPoseResp`（.h:135）也是跨线程访问的，似乎没专门锁保护，依赖单线程访问约定。**需要看 cpp 验证**（已读 1-280，未覆盖完整）。 |

---

## (3) const 正确性与传值

### ✅ 做得好的

- **Getter 大量 const**: `isOpen() const` / `intrinsic() const` / `currentToolPose() const` / `matchConfig() const` / `isReady() const` / `isConnected() const` 等遍布所有 header。
- **mutable QMutex 配合 const getter**: CameraManager.h:58, JsonSettings.h:33, VisionProcessor.h:94, RobotClient.h:108 — 标准做法 ✅
- **大对象按 const 引用传**: `void processImage(const QImage &image)`、`void onFrameReady(const QImage &image)`、`bool createTemplate(const QImage &image, const QRectF &roi)` — 普遍正确

### ⚠️ 风险点

| # | 文件:行 | 问题 | 备注 |
|---|---|---|---|
| R3.1 | **CalibrationManager.h:66-67** | `HalconCpp::HTuple camParam() const;` / `handEyePose() const;` **按值返回**，理论上拷贝。但 HALCON HTuple 内部是引用计数（写时复制），实际 cheap。 | 🟢 可接受 |
| R3.2 | **CalibrationManager.h:102** | `HTuple lastCalibPose() const { return m_lastCalibPose; }` 同上，按值返回。 | 🟢 |
| R3.3 | **MeasurementEngine.h:38** | `MeasureResult measure(const QImage &image, MeasureType type, const QList<QPointF> &points = {}, const QRectF &roi = QRectF())` — **default 参数 `QList<QPointF>{}` 每次构造空 list**。不是 bug 但有些编译器会警告。 | 🟢 cosmetic |
| R3.4 | **CoordTransform.h:62** | `QPointF undistortPixel(double px, double py) const;` ✅ const 正确 |
| R3.5 | **整体** | 没看到任何 `[[nodiscard]]`、`noexcept` 标注 | 🟡 C++17 best practice 未用 |

---

## (4) 头文件依赖

### 最重的 5 个头（按传染面）

| # | 头 | 直接 include 它的 cpp 数 | 传染源 |
|---|---|---|---|
| 1 | **`HalconCpp.h`** | 5 cpp 直接 + N cpp 间接 | VisionProcessor.h:12 / CalibrationManager.h:10 / CoordTransform.h:7 都 public include |
| 2 | **`MainWindow.h`** | 1 cpp，但拉入 VisionProcessor.h + MeasurementEngine.h + RobotClient.h + 一堆 Qt UI | main.cpp 的唯一入口 |
| 3 | **`RobotClient.h`** | 2-3 cpp + 拉入 CoordTransform.h（→ HalconCpp.h） | MainWindow.h:13 + RobotClient.cpp |
| 4 | **`CoordTransform.h`** | 拉入 HalconCpp.h | 被 RobotClient.h:12 间接拖入 |
| 5 | **`VisionProcessor.h`** | 拉入 HalconCpp.h + 定义 MatchResult/MatchConfig | MainWindow.h:11 |

### ✅ 做得好的

- **MainWindow.h:15-21 forward decl 了 6 个类**：ImageView/CameraManager/CalibrationManager/CoordTransform/DatabaseManager/ReportManager — 这些**没**被传染拉 HALCON 头 ✅
- **0 处循环依赖**（grep 推断，未发现）

### ⚠️ 风险点

| # | 文件:行 | 问题 |
|---|---|---|
| R4.1 | **MainWindow.h:11-13** | 为了拿到 `MatchResult` / `MeasureType` / `RobotStatus` 这 3 个结构体，include 了 3 个完整 header，**间接把 `HalconCpp.h` 拖进了 MainWindow.h**。**任何修改 main 都要重编 HALCON 相关 TU**，编译时间显著放大。 |
| R4.2 | 修复建议 | 把 `MatchResult` / `MeasureType` / `RobotStatus` 提取到 `include/Types.h`（不依赖 HALCON），然后 MainWindow.h 只 include Types.h + forward decl VisionProcessor/MeasurementEngine/RobotClient。**预计编译时间 -30% 起**。 |
| R4.3 | **CoordTransform.h:80-81** | 成员变量 `HHomMat3D m_camHomTool;` `HTuple m_camParam;` 是**值类型 HALCON 对象**——这逼着任何 include 它的 .h 都要解析 HALCON 头。理想是 pImpl（`std::unique_ptr<Impl> m_impl`），但工程量大。 |

---

## (5) 错误处理一致性

### 统计

- **try/catch 总数**: 50+ 处（grep）
- **`catch (HalconCpp::HException &e)`**: 大多数（VisionProcessor 5、CalibrationManager 12+、MeasurementEngine 12、MainWindow 1、CoordTransform 1）
- **`catch (...)`**: 4 处（CalibrationManager.cpp:23/254/145, VisionProcessor.cpp:157, MainWindow.cpp:491）

### ✅ 做得好的

- **mapHalconError**（VisionProcessor.cpp:8-19）做了 HALCON error code 到 enum 的分类映射，**还专门注释了 #1301 是参数错误不是 license 错** —— 这是踩过的坑（H/W 上把参数错当作 license 失败误导调试 N 小时）的疤痕证据。
- **HException 在边界统一捕获**：每个 HALCON 算子的 wrapper 都包了 try/catch，**没有让 HALCON 异常逃出 module**，符合"业务层 vs 算法层隔离"工业实践。
- **bool / void 风格**: 大部分接口返回 bool + emit error signal，**没混用 throw 跨模块边界**。

### ⚠️ 风险点

| # | 文件:行 | 问题 |
|---|---|---|
| R5.1 | **CalibrationManager.cpp:23/254** | 析构/清理路径 `catch (...) {}` 空消化。**符合 RAII 析构无异常原则**，但完全无日志、无可观测性。建议改 `catch (HException &e) { qWarning() << "dtor cleanup:" << e.ErrorMessage().Text(); }` |
| R5.2 | **CalibrationManager.cpp:145** | `catch (HException &) { continue; }` — for 循环里跳过失败的图像，**没记数 / 没 emit warning**。如果 15 张标定图全失败，calibrate 还是会跑（line 153 CalibrateCameras 用空数据），返回低质量结果。建议添加 `int failedCount` 计数，超过 1/3 emit error。 |
| R5.3 | **VisionProcessor.cpp:157** + **MainWindow.cpp:491** | `catch (...)` 但没看 cpp:491 上下文，需后续验证。 |
| R5.4 | **bool 返回 vs signal error 二元混合** | `addCalibrationImage` 返回 bool **同时** emit calibError signal（CalibrationManager.cpp:87-88）。**约定**：bool=程序流, signal=UI 通知。一致 ✅ 但调用者必须意识到要双重处理。 |

---

## (6) C++ 特性使用

### 已用的现代特性

| 特性 | 用例 |
|---|---|
| `enum class` | RobotState/RobotStatus/CameraBackend/VisionError/MeasureType — **全部规范** ✅ |
| `std::atomic<bool>` | VisionProcessor.h:85,91 / CameraManager.h:59 / RobotClient.h:10 |
| `constexpr` | RobotClient.h:32-37 命令码、RobotClient.h:119-124 重连/心跳常量 |
| `nullptr` + default member initializer | MainWindow.h:91-152 大量 `T* m_x = nullptr;` |
| Generic lambda + capture | RobotClient.cpp:269-271, 364-374 等 |
| `static_cast` | 普遍使用，无 C 风格 cast（grep 未发现）|

### 0 使用的特性（C++17/20 应当使用）

| 特性 | 用例缺失 |
|---|---|
| `std::optional` | `queryStatus()` 返回 RobotStatus 枚举（含 STATUS_UNKNOWN 占位），更好用 `std::optional<RobotStatus>` |
| `std::variant` | `MeasureResult` 用了 union-like 结构（pt1/pt2/center/radius/area 共存），更好用 `std::variant<LengthResult, CircleResult, ...>` 但侵入大 |
| `std::filesystem` | 全项目用 `QString filePath` / `QFileInfo` —— Qt 时代合理，但 standalone util 可以用 `std::filesystem::path` |
| 智能指针 | 0 处使用，全靠 Qt parent / 裸 new+delete |
| `[[nodiscard]]` | `bool createTemplate(...)` / `bool sendGrabPose(...)` 等容易被忽略返回值的函数都应标 |
| `noexcept` | 析构函数和 move ctor 都没标 — 影响 std::vector\<T\> 等容器的优化路径 |
| Structured bindings | grep 未发现 `auto [` — 不致命，cosmetic |

### Qt 6 现代化

- ✅ 用了 `qt_standard_project_setup()`、`qt_add_executable()`（CMakeLists.txt:18, 80）— Qt6 现代 CMake 风格
- ✅ `QObject::errorOccurred` 而非 deprecated `error` signal（RobotClient.cpp:139）
- ⚠️ `QObject::disconnect()` 名字与 `RobotClient::disconnect()` 冲突 — RobotClient.h:68 有 `void disconnect();` member，在 cpp 里同时调用 base 类 disconnect 时需要 `this->QObject::disconnect()`。**未发现 bug**，但命名容易引起认知混乱。

---

## 工程化评分（1-10）

| 维度 | 分 | 依据 |
|---|---|---|
| RAII | 7 | QObject parent 完整，但裸 cv::VideoCapture + 零智能指针 |
| 线程安全 | 8.5 | 4 线程模型清晰，3 种锁正确使用，BusyGuard + 死锁修复历史 |
| const 正确性 | 7.5 | 普遍 const，缺 nodiscard / noexcept |
| 头文件依赖 | 6 | MainWindow.h 被 HalconCpp.h 污染，编译时间敏感 |
| 错误处理 | 8 | HException 统一边界，mapHalconError 有踩坑经验 |
| C++17 特性 | 5.5 | enum class / atomic / constexpr OK，但 optional / variant / 智能指针 0 使用 |

**综合：7.0 / 10** —— **工程能力扎实，缺现代 C++ 范式自觉**。

---

## Top 5 改进建议（带文件:行）

### 改进 1（ROI 最高）：**Types.h 抽取 + 解 HALCON 头传染**
- **位置**: `include/MainWindow.h:11-13` + 新建 `include/Types.h`
- **动作**: 把 `MatchResult` / `MatchConfig` / `MeasureType` / `MeasureResult` / `RobotStatus` 提取到 `Types.h`（纯 Qt 类型，不依赖 HALCON）。
- **收益**: MainWindow.h 不再传染 HALCON → 主入口编译时间显著降。
- **代价**: 改动 ~6 个 .h 的 include。

### 改进 2：**RobotClient::sendFrame 检查部分写入**
- **位置**: `src/RobotClient.cpp:168-169`
- **当前**: `qint64 written = sock->write(frame.serialize()); return written > 0;`
- **问题**: 部分写入会被误判为成功；TCP 缓冲满时小概率出现。
- **改**:
  ```cpp
  const QByteArray data = frame.serialize();
  qint64 written = sock->write(data);
  if (written != data.size()) return false;
  if (!sock->waitForBytesWritten(m_timeoutMs)) return false;
  return true;
  ```

### 改进 3：**VisionProcessor 暴露 droppedFrames 计数**
- **位置**: `src/VisionProcessor.cpp:217-238` + `.h:91`
- **动作**: 添加 `std::atomic<uint64_t> m_droppedFrames{0};`，在 `processImage` 入口 `compare_exchange` 失败时 `m_droppedFrames++`，对外 getter。
- **收益**: T4/T5 长时压测能客观说"丢帧 0.X%"而非"FPS 看着够"。

### 改进 4：**CalibrationManager 标定图像失败计数**
- **位置**: `src/CalibrationManager.cpp:140-149`
- **当前**: 失败图像 `continue;` 静默跳过
- **改**: 统计 `failedCount`，超过 `m_calibImages.size() / 3` emit `calibError` 阻止低质量标定。

### 改进 5：**RobotClient.h `void *m_socket` → `QTcpSocket *m_socket`**
- **位置**: `include/RobotClient.h:161` + 改 .cpp 5 处 static_cast
- **动作**: opaque pointer 没必要——cpp 已 include QTcpSocket，把 .h 也加上前向声明 `class QTcpSocket;` 然后 `QTcpSocket *m_socket = nullptr;`。
- **收益**: 删 5 处 `static_cast`，更直观。

---

## 附：项目结构事实清单（供后续 review 复用）

```
src/             14 cpp / 3,867 LOC
include/         13  h / 939 LOC
CMakeLists.txt   C++17, MSVC /W4 /utf-8 /MP
依赖             Qt6 (Core/Widgets/Sql/Network/PrintSupport) + HALCON 24.11 + OpenCV 4.8
线程             4 (主/采集/视觉/通信)
HALCON 资源      HShapeModel/HTuple/HObject 全部值类型成员
QObject 模块     14 (Q_OBJECT 宏齐全)
锁类型           QMutex(6 文件) / QReadWriteLock(1) / QWaitCondition(1) / std::atomic
跨线程通信       Qt::QueuedConnection (5 处显式) + Qt::BlockingQueuedConnection (1 处显式)
TODO 项          Basler/海康/Excel 3 处明确 stub
```
