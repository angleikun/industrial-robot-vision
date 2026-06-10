# Review 2: HALCON 算法层深度

**审查范围**: VisionProcessor / CalibrationManager / MeasurementEngine / CoordTransform（HALCON 集中地）
**HALCON 版本**: 24.11（CMake 强制 `HALCON_LIB halconcpp REQUIRED`）
**审查日期**: 2026-06-06
**视角**: HALCON 业界 best practice，不只是"能跑通"

---

## 未能验证的项

- HALCON DLL 部署完整性（运行时验证范围，本次只读静态代码）
- License 类型（单机/网络/试用）— 仅从代码看到 5200-5399 错误码分类

---

## (1) HALCON 资源生命周期

### ✅ 做得好的

- **HShapeModel 在 ctor/dtor 配对**:
  - ctor 默认构造空（VisionProcessor.cpp:21-24）
  - dtor → clearTemplate() → clearTemplateUnlocked() → `m_shapeModel.ClearShapeModel()`（VisionProcessor.cpp:152-162）
  - loadTemplate 重入也先清旧模型（VisionProcessor.cpp:107-122）
- **HCalibData 显式清理**:
  - `~CalibrationManager` (.cpp:14-24): `ClearCalibData(m_calibDataID)` + `m_distortionMap.Clear()`
  - calibrateHandEye 结束后立刻 `ClearCalibData(hv_HE_CalibDataID)`（.cpp:365）—— **手眼标定后清，符合 best practice**
- **HObject / HTuple 全部栈对象**：grep 100 处使用，没有 `new HObject` / `new HTuple`，自动 RAII ✅
- **循环里的 HObject 不重复初始化**: CalibrationManager.cpp:140-149 循环里每次 `HalconCpp::HTuple empty` 是栈局部，符合 HALCON 引用计数语义

### ⚠️ 风险点

| # | 文件:行 | 问题 |
|---|---|---|
| H1.1 | **CalibrationManager.cpp:23** | `~CalibrationManager() { try {...} catch (...) {} }` — 析构沉默吃异常。RAII 正确，但**完全无日志**。如果 ClearCalibData 反复失败，永远不会发现。建议改 `catch (HException &e) { qWarning() << ... }`。 |
| H1.2 | **CalibrationManager.cpp:174-175** | `GetCalibDataObservPose(m_calibDataID, 0, 0, m_calibImages.size() - 1, &m_lastCalibPose);` —— 取最后一张图的标定板 pose 作为 m_lastCalibPose（喂给 CoordTransform）。**如果最后一张图正好是失败的（line 141-148 silently skipped）**，m_lastCalibPose 是无效 pose。**实质 bug**：应取**成功标定的**最后一张图。 |
| H1.3 | **VisionProcessor.cpp:148** + **88** | createTemplate 持锁全程跑 `CreateScaledShapeModel`（可能数秒）。同时 setMatchConfig (.cpp:242) 也持锁。**冲突场景**：UI 调 score 阈值时正在创建模板，会阻塞 UI 直至 create 完成。已通过 BusyGuard 在 processImage 侧避免 piling up（.cpp:225），但 UI 线程的小操作仍可能卡 1-3s。 |

---

## (2) 算子参数选择

### CreateScaledShapeModel（VisionProcessor.cpp:88-92）

| 参数 | 当前值 | 评估 |
|---|---|---|
| NumLevels | `0`（auto） | ✅ 推荐 |
| AngleStart/Extent | `m_config.angleStart/Extent` rad | ✅ UI 可调 |
| AngleStep | `0`（auto） | ✅ 推荐 |
| ScaleMin/Max | `m_config.minScale/maxScale` | ✅ UI 可调 |
| ScaleStep | `0`（auto） | ✅ 推荐 |
| Optimization | `"auto"` | ✅ 推荐 |
| **Metric** | **`"use_polarity"`** | ✅ 推荐（同极性匹配，更稳） |
| Contrast | `"auto"` | ✅ |
| MinContrast | `"auto"` | ✅ |

**结论**: 模板创建参数选择**符合 HALCON 官方建议**。注释明确说为什么选 ScaledShapeModel（而非 ShapeModel）— "Scaled model tolerates size changes... that was why a small move of the bottle lost the match." **有踩坑+迭代经验**。

### FindScaledShapeModel（VisionProcessor.cpp:184-194）

