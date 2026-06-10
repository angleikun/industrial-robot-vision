# 测量功能 UX 修复实施计划

**范围**：09_ux_fix_plan.md 覆盖 08_measure_ux.md 报告中全部 P0-P2 修复点
**模式**：本文档为**纯设计文档**，未动任何代码。等用户审完再开工。
**约束**：每个 Fix 严格独立 → diff → 编译验证 → 等待"继续 N+1"。不静默扩大 scope。

---

## 0. 08 报告补正/勘误

实施前重读代码发现 3 处需在 08 报告基础上补正：

### 补正 1：**`m_pointsNeeded` 在测量结束时未复位**（隐藏 Bug，08 报告未涵盖）
- **现象**：onMeasure 在切换类型时设置 `m_pointsNeeded` (L812/L817/L822)，但所有测量结束路径（onPointPicked 成功 L869 / ANGLE 失败 L851 / onRoiSelectedForMeasure 末尾 L894）**只切回 ROI InteractionMode，不复位 m_pointsNeeded=0**。
- **后果链**：用户完成一次 ANGLE 测量 → m_pointsNeeded=4 → 用户改选"面积"但不点执行测量直接拖 ROI → ImageView 发 roiSelected → setupConnections lambda（L344）`else if (m_pointsNeeded == 0)` 不成立 → 静默丢弃。光标已是箭头（误导用户"现在该拖"）但拖了没反应。
- **归属**：并入 **Fix 8（误拖反馈）** 同时修复。

### 补正 2：**`ImageView::clearOverlays()` 不分类清空**（潜在 P0 边界）
- **现象**：`ImageView::clearOverlays()`（ImageView.cpp:59-65）同时清 `m_detections`、`m_measurements`、`m_calibCorners`。`onMatchResult`（MainWindow.cpp:684）在每帧匹配完成时调用此函数，**会附带擦掉用户刚才的测量 overlay**。
- **后果**：相机持续采集 + 模板匹配开启时，用户测一次距离/角度，0.1 秒后被下一帧匹配的 clearOverlays 抹掉。Fix 1 修完渲染问题后，这个清空策略立刻成为新的 P0。
- **归属**：**Fix 1** 之外**额外加入 Fix 2**（分类 clear API），onMatchResult 改用 `clearDetectionOverlays()`。

### 补正 3：**`btnMeasure` 是局部 auto 变量，非成员**（影响 Fix 5 实施难度）
- **现象**：MainWindow.cpp:154 `auto *btnMeasure = new QPushButton("执行测量");`，未存为 `m_btnMeasure`。
- **影响**：Fix 5（按钮 checked 视觉态）需要外部 slot（onPointPicked / onRoiSelectedForMeasure / cancelMeasurement）能访问此按钮以 setChecked(false)。
- **决策**：把 btnMeasure 提升为成员 `m_btnMeasure`（MainWindow.h 新增 1 行 + setupControlDock 改 1 行）。这是 Fix 5 的前置改动，**不单列 Fix**，作为 Fix 5 的一部分。

---

## 1. 设计选择：Fix 5（下拉框 + 状态机）的方案对比

08 报告 P1 #2"下拉框选择不触发 slot"问题，**不能简单加 currentIndexChanged 了事**。设计选项：

### 方案 A — combo change 立即切换 InteractionMode
- 选了角度即刻把 ImageView 切到 POINT_PICK + pointsNeeded=4 + 光标十字
- 用户不需要点"执行测量"
- ❌ 用户可能只是浏览下拉框；中途切换会把 m_collectedPoints 清空导致误操作
- ❌ "执行测量"按钮成为冗余（仍要保留？语义模糊）

