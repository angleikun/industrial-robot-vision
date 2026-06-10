# Review v2 — Report 6: UI 完整性静态分析

**审查范围**: `src/MainWindow.cpp` + `include/MainWindow.h` + `src/CalibrationWizard.cpp` + `include/CalibrationWizard.h` + `src/ImageView.cpp` + `src/ReportManager.cpp`（被 UI 间接调用）
**审查方法**: 纯静态分析，未运行程序、未点击任何按钮、未修改代码
**结论提要**: 13 个 QPushButton + 9 个 QAction + 3 个 QMenu + 0 个 QShortcut。**5 个控件实际指向 stub / 残缺实现，3 个控件长操作无 busy 反馈，1 个流程缺取消清理路径**。

---

## 未能验证的项

- 无（所有 UI 文件、所有相关 slot 实现、所有被调用的 `Manager` 均成功读取）
- ImageView.cpp 已 grep 确认无 `QPushButton/QAction/QMenu/QShortcut`，仅承载 overlay 渲染 + 鼠标交互

---

## 1. QPushButton 控件映射表（13 个）

| # | 控件名 | 显示文本 | 所在窗口 | clicked → slot | slot 状态 | 风险 |
|---|---|---|---|---|---|---|
| 1 | `m_btnStartAcq` | 开始采集 | MainWindow / 控制面板 | `onStartAcquisition` (MainWindow.cpp:553) | ✅ 正常 | 无 |
| 2 | `m_btnStopAcq` | 停止采集 | MainWindow / 控制面板 | `onStopAcquisition` (MainWindow.cpp:571) | ✅ 正常 | 无 |
| 3 | `m_btnCreateTpl` | 创建模板 | MainWindow / 控制面板 | `onCreateTemplate` (MainWindow.cpp:599) | ✅ 正常 | 无（License/降级/图像三重前置检查到位） |
| 4 | `m_btnLoadTpl` | 加载模板 | MainWindow / 控制面板 | `onLoadTemplate` (MainWindow.cpp:662) | ✅ 正常 | 无 |
| 5 | `btnMeasure`（局部变量） | 执行测量 | MainWindow / 控制面板 | lambda → `onMeasure(measureType->currentIndex())` (MainWindow.cpp:176-179) | 🔴 **部分调用 stub** | **当类型 = 角度 / 面积时进入 stub**——`MeasurementEngine::measureAngle` 忽略用户点选的 4 个点、`measureArea` 忽略用户拖的 ROI（见 `02_halcon.md`）；UI 仍显示"测量结果: 45.3°"假装成功 → **欺骗用户** |
| 6 | `m_btnConnect` | 连接机器人 / 断开连接 | MainWindow / 控制面板 | `onConnectRobot` (MainWindow.cpp:866) | ✅ 正常 | 无 |
| 7 | `m_btnSendPose` | 发送抓取位姿 | MainWindow / 控制面板 | `onSendGrabPose` (MainWindow.cpp:884) | ⚠️ **简单实现** | 调用 `queryToolPose` 同步阻塞 ≤ 1s，期间 UI 无 busy 提示；标定+检测+空间校验链路完整，但**连接成功后立即 setEnabled(true)**（line 877），未等 statusChanged → 用户可在 STATUS_UNKNOWN 时按下 |
| 8 | `calibBtn`（局部变量） | 相机标定 | MainWindow / 参数面板 | `onCalibrateCamera` (MainWindow.cpp:746) | ✅ 正常 | 无 |
| 9 | `handEyeBtn`（局部变量） | 手眼标定 | MainWindow / 参数面板 | `onCalibrateHandEye` (MainWindow.cpp:769) | ✅ 正常 | 无（前置要求内参标定完成） |
| 10 | `btnQuery`（局部变量） | 查询最近100条 | MainWindow / 历史面板 | `onQueryHistory` (MainWindow.cpp:973) | ✅ 正常 | 无 |
| 11 | `btnExport`（局部变量） | 导出CSV | MainWindow / 历史面板 | `onExportCSV` → `doExport("csv")` (MainWindow.cpp:990) | ⚠️ **简单实现** | 30 天 10000 条记录无进度反馈（ReportManager 内部 `emit exportProgress`，但 MainWindow **未连接该信号** → 进度心跳无效，line 69 信号沉默） |
| 12 | `m_btnCapture` | 采集当前帧 | CalibrationWizard | `onCaptureClicked` (CalibrationWizard.cpp:103) | ✅ 正常 | 无 |
| 13 | `m_btnCalibrate` | 开始标定 / 标定中... | CalibrationWizard | `onCalibrateClicked` (CalibrationWizard.cpp:130) | ⚠️ **简单实现** | calibrateCamera/calibrateHandEye 在 GUI 线程同步阻塞数秒，仅靠文本切换"标定中..."提示，**期间 dock + main window 仍可点击其它按钮（弱模态）** |

