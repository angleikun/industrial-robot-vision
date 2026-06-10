# 15 — grep 模式扫描（路径 B）

> 工具：Grep（ripgrep）  
> 扫描范围：`src/` + `include/`  
> 原则：只列出位置，不做"是不是 bug"的判断（用户自决）  
> 排除：上一轮 review_v2 已修复的 B22–B28 不重复列

cppcheck 路径（路径 A）：**未安装**。winget/手动安装会触发 UAC 弹窗，未自动执行——本文档只覆盖路径 B 的 10 个 grep 模式。

---

## 1. TODO / FIXME / XXX / HACK

**匹配数：4**（均在 `src/`，`include/` 0 匹配）

| 文件:行 | 内容 |
|---|---|
| `src/CameraManager.cpp:90` | `// Basler: TODO: 需要 Pylon SDK 链接后实现` |
| `src/CameraManager.cpp:96` | `// Hikvision: TODO: 需要 MVS SDK 链接后实现` |
| `src/CameraManager.cpp:208` | `// Basler via Pylon (TODO)` |
| `src/CameraManager.cpp:209` | `// Hikvision via MVS (TODO)` |

---

## 2. `/* paramName */`（AI stub 注释参数模式 / B22 同款）

**匹配数：0**（src/include 均无）

→ 本仓库不存在 B22 同款"注释掉的参数名"占位。

---

## 3. `return false; / return -1; / return nullptr;`

**匹配数：43**（`src/` 共 7 文件）

按文件汇总（含前一行上下文，判断是否有错误信息）：

| 文件:行 | 前置是否有 log / emit |
|---|---|
| `src/CameraManager.cpp:82` | ✅ `emit error("无法打开 USB 相机: " + id);` |
| `src/CameraManager.cpp:93` | ✅ `emit error("Basler 相机后端暂未实现…");` |
| `src/CameraManager.cpp:99` | ✅ `emit error("海康相机后端暂未实现…");` |
| `src/CameraManager.cpp:102` | ❌ 无（函数末尾兜底 fallback） |
| `src/CalibrationManager.cpp:88` | ✅ `emit calibError(...)` |
| `src/CalibrationManager.cpp:98` | ✅ `emit calibError(msg)` |
| `src/CalibrationManager.cpp:220` | ❌ `if (!m_intrinsicDone) return false;`（前置 if-单行） |
| `src/CalibrationManager.cpp:226` | ✅ `emit calibError("WriteCamPar 失败…")` |
| `src/CalibrationManager.cpp:238` | ✅ `emit calibError("ReadCamPar 失败…")` |
| `src/CalibrationManager.cpp:264` | ✅ `emit calibError("请先完成相机内参标定")` |
| `src/CalibrationManager.cpp:285` | ✅ `emit calibError("标定板标记点不足…")` |
| `src/CalibrationManager.cpp:299` | ✅ `emit calibError(msg)` |
| `src/CalibrationManager.cpp:380` | ❌ `if (!m_handEyeDone) return false;`（前置 if-单行） |
| `src/CalibrationManager.cpp:386` | ✅ `emit calibError("WritePose 失败…")` |
| `src/CalibrationManager.cpp:398` | ✅ `emit calibError("ReadPose 失败…")` |
| `src/DatabaseManager.cpp:46` | ✅ `emit databaseError("无法打开数据库…")` |
| `src/DatabaseManager.cpp:59` | ✅ `emit databaseError("无法创建数据库表")` |
| `src/DatabaseManager.cpp:110` | ✅ `qWarning() << "create detections table failed…"` |
| `src/DatabaseManager.cpp:172` | ⚠️ 注释 `// async: ID not yet available`（语义：还没好，不是失败） |
| `src/DatabaseManager.cpp:198` | ✅ `emit databaseError("保存检测记录失败…")` |
| `src/DatabaseManager.cpp:384` | ✅ `emit databaseError("保存抓取记录失败…")` |
| `src/DatabaseManager.cpp:444` | ✅ `emit databaseError("保存标定记录失败…")` |
| `src/DatabaseManager.cpp:486` | ❌ **无任何 log / emit**——`if (!q.exec()) return false;` 静默吞 SQL 失败 |
| `src/DatabaseManager.cpp:496` | ❌ `if (!m_open) return false;`（前置 if-单行） |
| `src/JsonSettings.cpp:20` | ✅ `qWarning() << "JsonSettings: cannot open…"` |
| `src/JsonSettings.cpp:26` | ✅ `qWarning() << "JsonSettings: parse error…"` |
| `src/JsonSettings.cpp:83` | ✅ `qWarning() << "JsonSettings: cannot write…"` |
| `src/ReportManager.cpp:49` | ✅ `emit exportError(path, "无法创建文件")` |
| `src/ReportManager.cpp:86` | ✅ `emit exportError(path, "Excel 导出需 libxlsxwriter…")` |
| `src/ReportManager.cpp:106` | ✅ `emit exportError(path, "无法创建 PDF 文件")` |
| `src/RobotClient.cpp:92,95,102,105,119,123` | ❌ 协议帧解析失败 return false 全部无 log（合理：解析失败不该刷屏，但完全无可观测性 6 处） |
| `src/RobotClient.cpp:167` | ❌ `if (!isConnected()) return false;`（前置 if-单行） |
| `src/RobotClient.cpp:257` | ✅ `setState(RobotState::FAULT);` |
| `src/RobotClient.cpp:303` | ❌ `if (m_state != RobotState::CONNECTED) return false;`（前置 if-单行） |
| `src/VisionProcessor.cpp:103` | ❌ `m_modelValid = false; return false;` 前置无 emit |
| `src/VisionProcessor.cpp:120` | ✅ `emit processingError(...)` |
| `src/VisionProcessor.cpp:128` | ❌ `if (!m_modelValid) return false;`（前置 if-单行） |
| `src/VisionProcessor.cpp:137` | ✅ `emit processingError(...)` |

