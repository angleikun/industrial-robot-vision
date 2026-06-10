# Review v2 顶层汇总

**审查范围**: RobotVisionSystem（C++/Qt/HALCON, 4,806 LOC）+ metal-defect-detection（横向对比）
**审查日期**: 2026-06-06
**视角**: 不重复前 3 轮 P0-P8 评测体系，从 5 个新角度补漏 + 沉淀

---

## 5 份独立报告

| # | 报告 | 综合评分 | 核心 3 句结论 |
|---|---|---|---|
| 1 | [`01_cpp_qt.md`](01_cpp_qt.md) | **7.0** | 1️⃣ Qt 4 线程模型清晰、`QMutex`/`QReadWriteLock`/`QWaitCondition` 三层锁正确使用；2️⃣ **C++17 项目 0 处智能指针**，全靠 Qt parent + 裸 new/delete，是"未充分使用语言能力"；3️⃣ MainWindow.h 通过 3 个 header 间接 include `HalconCpp.h`，**主入口被 HALCON 编译时间污染**。 |
| 2 | [`02_halcon.md`](02_halcon.md) | **6.5** | 1️⃣ HShapeModel + HCalibData 资源生命周期管理标准；License 错误码分类有踩坑经验；2️⃣ **5 种测量中 measureAngle 完全忽略用户选的两条线、measureArea 完全忽略 ROI + 阈值硬编码 128**，两个 demo 级算法 bug；3️⃣ 内参标定 pixelSize/focalLen/分辨率全部硬编码，工业级应来自 settings.json + UI 输入。 |
| 3 | [`03_industrial.md`](03_industrial.md) | **6.5** | 1️⃣ TCP 二进制帧 CRC-16/MODBUS 实现正确、DoS 保护 + 粘包/拆包到位；fire-and-forget 重连 + QWaitCondition 同步查询是高水准做法；2️⃣ **`queryStatus()` 是 stub**（直接 return STATUS_UNKNOWN），白名单空 = permissive default、CRC 失败静默吞；3️⃣ **T4 报告 FAIL 实际是测试脚本 bug**（把进程退出误读为句柄归零），T5 "0.10ms 延迟" 是 localhost 自环非真实工业网络。 |
| 4 | [`04_devlog_compare.md`](04_devlog_compare.md) | **5.5** | 1️⃣ Robot 21 bug + Metal 5 Insight 显示**两项目都严格遵守"现象/根因/修复/教训"四段式**；2️⃣ 跨项目共同模式有 5 类（配置硬编码→UI、可观测性缺失、字节/编码、防呆默认、并发陷阱），其中 Robot 5 个并发 bug 是 Qt 多线程项目的**通用迁移资产**；3️⃣ **`D:\dev_notes\AI_CODING_TRAPS.md` 不存在**，最高 ROI 动作是按 5 章大纲（C++/Python/通用/AI 协作/工程）新建 v2，先写 10 条 P0。 |
| 5 | [`05_resume.md`](05_resume.md) | **7.0** | 1️⃣ "Python ML 全链路 + C++ 工业全链路"双栈是稀缺组合，匹配视觉算法工程师 / 工业视觉算法 / C++ SDK 三类岗位；2️⃣ Metal README 自带 badges+截图+Key Metrics 表 HR 30 秒可懂，**Robot README 0 张 UI 截图、0 个 metric，HR 直接划走**；3️⃣ 投递前必修 3 件事：Robot README 加 UI 截图+metrics 表（半小时）/ 修 measureAngle+measureArea+queryStatus 三个 stub（半天）/ 新建 AI_CODING_TRAPS_v2.md P0 10 条（半天）。 |

---

## 跨报告的共性发现（在 ≥2 份报告里同时出现）

### 共性 1：**测试报告的"口径不显式"成为系统性弱点**
- Review 3 提到 T4 报告失真 + T5 "0.10ms" 是软件侧而非端到端
- Review 4 提到 Metal Insight #1 "评测口径不统一" 与 Robot T4/T5 是同一类问题（**最大双向迁移点**）
- Review 5 把它作为面试 **Top 5 杀伤力问题 Q1/Q2** 直接拎出来
- **🎯 跨报告动作**: 两项目都加"测试口径速查表"小节

### 共性 2：**stub / placeholder / 占位实现没补全**
- Review 1 提到 catch(...) 部分是合理 RAII，但与 stub 混杂
- Review 2 提到 measureAngle 忽略 line1/line2 参数、measureArea 忽略 ROI（**实质 stub**）
- Review 3 提到 `queryStatus()` 直接 return STATUS_UNKNOWN（**100% stub**）
- Review 4 提到 AI 协作模式 A2 "AI 容易留 stub 不补"
- Review 5 把它作为面试 Q3 杀伤力问题
- **🎯 跨报告动作**: `grep -rn "TODO\|占位\|placeholder\|未实现\|待实现"` 每次 commit 前必跑

### 共性 3：**沉默吃异常 / silent drop / 无计数**
- Review 1: `catch (...) {}` 4 处（部分合理，部分应该 log）
- Review 2: CalibrationManager.cpp:141-148 失败图像静默跳过
- Review 3: RobotClient CRC 失败静默 drop + 无统计；T4 报告无法检测
- Review 4: 模式 C "可观测性缺失" 4+ 次跨项目复现
- **🎯 跨报告动作**: 每个 silent drop 加一个 `std::atomic<uint64_t> m_dropCount` 暴露到 UI

