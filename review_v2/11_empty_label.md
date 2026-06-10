# Hotfix 1 测量失败弹窗内容为空 诊断报告

> **命名修正**：本修复 = **Hotfix 1**（Fix 3-6 之前的紧急修复）。
> 之前实施计划中的 "Bug 4" = T4 silent mode，与本 bug 无关，未开始修。

**报告状态**：纯诊断，未动代码
**现象**：日志显示 `测量失败:` 后跟空白；同时 QMessageBox 弹出但 informative text 区为空（仅图标 + OK）
**复现时间戳**：`21:18:08.587`、`21:19:08.888`（两次 ROI 拖拽测量）

---

## 0. 修复前确认（动代码前必做）

### Q1：三个 catch 块当前是否设置了 `r.valid = false`？

| 函数 | catch 块位置 | 当前 catch 内容 | 设了 r.valid=false？ |
|------|--------------|-----------------|----------------------|
| `measureLength`   | MeasurementEngine.cpp:150-152 | 仅 `qWarning(...)` | **❌ 没设** |
| `measureCircle`   | MeasurementEngine.cpp:201-203 | 仅 `qWarning(...)` | **❌ 没设** |
| `measureDistance` | MeasurementEngine.cpp:230-232 | 仅 `qWarning(...)` | **❌ 没设** |

→ 三处全部仅 qWarning。

### Q2：`MeasureResult.valid` 字段默认值？

`include/MeasurementEngine.h:17-27`：
```cpp
struct MeasureResult {
    bool    valid = false;          // ← 已显式 = false（C++11 in-class initializer）
    MeasureType type;
    double  value    = 0.0;
    ...
};
```

→ **valid 字段已显式默认初始化为 false**，不是未定义行为。即 catch 块不显式设 r.valid，返回时 valid 仍然 == false（功能上不会假成功）。

### 判定属于情况 A 还是情况 B

按用户给的判定标准：
- 情况 A：catch 块**已**设 r.valid=false **且** struct 已默认初始化 → 当前**不满足**（catch 没设）
- 情况 B：catch 块**未**设 r.valid=false **或** struct 未默认初始化 → 当前**满足**（"或"的左支：catch 未设）

→ **判定为情况 B**。

### 情况 B 下 Hotfix 1 的实际改动

- 三处 catch 块各加 2 行：`r.valid = false;` + `r.label = QString("XX测量失败: ") + e.ErrorMessage().Text();`
- struct 定义**不需要改**（valid 已 = false，改了反而引入无关 churn）
- 共 **6 行新增**、0 行删除、0 行 struct 修改

**为什么 struct 已 = false 还要写 r.valid = false**：
1. 与 `measureAngle:335` / `measureArea:413` 既有写法完全对称（自文档化）
2. 防御未来 struct 默认值被改时本处行为发生静默漂移
3. 不增加运行时成本（编译器会折叠掉重复赋值）

---

---

## 1. 入口路径定位

QMessageBox 弹窗的唯一源头在 `MainWindow.cpp`：

| 入口 | 文件:行 | 失败时行为 |
|------|---------|------------|
| `onRoiSelectedForMeasure` | MainWindow.cpp:917-918 | `appendLog("测量失败: " + r.label)` + **`QMessageBox::warning(this, "测量失败", r.label)`** |
| `onPointPicked` | MainWindow.cpp:892 | 仅 `appendLog("测量失败: " + r.label)`，**不弹窗** |

→ **空白 QMessageBox 必然来自 ROI 触发路径**，即 `onRoiSelectedForMeasure`。

ROI 触发路径走的测量类型：**LENGTH、CIRCLE、AREA**（dispatcher 中 ROI 字段为非空的入口）。
点位触发路径走的测量类型：**DISTANCE、ANGLE**（不弹窗，只写日志）。

---

## 2. r.label 在各失败路径上的赋值情况

逐个 measure 函数审计 `catch (HalconCpp::HException &e)` 块：

| 函数 | catch 行 | catch 块行为 | r.label 在失败时 | QMessageBox 内容 |
|------|----------|--------------|-------------------|-------------------|
| `measureLength`  | MeasurementEngine.cpp:150-152 | **只 `qWarning`** | **空字符串** ❌ | **空白** ❌ |
| `measureCircle`  | MeasurementEngine.cpp:201-203 | **只 `qWarning`** | **空字符串** ❌ | **空白** ❌ |
| `measureDistance`| MeasurementEngine.cpp:230-232 | **只 `qWarning`** | **空字符串** ❌ | （不走弹窗路径） |
| `measureAngle`   | MeasurementEngine.cpp:335-336 | `r.label = "角度测量失败: " + e.ErrorMessage()` | 有内容 ✅ | （不走弹窗路径） |
| `measureArea`    | MeasurementEngine.cpp:411-414 | `r.label = "面积测量失败: " + e.ErrorMessage()` | 有内容 ✅ | 有内容 ✅ |

