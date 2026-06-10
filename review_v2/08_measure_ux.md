# 测量功能 UX 流程审计

**审计范围**：用户从"想测一个角度"到"看到角度结果"的全部 UI 操作链
**审计模式**：纯静态代码分析（read-only），未启动 GUI
**关键文件**：
- `src/MainWindow.cpp` — 测量面板 setupControlDock(L149-181)、setupConnections(L338-347)、onMeasure(L795-826)、onPointPicked(L828-871)、onRoiSelectedForMeasure(L873-895)
- `src/ImageView.cpp` — setInteractionMode(L69-73)、mousePressEvent(L202-217)、mouseMoveEvent(L219-237)、mouseReleaseEvent(L239-267)、drawOverlays(L154-178)、addMeasurementOverlay(L44-51)
- `include/ImageView.h` — InteractionMode 枚举(L10)、m_interactionMode 默认 ROI(L66)、m_measurements 容器(L87)

---

## 1. 测量类型下拉框 → slot 追溯

### Q1.1 选了"角度"后，触发什么 slot？
**答：什么也不触发。** `m_measureType` 这个 QComboBox 在 setupControlDock(L152-153) 创建后，**没有 connect 任何 currentIndexChanged / currentTextChanged 信号**：

```cpp
// src/MainWindow.cpp:152-156
m_measureType = new QComboBox;
m_measureType->addItems({"长度", "圆直径", "距离", "角度", "面积"});
auto *btnMeasure = new QPushButton("执行测量");
measLayout->addWidget(new QLabel("测量类型:"));
measLayout->addWidget(m_measureType);
```

下拉框只是个"被读取的容器"，选择本身不改变任何应用状态。

### Q1.2 `m_pendingMeasureType` 何时被设置？
仅在 `onMeasure(MeasureType)` 入口（L805）：
```cpp
// src/MainWindow.cpp:795-805
void MainWindow::onMeasure(MeasureType type) {
    ...
    m_collectedPoints.clear();
    m_pendingMeasureType = type;   // ← 唯一赋值点
```
而 `onMeasure(type)` 只有一个调用方：执行测量按钮的 lambda（L178-181）：
```cpp
// src/MainWindow.cpp:178-181
connect(btnMeasure, &QPushButton::clicked, this, [this]() {
    int idx = m_measureType->currentIndex();
    onMeasure(static_cast<MeasureType>(idx));
});
```

**结论**：必须点"执行测量"按钮，下拉框的选择才生效。下拉框选了"角度"后用户若直接去图像上点击，会被当成上一次的 `m_pendingMeasureType`（默认 `MeasureType::LENGTH`，见 MainWindow.h:137）处理。

---

## 2. "执行测量"按钮 → slot

### Q2.1 触发什么？
匿名 lambda（L178-181，见上）。

### Q2.2 lambda 里做了什么？
读取 combo 的 currentIndex，强转为 MeasureType，调用 `onMeasure(type)`（L795-826）。

`onMeasure(type)` **不直接调用** `m_measureEng->measure()`，它只做：
1. 校验降级模式 + 图像不为空（L797-802）
2. 清空 `m_collectedPoints`，写入 `m_pendingMeasureType`
3. **根据 type 切换 ImageView 的交互模式 + 设置 `m_pointsNeeded`**（L807-825）：

| 类型 | InteractionMode | pointsNeeded | 日志提示 |
|------|-----------------|--------------|----------|
| LENGTH / CIRCLE / AREA | **ROI** | 0 | "请在图像上拖拽选择测量区域..." |
| DISTANCE | POINT_PICK | 2 | "请在图像上依次点击两个点..." |
| ANGLE | POINT_PICK | 4 | "请在图像上依次点击四个点（两条直线）..." |

4. 然后 `onMeasure()` 返回。**`measure()` 在此函数中不被调用**。

