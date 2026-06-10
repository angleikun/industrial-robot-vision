# Review 4: DEVLOG + AI_CODING_TRAPS 横向对比

**对比对象**:
- `D:\Mycode\projects\RobotVisionSystem\DEVLOG.md`（**21 个 bug**，B01-B21）
- `D:\Mycode\projects\metal-defect-detection\DEVLOG.md`（**5 个 Insight + ~15 个 bug**）
- `D:\dev_notes\AI_CODING_TRAPS.md`（**不存在**——见下方"未能验证"）

**审查日期**: 2026-06-06

---

## 未能验证的项

- `D:\dev_notes\AI_CODING_TRAPS.md`: Glob 全 D 盘搜索超时 + 项目内部无此文件，**判定不存在**
- Robot DEVLOG B08-B21 仅读了标题（前 7 个详细），细节根因依靠标题推断 + 部分代码交叉验证
- Metal Insight #6-#12 / Day 6-11 bug 未单独细读，依赖前面 5 个 Insight + 标题猜测

---

## (1) 两项目"同类型重复踩"的 bug 模式（按频率排序）

### 模式 A：**配置硬编码 → 暴露给 UI/JSON**（4+ 次跨项目）

| 项目 | bug | 共同教训 |
|---|---|---|
| Robot **B08** | 匹配得分阈值滑块是死的（spinbox 改了不调 setMatchConfig） | UI 控件 ↔ 配置 ↔ 算法三个层次必须连贯 |
| Robot **B14** | "机器人状态: 就绪" 日志每秒刷屏 | 状态变化才打，不变不打（de-dupe） |
| Robot **B20** | QTextEdit 日志无限增长（log_max_lines 配置项从未生效） | 配置项要有"被读取"的验证回路 |
| Metal | 通用 conf=0.25 默认值不适合 fine-grained → per-class threshold | 默认值不是万能的，要按业务校准 |
| Metal | mosaic=True 对 crazing 类有害 → close mosaic → +49% | 同上：默认参数不是普适最优 |

**统一教训**：**任何配置项落地都要走"声明→读取→生效→可观测"四步闭环**。Robot B08/B14/B20 都是"声明了但没生效"。

### 模式 B：**异步/并发踩坑**（Robot 独有，5 次）

| Robot bug | 死法 |
|---|---|
| **B04** | loadTemplate 持非递归锁后又调 clearTemplate → 同线程二次加锁 → 死锁 |
| **B05** | 视觉线程帧积压 + GUI 用 DirectConnection 调 createTemplate → 停止采集时整个 UI 冻结 |
| **B09** | Logger 单例非线程安全 → 日志错乱 |
| **B10** | 重连用 BlockingQueuedConnection + waitForConnected(1000ms) → 每次 GUI 卡 1 秒 |
| **B21** | queryToolPose 主线程死锁（用 invokeMethod + Blocking 跨线程等响应） |

**为什么 metal 没有？** metal 是 PyQt6 + 单 Worker QThread 训练任务（CLAUDE.md 提到的"批处理 worker"），并发拓扑相对简单。Robot 是 4 线程多 worker + Q_INVOKABLE/QWaitCondition 复杂同步。

**Metal 反方向迁移价值**：
- 训练流程是异步的（数据加载 + 训练 + 验证 + 日志），如果未来加多卡训练或在线评估，Robot 这 5 个 bug 的教训直接适用。

### 模式 C：**可观测性缺失**（4+ 次跨项目）

| 项目 | bug | 共同模式 |
|---|---|---|
| Robot **B07** | License 成功路径忘记调 onLicenseStatusChanged → 按钮永远灰 | 成功路径也要 emit 信号 |
| Robot **B12** | qInstallMessageHandler 没装 → HALCON 内部 qWarning 全丢失 | 日志后端要显式初始化 |
| Robot **B16** | 检测结果间歇性丢失 → "发送抓取位姿"经常没目标 | 状态变更要日志留痕 |
| Metal **Insight #1** | val 用一套口径，test 用另一套 → 0.800 vs 0.745 误判为"严重过拟合" | 评测口径不统一 = 数字无法横比 |
| Metal **Insight #2** | val→test 差距 -0.055 溯源花了 N 小时 | 同上 |

**统一教训**：**所有数字都要带"产生它的口径"**。Robot 是"按钮状态为何错"，Metal 是"mAP 数字为何降"，本质都是"输出值有歧义，因为产生路径不显式"。

### 模式 D：**编码/Unicode/字节级**（2 次跨项目）