### 命中行号汇总

| 控件 | declare | connect | slot |
|---|---|---|---|
| `m_btnStartAcq` | MainWindow.cpp:127 | MainWindow.cpp:170 | 553 |
| `m_btnStopAcq` | MainWindow.cpp:128 | MainWindow.cpp:171 | 571 |
| `m_btnCreateTpl` | MainWindow.cpp:139 | MainWindow.cpp:172 | 599 |
| `m_btnLoadTpl` | MainWindow.cpp:140 | MainWindow.cpp:173 | 662 |
| `btnMeasure` | MainWindow.cpp:152 | MainWindow.cpp:176 | 793 (`onMeasure`) |
| `m_btnConnect` | MainWindow.cpp:161 | MainWindow.cpp:174 | 866 |
| `m_btnSendPose` | MainWindow.cpp:162 | MainWindow.cpp:175 | 884 |
| `calibBtn` | MainWindow.cpp:203 | MainWindow.cpp:208 | 746 |
| `handEyeBtn` | MainWindow.cpp:204 | MainWindow.cpp:209 | 769 |
| `btnQuery` | MainWindow.cpp:227 | MainWindow.cpp:236 | 973 |
| `btnExport` | MainWindow.cpp:228 | MainWindow.cpp:237 | 1013 (`onExportCSV`) → 990 (`doExport`) |
| `m_btnCapture` | CalibrationWizard.cpp:71 | CalibrationWizard.cpp:91 | 103 |
| `m_btnCalibrate` | CalibrationWizard.cpp:81 | CalibrationWizard.cpp:92 | 130 |

---

## 2. QAction 控件映射表（9 个）