| 参数 | 当前值 | 评估 |
|---|---|---|
| MinScore | `m_config.minScore` (UI 可调) | ✅ |
| NumMatches | `m_config.maxTargets` | ✅ |
| MaxOverlap | **`0.5` 硬编码** | 🟡 可暴露给 UI |
| SubPixel | `"least_squares"` | ✅ 推荐（亚像素） |
| NumLevels | `0`（auto） | ✅ |
| Greediness | `0.9` | 🟡 偏激进（默认 0.9 = 接受较差初值的快速搜索；保守用 0.5）|

### EdgesSubPix（MeasurementEngine.cpp 3 处重复）

```cpp
EdgesSubPix(..., "canny", 0.5, 10, 30)  // line 98-100, 151-153, 228-230
```

- **`alpha=0.5`** 中等平滑 ✅
- **`low=10, high=30` 硬编码** 🟡 — 光照变化时滞回阈值要调
- **重复 3 处**：应提取常量 `static constexpr int CANNY_LOW = 10, CANNY_HIGH = 30;` 或暴露到 settings.json

### Threshold（MeasurementEngine.cpp:299-300）

```cpp
Threshold(hImg, &thresholdRegion, 0, 128)  // 硬编码 0-128
```

🔴 **严重**：measureArea 用固定灰度阈值 128，**光照一变就全错**。工业级应：
- `BinaryThreshold(hImg, "max_separability")`（Otsu）
- 或用 `DynThreshold` 配合局部均值
- 或暴露阈值到 settings.json + UI

---

## (3) 坐标系转换

### 完整链路（CoordTransform.cpp:71-171）

```
像素(px, py)
  → [Step 1] ImagePointsToWorldPlane(camParam, calibPose, ...) → 相机坐标 (X_cam, Y_cam, Z=0)
  → [Step 2] AffineTransPoint3d(m_camHomTool) → 工具坐标 (X_tool, Y_tool, Z_tool)
  → [Step 3] AffineTransPoint3d(baseHomTool) → 基座坐标 (X_base, Y_base, Z_base)
```

### ✅ 做得好的

- **行列顺序明确注释**：CoordTransform.cpp:104 `ImagePointsToWorldPlane uses (Row=y, Col=x) — note swapped order` — **踩过坑+留 trace**
- **HHomMat3D + HPose 互转**：用 HALCON 官方 `pose.PoseToHomMat3d()`（.cpp:28, 133）
- **工作空间校验**：isInWorkspace() (CoordTransform.cpp:191) + emit outOfWorkspace signal (.cpp:160)
- **TestMode**: setTestMode(.cpp:38) 绕过 HALCON 投影用 px × 比例直接出 mm，**联调用途**（T3 测试无相机时用）—— 实际工业代码常需要这种"测试桩"
- **手眼方向一致**: HALCON `CreateCalibData("hand_eye_moving_cam", ...)`（CalibrationManager.cpp:317） + `GetCalibData("camera", 0, "tool_in_cam_pose", ...)`（line 348-350）—— 即 **eye-in-hand** （相机随机器人末端移动），符合 README/CLAUDE.md 描述

### ⚠️ 风险点

| # | 文件:行 | 问题 |
|---|---|---|
| H3.1 | **CoordTransform.cpp:148-151** | `worldAngle = angle + base_H_tool.rz` — **只用 Z 旋转**，假设工作平面水平、相机俯视。**2.5D 简化**，不写在文档/UI 提示里 → 工业场景斜置工件时角度错。 |
| H3.2 | **CoordTransform.cpp:180-187** `undistortPixel` | 用 `CameraIntrinsic` (fx/fy/cx/cy) 做归一化坐标变换 — **但 m_intrinsic 的 fx/fy 单位是像素**还是**米**？(CalibrationManager.cpp:120-131 用米作为输入 startCamParam) 单位不明会出 bug。 |
| H3.3 | **CoordTransform.h:88-89** | `m_hasCamParam` 和 `m_handEyeConfigured` 两个 bool 旁路 `m_intrinsic.valid`。三个状态彼此独立设置（setIntrinsic / setCameraParam / setHandEyePose 三种入口），易出现**部分 set / 不一致**。建议合并到一个 `enum class CalibState`。 |

---

## (4) 标定流程：工业级 vs demo 级

### 内参标定（CalibrationManager.cpp:102-184）