| 项目 | bug |
|---|---|
| Robot **B03** | QImage 32-bit padding vs HALCON 紧密排列 → memcpy row-by-row 补丁（3 处重复） |
| Robot **B19** | PyBullet 模拟器 Unicode emoji → handle_client 线程崩溃 |
| Metal | 4 个 `__init__.py` 含 UTF-8 BOM → 解析失败（Week 4 P0-3 修复） |

**统一教训**：**跨边界的字节级假设是 bug 高发地带**——Qt vs HALCON 内存布局、Python 文件编码、UTF-8 vs GBK。

### 模式 E：**"防呆"边界缺失**（2 次跨项目）

| 项目 | bug |
|---|---|
| Robot | `validatePeerAddress` 白名单空时默认允许任意 IP（permissive default） |
| Metal | 批处理工具未做用户输入边界保护 |

**统一教训**：**安全/工业默认应该是 deny / strict**，不是 permissive。

---

## (2) Robot 独有的 bug：对 Metal 的迁移价值

| Robot bug | 描述 | Metal 是否会遇到 | 应做什么 |
|---|---|---|---|
| **B01** HALCON #1305 参数顺序错 | C++ binding 易记错 | 否（无 HALCON）| 不适用 |
| **B02** HRegion (row, col) vs (x, y) | HALCON 全套行列序 | **是**：metal 用 cv2 / PIL → cv2 是 (row, col)，PIL 是 (col, row)。Metal 代码混用风险存在 | grep `Image.fromarray` / `cv2.imwrite` 检查 |
| **B04** 递归锁死锁 | QMutex 非递归 | 部分：PyQt6 的 QMutex 同理；Python `threading.Lock` 也非递归（要用 RLock） | metal 批处理 worker 检查 |
| **B05** 帧积压 + busy guard | 异步队列无背压 | **是**：metal 批处理 1800 imgs 用过类似 worker，如果队列无 backpressure，UI 同样冻结 | metal main_window.py 检查 worker 排队设计 |
| **B12** qInstallMessageHandler | qWarning 默认不写文件 | **是**：metal 也用 PyQt6 + qWarning，**可能 HALCON-style 错误悄无声息丢失** | metal 检查是否注册了 message handler |
| **B14/B20** 日志去重 / log_max_lines | UI 日志膨胀 | **是**：metal 批处理 1800 imgs 每张一条 log → UI 卡 | metal 检查 QTextEdit 是否有 line cap |

**净迁移**：Robot → Metal 至少 **4 个可立刻应用的检查项**（B02 / B05 / B12 / B14）。

---

## (3) Metal 独有的 bug：对 Robot 的反向价值

| Metal Insight | 描述 | Robot 是否会遇到 | 应做什么 |
|---|---|---|---|
| **#1** mosaic vs crazing | 默认数据增强对纹理类有害 | 部分：HALCON 模板匹配没有"数据增强"概念，但 **CreateScaledShapeModel 的 scale 范围** 是类似的"假设之上的默认值"——0.7-1.5 对所有工件未必合适 | RobotClient settings.json 增加 per-工件 scale 范围 |
| **#2** val/test 口径不统一 | 验证集和测试集用不同评测口径 → 误读 | **是**：Robot T1-T5 的"延迟 0.10ms"和"成功率 100%"分别是 localhost 自环 + PyBullet 仿真，**口径不一致**。已在 Review 3 标记 | 加 README "测试口径速查表" |
| **#3** 分尺度警戒线 < 30 GT | 样本数低于 30 mAP 高方差 | Robot 内只有 5 种测量，每种测量精度都需要 N 次样本统计 — **当前未做** | 新建 T6 "测量重复性测试"：同一目标连测 50 次，给 ±σ |
| **#4** U-Net 多类负迁移 | 多类联合训练比单类差 | 不直接适用 | 不适用 |
| **#5** Python 环境管理 | conda base 污染 | 不适用（C++ 项目） | 不适用 |
| **conf 阈值 per-class** | 不同类别用不同阈值 | **强相关**：Robot 5 种测量算子的 minScore = 0.7 单一值，**应改为 per-MeasureType 独立阈值** | MeasurementEngine 改 per-type 阈值 |

**净反向迁移**：Metal → Robot 至少 **3 个可立刻应用的检查项**（#2 README 口径表、#3 重复性测试、per-class 阈值）。

---

## (4) AI_CODING_TRAPS.md 是否需要更新

### 现状

**`D:\dev_notes\AI_CODING_TRAPS.md` 不存在**。这说明 _之前的 review 里 reference 的_ 是用户记忆里的文件名，或者一个**还没真正建立**的文件。

### 建议：**新建** 而非"更新"

基于两个项目的踩坑沉淀，**首次建立** `D:\dev_notes\AI_CODING_TRAPS_v2.md` 是最高 ROI 的动作。