| # | 控件名 | 显示文本 | 所在菜单 | triggered → slot | slot 状态 | 风险 |
|---|---|---|---|---|---|---|
| 1 | `actLoadTpl`（局部） | 加载模板... | 文件(&F) | `onLoadTemplate` (MainWindow.cpp:662) | ✅ 正常 | **降级模式下未禁用**（onLoadTemplate 内部无 `m_degradedMode` 检查，但 `loadTemplate` 在 visionProcessor 内会触发 HALCON 异常）→ 弹窗 IOError 体验差 |
| 2 | `actExportCSV`（局部） | 导出CSV... | 文件(&F) | `onExportCSV` (MainWindow.cpp:1013) | ✅ 正常 | 同 #11 无进度反馈 |
| 3 | `actExportReport`（局部） | 导出报表... | 文件(&F) | `onExportReport` → `doExport("pdf")` (MainWindow.cpp:1008) | ⚠️ **简单实现** | 10000 条 PDF 渲染（QPainter 在 GUI 线程同步）**期间 UI 完全冻结**，无进度，无 cancel |
| 4 | `actExit`（局部） | 退出(&X) | 文件(&F) | `QMainWindow::close` (MainWindow.cpp:255) | ✅ 正常 | 无 |
| 5 | `actIntrinsic`（局部） | 相机内参标定... | 标定(&C) | `onCalibrateCamera` (MainWindow.cpp:746) | ✅ 正常 | 无 |
| 6 | `actHandEye`（局部） | 手眼标定... | 标定(&C) | `onCalibrateHandEye` (MainWindow.cpp:769) | ✅ 正常 | 无 |
| 7 | `actTestCalib`（局部，checkable） | 测试标定模式 (无机器人时联调 T3) | 标定(&C) | lambda → `m_coordTrans->setTestMode(on, 0.05)` (MainWindow.cpp:266) | ✅ 正常 | 无（已附明显警示日志） |
| 8 | `actT4`（局部，checkable） | T4 稳定性测试模式 (每30秒自动发送抓取) | 帮助(&H) | lambda → `m_t4Timer->start/stop` (MainWindow.cpp:281) | 🔴 **缺前置检查** | T4 触发后 30 s 一次 `onSendGrabPose`，若**标定未完成 / 模板未加载 / 机器人未连接 / 当前无目标** → 每 30 s 弹一次模态 QMessageBox → **测试期间用户被阻塞**（详见风险 R3） |
| 9 | `actAbout`（局部） | 关于... | 帮助(&H) | lambda → `QMessageBox::about` (MainWindow.cpp:292) | ✅ 正常 | 无 |

---

## 3. QMenu / QShortcut

| 类型 | 名称 | 实现 |
|---|---|---|
| QMenu | 文件(&F) | MainWindow.cpp:242 — Alt+F 助记符正常 |
| QMenu | 标定(&C) | MainWindow.cpp:257 — Alt+C 助记符正常 |
| QMenu | 帮助(&H) | MainWindow.cpp:273 — Alt+H 助记符正常 |
| QShortcut | （无） | 全工程 `grep "QShortcut"` 0 命中——**Ctrl+S 保存模板、Ctrl+E 导出、Esc 取消框选模板 / 测量** 均未配置 → 工业 UI 习惯缺失 |

**未被禁用的 menu action（risk）**:
- 降级模式下：`actLoadTpl`、`actIntrinsic`、`actHandEye`、`actTestCalib`、`actT4` 均**未在 `onLicenseStatusChanged` 中调用 `setEnabled(false)`**——它们点击后会被 slot 内的 `if (m_degradedMode)` 拦截然后弹警告，但**点击之前 menu 不灰显**，违反 Qt UX 惯例（应让用户一眼看到不可用）

---

## 4. QComboBox / QSpinBox / QDoubleSpinBox（非按钮但影响行为）

| 控件 | 用途 | 连接 | 风险 |
|---|---|---|---|
| `m_cameraCombo` (QComboBox) | 选择相机 | **无 currentIndexChanged 连接**，仅 `onStartAcquisition` 内 `currentText()` 即时取值 (MainWindow.cpp:555) | `onCameraSelected(int)` slot 在头文件存在但**永不被调用**（MainWindow.cpp:548-551 `Q_UNUSED` 空体）→ 死代码 |
| `m_measureType` (QComboBox) | 选择测量类型 | 通过 `btnMeasure` 间接读取 | 无 |
| `m_scoreThresh` (QDoubleSpinBox) | 匹配得分阈值 | valueChanged → `applyMatchConfig` (MainWindow.cpp:331) | ✅ 实时同步到 VisionProcessor |
| `m_maxTargets` (QSpinBox) | 最大目标数 | valueChanged → `applyMatchConfig` (MainWindow.cpp:333) | ✅ 实时同步 |

---

## 5. QFileDialog / QMessageBox 编码风险

### 5.1 全工程 QFileDialog（2 处）

