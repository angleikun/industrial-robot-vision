# Fix 2 范围讨论（实施前讨论文档，未动任何代码）

> **命名映射说明**：本"Fix 2" 指执行序列 `1 → 3 → 2 → 5 → 6 → 4` 中的第三步，
> 即 `09_ux_fix_plan.md` 中关于 **combo 联动 + 执行测量按钮 checked 视觉态**
> 的修复（原 09 文档编号为 Fix 5）。
>
> `09_ux_fix_plan.md` 原 Fix 2（clearOverlays 分类拆分）在 Fix 1 实施期已合并完成
> （`clearDetectionOverlays` / `clearMeasurementOverlays` / `clearCalibrationOverlays` /
> `clearAllOverlays` 四个 API 已上线，`onMatchResult` 已改用 `clearDetectionOverlays`，
> `applyMeasurementOverlay` 进入前先调 `clearMeasurementOverlays` 实现"只显示最新一次"）。

**约束**：本文档纯讨论，不写代码。等用户拍板范围后再开工。

---

## 1. 原方案 B（09 文档 Fix 5）在 Fix 3 之后还合理吗？

### 原方案 B 内容回顾

```
- combo.currentIndexChanged → 更新状态栏 m_statusMeasure 文字
  "测量: 已选 角度（按"执行测量"开始）"
- m_btnMeasure 提升为成员 + setCheckable(true)
- 按钮 clicked → 若 checked 进入 onMeasure；若再次点 → cancelMeasurement
- onMeasure 入口：setChecked(true) + 状态栏 "测量中: 角度（请...）"
- 各成功/失败/取消路径：setChecked(false) + 状态栏 "测量: 空闲"
- combo 切换在 ACTIVE 状态下被忽略（return 早退）
```

### Fix 3 之后哪些部分仍有必要？

| 子项 | Fix 3 是否覆盖 | 仍需 Fix 2 处理？ | 理由 |
|------|----------------|---------------------|------|
| combo 选了类型但无反馈 | ❌ 没碰 combo | **仍需** | 用户选了下拉框 → 没任何 UI 变化 → 不知道是否记下；marker 此时还没开始画 |
| 按钮按了无 checked 视觉态 | ❌ 没碰按钮 | **仍需** | 用户点了"执行测量"后按钮立刻弹回 → 不知道现在是否在"测量中" |
| 测量进行中 N/M 点进度文字 | ✅ marker 已表达 | **不需要** | marker 颜色 + label（线1点1 / 线2点1）已经把进度可视化；statusBar 重复 |
| 测量启动前提示该按按钮 | ❌ 没碰 | **仍需**（如果做） | "选了角度，按执行测量开始" 类 hint 在 marker 出现前唯一能给的反馈 |
| 测量结果反馈 | ✅ Fix 1 + Hotfix 1 已覆盖 | 不需要 | overlay + log + 弹窗（失败时）都到位 |
| 取消测量 | ❌ Esc 留 Fix 6 | Fix 2 暂不处理 | 按钮 checked 状态的"取消路径"如果先于 Fix 6 做，需要内联取消逻辑或留 TODO |

**结论**：方案 B 的"combo 联动"和"按钮 checked 视觉态"两块**仍然有价值**——Fix 3 的 marker 解决"测量进行中"的反馈，方案 B 解决的是**"测量启动前/启动时"的反馈**，时段不重叠。

方案 B 的"statusBar 进度文字"和"测量中具体内容"那一层**已被 marker 抢光语义**，可以从 Fix 2 切除。

---

## 2. marker 已覆盖进度后，还需要 statusBar 文字提示吗？

按时段切分：

| 时段 | marker 是否有效 | statusBar 是否冗余 |
|------|------------------|---------------------|
| 用户改 combo（启动前） | ❌ 不存在 marker | **不冗余** → 此时 statusBar 或类似通道是唯一反馈渠道 |
| 用户点"执行测量"瞬间 | ❌ 还没开始点 | **不冗余** → 需要某处告诉用户"现在该点击/拖拽了" |
| 用户在点击/拖拽中 | ✅ marker 在画 | **冗余** → "N/M 点"的统计与 marker 个数 1:1 重复 |
| 收齐输入 → 计算中（瞬时） | — | 通常不需要专门反馈 |
| 测量成功 | ✅ overlay + log | **冗余** |
| 测量失败 | — | log + 弹窗已够（Hotfix 1）|