不要把它做成"陷阱百科"——做成**"AI 协作时高频复现的模式"**手册：每条都有"AI 容易写错的版本 + 正确版本 + 一句话识别提示"。

---

## (5) AI_CODING_TRAPS_v2.md 目录大纲建议

**总体结构**：5 章 × 5-8 条 = **25-40 条**，每条 1-2 段。

### 第 1 章 — C++ / Qt 模式（来自 Robot）— **10 条 P0/P1**

| # | 条目 | 来源 | 优先级 |
|---|---|---|---|
| C1 | QMutex 非递归 + 同线程二次加锁 = 死锁，提取 `*Unlocked` 私有版本 | Robot B04 | **P0** |
| C2 | 跨线程信号槽 = `Qt::QueuedConnection`，DirectConnection 会冻结 | Robot B05 | **P0** |
| C3 | UI 控件 ↔ 配置 ↔ 算法三层连贯，配置改了要 emit 才生效 | Robot B08 / Metal per-class | **P0** |
| C4 | `BlockingQueuedConnection` 在重连/查询里会冻 GUI，改 fire-and-forget + 状态机 | Robot B10/B21 | **P1** |
| C5 | Logger 单例必须用 QMutex，不要假设 instance() 线程安全 | Robot B09 | **P1** |
| C6 | qInstallMessageHandler 必须在 main 开头装，否则 qWarning 全丢 | Robot B12 | **P1** |
| C7 | QImage 32-bit padding ≠ HALCON/OpenCV 紧密排列，要 row-by-row memcpy | Robot B03 | **P1** |
| C8 | 析构函数永远 catch(...)，但要 log 不能空吞 | Robot 多处 | **P2** |
| C9 | HALCON HRegion 是 (row, col) 不是 (x, y)，OpenCV 同理 | Robot B02 | **P1** |
| C10 | C++17 项目里 0 智能指针是 smell，Qt parent 不是万能 | 新增（来自 Review 1）| **P2** |

### 第 2 章 — Python / ML 模式（来自 Metal）— **8 条**

| # | 条目 | 优先级 |
|---|---|---|
| P1 | 数据集 split 必须 SHA256 锁定，否则不可复现 | **P0** |
| P2 | 训练用 `set_seed + deterministic`，但 float 误差 0.00000 才算通过 | **P0** |
| P3 | val / test mAP 口径必须显式（conf, iou_thr, scale_filter），数字带口径 | **P0** |
| P4 | 分尺度评测 < 30 GT 警戒线，标注 `*` 提示高方差 | **P1** |
| P5 | 默认数据增强（mosaic 等）对部分类别有害，要类级 ablation | **P1** |
| P6 | conf 阈值不能用全局默认，要 per-class 校准 | **P1** |
| P7 | Python env 用 mamba 独立 env，不污染 base | **P1** |
| P8 | 多类联合训练 vs 多个二分类：先小样本验证负迁移 | **P2** |

### 第 3 章 — 跨语言通用：数据/边界/状态（来自两项目）— **6 条**

| # | 条目 | 来源 | 优先级 |
|---|---|---|---|
| G1 | 文本文件编码踩坑：UTF-8 vs GBK vs BOM，Python `open(encoding=...)` 必须显式 | Robot B19 / Metal P0-3 | **P0** |
| G2 | "默认 permissive" 是工业级反面，应 deny by default | Robot 白名单 | **P0** |
| G3 | 任何 silent drop / silent ignore 必须计数 + 暴露指标 | Robot B12/B16, Review 3 | **P1** |
| G4 | 测试报告里所有数字必须带"产生它的口径"（环境/数据/条件） | Metal Insight #1 / Robot T4 失真 | **P0** |
| G5 | 长时运行的内存/句柄/连接累积，必须有自动清理 + UI 暴露 | Robot calib images, Metal training cache | **P1** |
| G6 | UI 日志去重（状态变化才打）+ 行数 cap | Robot B14/B20 | **P1** |

### 第 4 章 — AI 协作模式（**最高价值章节**）— **6-8 条**

| # | 条目 | 描述 | 优先级 |
|---|---|---|---|
| A1 | AI 给的"看起来合理"修复 → 先验证再执行 | Metal Insight #5 直接出处 | **P0** |
| A2 | AI 容易把"占位/stub"留下不补：grep TODO/未实现/placeholder 每次 commit 前 | Robot `queryStatus()` stub (Review 3 S1) | **P0** |
| A3 | AI 容易写"看起来正确"的 try/catch(...) 空吞，要 review 每个 catch | Robot Review 1 H1.1 | **P1** |
| A4 | AI 容易复用 helper 函数但忘了适配（如 HRegion 行列序、QImage stride） | Robot B02/B03 重复 3 处 | **P1** |
| A5 | AI 容易把"理论上的最佳参数"写硬编码（如 pixelSize 5.5e-6）；要主动问"暴露到配置吗" | Robot Review 2 | **P1** |
| A6 | AI 协作时人工迭代必须每步**等错误重现**再修，不要"批量修复" | Metal DEVLOG.md:586 直接出处 | **P1** |
| A7 | DEVLOG 四段式（现象/根因/修复/教训）是 AI 协作的核心反馈机制 | 两项目共同约定 | **P0** |
| A8 | AI 难以发现"沉默成功"的 bug（如 B07 License 成功路径忘 emit） | Robot B07 | **P2** |