### 方案 B — combo change 只更新状态栏提示，激活仍点按钮（保守）
- combo 变更 → 只更新状态栏 `m_statusMeasure` 文字 "已选: 角度（按"执行测量"开始）"
- 按钮 setCheckable，按下进入测量状态 + checked，收齐输入/取消时 uncheck
- ✅ 不破坏现有按钮语义；不打断浏览
- ❌ 用户仍可能"选了类型直接画" — 但因 InteractionMode 默认 ROI，AREA/LENGTH/CIRCLE 拖 ROI 走上一次 type 的处理（这个隐患被补正 1 + Fix 8 误拖反馈共同覆盖）

### 方案 C — 显式三态机：IDLE / READY / ACTIVE
- IDLE：m_pointsNeeded=0, ROI 模式, 状态栏 "空闲"
- READY：用户已选类型未启动, 状态栏 "已选: <类型>（按执行测量开始）"
- ACTIVE：测量进行中, 状态栏 "测量中: <类型> (2/4 点)"
- 触发：combo → READY；执行测量 → ACTIVE；收齐输入/Esc → IDLE
- ✅ 状态语义最清晰
- ❌ 改动面大；需要枚举 + 多处状态转移函数

### **推荐方案：B（保守+视觉反馈）**
**理由**：
1. 与"执行测量"按钮的现有语义一致，最小破坏现有用户心智
2. 状态栏 + 按钮 checked 已足够覆盖 P0/P1 的视觉反馈缺失
3. 方案 C 的隐含状态在方案 B 里已通过"状态栏文字 + 按钮 checked 二值"达成
4. 实施复杂度低，回归风险小

**等用户审核**。如果用户希望走方案 C，本文 Fix 5 改写。

---

## 2. Fix 列表（10 个）

### Fix 1: `drawOverlays` 渲染 `m_measurements`
- **优先级**：P0（根因 fix；Bug 1 的 r.pt1/r.pt2 输出之所以白做的根因）
- **当前位置**：`src/ImageView.cpp:154-178`
- **当前行为**：
  ```cpp
  void ImageView::drawOverlays(QPainter &painter) {
      for (const auto &d : m_detections) { ... }
      if (m_selecting) { ... }
      painter.setPen(QPen(Qt::yellow, 2));
      for (const auto &pt : m_calibCorners) { ... }
      // ↑ m_measurements 完全未引用
  }
  ```
- **修复后**：在 drawOverlays 末尾增加 m_measurements 迭代，渲染十字 + label：
  ```cpp
  // Measurement overlays
  painter.setPen(QPen(QColor(255, 200, 0), 2));
  QFont labelFont = painter.font();
  labelFont.setPointSize(10);
  labelFont.setBold(true);
  painter.setFont(labelFont);
  for (const auto &m : m_measurements) {
      QPointF vp = imageToView(m.pos);
      // crosshair
      painter.drawLine(vp + QPointF(-6, 0), vp + QPointF(6, 0));
      painter.drawLine(vp + QPointF(0, -6), vp + QPointF(0, 6));
      // label (skip if empty so 2nd endpoint of distance/angle doesn't show duplicate)
      if (!m.label.isEmpty()) {
          painter.drawText(vp + QPointF(8, -8), m.label);
      }
  }
  ```
- **涉及其他文件**：无
- **编译影响**：ImageView.cpp 改动 ~14 行；ImageView.o 重编
- **依赖**：无
- **风险**：极低。drawOverlays 只是 paint，无状态变更
- **T1-T5 影响**：T1 视觉检测、T2/T3 标定不受影响；T4 稳定性测试不调测量；本 fix 让 T1 中测量功能首次"看得见"
- **估算**：10 min

---

### Fix 2: `clearOverlays` 分类拆分，`onMatchResult` 改用 `clearDetectionOverlays()`
- **优先级**：P0（补正 2，与 Fix 1 是一对，单独修 Fix 1 而不修 Fix 2 → 测量 overlay 被下一帧匹配立即抹掉）
- **当前位置**：
  - `include/ImageView.h:28` — `void clearOverlays();`
  - `src/ImageView.cpp:59-65` — 一次清三类
  - `src/MainWindow.cpp:684` — `m_imageView->clearOverlays();`