| 位置 | 标题 | 中文路径风险 |
|---|---|---|
| MainWindow.cpp:650 | "保存模板" → `*.shm` | ⚠️ **低风险**——QString 全程 UTF-16，Qt6 默认 Win API 走 W 系列，中文路径可安全；但若 `QApplication::applicationDirPath()` 返回路径含中文（用户安装到"D:\我的程序\"），Save 弹窗显示正常，文件创建正常 |
| MainWindow.cpp:664 | "加载模板" → `*.shm` | 同上 |

**结论**: Qt6 + MSVC 2022 + UTF-8 源码（CMakeLists.txt `/utf-8` 已确认）→ **无 GBK 乱码风险**。但 **CSV/PDF 输出路径**（ReportManager 内）通过 `QFile` 写入也是 UTF-16 → W API，安全。

### 5.2 全工程 QMessageBox（10 处）

全部使用中文字符串字面量 + UTF-8 源码 + Qt6 → 显示安全。

**唯一隐患**: ReportManager.cpp:85 `emit exportError(path, "Excel 导出需 libxlsxwriter 或 QAxObject")` 信号**未被 MainWindow 连接** → exportError 发射但无 UI 响应，用户只看到"XLSX 导出失败"日志，**真因丢失**。

---

## 6. 「未就绪时禁用」保护

| 按钮 | 应等待的状态 | 当前保护 | 评分 |
|---|---|---|---|
| 开始采集 | 相机选择非空 | 内部检查 QMessageBox 提示 (MainWindow.cpp:556) | ✅ |
| 停止采集 | 正在采集 | `setEnabled(false)` 初始 + 开始采集后翻转 (MainWindow.cpp:129, 565) | ✅ |
| 创建模板 | License OK + 有图像 | License 启动时 setEnabled (line 536)，图像检查在 slot (line 605) | ✅ |
| 加载模板 | License OK | **无 setEnabled 保护**（line 142 默认 true），slot 内无 degradedMode 检查 | ⚠️ |
| 执行测量 | License OK + 有图像 | slot 内 `m_degradedMode` 检查 (line 795) + 图像检查 (line 800) | ✅ 检查在 slot |
| 连接机器人 | （无前置） | 无 | ✅ |
| **发送抓取位姿** | 机器人 status=READY + 标定完成 + 有有效检测 | `onRobotStatusChanged` 依据 status 切换 enabled (line 956)；slot 内补检测 + 标定校验 | ⚠️ **连接成功瞬间** line 877 立即 `setEnabled(true)`，**先于 statusChanged 到达** → 短时间窗可点击 |
| 相机标定 | License OK + 相机采集中 | slot 内三重检查 (line 747-755) | ✅ |
| 手眼标定 | License OK + 内参已完成 | slot 内三重检查 (line 770-778) | ✅ |
| 查询/导出 | DB 已 open | DB 启动时 mkpath+open；按钮无前置保护 | ⚠️ DB 失败时按钮仍可点 |
| 标定向导「开始标定」 | ≥15 帧 | `updateProgress` 中 `m_btnCalibrate->setEnabled(m_capturedCount >= 15)` (CalibrationWizard.cpp:184) | ✅ |

---

## 7. 长操作进度反馈

| 长操作 | 估计时长 | 当前反馈 | 风险 |
|---|---|---|---|
| 模板创建（HALCON CreateScaledShapeModel） | 1–5 s | 按钮文本变 "框选中…(再次点击取消)" / BlockingQueuedConnection (line 639-643) | ⚠️ 创建瞬间 UI 仍冻结，无 cursor 切换 |
| 标定（calibrateCamera/HandEye） | 1–10 s | 按钮文本变 "标定中..." (CalibrationWizard.cpp:133) | ⚠️ 主窗口仍可点击 |
| 发送抓取（queryToolPose 同步等待） | 0–1 s | 无 | 🔴 UI 期间无反馈 |
| 历史查询（DB 7 天 × 100 条） | < 0.1 s | 无 | ✅ 可忽略 |
| 导出 CSV（30 天 × 10000 条） | 1–10 s | ReportManager emit `exportProgress`，**MainWindow 未 connect** (ReportManager.cpp:69) | 🔴 **进度信号沉默** |
| 导出 PDF（QPainter 渲染 10000 行） | 5–30 s | 无 progress emit | 🔴 GUI 线程同步阻塞 |
| 标定后 emit handEyeCompleted（lambda 链） | < 0.01 s | 无 | ✅ |