→ **statusBar 文字提示只在"启动前"和"按按钮瞬间"两个时段必要**；
"测量中 N/M 点"该砍掉（原 09 文档 Fix 7 完全可以放弃）。

---

## 3. 下拉框联动的真实目的是什么？

**真实目的 = 消除"沉默操作"困惑**。

用户的具体心智路径：
```
1. 用户看到 combo 下拉，选了"角度"
2. 用户期望某种反馈：图变？光标变？按钮高亮？状态文字？
3. 当前实现：全部都没有
4. 用户：「我选了吗？要不要再点一下？」「是不是要按那个'执行测量'按钮？」
```

→ 联动的核心**不是切交互模式**（方案 A 会做的，过激），而是
**告诉用户"你的选择已经被记下，下一步该做什么"**。

这等价于给用户一个"contract acknowledgment"——告诉他"系统看到了"。

---

## 4. 最简单的实施方式

按"提示通道"挑：

### 方案 α：**改按钮文字承担提示**（最轻量，0 新增 widget）

```
combo.currentIndexChanged → m_btnMeasure->setText("执行 <类型> 测量")
m_btnMeasure setCheckable(true)
m_btnMeasure clicked(checked=true) → onMeasure(type)
onMeasure 入口：保持 checked（QPushButton 已自动 checked）
成功/失败结束路径：setChecked(false) → 按钮自动恢复非按下视觉态
```

**优点**：
- 不依赖 09 文档 Fix 4 的 m_statusMeasure 基础设施
- 按钮文字本身在 combo 旁边，用户视线已经在这——反馈通道就近
- 按钮的 checked 视觉态（QPushButton 自带按下/凸起）天然覆盖"测量中/空闲"二态
- 改动量：MainWindow.h +1 行（成员），setupControlDock ~3 行，setupConnections ~5 行，onMeasure/onPointPicked/onRoiSelectedForMeasure 各 1 行 setChecked。**合计 ~12-15 行**

**缺点**：
- 按钮文字会随类型变化（一种"动态文字"，可能不符合一些设计规范的"按钮静态语义"）
- 按钮无空闲文字（如"未选类型"），首次启动 combo 默认在某 index，文字立刻是 "执行长度测量"——可接受
- 取消路径（用户再点 checked 按钮）：方案 α 暂不实现 cancel，setCheckable 让按钮"按一下激活、再按弹回"的视觉自然存在，但 lambda 必须处理 checked==false 的情况——可以"再点 = cancel"，cancel 实现留到 Fix 6 时统一接管

### 方案 β：**新增 m_statusMeasure label**（即 09 文档 Fix 4 提前）

```
setupStatusBar 加 QLabel *m_statusMeasure = new QLabel("测量: 空闲");
m_btnMeasure 成员化 + setCheckable
combo.currentIndexChanged → m_statusMeasure->setText("测量: 已选 <类型>（按"执行测量"开始）")
onMeasure → setChecked(true) + m_statusMeasure->setText("测量: 进行中 <类型>")
结束路径 → setChecked(false) + m_statusMeasure->setText("测量: 空闲")
```

**优点**：
- statusBar 是"系统态"的传统通道，语义干净
- 按钮文字不变，UI 一致性好
- 顺手把 09 文档 Fix 4 做了，后续 Fix 6（Esc 取消）也能直接复用

**缺点**：
- 改动面比 α 大：MainWindow.h +1 label 成员 +1 button 成员 + helper；setupStatusBar 改；setupConnections 改；3 个结束路径改
- 用户视线常停在 combo + 按钮（控制 dock 右侧）；statusBar 在窗口最底——视线距离更远，反馈感知略弱
- 改动 ~30 行（与原 09 文档 Fix 5 估算 30 min 一致）

### 方案 γ：**仅按钮 checked 视觉态，combo 改用 log 提示**（最保守）