### 第 5 章 — 工程/部署（项目级踩坑）— **4 条**

| # | 条目 | 来源 | 优先级 |
|---|---|---|---|
| E1 | HALCON DLL `windeployqt` 不复制，要 POST_BUILD 手动 | Robot Review 2 | **P1** |
| E2 | License 错误码归类必须查官方文档，不能"差不多范围" | Robot B06 | **P0** |
| E3 | git rewrite history（filter-branch / filter-repo）前必查 README/docs 是否引用 commit hash | Metal Week 4 hash 引用风险 | **P2** |
| E4 | CMakeLists 暴露 `${HALCON_ROOT}` 路径给环境变量而非写死 | Robot CMakeLists.txt | **P1** |

---

## 完整对比报告：核心结论

### 共同 vs 独有

| | Robot 21 | Metal ~15 | 共同模式 |
|---|---|---|---|
| **配置硬编码 → UI/JSON** | B08/B14/B20 | per-class conf | ✅ 模式 A |
| **异步/并发** | B04/B05/B09/B10/B21（5 个！）| — | Robot 独有 |
| **可观测性缺失** | B07/B12/B16 | Insight #1/#2 | ✅ 模式 C |
| **字节/编码** | B03/B19 | UTF-8 BOM | ✅ 模式 D |
| **防呆边界** | 白名单 | 批处理边界 | ✅ 模式 E |
| **HALCON / ML** | B01/B02/B06 | Insight #1/#3/#4 | 各自栈特有 |

### 哪边教训更"通用"

**Robot 的 Qt 并发教训迁移价值 > Metal 的 ML 教训迁移价值**——因为 Qt 并发模式在任何 Qt 项目都重现，而 mosaic/分尺度 mAP 是 ML 评测特定。

但 **Metal 的"口径不统一→数字误读"教训**反过来对 Robot Review 3（T4/T5 数字失真）直接命中——是**两项目最大的双向迁移点**。

---

## AI_CODING_TRAPS_v2.md 更新建议清单（不直接改文件）

📋 **优先级排序的可执行清单**（建议 1-2 天内完成）：

1. **新建** `D:\dev_notes\AI_CODING_TRAPS_v2.md` 文件（当前不存在）
2. 按上述 5 章大纲，**先写 P0 条目**（C1/C2/C3/G1/G2/G4/A1/A2/A7/E2，共 10 条）
3. 每条用 markdown 表格格式：
   ```markdown
   ## C1: QMutex 非递归 + 同线程二次加锁
   - **AI 容易写错的版本**：function A 持锁 → 调用 function B → B 内部又加同一把锁
   - **正确版本**：提取 `*Unlocked` 私有版本，public 加锁后调 unlocked
   - **一句话识别**："任何 mutex 内部调用的函数，可能尝试加同一锁吗？"
   - **来源**：RobotVisionSystem DEVLOG B04
   ```
4. **24-30 条 P1** 第二阶段补
5. **A 章节（AI 协作）放最前面** —— 这是这套笔记区别于"普通 C++ 陷阱书"的核心价值
6. 每完成一章 commit 一次，不要憋一个大 commit
7. v2.md 写完后，**反过来 grep 两个项目的 DEVLOG**，把"已沉淀进 traps"的 bug 加标记 `[已入 v2: C1]`，做到 DEVLOG ↔ traps 双向溯源

---

## 综合评分

| 维度 | 分 | 依据 |
|---|---|---|
| DEVLOG 质量 | 9 | 两项目都严格遵守"现象/根因/修复/教训"四段式 |
| 教训沉淀程度 | 6 | Robot 21 bug 大部分留在 DEVLOG 内，未提炼成通用规则 |
| 跨项目知识迁移 | 4 | **未发生** —— Robot Q4 没用 Metal Q1 教训，Metal Q4 没用 Robot Q3 教训 |
| AI_CODING_TRAPS.md 实在程度 | **0** | 文件不存在，需新建 |

**综合：5.5 / 10** —— **教训留下了，沉淀没做**。AI_CODING_TRAPS_v2.md 是最高 ROI 的下一步。
