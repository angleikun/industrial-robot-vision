# v1.3 Backlog

v1.2 release 时已识别但未处理的工程债，按优先级排序。

## P1 可观测性补强

1. **RobotClient.cpp 协议帧解析 6 处 `return false` 无日志**
   - 位置: line 92, 95, 102, 105, 119, 123
   - 现象: 帧错误时静默 RECONNECT，无法定位（CRC 不匹配 / CMD 未知 / LEN 越界 等无法区分）
   - 工时: 30 分钟

2. **DatabaseManager.cpp:486 SQL 失败静默 return**
   - 现象: `if (!query.exec()) return false;` 丢弃 `query.lastError().text()`
   - 修复: 先 `qWarning() << query.lastError().text();` 再 return
   - 工时: 5 分钟

3. **VisionProcessor.cpp:103 漏 emit processingError**
   - 现象: 异常路径未通过信号通知 UI 层
   - 工时: 5 分钟

## P2 资源管理

4. **CameraManager.cpp:79, 110 `cv::VideoCapture*` → `std::unique_ptr`**
   - 现象: 裸指针在异常/早返回路径可能泄漏
   - 工时: 1 小时（含异常路径测试）

## 已识别可不处理（说明）

- **CoordTransform 按值返回大结构体**（`intrinsic()` / `currentToolPose()`）: 现代 CPU 上 RVO 优化覆盖，不构成实质性能问题
- **ImageView 18 处 `Q_UNUSED`**: 多为继承父类 slot 的占位参数，不是 stub
- **`Q_ASSERT` Release 残留**（3 处）: Qt 在 Release build 自动失效，符合预期行为
- **cppcheck 报告的 65 处 `unusedFunction`**: 多为 header 公共 API 或信号槽通过 MOC 调用，cppcheck 看不见调用方
