# Bug 1 measureAngle HALCON #1405 诊断报告

**报告状态**：纯诊断，未动代码
**测试现场**：用户在静态相机帧上点了 4 个点（667,320 / 746,318 / 658,359 / 761,359）→ 进入 measureAngle 后异常 HALCON #1405

---

## 1. 错误码核对

从 HALCON 24.11 扩展包文档 `extension_package_programmers_manual_0153.html`：

| 错误码 | 符号 | 含义 |
|--------|------|------|
| 1401–1499 | H_ERR_WIPN1..N99 | Wrong number of values of **control parameter N**（N = 错误码 − 1400） |
| **1405** | **H_ERR_WIPN5** | **Wrong number of values of control parameter 5** |

"WIPN" = Wrong Input Parameter Number（**数量**错，不是类型错也不是值范围错）。

---

## 2. 候选算子 × 第 5 控制参数定位

`measureAngle` 内部按调用顺序，5 个算子（src/MeasurementEngine.cpp:278-306）：

| # | 算子 | 控制参数数量 | 第 5 个是？ | 可能抛 1405? |
|---|------|--------------|-------------|--------------|
| 1 | `EdgesSubPix(reduced, &edges, "canny", 0.5, 10, 30)` | 4 | — | ❌ 不到 5 个 |
| 2 | `SelectShapeXld(edges, &selected, "contlength", "and", 10, 99999)` | 4 | — | ❌ 不到 5 个 |
| 3 | `CountObj(selected, &cnt)` | 0 | — | ❌ 无 |
| 4 | **`FitLineContourXld(selected, "tukey", -1, 0, 5, 2, ...)`** | **5** | **ClippingFactor (= 2)** | ✅ 可能 |
| 5 | **`AngleLl(R1s, C1s, R1e, C1e, R2s, C2s, R2e, C2e, &angle)`** | **8** | **RowB1 = R2s** | ✅ 可能 |

候选缩到 **FitLineContourXld** 和 **AngleLl** 两个。

---

## 3. 假设 A / B / C 验证

### 假设 A：HTuple 字面量包装问题（"标量未包装成 HTuple"）
**证据反驳**：代码已显式 `HalconCpp::HTuple(2)`、`HalconCpp::HTuple(-1)` 等。HTuple 构造单参数 = 1 元素值。Length() == 1。**排除**。

### 假设 B：算子签名版本差异
**证据**：HALCON 24.11 官方文档 `fit_line_contour_xld.html` 签名：
```
fit_line_contour_xld(Contours :: Algorithm, MaxNumPoints, ClippingEndPoints, Iterations, ClippingFactor :: RowBegin, ColBegin, RowEnd, ColEnd, Nr, Nc, Dist)
```
与代码调用顺序一致（"tukey", -1, 0, 5, 2）。**排除**签名差异。

### 假设 C：SelectShapeXld 返回 N>1 contour，导致下游 tuple 元素数不一致
**证据支持**：
- 用户点的 4 个点构成一个 ~80px 宽、~40px 高的窗口（line1）和一个 ~100px 宽、0px 高的窗口（line2）
- 加 20px padding 后的 ROI 是 ~120×80 和 ~140×40 像素的边缘检测区域
- EdgesSubPix（"canny", α=0.5, low=10, high=30）在工业相机帧上**典型产生多条边**：用户瞄准的线 + 平行噪声边 + 转角处碎片边
- SelectShapeXld 过滤 `contlength ∈ [10, 99999]` 不严格，多个"长边"都会留下
- `CountObj` 检查只防 0 contour，不防 N>1
- 所以两个 ROI 各产生 M、N 个 contour 是大概率事件

**关键来自 HALCON 24.11 `angle_ll.html` 文档**：

> "This operator supports parameter broadcasting. This means that each parameter can be given as a tuple of length **1 or N**. Parameters with tuple length **1** will be repeated internally such that the number of computed angles is always **N**."

即 AngleLl 控制参数必须 size 全为 1 或 全为 N（或部分 1 部分 N，N 必须一致）。否则错。

**FitLineContourXld 的行为**：输入 contour 集合 N 个，输出 Rs/Cs/Re/Ce 均为 N 元素 tuple（每 contour 一组拟合结果）。

**所以最可能的失败链是**：
```
line1 ROI → SelectShapeXld → 假设 M 个 contour
                              ↓
                              FitLineContourXld
                              ↓
                              R1s, C1s, R1e, C1e: 长度 M（M ≥ 2）

line2 ROI → SelectShapeXld → 假设 N 个 contour（N ≠ M 且 N ≠ 1）
                              ↓
                              R2s, C2s, R2e, C2e: 长度 N

AngleLl(R1s={M}, C1s={M}, R1e={M}, C1e={M},   ← 建立"全局元素数 = M"
        R2s={N}, ...)                          ← 在 param 5 RowB1 处发现 N ≠ M 且 N ≠ 1
                                                ↓
                                                抛 H_ERR_WIPN5 (1405)
```

**结论**：**最可能的根因是 SelectShapeXld 未把 contour 收敛到 1 个，导致下游 AngleLl 在 param 5 (R2s) 上检测到 tuple 元素数与前 4 个 param (R1s/C1s/R1e/C1e) 不匹配**。

**次要可能性**：FitLineContourXld 自身在 N≥2 contour 输入下因某些极端情况（例如某个 contour 太短拟合失败）抛 1405。但 HALCON 文档明确该算子对多 contour 输入的处理是"每 contour 一组输出"，不应该因 count 报错。