---

## 4. 空 catch 块 `catch (...) { }`

**匹配数：2**（均在 `src/CalibrationManager.cpp`）

| 文件:行 | 上下文 |
|---|---|
| `src/CalibrationManager.cpp:23` | 析构函数 cleanup（`ClearCalibData` + `m_distortionMap.Clear`） |
| `src/CalibrationManager.cpp:254` | `clearCalibImages()` cleanup（`ClearCalibData`） |

两处都是清理路径的 `catch (...) {}`——**异常被完全吞没，没有 log**。

---

## 5. qWarning / qDebug 日志点

**匹配数：17**（`src/`，`include/` 0）

| 文件:行 | 内容（关键字） |
|---|---|
| `src/CalibrationManager.cpp:213` | `qWarning() << "undistort failed…"` |
| `src/CoordTransform.cpp:166` | `qWarning() << msg;` |
| `src/DatabaseManager.cpp:109` | `qWarning() << "create detections table failed…"` |
| `src/JsonSettings.cpp:19,25,82` | 3× `qWarning() << "JsonSettings: …"` |
| `src/MeasurementEngine.cpp:151,204,235,340,418` | 5× `qWarning() << "measureXXX failed…"` |
| `src/VisionProcessor.cpp:100,118,135,210` | 4× `qWarning() << "…failed…"` |
| `src/main.cpp:8,9` | 注释引用，非调用 |

观察：CreateScaleShapeModel / ReadShapeModel / FindScaledShapeModel 等"模型生命周期级"错误用 `qWarning`，从严重度看可能更适合 `qCritical`。但属于偏好问题。

---

## 6. `// fix` / `// hack` / `// workaround` / `// 暂时`

**匹配数：0**（大小写均搜过；src/include 0 匹配）

→ 仓库无显式临时修复标记。

---

## 7. 不安全 C 函数 `memcpy / strcpy / sprintf / strcat`

**匹配数：5**（均 `memcpy`，无 `strcpy / sprintf / strcat`）

| 文件:行 | 上下文 |
|---|---|
| `src/CalibrationManager.cpp:43` | B03 已知安全模式（QImage→HImage stride 紧密化） |
| `src/MeasurementEngine.cpp:28` | 同上 |
| `src/VisionProcessor.cpp:50` | 同上 |
| `src/RobotClient.cpp:312` | `memcpy(&raw, &v, 4);`（float → uint32 字节级，4 字节固定 size） |
| `src/RobotClient.cpp:427` | `memcpy(&f, &raw, 4);`（同上反向） |

---

## 8. `new X` 调用

**匹配数：~70**（`src/` 70 行，`include/` 0）

按归属分类：
- **Qt parent 持有（`new X(this)` 或 `new X(parentLayout)`）**：~67 处。Qt 对象树自动管理生命周期。
- **手动管理**：
  - `src/CameraManager.cpp:79,110` — `m_capture = new cv::VideoCapture(...);` + 对应 `delete m_capture;`
  - `src/MainWindow.cpp:49` — `m_visionProc = new VisionProcessor;`（注释说 "no parent: moved to worker thread"，依赖 thread 关闭/MainWindow 析构链回收）
  - `src/CameraManager.cpp:294` — `m_worker = new CameraWorker;`（moveToThread 模式）
  - `src/RobotClient.cpp:487` — `m_worker = new RobotCommunicator;`（同上）

`std::unique_ptr / std::shared_ptr / QScopedPointer` 在仓库内 **0 匹配**。

---

## 9. 类型转换 `static_cast / reinterpret_cast / const_cast / dynamic_cast`

**匹配数：18**（`src/`）