### Q2.3 那 line1/line2/roi 从哪来？
- **POINT_PICK 路径**：用户在图像上点击 → ImageView 发出 pointPicked 信号 → `onPointPicked(p)` 累积 `m_collectedPoints` → 累积到 `m_pointsNeeded` 个时，调用 `m_measureEng->measure(img, m_pendingMeasureType, m_collectedPoints, QRectF())`（L858-859）。ANGLE 路径中 4 个点在 dispatch 时再拆成 `points.mid(0,2)` + `points.mid(2,2)` 作为 line1/line2。
- **ROI 路径**：用户在图像上左键拖拽 → ImageView 发出 roiSelected(QRectF) → 走 lambda 分流（L340-347）→ 当 `!m_awaitingTemplateRoi && m_pointsNeeded==0` 时调用 `onRoiSelectedForMeasure(roi)` → 在此调用 `m_measureEng->measure(img, m_pendingMeasureType, {}, roi)`（L885）。

---

## 3. 用户怎么"画线"/"拖 ROI"

### Q3.1 信号—槽链
| 用户动作 | ImageView 路径 | 发射的信号 | 接收槽 |
|----------|----------------|------------|--------|
| 左键拖拽（drag 矩形）>10×10 px | mousePressEvent(L211-216) → mouseMoveEvent(L230-233) → mouseReleaseEvent(L246-258) | `roiSelected(QRectF)` | 匿名 lambda(L340-347) |
| 左键单击或微拖（≤10×10 px）<br>且 `m_interactionMode==POINT_PICK` | mouseReleaseEvent(L262-265) | `pointPicked(QPointF)` | `onPointPicked(QPointF)` |
| 右键拖拽 | mousePressEvent(L206-210) → mouseMoveEvent(L221-228) → mouseReleaseEvent(L241-244) | （无）— 仅做 pan | — |
| 滚轮 | wheelEvent(L269-275) | `zoomChanged` | — |

### Q3.2 触发条件
**关键事实：左键拖拽永远会 emit roiSelected**，不分模式（L246-258 的判定是 width>10 && height>10，与 interactionMode 无关）。

只有"左键 release 时如果没有有效 ROI" 才走 L262 的 if 检查 interactionMode：
```cpp
// src/ImageView.cpp:262-266
if (m_interactionMode == InteractionMode::POINT_PICK) {
    QPointF imgPos = viewToImage(event->position());
    emit pointPicked(imgPos);
}
```

**断裂点 1**：在 DISTANCE / ANGLE 模式下，用户哪怕只是稍微拖了几个像素（>10×10 即超）就会发 roiSelected。而 setupConnections lambda 里只有 `m_pointsNeeded==0` 时才调用 onRoiSelectedForMeasure：
```cpp
// src/MainWindow.cpp:340-347
connect(m_imageView, &ImageView::roiSelected, this, [this](const QRectF &roi) {
    if (m_awaitingTemplateRoi) {
        m_awaitingTemplateRoi = false;
        createTemplateWithRoi(roi);
    } else if (m_pointsNeeded == 0) {
        onRoiSelectedForMeasure(roi);
    }
    // else: 静默丢弃 ← DISTANCE/ANGLE 模式下的误拖会落到这里
});
```
此时该次点击既不发 pointPicked（因为前面 return 了，L257），也不被作为 ROI 使用 → **用户看到光标十字、点了一下，但 Log 没动**。

**断裂点 2**：在 ROI 模式（LENGTH/CIRCLE/AREA）下用户单击（不拖）→ release 时 ROI 太小（width<=10 或 height<=10）不发 roiSelected，且 interactionMode==ROI 不发 pointPicked → **完全沉默**。

### Q3.3 InteractionMode 由谁切换？
仅 4 处 setInteractionMode 调用：
- `onMeasure()` 入口（L811、L816、L821）— 根据类型设置 ROI 或 POINT_PICK
- `onPointPicked()` 累积完成后（L852、L868）— 复位回 ROI
- `onRoiSelectedForMeasure()` 末尾（L894）— 复位回 ROI