---

## 4. 修复方案（最小改动版本）

### 方案 1（推荐）：fitLineInRoi 内补一步"选最长 contour"
在 SelectShapeXld 之后、CountObj 之后、FitLineContourXld 之前，加 ~6-8 行：

```cpp
// After: SelectShapeXld(...) + CountObj(...)

// If more than one contour survived, keep only the longest one.
// EdgesSubPix in a noisy ROI typically returns several long edges;
// fitting all of them and feeding the multi-element tuples to AngleLl
// downstream triggers HALCON error 1405 on control parameter 5.
HalconCpp::HObject longest = selected;
if (cnt[0].I() > 1) {
    HalconCpp::HTuple lens;
    HalconCpp::LengthXld(selected, &lens);
    int maxIdx = 0;
    double maxLen = lens[0].D();
    for (int i = 1; i < lens.Length(); ++i) {
        if (lens[i].D() > maxLen) { maxLen = lens[i].D(); maxIdx = i; }
    }
    HalconCpp::SelectObj(selected, &longest, maxIdx + 1);   // HALCON 用 1-based 索引
}

HalconCpp::HTuple Nr, Nc, Dist;
HalconCpp::FitLineContourXld(longest, ...);   // 改 selected → longest
```

**优点**：
- 改动仅在 `fitLineInRoi` lambda 内部，不影响 dispatcher / API
- 保证 FitLineContourXld 输入恰好 1 contour，输出 Rs/Cs/Re/Ce 均为 length 1
- AngleLl 收到 8 个 length-1 tuple，必然工作正常
- 同时也修复了"用户瞄准了线但 ROI 内还有其他强边"的语义问题（选最长的最接近用户意图）

**风险**：
- 如果用户瞄准的不是最长边，会拟合错误。但用户用 20px padding ROI 已经把"用户的线"作为视觉焦点框出来了，最长边几乎总是用户的目标
- LengthXld 是 HALCON 标准 XLD 算子，无 license / 性能问题

### 方案 2（更鲁棒但改动大）：先用 SelectShapeXld 选最长，跳过手动 indexing
HALCON 没有内置 "select longest XLD" 算子，需要用 LengthXld + SelectObj 组合。本质上和方案 1 等价，但能抽成 helper。**不建议为单次调用抽 helper**。

### 方案 3（最简但语义弱）：在 AngleLl 之前对 R2s 等取 [0]
```cpp
HalconCpp::AngleLl(R1s.TupleSelect(0), C1s.TupleSelect(0),
                    R1e.TupleSelect(0), C1e.TupleSelect(0),
                    R2s.TupleSelect(0), C2s.TupleSelect(0),
                    R2e.TupleSelect(0), C2e.TupleSelect(0),
                    &hv_Angle);
```
**反对**：这只是把 N 个 contour 中"任意第一个"的拟合用作答案 — 第一个可能不是用户意图的线。**与方案 1 比鲁棒性差，不推荐**。

---

## 5. 推荐方案 1 的修复后预期行为

### 路径 A：单 contour（最常见情况）
- SelectShapeXld 返回 1 个 contour（高对比度图像）
- `cnt[0].I() == 1`，跳过 if 分支，`longest = selected`
- 行为与修复前等价
- ✅ 无回归

### 路径 B：多 contour（本次失败案例）
- SelectShapeXld 返回 N ≥ 2 个 contour
- LengthXld 获取每个长度
- 手动循环找最大长度索引
- SelectObj 取出最长的那个，`longest` 仅含 1 个 contour
- FitLineContourXld 输出 length-1 tuple
- AngleLl 收到 length-1 输入 → 计算 1 个角度 → 成功 ✅

### 路径 C：0 contour
- 已有 `if (cnt[0].I() == 0) throw` 拦截
- 行为不变 ✅

---

## 6. 修复后用户视觉效果

- 4 点点击后立即在图上看到 2 条青色线段（用户原始点击连线，方案 B overlay）+ 在 line1 起点附近的青色角度数值标签（如 "角度 = 2.31 °"）
- 不再弹出"测量失败: HALCON error #1405..." 日志条
- 一次测量后，下一帧匹配抹掉旧 detection 但不抹测量 overlay（Fix 1 clearDetectionOverlays 已生效）

---

## 7. 不应该做的事

- **不要**改 Bug 1 的核心算法骨架（ReduceDomain + EdgesSubPix + FitLineContourXld + AngleLl 不变）
- **不要**给 MeasureResult 加新字段（不影响 ABI、不影响其他 Bug）
- **不要**为 ClippingFactor 改 HTuple 包装（已确认不是问题）
- **不要**降低 SelectShapeXld 的 contlength 下限（会让噪声碎片混进来，更糟）

---

## 8. 不确定性

1. **不能 100% 排除 FitLineContourXld 是抛错算子（而非 AngleLl）**。但方案 1 同时覆盖两种情况（输入恒为 1 contour 后两个算子都不会因 count 错失败）。
2. **不能 100% 确定"最长 contour 一定是用户意图"**。但 20px padding 的小窗口里，最长边通常就是用户瞄准的目标线；若发现误识别，下一步迭代可改为"最长 + 方向最接近用户两点连线"双权重选择。

---

## 等用户确认

**Q**：是否照"方案 1：fitLineInRoi 内补选最长 contour"修？

如确认，请说"按方案 1 修 Bug 1"，我会改 `src/MeasurementEngine.cpp::fitLineInRoi` 内部 ~10 行代码并重新编译。