| 类型 | 数量 | 代表位置 |
|---|---|---|
| `static_cast<size_t>` | 9 | 图像 stride 计算（CalibrationManager / MeasurementEngine / VisionProcessor 各 3） |
| `static_cast<QTcpSocket*>` | 5 | `src/RobotClient.cpp:147,154,161,166,174`（`m_socket` 是 `QObject*`，downcast 到具体 socket 类） |
| `static_cast<MeasureType>(int)` | 1 | `src/MainWindow.cpp:182`（combo idx → enum） |
| `static_cast<RobotStatus>(frame.cmd)` | 1 | `src/RobotClient.cpp:441`（uint8 → enum） |
| `reinterpret_cast<const uchar *>` | 1 | `src/CalibrationManager.cpp:208`（HALCON 指针 → QImage 数据） |
| `static_cast<int>(frame.step)` | 1 | `src/CameraManager.cpp:178`（size_t → int 用于 QImage 构造） |
| `const_cast / dynamic_cast` | 0 | — |

---

## 10. magic / hardcoded / 硬编码

**匹配数：3**（`src/`）

| 文件:行 | 内容 |
|---|---|
| `src/Logger.cpp:13` | 注释 `// C++11 magic statics: …`（B09 修复痕迹） |
| `src/MeasurementEngine.cpp:355` | 注释 `// phantom number computed over the whole image with a hardcoded threshold).`（B23 修复痕迹） |
| `src/MeasurementEngine.cpp:375` | 注释 `// Auto Otsu threshold for dark objects (replaces hardcoded [0,128]).`（B23 修复痕迹） |

→ 三处均为**已修复 bug 的修复说明**，不是新问题。

---

## 总结

| 模式 | 匹配数 |
|---|---|
| 1. TODO/FIXME/XXX/HACK | 4 |
| 2. `/* paramName */` | 0 |
| 3. `return false/-1/nullptr` | 43 |
| 4. 空 catch | 2 |
| 5. qWarning/qDebug | 17 |
| 6. `// fix/hack/workaround/暂时` | 0 |
| 7. 不安全 C 函数 | 5（全部 memcpy，安全使用） |
| 8. `new X` | ~70 |
| 9. C++ cast | 18 |
| 10. magic/hardcoded/硬编码 | 3（均为修复痕迹） |
| cppcheck 高严重度 | N/A（未安装） |

---

## 推荐 "真 bug" 优先级前 5

按"实际可能在运行时造成不可观测失败 / 资源泄漏 / 调试杀手"的严重度排：

1. **`src/CalibrationManager.cpp:23 + :254` — 2 处空 `catch (...) {}` 完全吞异常**
   - 析构和 cleanup 路径里 ClearCalibData 真抛了你永远不知道
   - 析构不能再抛是 C++ 规矩，但**至少应当 `catch (HException &e) { qWarning() << e.ErrorMessage().Text(); }`** 留个 trace
   - B12 教训直接重演风险（"可观测性是基础设施，不是奢侈品"）

2. **`src/DatabaseManager.cpp:486` — `if (!q.exec()) return false;` 静默 SQL 失败**
   - 唯一一处 `q.exec()` 失败完全不 log 也不 emit databaseError 的位置
   - 是 `pruneOldRecords` / 清理类查询失败时用户和日志都不知道
   - 对比同文件其他 SQL 失败路径全部 emit/qWarning，明显是漏写

3. **`src/CameraManager.cpp:79,110` — `delete m_capture` 手动管理裸 `cv::VideoCapture*`**
   - 若 openCamera 中途抛异常（OpenCV 内部 throw），`new` 后到 `delete` 之间的异常路径泄漏
   - 改 `std::unique_ptr<cv::VideoCapture>` 一行修完，永久解决
   - 不是必现 bug，但属于"等异常路径触发才暴露"的潜伏型

4. **`src/RobotClient.cpp:92,95,102,105,119,123` — 协议解析 6 处 return false 全部无 log**
   - 攻击/异常帧场景下完全无诊断信息（"为什么我刚才那个帧被吞了？"）
   - 不该每帧都 log（DOS 风险），但**至少加 frame 计数器 + 周期性 log "解析失败 N 次"** 这种聚合可观测性
   - 属于 production 调试时的"盲区"

5. **`src/VisionProcessor.cpp:103` — `m_modelValid = false; return false;` 无 emit**
   - 比其他 catch 路径少一次 `emit processingError(...)` 通知调用方
   - 调用方（UI）可能不知道"刚刚模型变无效了"，按钮状态可能与实际不同步
   - 类比 B07 的"状态机单一信号源"问题

——以上 5 条都是"会拖低后续调试质量"或"潜在资源泄漏"，没有一条是必现崩溃；当前代码整体可以认为已经稳定。