- **当前行为**：
  ```cpp
  void ImageView::clearOverlays() {
      m_detections.clear();
      m_measurements.clear();
      m_calibCorners.clear();
      update();
  }
  ```
- **修复后**：
  - `ImageView.h` 新增 3 个分类 API：`clearDetectionOverlays()` / `clearMeasurementOverlays()` / `clearCalibOverlays()`，保留 `clearOverlays()` 调用全部三个
  - `ImageView.cpp` 实现各分类清空
  - `MainWindow.cpp:684` 由 `clearOverlays()` 改为 `clearDetectionOverlays()`
  - `MainWindow.cpp:onPointPicked` 调 measure 之前调 `clearMeasurementOverlays()`（避免连续测量结果堆积；属于 08 报告 P0 第 2 条）
  - `MainWindow.cpp:onRoiSelectedForMeasure` 调 measure 之前同样调 `clearMeasurementOverlays()`
- **涉及其他文件**：`include/ImageView.h`、`src/ImageView.cpp`、`src/MainWindow.cpp`
- **编译影响**：3 个文件重编
- **依赖**：**必须先做 Fix 1**（否则 m_measurements 没人画也就没必要清）
- **风险**：低。clearOverlays 仍保留，但内部改为调用 3 个分类 API；老调用方语义不变
- **T1-T5 影响**：T1 视觉检测的清屏行为不变（onMatchResult 仍清掉旧检测框）；标定流程的 clearCalibOverlays 不变
- **估算**：15 min

---

### Fix 3: ANGLE 路径点击点 marker + 线段归属指示
- **优先级**：P1（08 报告断裂点 #4）
- **当前位置**：`src/MainWindow.cpp:828-871` `onPointPicked`
- **当前行为**：
  ```cpp
  void MainWindow::onPointPicked(const QPointF &p) {
      m_collectedPoints.append(p);
      appendLog(QString("点 %1: (%2, %3)")...);
      // 图像上无任何 marker
  }
  ```
- **修复后**：
  - 每次 onPointPicked append 一个新的 `ImageView::addClickedPointMarker(pos, color, label)`（**新增** ImageView API）
  - color 根据 ANGLE 路径下的归属：点 1-2 用青色 (0, 200, 255) → "L1A" / "L1B"；点 3-4 用品红 (255, 100, 200) → "L2A" / "L2B"
  - DISTANCE 路径 2 个点都用同色 → "P1" / "P2"
  - Log 文字也改为带语义："线 1 第 1 点: (...)"
  - 收齐输入触发 measure 之前调 `m_imageView->clearClickedPointMarkers()`（**新增**），避免点击 marker 与测量 marker 同框堆积
- **新增 ImageView API**：
  ```cpp
  // ImageView.h
  void addClickedPointMarker(const QPointF &pos, const QColor &color, const QString &label);
  void clearClickedPointMarkers();
  ```
  内部新增 `struct ClickedPoint { QPointF pos; QColor color; QString label; };` 容器 + drawOverlays 渲染
- **涉及其他文件**：`include/ImageView.h`、`src/ImageView.cpp`
- **编译影响**：3 文件
- **依赖**：必须先做 Fix 1（drawOverlays 改造范式确立），共享渲染习惯
- **风险**：中。引入新数据通路，但纯前端；测量逻辑不动
- **T1-T5 影响**：仅 T1 测量路径有视觉变化
- **估算**：25 min

---