🔴 **3 处 demo 级硬编码**：

| 行 | 硬编码 | 工业级应该 |
|---|---|---|
| 120 | `pixelSize = 5.5e-6` (5.5 μm) | 从相机厂商手册或 EEPROM 读取；最少应是 settings.json 配置项 |
| 121 | `focalLen = 0.012` (12mm) | 同上 |
| 128-131 | `cx=960, cy=540, w=1920, h=1080` | 应从 m_camera 当前分辨率推导 |

⚠️ **API 失真**：
- `CalibResult` struct 暴露 K1/K2/K3/P1/P2 看起来像 OpenCV 5 参畸变模型
- 实际 HALCON `area_scan_division` 模型**只返回 1 个 Kappa** (.cpp:162)
- K2/K3/P1/P2 **硬编为 0**（.cpp:163-166）
- 用户/调用者会误以为得到了 OpenCV 标准畸变系数

✅ **reprojectionError 暴露**（.cpp:159）—— 工业级最重要的指标 ✅

🟡 **失败图像静默跳过**：line 141-148 `catch (HException &) { continue; }`，**无累计统计、无最低成功率检查**。15 张标定图全失败，照样 CalibrateCameras（line 153）只是返回结果 quality 差。

### 手眼标定（CalibrationManager.cpp:303-376）

✅ **设计合理**：
- 用 HALCON 官方 `CalibrateHandEye` + CalibData API 模式
- 模型 `"hand_eye_moving_cam"` = eye-in-hand ✅
- 两个误差暴露：translation (mm) + rotation (deg)（line 353-354，弧度→度正确转换）
- Cleanup `ClearCalibData(hv_HE_CalibDataID)` ✅（line 365）

⚠️ **没暴露重投影/位姿一致性矩阵**：CalibrateHandEye 还能输出每组位姿的残差，**当前仅用 hv_Errors[0/1]** 全局统计。多组位姿中**哪一组是离群点**没暴露。漂移检测/troubleshooting 困难。

### 标定数据持久化

- **内参**: `WriteCamPar` / `ReadCamPar`（HALCON 原生二进制 .cpd 格式）—— calibration.cpp:222, 233
- **手眼**: `WritePose` / `ReadPose`（HALCON 原生 .dat）—— line 382, 393
- 🟡 **格式不可读+不可比对**：HALCON 私有格式，用户无法 cat 看；**多次标定结果对比**（漂移检测）需要解析二进制。建议同时输出一份 JSON。

### 多次标定结果对比（漂移检测）

❌ **完全缺失**：当前每次 calibrate 直接覆盖 m_camParam / m_handEyeCamInToolPose。
- **工业级要求**：保留近 N 次标定结果，对比 reprojectionError、cx/cy 变化、手眼平移误差变化
- DatabaseManager 有 `CalibSessionRecord` 表（DatabaseManager.h:42-48）但只存 reprojectionErr + filePath，**未存详细参数**
- **改进**：扩展 CalibSessionRecord 增加 camParam JSON，UI 加"标定历史"窗口对比近期变化

---

## (5) 5 种测量功能算法正确性

| # | 函数 | 算法 | 评估 |
|---|---|---|---|
| 1 | **measureLength** | EdgesSubPix → SelectShapeXld → **FitLineContourXld Tukey** | ✅ 用拟合 + 鲁棒估计，工业级 |
| 2 | **measureCircle** | EdgesSubPix → SelectShapeXld → **FitCircleContourXld `"algebraic"`** | 🟡 algebraic 是代数法（一阶近似），**对部分圆弧/有畸变的圆精度差**。HALCON best practice: **改 `"geometric"` 或 `"huber"` 更精确** |
| 3 | **measureDistance** | 直接 `DistancePp(p1, p2)` × pixelEquivalent | 🟡 **完全没用图像**（line 194 `hImg` 加载但未用）—— 直接用 UI 点击像素，**没做亚像素吸附**。工业级应在点击位置局部 EdgesSubPix 找最近边缘亚像素中心。 |
| 4 | **measureAngle** | 自动取整图最长 2 条边 → AngleLl | 🔴 **算法 bug**：`/*line1*/`（line 217）和 `/*line2*/`（line 218）**忽略用户传入参数**！UI 让用户点 4 个点选两条线，**代码完全不用**，自动 SelectShapeXld 选边。**和 UI 承诺不符**。SortContoursXld 用 `"upper_left"` 排序（line 243）也不是按长度排，逻辑也错。 |
| 5 | **measureArea** | Threshold 0-128 → Connection → SelectShapeStd max_area ≥70 | 🔴 **多重问题**：① 灰度阈值 **128 硬编码**（光照变即失效）② **完全忽略 roi 参数**（line 289 `/*roi*/`）—— 对整图测量；③ SelectShapeStd `"max_area"` 选最大面积区域，**用户没法选目标对象** |