**没有任何"取消测量"或"重置"入口**。用户开始 ANGLE 测量后若中途反悔，没有按钮能退出 POINT_PICK 模式 — 必须收齐 4 个点（其中一段距离可能 <10 px 触发警告并清空）或重新点"执行测量"切换类型。

---

## 4. 视觉提示

| 提示通道 | 实际表现 | 是否足够 |
|----------|----------|----------|
| **鼠标光标** | POINT_PICK 模式：十字（CrossCursor）；ROI 模式：箭头（ArrowCursor）（L72） | 部分有效 — 光标变化太微妙，且未区分"等点击 vs 等拖拽" |
| **状态栏** | 5 个 QLabel：相机/机器人/License/FPS/检测（L304-310）— **无任何"当前测量状态"** | 不足 — 用户无法从状态栏知道流程到哪步 |
| **按钮高亮** | "执行测量"按钮点击后无 checked 状态、无变色、无禁用其他冲突按钮 | 不足 |
| **图像 overlay 指示线** | drawOverlays(L154-178) 只画：① 已有检测框；② 进行中的 ROI 选框（L167-170）；③ 标定角点。**已点击的点没有任何 marker 显示在图上** | **严重不足** — 用户点了 3 个点不知道第 3 点落在哪、不知道前两点构成第 1 条线 |
| **日志框** | onMeasure 发起提示（L813/L818/L823），onPointPicked 每次回显坐标（L831-833） | 唯一的明确流程信号，但日志是被动 review 信息，用户视线在图像上时容易漏看 |
| **进度对话框 / 状态计数** | 无 | 不足 — "点击四个点"过程中无 1/4、2/4、3/4、4/4 的视觉计数 |

**断裂点 3**：用户在 ANGLE 模式下点 4 个点，**没有任何视觉反馈区分"这 2 个点是 line1"和"那 2 个点是 line2"**。前 2 个点已用于 line1，第 3 个点开始算 line2 — 用户看图时不知道，且无 overlay 显示已点击位置。

---

## 5. 测量结果的呈现

### Q5.1 各通道
| 通道 | 实现位置 | 实际行为 |
|------|----------|----------|
| **日志框** | onPointPicked L861-862 / onRoiSelectedForMeasure L887-888 | ✅ 正常 — 写入 "测量结果: %1 = %2 %3" |
| **图像 overlay** | onPointPicked L863-864 / onRoiSelectedForMeasure L889 调用 `m_imageView->addMeasurementOverlay()` | ❌ **画不出来 — 见断裂点 4** |
| **状态栏** | — | 不更新 |
| **弹窗** | 失败路径：onRoiSelectedForMeasure L892 `QMessageBox::warning(..., r.label)`；onPointPicked **失败路径只写 Log 不弹窗**（L866） | 部分不一致 — 同样的失败两条入口反馈程度不同 |

### Q5.2 ★★★ 断裂点 4（严重 P0）: addMeasurementOverlay 收集数据但永不绘制
- `ImageView::addMeasurementOverlay(x, y, label)` 把数据塞入私有 `m_measurements`（QList<MeasurementItem>），src/ImageView.cpp:44-51。
- `paintEvent` 调用 `drawOverlays(painter)`，src/ImageView.cpp:154-178，**该函数体内对 `m_measurements` 零引用**：

```cpp
// src/ImageView.cpp:154-178 — drawOverlays 全文
void ImageView::drawOverlays(QPainter &painter) {
    for (const auto &d : m_detections) { ... }      // 画检测框
    if (m_selecting) { ... }                         // 画进行中 ROI
    painter.setPen(QPen(Qt::yellow, 2));
    for (const auto &pt : m_calibCorners) { ... }    // 画标定角点
}
```