### Fix 4: 状态栏新增 `m_statusMeasure` label（Fix 5/6 的基础设施）
- **优先级**：P2（08 报告断裂点 #8）
- **当前位置**：`src/MainWindow.cpp:setupStatusBar()` L302-317
- **当前行为**：5 个 label（检测/相机/机器人/License/FPS），无测量状态
- **修复后**：
  - `MainWindow.h` 新增 `QLabel *m_statusMeasure = nullptr;`
  - setupStatusBar 增加 `m_statusMeasure = new QLabel("测量: 空闲");` + `setMinimumWidth(180);` + `addPermanentWidget` 放在 m_statusDetection 之后
  - 新增 private helper `void updateMeasureStatus(const QString &text);`，仅做 m_statusMeasure->setText
- **涉及其他文件**：`include/MainWindow.h`
- **编译影响**：2 文件
- **依赖**：无（独立改动；Fix 5/6/7 引用此 helper）
- **风险**：极低
- **T1-T5 影响**：无（仅新增显示元素）
- **估算**：10 min

---

### Fix 5: 下拉框联动 + 执行测量按钮 checked 视觉态（方案 B）
- **优先级**：P1（08 报告断裂点 #2 + #9）
- **当前位置**：
  - `src/MainWindow.cpp:154` `auto *btnMeasure = new QPushButton("执行测量");`
  - `src/MainWindow.cpp:178-181` btnMeasure clicked lambda
  - `src/MainWindow.cpp:m_measureType` 无 currentIndexChanged 连接
- **当前行为**：
  - 下拉框变化 → 无 slot
  - 按钮无 checked、无禁用、无视觉态切换
- **修复后**：
  - `MainWindow.h` 新增 `QPushButton *m_btnMeasure = nullptr;`（补正 3）
  - setupControlDock L154 改为 `m_btnMeasure = new QPushButton("执行测量");` + setCheckable(true)
  - setupControlDock 新增 connect：
    ```cpp
    connect(m_measureType, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int idx) {
            // 仅在 IDLE（按钮非 checked）状态下更新提示；ACTIVE 时不让 combo 切换打断测量
            if (m_btnMeasure->isChecked()) {
                // 已有正在进行的测量；恢复 combo 选择避免静默改 type
                // （或者在 Fix 6 添加 cancelMeasurement 后改为：先 cancelMeasurement 再切换）
                return;
            }
            const QString name = m_measureType->itemText(idx);
            updateMeasureStatus(QString("测量: 已选 %1（按"执行测量"开始）").arg(name));
        });
    ```
  - btnMeasure clicked lambda 修改为：
    ```cpp
    connect(m_btnMeasure, &QPushButton::clicked, this, [this](bool checked) {
        if (checked) {
            int idx = m_measureType->currentIndex();
            onMeasure(static_cast<MeasureType>(idx));
        } else {
            // 用户主动取消（点已 checked 的按钮一次）
            cancelMeasurement();   // Fix 6 提供
        }
    });
    ```
  - onPointPicked 成功路径 L868-869 增加 `m_btnMeasure->setChecked(false);` + `m_pointsNeeded=0;`（补正 1）+ `updateMeasureStatus("测量: 空闲");`
  - onPointPicked ANGLE 失败路径 L851-853 同上（按钮 uncheck + 复位）
  - onRoiSelectedForMeasure 末尾 L894 同上
  - onMeasure 入口（L805 后）增加 `m_btnMeasure->setChecked(true);` + 状态栏 "测量中: <类型>（请...）"
- **设计选择标注**：按方案 B，combo 变化不切 InteractionMode；激活仍由按钮承担
- **涉及其他文件**：`include/MainWindow.h`
- **编译影响**：2 文件
- **依赖**：必须先做 **Fix 4**（updateMeasureStatus 由 Fix 4 提供）；推荐先做 **Fix 6**（cancelMeasurement helper 在 Fix 6 引入，否则按钮 uncheck 的取消路径要内联写）
- **风险**：中。改动 onMeasure / onPointPicked / onRoiSelectedForMeasure 三处共用 helper（按钮态 + 状态栏 + 复位），抽出 `enterMeasurement(type)` / `exitMeasurement()` 两个 private helper 更稳
- **T1-T5 影响**：仅 T1 测量交互流程
- **估算**：30 min