---

## 8. CalibrationWizard 取消路径缺陷

**问题定位**: CalibrationWizard.cpp:130-179 完整流程，但**无 "取消" 按钮**。

通过窗口 [X] 关闭时：
1. QDialog 默认 reject() → 返回到 MainWindow 的 `wiz->exec()` (MainWindow.cpp:765, 787)
2. `calibrationCompleted(true)` 不发射（line 174 仅在成功时 emit）
3. **`m_calib->addCalibrationImage(...)` 已累计的图像残留在 CalibrationManager 内部**——`02_halcon.md` 已 flag 此项无自动 cleanup
4. 用户再次开向导 → 残留图像 + 新图像混合 → **标定结果污染**

**修复建议**: CalibrationWizard 应：
- 添加 "取消" 按钮
- 在 `reject()` / closeEvent 时调用 `m_calib->clearCalibImages()`（该方法在 02_halcon.md 中也是缺失的，需新增）
- 或在 MainWindow 中 `wiz->exec()` 返回后检查 `wiz->result() != QDialog::Accepted` → 主动清理

---

## 9. 综合风险清单（优先级排序）

### 🔴 P0（功能性 stub / 欺骗用户）

| ID | 风险 | 位置 | 影响 |
|---|---|---|---|
| **R1** | "执行测量" 按钮 → 当选 "角度" 或 "面积" 时进入 stub，用户的点选/ROI 被无声丢弃，UI 仍显示假数值 | MainWindow.cpp:176 → 793 → MeasurementEngine measureAngle/measureArea | **客户演示一票否决** |
| **R3** | T4 actT4 定时器无前置检查，未连接/未标定时每 30 s 弹模态 QMessageBox | MainWindow.cpp:281-289 | T4 长跑测试**实际不可用** |

### ⚠️ P1（用户体验 / 数据完整性）

| ID | 风险 | 位置 | 影响 |
|---|---|---|---|
| **R2** | `m_btnSendPose` 在 m_robotClient 连接成功瞬间立即 setEnabled(true)，可在 STATUS_UNKNOWN 时点击 | MainWindow.cpp:877 | 偶发"机器人未就绪"误操作 |
| **R4** | 导出 CSV/PDF 大记录集无进度反馈；ReportManager.exportProgress 信号无消费者 | MainWindow.cpp:1000-1005 + ReportManager.cpp:69 | 10000 条时 UI 冻结 5-30s |
| **R5** | CalibrationWizard 无 "取消" 按钮 + 通过 [X] 关闭时残留累计图像 | CalibrationWizard.cpp:32-93 + 02_halcon.md 已 flag | 二次标定**结果污染** |
| **R6** | 降级模式下 menu action 未灰显（仅 slot 内拦截后弹警告） | MainWindow.cpp:240-298 + onLicenseStatusChanged:510 | 违反工业 UI 惯例 |
| **R7** | ReportManager.exportExcel 是占位实现，emit exportError 但 MainWindow 未连接该信号 → 错误真因丢失 | ReportManager.cpp:80-87 + MainWindow.cpp:无 connect | 当前 UI 无 xlsx 触发路径，**dormant stub** |

### ⚠️ P2（健壮性 / 工业习惯）