### 结构默认值

`MeasureResult` 在头文件 `MeasurementEngine.h:17-27`：

```cpp
struct MeasureResult {
    bool    valid = false;
    MeasureType type;
    double  value    = 0.0;
    QString unit;
    QPointF pt1, pt2;
    QPointF center;
    double  radius  = 0.0;
    double  area    = 0.0;
    QString label;        // ← 默认构造 = 空 QString
};
```

→ catch 块若不显式赋值 `r.label`，返回时 `r.label.isEmpty() == true`。

---

## 3. 根因结论（结合现象）

**用户 21:18 / 21:19 两次空白弹窗 = `measureLength` 或 `measureCircle` 在 ROI 内抛 HException，catch 块没设 r.label**。

LENGTH/CIRCLE 的 HALCON pipeline：
```
ReduceDomain → EdgesSubPix → SelectShapeXld(contlength) → FitLineContourXld / FitCircleContourXld
```

抛错最常见的具体场景：
1. **SelectShapeXld 过滤后 contour 集合为空** → FitLineContourXld / FitCircleContourXld 输入空 XLD 集合 → HALCON 抛 `H_ERR_EMPTYREGION` 或类似码
2. **ROI 越出图像边界** → ReduceDomain 抛错
3. **ROI 内是纯背景**（无边缘）→ EdgesSubPix 输出空，下游全空
4. **FitCircleContourXld 在 contour 不构成圆时拒绝拟合** → 抛错
5. **measureLength 在 contour 长度 < 阈值 50 时**（注意 LENGTH 用了 50，比 CIRCLE 的 30、ANGLE 的 10 严格）→ 更容易触发空集

---

## 4. 假设 A / B / C / D 验证

### 假设 A：dispatcher 中 ANGLE/AREA 早返回时 label 已设但其他类型早返回未设
**反驳**：dispatcher 早返回路径全部都设了 label：
- DISTANCE 点数不足（line 75）："需要选择两个点"
- ANGLE 点数不足（line 85）："需要先在图像上点选两条线..."
- AREA ROI 空（line 95）："需要先选择 ROI 区域"

LENGTH/CIRCLE 在 dispatcher 没有早返回（直接进 measureLength/measureCircle）。

dispatcher 路径**不是**空 label 的源。**排除**。

### 假设 B：ImageView::currentImage() 返回空 QImage
**反驳**：`onRoiSelectedForMeasure:908-909` 已有保护：
```cpp
QImage img = m_imageView->currentImage();
if (img.isNull()) return;
```
直接 return，**不会走到 QMessageBox**。**排除**。

注意：`onPointPicked:884` 没有这个保护——但点位路径本来就不弹窗，不影响本 bug。

### 假设 C：Bug 2 的 measureArea catch 块设的 label 是空字符串
**反驳**：`measureArea:413-414` 明确设：
```cpp
r.label = QString("面积测量失败: ") + e.ErrorMessage().Text();
```
即使 `e.ErrorMessage()` 极端情况下返回空 Text，也至少有 `"面积测量失败: "` 前缀。**排除**（除非 Bug 2 在 ROI 越界这种 ReduceDomain 失败时根本走不到这条 catch——但 HException 是基类，所有 HALCON 错都会被 catch 到）。

### 假设 D：用户拖了一个边界 ROI（比如部分越出图像）
**支持**：是 §3 中场景 2 的子集——HException 在 ReduceDomain 处抛，进 measureLength/measureCircle 的 catch，**不设 label** → 空白弹窗。

但即便 ROI 完全合规，§3 的场景 1/3/4/5 也能复现。

---

## 5. 修复方案

### 方案 A（推荐）：在三个 catch 块统一设默认错误消息

修改点：`MeasurementEngine.cpp:150-152 / 201-203 / 230-232`，参考 angle/area 已有的模式。