```
combo.currentIndexChanged → appendLog("已选: <类型>。按"执行测量"开始。")
m_btnMeasure setCheckable + 成员化
按钮按下 checked → onMeasure
结束路径 → setChecked(false)
```

**优点**：
- 不动 statusBar，不动按钮文字
- log 是既有渠道，0 新增 widget

**缺点**：
- log 是历史流，combo 反复改 → 多行 log 重复，淹没其他信息
- 按钮 checked 状态外不提示当前已选类型——但 combo 自己显示，多此一举？
- combo 反复改不 throttle 会刷屏

---

## 5. 推荐与对比矩阵

| 维度 | 方案 α 改按钮文字 | 方案 β statusBar | 方案 γ log 提示 |
|------|---------------------|---------------------|---------------------|
| 实施量 | **~12 行 (最小)** | ~30 行 | ~10 行 |
| 反馈视觉距离 | **就近 (combo 旁)** | 远 (窗口底部) | 中 (log 框) |
| 是否依赖 Fix 4 | ❌ | **是（顺带做）** | ❌ |
| 是否污染按钮 default text | **是** | ❌ | ❌ |
| log 反复刷屏风险 | ❌ | ❌ | **有** |
| 与未来 Fix 6 / Fix 7 复用度 | 低（无 statusBar 基础设施）| **高** | 低 |

### 我的推荐

**方案 α（改按钮文字）**——单纯按"最简单的实施方式"看就是它。
但如果用户希望同时为 Fix 4/6/7 铺路（statusBar 是后续三个 Fix 公用基础设施），
则推荐 **方案 β**。

α 与 β 的真正区别是：**只做 Fix 2，还是把 Fix 4 一并做了**。
γ 不推荐。

---

## 6. Fix 2 暂不处理的（明确划出）

- ❌ "测量中 N/M 点" 进度文字（原 09 文档 Fix 7）：marker 已覆盖，**整个 Fix 7 可以删除**
- ❌ Esc 取消（Fix 6）：本 Fix 2 不实现 cancelMeasurement；按钮 checked==false 的回调先简单 setChecked(false)，cancel 真正逻辑留 Fix 6
- ❌ `m_pointsNeeded` 测量结束复位（09 文档补正 1）：留 Fix 5 / Fix 6 一起处理，本 Fix 不动
- ❌ 误拖反馈（原 09 文档 Fix 8）：依赖 m_pointsNeeded 复位，留待后续

---

## 7. 等用户拍板

请回答以下 3 个问题，我据此确定 Fix 2 最终 scope：

**Q1（关键）**：方案 **α / β / γ** 选哪个？
- α = 改按钮文字（~12 行，最小，但按钮文字动态）
- β = 新增 m_statusMeasure（~30 行，顺手做 Fix 4 基础设施）
- γ = log 提示（不推荐）

**Q2**：是否同意"Fix 2 不再做测量中 N/M 进度文字（marker 已覆盖）"，即**完全砍掉原 09 文档 Fix 7**？

**Q3**：方案 α/β 中，按钮 checked==false 的回调（用户再点一次 checked 的按钮）这次怎么处理？
- (a) **不响应**（setChecked 的 toggle 视觉变化由 Qt 自动给，但 cancelMeasurement 逻辑留 Fix 6）→ 这次只把按钮 setChecked(true/false) 接进 onMeasure 入口和结束路径，clicked(checked) 只在 checked==true 时进 onMeasure，checked==false 时啥都不做（用户可能误以为"我刚才取消了！"——但实际 m_collectedPoints 还在）
- (b) **本 Fix 2 内联实现 cancel**：clicked(checked==false) → m_collectedPoints.clear() + setInteractionMode(ROI) + clearClickedPointMarkers() + appendLog("测量已取消。")（Fix 6 后续再加 Esc 入口）
- (c) **按钮强制设 toggle 但禁止用户点 checked 的按钮取消**（disable checked-while-active）→ Qt 不直接支持，需要 lambda 里强制 setChecked(true) 回去——很 hacky

我推荐 **(b)**：本 Fix 2 内联实现 cancel（5-6 行），Fix 6 时把 Esc 也接到同一个 cancelMeasurement helper 上，避免本 Fix 留 TODO。