---

### Fix 6: Esc 取消测量
- **优先级**：P1（08 报告断裂点 #5）
- **当前位置**：MainWindow.cpp 无任何 QShortcut / keyPressEvent
- **当前行为**：进入 POINT_PICK 模式后无任何主动退出途径
- **修复后**：
  - `MainWindow.cpp` 顶部 include `<QShortcut>`
  - setupConnections 增加：
    ```cpp
    auto *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escShortcut, &QShortcut::activated, this, &MainWindow::cancelMeasurement);
    ```
  - `MainWindow.h` 新增 private slot `void cancelMeasurement();`
  - 实现：
    ```cpp
    void MainWindow::cancelMeasurement() {
        if (!m_btnMeasure->isChecked() && m_pointsNeeded == 0) return;
        m_collectedPoints.clear();
        m_pointsNeeded = 0;
        m_imageView->setInteractionMode(InteractionMode::ROI);
        m_imageView->clearClickedPointMarkers();   // 来自 Fix 3
        m_btnMeasure->setChecked(false);
        updateMeasureStatus("测量: 空闲");
        appendLog("测量已取消。");
    }
    ```
- **涉及其他文件**：`include/MainWindow.h`
- **编译影响**：2 文件
- **依赖**：**Fix 4**（updateMeasureStatus）+ 推荐 **Fix 3**（clearClickedPointMarkers）。如果先于 Fix 3 做，markers 那行可省略并留 TODO
- **风险**：低。Esc 与 close dialog 等其他默认行为不冲突（QShortcut 在 MainWindow 焦点链外冲突可控）
- **T1-T5 影响**：无（cancel 是新增能力）
- **估算**：15 min

---

### Fix 7: 状态栏测量进度文字（点 N/M）
- **优先级**：P2（08 报告 #8 续）
- **当前位置**：`src/MainWindow.cpp:onPointPicked` L831-833 仅写 Log
- **当前行为**：用户只能从 Log 框追进度
- **修复后**：onPointPicked append 后增加：
  ```cpp
  updateMeasureStatus(QString("测量中: %1 (%2/%3 点)")
      .arg(measureTypeName(m_pendingMeasureType))
      .arg(m_collectedPoints.size())
      .arg(m_pointsNeeded));
  ```
  新增 private helper `QString measureTypeName(MeasureType t)`（switch 5 个 case，无 default）
- **涉及其他文件**：`include/MainWindow.h`
- **编译影响**：2 文件
- **依赖**：**Fix 4**
- **风险**：极低
- **T1-T5 影响**：无
- **估算**：10 min

---

### Fix 8: 误拖 / pointsNeeded!=0 时的 roiSelected 反馈
- **优先级**：P1（08 报告断裂点 #3 + 补正 1）
- **当前位置**：`src/MainWindow.cpp:340-347`
- **当前行为**：
  ```cpp
  connect(m_imageView, &ImageView::roiSelected, this, [this](const QRectF &roi) {
      if (m_awaitingTemplateRoi) { ... }
      else if (m_pointsNeeded == 0) { onRoiSelectedForMeasure(roi); }
      // else: 静默丢弃
  });
  ```
- **修复后**：
  ```cpp
  connect(m_imageView, &ImageView::roiSelected, this, [this](const QRectF &roi) {
      if (m_awaitingTemplateRoi) { m_awaitingTemplateRoi = false; createTemplateWithRoi(roi); return; }
      if (m_pointsNeeded == 0) { onRoiSelectedForMeasure(roi); return; }
      // 此时正处于 POINT_PICK 模式，用户误拖了 ROI
      appendLog(QString("当前为点击模式（已收 %1/%2 点），请单击不要拖拽。按 Esc 取消。")
                .arg(m_collectedPoints.size()).arg(m_pointsNeeded));
      // 不切换模式 / 不丢失已点击点
  });
  ```
  另外补正 1：所有测量结束路径在 Fix 5 中已统一加 `m_pointsNeeded=0;`，本 Fix 不重复