**用户在图像上看到的测量结果数：0**。`addMeasurementOverlay` 这个函数从生效角度看是死代码 — MainWindow 调它，append 了，update() 触发重绘，但下一次 paintEvent → drawOverlays 跳过 m_measurements。

---

## 测量功能 UX 流程图（按 ANGLE 路径走一遍）

```
┌──────────────────────────────────────────────────────────────────┐
│ 步骤 1. 用户在"测量类型"下拉框选"角度"                          │
│   ▶ 实际：无任何 slot 触发                                       │
│   ▶ m_pendingMeasureType 不变（保持上次值或默认 LENGTH）         │
│   ▶ 视觉反馈：仅下拉框文字变化                                   │
│ 【断裂点：选择行为本身不暗示"还需点'执行测量'"】                 │
└──────────────────────────────────────────────────────────────────┘
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 步骤 2. 用户点"执行测量"按钮                                     │
│   ▶ lambda 读 combo → onMeasure(ANGLE)                          │
│   ▶ ImageView 切到 POINT_PICK 模式，光标→十字                   │
│   ▶ pointsNeeded=4，collectedPoints 清空                        │
│   ▶ 日志："请在图像上依次点击四个点（两条直线）..."             │
│ 【UX 不足：状态栏不变、按钮无 checked 态、图像无指示线】        │
└──────────────────────────────────────────────────────────────────┘
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 步骤 3. 用户在图像左键单击第 1 点                                │
│   ▶ 如果"单击"实际是微拖 ≤10×10 px：                            │
│       mouseRelease L246-260 ROI 太小不发 roiSelected             │
│       → 落到 L262 检查 mode==POINT_PICK → 发 pointPicked ✓      │
│   ▶ 如果用户手抖拖了 >10 px：                                   │
│       mouseRelease 发 roiSelected → lambda 因 pointsNeeded!=0    │
│       静默丢弃，且 return 不发 pointPicked                       │
│   ▶ 成功路径：onPointPicked 累积，日志写 "点 1: (x, y)"         │
│ 【断裂点：图像上看不到"刚才点了哪里"的 marker】                  │
└──────────────────────────────────────────────────────────────────┘
                              ▼
              ... 重复 3 次直到累积 4 点 ...
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 步骤 4. 收齐 4 点 → onPointPicked 触发计算                       │
│   ▶ ANGLE 防御：检查 (0,1) 段长 & (2,3) 段长 ≥10 px              │
│     不满足 → MessageBox 警告 + 清空点 + 回 ROI 模式               │
│   ▶ 调用 m_measureEng->measure(img, ANGLE, points, QRectF())     │
│   ▶ dispatcher 内部 points.mid(0,2)/mid(2,2) → measureAngle      │
│   ▶ 返回 MeasureResult r                                         │
└──────────────────────────────────────────────────────────────────┘
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 步骤 5. 结果显示                                                 │
│   ✓ 日志：写 "测量结果: <label> = <value> ° "                    │
│   ✗ 图像 overlay：addMeasurementOverlay 调了但 drawOverlays 不画 │
│   ✗ 状态栏：不更新                                              │
│   ✗ 弹窗：成功不弹                                              │
│   失败路径只写 Log（onPointPicked），但 ROI 路径会弹窗 — 不一致  │
│   ▶ ImageView 切回 ROI 模式，光标→箭头                          │
│   ▶ m_collectedPoints 清空                                       │
└──────────────────────────────────────────────────────────────────┘
```

---

## 断裂点优先级汇总