```cpp
// measureLength
} catch (HalconCpp::HException &e) {
    qWarning() << "measureLength failed:" << e.ErrorMessage().Text();
    r.valid = false;
    r.label = QString("长度测量失败: ") + e.ErrorMessage().Text();
}

// measureCircle
} catch (HalconCpp::HException &e) {
    qWarning() << "measureCircle failed:" << e.ErrorMessage().Text();
    r.valid = false;
    r.label = QString("圆直径测量失败: ") + e.ErrorMessage().Text();
}

// measureDistance
} catch (HalconCpp::HException &e) {
    qWarning() << "measureDistance failed:" << e.ErrorMessage().Text();
    r.valid = false;
    r.label = QString("距离测量失败: ") + e.ErrorMessage().Text();
}
```

**优点**：
- 与 `measureAngle` / `measureArea` 完全对称，统一所有 measure 函数的 catch 契约
- 同时修复 `appendLog("测量失败: " + r.label)` 那条日志（不再是裸冒号）
- 同时为 DISTANCE 路径补上空 label（虽然 DISTANCE 不弹窗，但日志同样会"测量失败: "+空 = 信息缺失）
- 0 行新增、不改 try 体、不改 dispatcher、不改 MainWindow

**风险**：
- 显式 `r.valid = false` 是冗余的（默认值就是 false），但与 angle/area 既有写法对齐，保持一致性更重要

### 方案 B（次选）：在 QMessageBox 显示前防御性兜底

修改点：`MainWindow.cpp:917-918`：
```cpp
} else {
    const QString msg = r.label.isEmpty() ? "测量失败（详情见调试输出）" : r.label;
    appendLog("测量失败: " + msg);
    QMessageBox::warning(this, "测量失败", msg);
}
```

**缺点**：
- 把空 label 当合法状态接受，掩盖了 engine 侧契约不一致
- DISTANCE 路径的 appendLog 仍然空（line 892 没改）
- 用户看到 "详情见调试输出" 但调试输出在 qWarning 不在日志框，**用户实际看不到**

**不推荐**。

### 方案 A + B 组合
也可以同时做（方案 A 修根因，方案 B 防御后续新 measure 函数的同类疏忽）。但当前 5 个 measure 函数全部修齐后，方案 B 是死代码，**不建议**。

---

## 6. 推荐修复后的预期行为

### 路径 A：LENGTH ROI 内无可拟合 contour
- 修复前：弹窗空白 + 日志 "测量失败: "
- 修复后：弹窗显示 `"长度测量失败: HALCON error #xxxx: <具体错误>"` + 日志同步

### 路径 B：CIRCLE ROI 不构成圆
- 修复前：弹窗空白
- 修复后：弹窗显示 `"圆直径测量失败: HALCON error #xxxx: ..."`

### 路径 C：DISTANCE 在 DistancePp 抛错（极罕见）
- 修复前：日志 "测量失败: "
- 修复后：日志 "测量失败: 距离测量失败: ..."（注意：会有"双重失败"前缀，但语义清楚）

### 路径 D：ANGLE / AREA
- 行为不变（catch 已设 label）

---

## 7. 不应该做的事

- **不要**改 LENGTH/CIRCLE/DISTANCE 的 try 体算法（Bug 4 与算法无关）
- **不要**给 MeasureResult 加新字段（如 `errorCode`）——既有 r.label 字段足以承载错误文本
- **不要**改 `MeasureResult` 结构体的默认初始化（不影响本 bug，且改默认值会影响其他 ABI）
- **不要**把 QMessageBox 改成 statusBar 提示（与 Fix 5 一致性方案重叠，但本 bug 修的是"有内容/无内容"，不是"用什么 UI 通道")
- **不要**为修这个顺手"统一所有 catch 包括 catch(...) 兜底"——`catch (HalconCpp::HException &)` 已经覆盖所有 HALCON 错；新增 `catch (...)` 是 scope 蔓延

---

## 8. 不确定性

1. **不能 100% 确定用户那两次失败具体是 LENGTH 还是 CIRCLE**——用户截图未显示 m_measureType combo 状态。但只要修了方案 A，两种类型修复后都有内容。
2. **HException::ErrorMessage().Text() 在极个别错误码下可能返回空字符串**——若发生，前缀 `"长度测量失败: "` 仍然提供最低限度信息（用户至少知道是哪类测量）。
3. **修复后 qWarning 仍然有效**——开发者通过控制台仍能看到原始消息，用户通过 UI 看到带前缀的消息，两条通道互补。

---

## 等用户确认

**Q**：是否照"方案 A：在 measureLength / measureCircle / measureDistance 三个 catch 块统一设默认错误消息"修？

如确认，请说"按方案 A 修 Bug 4"，我会改 `MeasurementEngine.cpp` 三处共 ~6 行代码并重新编译。