- **涉及其他文件**：无（如果 Fix 5 已先做了 helper，则改 helper；否则只改 lambda）
- **编译影响**：1 文件
- **依赖**：**Fix 5**（统一 exitMeasurement helper 中保证 m_pointsNeeded=0 复位）
- **风险**：极低
- **T1-T5 影响**：T1 测量流程友好度
- **估算**：5 min

---

### Fix 9: ROI 单击/微拖时的反馈
- **优先级**：P2（08 报告断裂点 #6）
- **当前位置**：`src/ImageView.cpp:246-260` mouseReleaseEvent 左键路径
- **当前行为**：
  ```cpp
  if (m_selecting) {
      m_selecting = false;
      setCursor(Qt::ArrowCursor);
      QRectF roi = QRectF(m_roiStart, m_roiEnd).normalized();
      if (roi.width() > 10 && roi.height() > 10) {
          ...
          emit roiSelected(...);
          return;
      }
      update();   // 太小 → 什么也不发，沉默
  }
  if (m_interactionMode == InteractionMode::POINT_PICK) {
      emit pointPicked(...);
  }
  ```
- **修复后**：放宽 ROI 阈值条件 + 增加"太小"信号：
  ```cpp
  if (m_selecting) {
      m_selecting = false;
      setCursor(Qt::ArrowCursor);
      QRectF roi = QRectF(m_roiStart, m_roiEnd).normalized();
      // ROI 模式下：宽松发射，由 onRoiSelectedForMeasure 检查 5×5 像素图像最小值
      if (m_interactionMode == InteractionMode::ROI &&
          (roi.width() > 2 || roi.height() > 2)) {
          QPointF p1 = viewToImage(roi.topLeft());
          QPointF p2 = viewToImage(roi.bottomRight());
          emit roiSelected(QRectF(p1, p2));   // 即使小，让上层提示
          update();
          return;
      }
      // 大 ROI（>10×10）仍走原路径
      if (roi.width() > 10 && roi.height() > 10) {
          QPointF p1 = viewToImage(roi.topLeft());
          QPointF p2 = viewToImage(roi.bottomRight());
          emit roiSelected(QRectF(p1, p2));
          update();
          return;
      }
      update();
  }
  ```
- **可选替代**：保留 ImageView 行为，只在 onRoiSelectedForMeasure 现有的 5×5 检查上增加更友好的"请重新拖拽（最小 5×5）"提示。**优先选可选替代**（侵入最小）
- **涉及其他文件**：（替代方案下）无；（修改 ImageView 下）`src/ImageView.cpp`
- **编译影响**：1 文件
- **依赖**：无
- **风险**：低（替代方案下）/ 中（修改 ImageView 改动鼠标行为）
- **T1-T5 影响**：可能影响 T2 标定 ROI 拖拽（如改 ImageView） → **强烈推荐用替代方案**
- **估算**：5 min（替代方案）

---

### Fix 10: 统一失败反馈策略
- **优先级**：P2（08 报告断裂点 #7）
- **当前位置**：
  - `src/MainWindow.cpp:866` onPointPicked 失败：仅 `appendLog("测量失败: " + r.label);`
  - `src/MainWindow.cpp:891-892` onRoiSelectedForMeasure 失败：`appendLog` + `QMessageBox::warning`
- **修复后**：统一为"日志 + 状态栏闪烁 + 不弹窗"（弹窗打断 batch test 节奏；与 Bug 4 m_inBatchTest 兼容）：
  - onPointPicked 失败：保留 appendLog + `updateMeasureStatus(QString("测量: 失败 - %1").arg(r.label));` (5s 后 setTimer 复位为 "空闲")
  - onRoiSelectedForMeasure 失败：去掉 QMessageBox::warning，改为同上
  - **不去掉**：onPointPicked ANGLE 防御 L848-850 的 QMessageBox（这是 Bug 3.3 用户输入前置校验，性质不同；保留弹窗合理）
