# Cppcheck 静态分析报告

**工具**: Cppcheck 2.21.0
**命令**: `cppcheck --enable=all --inconclusive --std=c++17 --suppress=missingIncludeSystem --suppress=checkersReport src include`
**扫描范围**: `src/` + `include/` 全量
**原始输出**: `cppcheck_report.txt` (429 行)

---

## 总览（按严重程度计数）

| 级别 | 总条数 | 真 bug | 误报 / 噪声 |
|------|------|--------|------------|
| error | 8 | 2 (实际 1 个 bug，工具重复报告) | 6 (全部是 Qt 宏识别失败) |
| warning | 4 | 4 (其中 1 个是 error 的根因) | 0 |
| performance | 2 | 2 (轻度优化) | 0 |
| style | 84 | 0 | 84 (19 Qt emit shadow + 65 Qt 槽函数 unused) |
| information | 7 | 0 | 7 (缺 HalconCpp.h / .moc / normalCheckLevel) |

**高严重度 (error + warning) 去重后真 bug 数 = 4 条独立问题**
（1 个根因 + 3 个独立 uninit 成员）

---

## 高严重度 (error / warning)

### [error] MeasurementEngine.cpp:75 — `Uninitialized variable: r.type` / `Uninitialized struct member: r.type`

```cpp
// 行 73-76
case MeasureType::DISTANCE:
    if (points.size() < 2) {
        MeasureResult r; r.valid = false;
        r.label = "需要选择两个点"; return r;   // r.type 从未赋值
    }
```

- **真 bug 判断**: ✅ 是
- **根因**: `MeasurementEngine.h:19` `MeasureType type;` 无默认初始化器
- **影响**: 调用方读取 `result.type` 时为未定义值；DISTANCE 早返回路径独有，ANGLE/AREA 路径都正确赋值（参见 cpp 文件 81 行 `r.type = MeasureType::ANGLE;`）
- **修复建议**: 在 `MeasurementEngine.h:19` 改为 `MeasureType type = MeasureType::LENGTH;`（或任一默认枚举值）

### [warning] MeasurementEngine.h:19 — `MeasureResult::type` 无初始化器

- **真 bug 判断**: ✅ 是（上面 error 的根因，同一处修复）

### [warning] ImageView.h:92 — `DetectionItem::angle` 无初始化器
### [warning] ImageView.h:93 — `DetectionItem::score` 无初始化器
### [warning] ImageView.h:94 — `DetectionItem::valid` 无初始化器

```cpp
struct DetectionItem {
    QPointF center;
    double  angle;     // 无初始化
    double  score;     // 无初始化
    bool    valid;     // 无初始化
};
```

- **真 bug 判断**: ✅ 是（轻度）
- **影响**: 若构造 `DetectionItem` 后未逐字段赋值就读取，行为未定义。当前调用点都走显式赋值路径，但属于"等坑"代码
- **修复建议**: 改为 `double angle = 0.0; double score = 0.0; bool valid = false;`

### [error × 6] unknownMacro — Qt 宏识别失败（误报）

- `CalibrationWizard.h:29` `private slots:`
- `CameraManager.h:46` `private slots:`
- `DatabaseManager.cpp:158` `Q_UNUSED(fromVersion)`
- `VisionProcessor.h:63` `public slots:`
- `DatabaseManager.h:93` `private slots:`
- `RobotClient.h:88` `private slots:`

- **真 bug 判断**: ❌ 误报，Qt MOC 宏 cppcheck 未配置识别

---

## 中严重度 (performance)

### [performance] CoordTransform.h:37 — `intrinsic()` 应按 const 引用返回成员
### [performance] CoordTransform.h:69 — `currentToolPose()` 应按 const 引用返回成员

- **真 bug 判断**: 🟡 是优化建议，非 bug
- **影响**: 每次调用拷贝整个结构体；当前调用频率低，影响有限
- **修复建议**: 返回类型改为 `const CameraIntrinsic&` / `const RobotToolPose&`

---

## 低严重度 (style) — 全部归类为噪声

### shadowFunction × 19 — Qt `emit signalName(args)` 语法触发的误报

- 文件分布: `CalibrationManager.cpp` × 13, `CoordTransform.cpp` × 1, `ImageView.cpp` × 3, 等
- **真 bug 判断**: ❌ 误报。`emit` 是 Qt 关键字宏，cppcheck 把信号名当作局部变量解析

### unusedFunction × 65 — 大量"未使用"函数（误报为主）

- 主要文件: `CalibrationManager.cpp` (20), `CoordTransform.cpp` (14), `ImageView.cpp` (17), `JsonSettings.cpp` (3), `Logger.cpp` (4), `MeasurementEngine.cpp` (5+), 等
- **真 bug 判断**: ❌ 绝大部分误报
  - Header 公共 API 被未分析的 TU（如 MainWindow.cpp）调用
  - 信号槽通过元对象系统调用，cppcheck 看不见
  - 真正的 dead code 需要人工逐项审查后判断（不在本次扫描范围）

---

## 信息级 (information) — 全部归类为噪声

- `HalconCpp.h` not found × 4：环境未配置 HALCON include path（与本次扫描相关性低）
- `CameraManager.moc` not found × 1：Qt 生成文件不在源码树（正常）
- `normalCheckLevelMaxBranches` × 2：cppcheck 自身提示扫描深度（非问题）

---

## 真 bug 优先级（cppcheck 独立排序）

1. **[修复 1 处即解决 2 条 error + 1 条 warning]** `MeasurementEngine.h:19` 给 `MeasureResult::type` 加默认值
2. **[低风险预防]** `ImageView.h:92-94` 给 `DetectionItem` 3 个原生类型成员加默认值
3. **[可选优化]** `CoordTransform.h:37,69` 改返回类型为 const 引用

---

## 工具能力边界回顾

- ✅ 能检出: 未初始化成员、明显的未定义值传播路径、轻度性能反模式
- ❌ 不能检出: 空 catch 吞异常、SQL 静默失败、protocol 解析缺日志、缺 emit 等语义层问题（这些需要 Path B 的 grep 模式 + 人工 review）
- ⚠️ 噪声主源: Qt 宏（slots / Q_UNUSED / emit）+ MOC 自动连接的信号槽，cppcheck 在不加载 Qt 配置时全部误判