### 共性 4：**硬编码参数应该 → settings.json + UI**
- Review 1: m_pixelEquivalentMm 默认 0.05 写死、CANNY 阈值 10/30 重复硬编码
- Review 2: 相机内参 startCamParam 8 个参数全部硬编码、Threshold 0/128 硬编码
- Review 4: 模式 A "配置硬编码 → 暴露给 UI/JSON" 4+ 次跨项目
- **🎯 跨报告动作**: 整理一份 "硬编码清单"（grep `const\s+\w+\s*=` 找候选），半天工作量逐个提到 settings.json

### 共性 5：**单元测试 0 覆盖**
- Review 1: CMakeLists.txt:106 `option(BUILD_TESTS "Build unit tests" OFF)` 默认 OFF
- Review 4: CLAUDE.md 明确"尚未配置测试框架"
- Metal 项目同样无 tests/ 目录（前一次 Week 4 review 已 flag）
- **🎯 跨报告动作**: Robot 先给 RobotFrame::serialize/deserialize + computeCrc16Modbus 加 5-10 个 Qt Test 单测（半天）—— 高 ROI 因为协议层逻辑最易隔离测试

---

## 给作者的"下一步行动"清单（按 ROI 排序，前 10 条）

### 🔥 P0 — 24 小时内完成（影响简历转化率）

1. **Robot README 加 4 张 UI 截图 + Key Metrics 表 + Badges**
   - 当前 0 张 UI 截图是 README 最大缺口
   - 30-60 分钟，ROI 极高（HR 一关通过率翻倍）

2. **`grep TODO|占位|未实现|placeholder` 扫一遍 Robot src/，列出 stub 清单**
   - 已知至少 3 处：`queryStatus()`、measureAngle 忽略参数、measureArea 忽略 ROI
   - 30 分钟摸清家底，决定哪些立刻修

### 🚧 P1 — 一周内完成（堵简历压力测试漏洞）

3. **修 `RobotClient::queryStatus()`**（半天）
   - 参考 queryToolPose 的 QWaitCondition 模式
   - 让 MainWindow 拿到真实 RobotStatus
   - 消除面试杀伤力问题之一

4. **修 `measureAngle` / `measureArea` 真实接收用户 ROI**（半天）
   - measureAngle: line1/line2 作为小窗口 ROI 做 EdgesSubPix + FitLineContourXld
   - measureArea: ReduceDomain(roi) + BinaryThreshold("max_separability")
   - 消除面试 Q3 杀伤力问题

5. **修 T4 测试脚本 + 重测 + 重写 T4 报告**（半天-1 天）
   - 进程退出前 30s 停止采样，避免"句柄归零"误读
   - 重新 8 小时长跑，得到真实波动数字
   - 消除面试 Q2 杀伤力问题

6. **新建 `D:\dev_notes\AI_CODING_TRAPS_v2.md` 第一版 10 条 P0**
   - 按 Review 4 第 5 章大纲，C 章 4 条 + P 章 3 条 + G 章 2 条 + A 章 1 条（A7）
   - 半天到 1 天
   - 面试 Q14 反过来变成 +5 分展示机会

### 🛠️ P2 — 两周内完成（提升整体工程化）

7. **Robot 5-10 个 Qt Test 单元测试**（半天-1 天）
   - 聚焦 `RobotFrame::serialize/deserialize` + `computeCrc16Modbus`
   - 不强求覆盖率，证明"知道单元测试该怎么写"

8. **抽 `Types.h` 解 HALCON 头传染**（半天）
   - 把 `MatchResult` / `MeasureType` / `RobotStatus` 提取到独立 header
   - MainWindow.h 不再 include HalconCpp.h
   - 主入口编译时间预计 -30% 起

9. **Robot README 加"测试口径速查表"小节**（30 分钟）
   - T1-T5 每个数字标注环境/方法/局限
   - 防御 Q1 类穿透问题

10. **CalibrationManager 失败图像最低成功率检查 + 硬编码相机参数提到 settings.json**（半天）
    - line 120-131 pixelSize/focalLen/分辨率 → settings.json `calibration.*`
    - line 141-148 累计 failedCount，超过 1/3 emit calibError

---

## 评分小结

| 维度 | 评分 | 当前位置 |
|---|---|---|
| **代码硬实力** | 7.0/10 | C++17 + Qt6 + HALCON 工业全栈，扎实 |
| **算法正确性** | 6.5/10 | 模板匹配/标定/坐标变换没问题，2 种测量是 stub |
| **工业稳定性** | 6.5/10 | 协议/重连/DB 设计成熟，但 stub + silent drop 拉低 |
| **教训沉淀** | 5.5/10 | DEVLOG 完整，AI_CODING_TRAPS 未建 |
| **简历可读性** | 5.0/10 | Robot README 缺截图和数字 |
| **跨项目知识迁移** | 4.0/10 | Robot↔Metal 教训未互用 |

**当前综合**: **6.5/10**
**完成 P0+P1（5 件事）后预期**: **8.0/10**
**完成 P0+P1+P2（10 件事）后预期**: **8.5/10**

---

## 写在最后

Robot 项目展示了**完整的 C++ 工业视觉项目能力 + AI 协作下的 21 个真实 bug 修复轨迹**——这本身就是稀缺资产。当前主要被两件事拖累：

1. **README 包装不足** —— HR 看不见硬实力
2. **几个 stub 没补全** —— 技术面试官立刻能穿透

**24 小时内补完 P0 两件事 + 一周内补完 P1 四件事**——投递时项目可信度从 65% → 85%，且每个面试问题都有自信回答。

> **不重复的体系视角下，Robot 项目的弱点不在"还能做什么新功能"，而在"已做的 95% 是否真的可信"。这一轮 review 不是评判，是排雷。**