- **涉及其他文件**：可能新增 `QTimer::singleShot` 用法，include 已在
- **编译影响**：1 文件
- **依赖**：**Fix 4**
- **风险**：低。decision 待用户审 — 是否真去掉弹窗？测量失败弹窗对单次测量用户友好，但 batch test 模式会打断。如用户希望保留弹窗，则改为"统一两边都弹"也可
- **T1-T5 影响**：T4 稳定性测试不调测量，无影响；T1 体验有变化
- **估算**：10 min

---

## A. 完整汇总表

| Fix | 优先级 | 涉及文件 | 依赖 | 估算 |
|-----|--------|---------|------|------|
| 1. drawOverlays 渲染 m_measurements | P0 | ImageView.cpp | — | 10 min |
| 2. clearOverlays 分类拆分 | P0 | ImageView.h/cpp, MainWindow.cpp | Fix 1 | 15 min |
| 3. ANGLE 点击点 marker + 归属指示 | P1 | ImageView.h/cpp, MainWindow.cpp | Fix 1 | 25 min |
| 4. 状态栏 m_statusMeasure label | P2 | MainWindow.h/cpp | — | 10 min |
| 5. combo 联动 + 按钮 checked 视觉态 | P1 | MainWindow.h/cpp | Fix 4, 推荐 Fix 6 | 30 min |
| 6. Esc 取消测量 | P1 | MainWindow.h/cpp | Fix 4, 推荐 Fix 3 | 15 min |
| 7. 状态栏点 N/M 进度文字 | P2 | MainWindow.h/cpp | Fix 4 | 10 min |
| 8. 误拖 roiSelected 反馈 | P1 | MainWindow.cpp | Fix 5 | 5 min |
| 9. ROI 太小反馈（推荐替代方案：无改动） | P2 | MainWindow.cpp | — | 5 min |
| 10. 统一失败反馈 | P2 | MainWindow.cpp | Fix 4 | 10 min |

---

## B. 建议实施顺序

**不**按 P0→P1→P2 顺序，按依赖图拓扑排序：

```
Fix 1 (P0 drawOverlays)
  ↓
Fix 2 (P0 clearOverlays 分类)
  ↓
Fix 4 (P2 状态栏 label, 是 Fix 5/6/7/10 的基础设施)
  ↓
Fix 6 (P1 Esc cancelMeasurement, 是 Fix 5 取消路径的依赖)
  ↓
Fix 5 (P1 combo + 按钮 checked + helper exitMeasurement)
  ↓
Fix 3 (P1 ANGLE marker)   ← 与 Fix 8/9/10 可并列
Fix 8 (P1 误拖反馈)
Fix 7 (P2 点 N/M 文字)
Fix 9 (P2 ROI 太小 — 走替代方案)
Fix 10 (P2 统一失败反馈)
```

**执行序列**：1 → 2 → 4 → 6 → 5 → 3 → 8 → 7 → 9 → 10

**每个 Fix 后**：git diff → cmake 编译 → 等"继续 N+1"

---

## C. 总工时估算

| 阶段 | 工时 |
|------|------|
| 编码 | 10+15+25+10+30+15+10+5+5+10 = **135 min** ≈ 2h 15min |
| 编译验证（10 次 × ~30s Release 增量） | ~5 min |
| 计划/沟通 buffer（每 fix 间审 diff） | ~30 min |
| **总计** | **~3 小时** |

如用户希望批量验证（不逐 Fix 编译），编码工时可压到 ~2h 15min，但风险定位会变难。**仍建议保持逐 Fix gate**。

---

## D. 用户视角的前后对比