### 稳定性（同一目标多次测量）

❌ **代码层未做稳定性评估**。CalibResult 暴露了 reprojectionError，但 MeasureResult 只有 `valid` bool，**没有 confidence / sigma / residual**：
- FitLineContourXld 第 11 输出 `hv_Dist` 是残差 — **未保存到 MeasureResult**
- FitCircleContourXld 6 个输出参数有 3 个被 `emptyOut` 接收（line 167）—— 残差信息丢失
- 多次测量稳定性需要外部 Python 脚本 grep log 才能算出 — **工业级应该 UI 上直接显示** ±σ

---

## (6) License 与部署

### License 检查（MainWindow.cpp:464-495）

✅ **设计成熟**：
- **启动时一次性自检**: `GenImageConst("byte", 16, 16)` 触发 License 验证（line 468）
- **错误码分类**: 5200-5399 范围 OR 错误信息含 "License" 才进降级模式
- **#1301 反例**: 注释明确说 "wrong value of control parameter" 不是 License 问题。**踩过坑**：曾经 self-test hiccup 永久禁用了"创建模板"按钮，所以现在加了反例防御
- **降级模式**: License 失败时**相机/TCP/数据库仍正常工作**，只灰显视觉功能（CLAUDE.md 描述 + MainWindow::onLicenseStatusChanged）

### License 过期/缺失处理

✅ 处理路径完整：
- License 错误 → `onLicenseStatusChanged(false)` → `m_degradedMode = true`（MainWindow.h:151）+ UI 按钮 disable

⚠️ 缺：
- License **过期前 N 天预警**：没有 — License 即将过期时的"剩余天数"提示
- 网络 License 服务器**心跳检测**：没有 — 长时运行中网络 License 中断怎么办

### HALCON DLL 部署

⚠️ **CMakeLists.txt 缺 DLL 复制规则**：
- line 94-103 只复制 `config/` 和 `resources/`，**没复制 HALCON DLL**
- 用户必须自己把 HALCON `bin/x64-win64/*.dll` 加入 PATH，否则程序启动失败
- `windeployqt` 只处理 Qt DLL，**不处理 HALCON**
- 建议添加 `add_custom_command(POST_BUILD ... copy_if_different "${HALCON_ROOT}/bin/x64-win64/halconcpp.dll" ...)`

---

## HALCON 工程化成熟度评分（1-10）

| 维度 | 分 | 依据 |
|---|---|---|
| 资源生命周期 | 8.5 | HShapeModel/HCalibData 配对清理，HObject/HTuple 全栈对象 |
| 算子参数 | 7 | 模板匹配参数到位，但 EdgesSubPix/Threshold 硬编码 |
| 坐标变换 | 8 | 三步链清晰，注释踩过的坑 (row/col 顺序) |
| 标定流程 | 5.5 | hand-eye 设计成熟，但相机参数硬编码 + 漂移检测缺失 |
| 5 种测量 | **4** | **measureAngle 忽略参数、measureArea 忽略 ROI + 硬编码阈值 — 两个严重 bug** |
| License/部署 | 7 | License 分类 + 降级模式成熟，DLL 部署需手动 |

**综合：6.5 / 10** —— **HALCON API 用对了，但 5 种测量中 2 种是 demo 级实现**。

---

## 🎤 5 个面试时主动讲的 HALCON 设计决策

### 决策 1：为什么选 `CreateScaledShapeModel` 而不是普通 `CreateShapeModel`
> "项目早期用了 `CreateShapeModel`，但实测时发现工件离相机远近变化（哪怕 5cm）就丢失匹配。换成 `CreateScaledShapeModel` 并暴露 `m_config.minScale/maxScale` 后，工件 z 方向的小幅移动就不再丢匹配。这个修复记录在 `VisionProcessor.cpp:84-87` 的注释里。"