| ID | 风险 | 位置 | 影响 |
|---|---|---|---|
| **R8** | 全工程 0 个 QShortcut（Ctrl+S/Ctrl+E/Esc 全部缺失） | 全工程 grep 0 命中 | 工业操作员效率低 |
| **R9** | `onCameraSelected` 是死代码（slot 存在但 combo 永不 emit currentIndexChanged 给它） | MainWindow.h:38 + MainWindow.cpp:548 | 头文件冗余 |
| **R10** | 历史查询/导出按钮无 DB 状态保护（DB open 失败仍可点击） | MainWindow.cpp:227-228 | 失败时 silent return + 日志 |
| **R11** | 模板创建（BlockingQueuedConnection）期间无 wait cursor / busy indicator | MainWindow.cpp:639 | UI 短暂"假死"观感 |

---

## 10. 与既有 review 报告的差异性

本报告是 **首次以 UI 控件为单位、自上而下穷举映射**，不与 01-05 重复：

| 与之相关的既有发现 | 本报告新增视角 |
|---|---|
| 02_halcon.md flag measureAngle/Area 忽略参数 | **新增 R1**：用户操作通过 UI 流到 stub 是 *欺骗* 而非 *未实现*，UI 仍报"成功" |
| 03_industrial.md flag queryStatus stub | **新增 R2**：UI 在 statusChanged 到达前已 enable 按钮，**stub 之外的入口时序漏洞** |
| 03_industrial.md flag T4 报告失真 | **新增 R3**：T4 触发器本身**在 UI 链路上不可用**（30s 模态弹窗），T4 报告 FAIL 的根因之一就在这里 |
| 04_devlog_compare.md 模式 C "可观测性缺失" | **新增 R4**：信号连接遗漏属于 silent drop 的 *UI 层投影* |

---

## 11. 一句话总评

> **UI 层 13+9 个控件中，2 个直接通向 stub 假装成功，1 个长时间定时器无前置检查，3 个长操作无进度反馈，1 个流程缺取消清理。从用户角度，"按钮按下后什么也没发生" 与 "按钮按下后看到了假数据" 是两种不同的事故级别——R1 属于后者，必须最高优先级修复。**

**评分**: 6.5/10（UI 框架完整、信号槽连线清晰；扣分集中在 stub 经由 UI 暴露 + 长操作无反馈 + 工业 UI 习惯缺失）

---

## 附录 A：grep 验证命令（可复现）

```bash
# 列出所有 QPushButton 声明位置
grep -n "QPushButton" src/MainWindow.cpp src/CalibrationWizard.cpp

# 列出所有 connect 行
grep -n "connect(.*clicked\|connect(.*triggered" src/MainWindow.cpp src/CalibrationWizard.cpp

# 验证 ImageView 无按钮
grep -nE "QPushButton|QAction|QMenu|QShortcut" src/ImageView.cpp   # → 0 命中

# 验证全工程 QShortcut 缺失
grep -rn "QShortcut" src/ include/   # → 0 命中

# 验证 queryStatus stub
grep -n "RobotStatus RobotClient::queryStatus" src/RobotClient.cpp   # → line 347

# 验证 exportExcel 是 stub
grep -A 5 "bool ReportManager::exportExcel" src/ReportManager.cpp   # → return false (line 80-87)

# 验证 onCameraSelected 是死代码
grep -n "onCameraSelected\|cameraSelected\|m_cameraCombo.*currentIndex" src/MainWindow.cpp
#   → declare in header, body Q_UNUSED, never connected from combo
```

---

## 附录 B：本次报告的统计

- **扫描文件数**: 6（MainWindow.cpp/h、CalibrationWizard.cpp/h、ImageView.cpp、ReportManager.cpp）
- **UI 控件总数**: 13 QPushButton + 9 QAction + 3 QMenu + 0 QShortcut = **25**
- **🔴 P0 风险**: 2 处
- **⚠️ P1 风险**: 5 处
- **⚠️ P2 风险**: 4 处
- **死代码 / 残留 stub**: `onCameraSelected` + `exportExcel`
- **未连接的信号**: `exportProgress`、`exportError`（ReportManager → MainWindow 链路缺失 2 条）