### 修复前（当前行为）
```
1. 用户：下拉框选"角度"
   系统：[无反应]
2. 用户：点"执行测量"
   系统：光标变十字；日志写"请点击四个点"
3. 用户：在图像上点第 1 点
   系统：日志写"点 1: (x, y)"；图像上无任何标记
4. 用户：点第 2 点
   系统：日志写"点 2"；图像无变化；用户不知道这是 line1 还是 line2
5. 用户：点第 3、4 点（继续）
6. 系统：计算完成；日志写"测量结果: 角度 = 45.32 °"
   ※ 图像上 addMeasurementOverlay 调用了，但 drawOverlays 没画 → 用户看不到
7. 用户：[反悔了]想取消？→ 没有 Esc，没有取消按钮，被迫走完
8. 用户：测完想再测一次面积，选"面积" → 下拉框选了但不点执行测量直接拖 → 静默丢弃
```

### 修复后
```
1. 用户：下拉框选"角度"
   系统：状态栏 → "测量: 已选 角度（按"执行测量"开始）"
2. 用户：点"执行测量"
   系统：按钮变为按下状态（checked）；
        光标变十字；
        日志写"请点击 4 个点（线 1 第 1 点 / 线 1 第 2 点 / 线 2 第 1 点 / 线 2 第 2 点）"；
        状态栏 → "测量中: 角度 (0/4 点)"
3. 用户：在图像上点第 1 点
   系统：图像上出现青色十字 + 文字 "L1A"；
        状态栏 → "测量中: 角度 (1/4 点)"；
        日志写"线 1 第 1 点: (x, y)"
4. 用户：点第 2 点
   系统：图像上出现青色十字 + "L1B"；状态栏 (2/4 点)
        用户清楚知道这两点定义了 line1
5. 用户：点第 3 点
   系统：图像上品红色十字 + "L2A"；状态栏 (3/4 点)
        颜色变化告诉用户"开始 line2 了"
6. 用户：点第 4 点
   系统：4 点 markers 清除；
        图像上出现黄色十字 + "角度 = 45.32 °" 文字；
        状态栏 → "测量: 空闲"；
        按钮恢复非按下状态；
        日志写结果
7. 用户中途想取消？→ 按 Esc → 状态全清，日志写"测量已取消"
8. 用户再测面积：选"面积" → 状态栏提示 → 点执行测量 → 拖 ROI → 测量结果
   如果不点执行测量直接拖：当前为 ROI 模式（pointsNeeded=0 已复位），走 onRoiSelectedForMeasure；
   或者上次 ANGLE 残留 pointsNeeded=4 → 显式日志 "当前为点击模式（已收 0/4 点），请单击不要拖拽。按 Esc 取消。"
```

**关键 UX 提升点**：
- 测量结果可见（修 Fix 1）
- 进度可见（修 Fix 4/7）
- 输入语义可见（修 Fix 3）
- 可取消（修 Fix 6）
- 误操作有反馈（修 Fix 8）
- 状态机清晰（修 Fix 5）

---

## 等用户审核的开放问题

1. **方案选择**：Fix 5 推荐方案 B（combo 不切交互模式，仅提示），用户是否同意？走方案 C 需重写 Fix 5
2. **Fix 9 实施面**：推荐走"无改动 ImageView，靠 onRoiSelectedForMeasure 提示"的替代方案。同意？
3. **Fix 10 弹窗策略**：是去掉 onRoiSelectedForMeasure 的 QMessageBox（统一只日志+状态栏）还是反过来给 onPointPicked 失败也加弹窗？前者更适合 batch test，后者更适合人工单次测量
4. **m_inBatchTest 关联**：Bug 4 即将引入 m_inBatchTest 标志。Fix 10 是否要让 batch 模式下完全静默？目前未在 Fix 10 内联，等用户决策
5. **顺序固化**：执行序列 1→2→4→6→5→3→8→7→9→10 是否照此走？还是希望先把 P0+P1 集中做完再看 P2