**展示**：踩坑→迭代意识

### 决策 2：HALCON License 检查为什么要做错误码分类
> "我曾经因为 HALCON binding 里 #1301 'wrong value of control parameter' 被错误归类为 License 失败，导致 self-test 抖一下整个'创建模板'按钮永久灰掉。后来我加了错误码范围判断：只有 5200-5399 范围或消息含 'License' 才算 License 问题，其他错误一律保持功能可用（MainWindow.cpp:480-490）。这是踩了一次坑后写进代码的'反例防御'。"

**展示**：可观测性意识 + 工业级容错

### 决策 3：VisionProcessor 的非递归 mutex + clearTemplateUnlocked
> "VisionProcessor 用非递归 QMutex 保护 m_shapeModel。早期版本 loadTemplate 内部调 clearTemplate，clearTemplate 自己 lock 又导致同线程重入死锁。修复是提取了 `clearTemplateUnlocked` 给已持锁路径用，loadTemplate 持锁后直接调它。这种细节在 VisionProcessor.h:78-82 + .cpp:152 有完整注释。"

**展示**：死锁定位 + 非破坏性修复

### 决策 4：BusyGuard struct 阻止帧积压
> "vision worker 线程的 event queue 会积压未处理的 processImage 请求，做长时压测时这导致 createTemplate（GUI 线程触发）也卡在 mutex 上。我用 `std::atomic<bool> m_busy` + `compare_exchange_strong` 在入口直接 drop 重叠帧，并用 RAII BusyGuard struct 保证 busy flag 在异常路径也复位（VisionProcessor.cpp:225-234）。"

**展示**：背压设计 + RAII 习惯

### 决策 5：CoordTransform 三步链 + Row/Col 顺序埋雷
> "像素到世界坐标三步：图像→相机平面（ImagePointsToWorldPlane）→工具→基座。HALCON 的 ImagePointsToWorldPlane 接受 (Row, Col) 顺序，**和 QPoint 的 (x, y) 反过来**——我第一次写就反了，调了 3 小时。所以现在 CoordTransform.cpp:104 那一行注释专门提醒后续维护者：`Row=y, Col=x — note swapped order`。"

**展示**：跨 SDK 约定踩坑 + 留 trace 给后人

---

## 真问题待修（Top 5）

### Top 1（必修）：measureAngle 忽略用户选择的两条线
- **位置**: `src/MeasurementEngine.cpp:216-285`
- **现象**: UI 让用户点 4 个点选两条线，代码 `/*line1*/ /*line2*/` 完全没用，自动选整图最长 2 条边 → 结果与用户期望不符
- **修法**: 用 line1[0]/line1[1] 和 line2[0]/line2[1] 作为 ROI 中心，在小窗口内 EdgesSubPix + FitLineContourXld

### Top 2（必修）：measureArea 忽略 ROI + 硬编码阈值
- **位置**: `src/MeasurementEngine.cpp:289-324`
- **现象**: `/*roi*/` 注释、Threshold(0, 128) 硬编码 → 光照变化即失效，且测整图最大区域而非用户选区
- **修法**: ReduceDomain(roi) + BinaryThreshold("max_separability") 替代硬编码 128

### Top 3：相机内参标定硬编码 pixelSize/focalLen
- **位置**: `src/CalibrationManager.cpp:120-131`
- **修法**: 提到 settings.json `calibration.camera_pixel_size_um` / `calibration.lens_focal_mm`，UI 在标定向导里采集

### Top 4：FitCircleContourXld 用 algebraic
- **位置**: `src/MeasurementEngine.cpp:163`
- **修法**: `algebraic` → `huber`（鲁棒）或 `geometric`（无偏），精度提升 1-2 个数量级

### Top 5：CalibrationManager 失败图像无最低成功率检查
- **位置**: `src/CalibrationManager.cpp:140-149` + `260-301`
- **修法**: 累计 failedCount，超过 `m_minPoses / 3` 直接 emit calibError 阻止低质量标定

### Top 6（赠送）：HALCON DLL 不在 CMakeLists POST_BUILD 复制
- **位置**: `CMakeLists.txt:94-103`
- **修法**: 添加 `copy_if_different "${HALCON_ROOT}/bin/x64-win64/halconcpp.dll"` 等 DLL 到输出目录