| # | 断裂 | 位置 | 严重度 | 用户感受 |
|---|------|------|--------|----------|
| **1** | **`addMeasurementOverlay` 永不绘制** — `drawOverlays` 不迭代 `m_measurements` | ImageView.cpp:154-178 | **P0** | 测量后图像上完全没有标记，必须看 Log 才知结果 |
| **2** | 下拉框选择不联动 — 必须点"执行测量"才生效 | MainWindow.cpp:152-181 缺少 currentIndexChanged 连接 | P1 | 选了类型直接去画，得到的是上一次类型的结果 |
| **3** | DISTANCE/ANGLE 模式下手抖拖拽 >10 px → 静默丢弃 | MainWindow.cpp:340-347 lambda 在 pointsNeeded!=0 时 else 分支无日志 | P1 | 点了图像但 Log 不动，疑似系统冻结 |
| **4** | ANGLE 路径无线段归属指示 — 4 个点哪 2 个是 line1 用户不知道 | ImageView 无 marker；MainWindow 无"点 N/4 line A"日志 | P1 | 测错了不知道为啥 |
| **5** | 无"取消测量"入口 — POINT_PICK 模式无法主动退出 | 无 ESC / Cancel button / setInteractionMode 调用方 | P1 | 误点了执行测量就被迫走完 |
| **6** | ROI 模式下单击（不拖）完全沉默 | ImageView.cpp:246-266 — width<=10 不发 roiSelected，mode==ROI 也不发 pointPicked | P2 | 不致命，但增加困惑 |
| **7** | onPointPicked 失败路径只写 Log，onRoiSelectedForMeasure 失败路径弹窗 — 反馈不一致 | MainWindow.cpp:866 vs L892 | P2 | 同样的失败两次入口提示强度不同 |
| **8** | 状态栏无测量状态指示 — 用户从状态栏无法判断当前是否在测量、点了几个 | MainWindow.cpp:302-317 | P2 | 视线在图像上时容易忘了流程在哪 |
| **9** | "执行测量"按钮无 checked 态、不禁用冲突按钮（如再次点击会重新进入 onMeasure 清空已收集的点） | MainWindow.cpp:178-181 | P2 | 误操作会丢失进度 |

---

## 修复建议（按 P0→P2 排序，**仅建议，未实施**）

**P0 必修**：
1. `ImageView::drawOverlays` 内补充对 `m_measurements` 的迭代渲染：在结果坐标处画十字 + 文字 label。预计改动 5-10 行。
2. 在 `onPointPicked`/`onRoiSelectedForMeasure` 调用 measure 前后增加 `ImageView::clearOverlays()` 或单独清理 m_measurements，否则连续测多次会堆积。

**P1 应修**：
3. 给 `m_measureType` 加 currentIndexChanged 连接，自动切换 InteractionMode 并提示用户。同步在 setupControlDock 中给"执行测量"按钮加 setCheckable + 视觉态。
4. setupConnections lambda 中加 else 分支：当 `pointsNeeded != 0` 且收到 roiSelected 时，写 Log 提示 "当前是点击模式，请单击不要拖拽"。
5. ANGLE 路径下每次 onPointPicked 在 Log 中区分 "线 1 第 1 点 / 线 1 第 2 点 / 线 2 第 1 点 / 线 2 第 2 点"；在 ImageView overlay 中画已点击的 dot。
6. 添加 Esc 快捷键调用一个新 cancelMeasurement() — 清空 collectedPoints + 切回 ROI 模式 + Log 提示。

**P2 优化**：
7. 状态栏新增 m_statusMeasure label：空闲 / "测量中(2/4 点)" / "等待拖拽 ROI"。
8. 统一失败反馈：要么都 Log + 弹窗，要么都只 Log。
9. 图像右上角加测量模式 HUD 文字。

---

## 静态结论

**测量功能"能跑通"但"用户用不明白"。**
按现有代码，从想测角度到看到角度数值这条 happy path 在 Log 里能完成。但：
- 用户在图像上看不到任何测量结果可视化（P0 #1）
- 多个"用户认为有反应、系统认为没反应"的灰区（P1 #2、#3、#5）
- 4 点输入的语义对用户完全是黑盒（P1 #4）

**优先级评分**：UX 完整度 **3/10**（功能可用，但需要熟悉源码的人才能正确操作）。
