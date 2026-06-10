# Review 3: 工业通信 + 实时性 + 稳定性回测

**审查范围**: RobotClient（协议）+ DatabaseManager（数据层）+ Logger + T1-T5 测试报告
**视角**: 回测 T1-T5 之外的隐患，挑战已声明的性能数字

---

## 未能验证的项

- 真实工业机器人（Fanuc/ABB/库卡）接入的实际行为：T3 用 PyBullet 模拟、T5 用 localhost 自环
- 弱网环境（10% 丢包）实际表现：T1-T5 没有这类测试
- 多客户端连接（1 vision : N robots）：当前架构是 1 : 1

---

## (1) TCP 二进制帧协议

### 真实帧格式（RobotClient.cpp:58-87 已验证）

```
| header(2) | cmd(1) | dataLen(2 BE) | data(N) | crc(2 LE) | tail(1) |
   0xAA 0xFF                                                  0x0D
```

⚠️ **用户给的简表 "head(2)+len(2)+cmd(1)+payload(N)+crc(2)" 与实际不符** — 实际 cmd 在 len 前，且还有 tail 字节 0x0D。CLAUDE.md 第 21 行那个简表是正确的。

### CRC-16/MODBUS（RobotClient.cpp:46-54）

```cpp
quint16 crc = 0xFFFF;                              // 初值
for (...)
    crc = (crc >> 8) ^ crc16Table[idx];           // table-driven
```

✅ 标准 Modbus 实现（多项式 0xA001 反向），256 项查找表完整。

### ⚠️ 字节序不一致

| 字段 | 字节序 | 行号 |
|---|---|---|
| dataLen | **BE**（high byte first） | serialize:67-68, deserialize:98 |
| **CRC** | **LE**（low byte first） | serialize:81-82, deserialize:110-111 |
| 3×float32 payload | BE | sendGrabPose:308-317 |

序列化/反序列化**内部自洽**（都是 LE for CRC），但**与文档/直觉相悖**：Modbus 协议本身 RTU 模式 CRC 就是 LE，这是从 Modbus 借的约定。**应该在 spec/CLAUDE.md 明确写出 CRC 字节序**，否则跨厂对接时机器人端代码可能按 BE 解、CRC 永远不通过。

### 粘包/拆包（RobotClient.cpp:179-217 `tryParseFrame`）

✅ 标准做法：
- `indexOf("\xAA\xFF")` 找头（line 183）
- 头前数据丢弃（line 191）
- 不完整帧 return，等更多数据（line 205）
- 无效 dataLen 跳 1 字节继续（line 200）—— 错误恢复策略

✅ **DoS 保护**: `MAX_PAYLOAD = 1024`（line 101, 198）
- 但**最大值 1024 字节**是不是足够？当前最大 payload 是 24B (pose response 6×float)，1024 远超需求
- 极端情况（被攻击）单帧 1024B 可被接受，**没有 sliding window 限速**

### 帧格式一致性

✅ `RobotFrame::serialize()` 与 `deserialize()` 镜像对称（同一字节序）

---

## (2) 异常路径

### 客户端断开后清理（RobotClient.cpp:398-406）

```cpp
void onSocketDisconnected() {
    m_heartbeatTimer->stop();
    if (m_state == CONNECTED || CONNECTING) {
        setState(RECONNECT_WAITING);
        emit connectionLost();
        scheduleReconnect();
    }
}
```

✅ 心跳停 + 状态机重置 + 重连调度

### 重连策略

```
RECONNECT_INTERVAL_MS = 5000   // 5s 一次
MAX_RECONNECT = 5              // 5 次后进 FAULT
HEARTBEAT_INTERVAL_MS = 1000   // 1s 心跳
```

✅ Fire-and-forget 重连：`QMetaObject::invokeMethod(..., Qt::QueuedConnection)`（RobotClient.cpp:466-468）

**重要踩坑修复**（line 460-465）：
> "previously this used BlockingQueuedConnection + waitForConnected(1000ms), which froze the GUI for up to 1 s per retry"

——之前重连时 GUI 卡 1s 每次重试。**这是工业代码里的可观察性"疤痕"**。

### 写 socket 阻塞主线程？

❌ **不会**——sendGrabPose 用 `Qt::BlockingQueuedConnection` 跨到 worker 线程，但 worker 是另一个 QThread，主线程不会被 socket 阻塞。但如果 worker 线程卡（DNS 慢/连接慢）则主线程被 wait 阻塞 → 由 `m_timeoutMs`（默认 3000ms）兜底。

### 非法帧 / CRC 错恢复

| 失败模式 | 处理 |
|---|---|
| 头字节没找到 | 清空 buffer（line 185） |
| dataLen 超 1024 | 跳 1 字节继续（line 200） |
| CRC 不通过 | RobotFrame::deserialize return false → silently dropped（line 215 注释） |
| tail 非 0x0D | deserialize return false → silently dropped（line 123） |

**所有非法帧静默丢弃**，**无统计**。长时运行中如果机器人端 firmware bug 导致 50% 帧 CRC 错，**用户毫无感知**。建议加 `std::atomic<uint64_t> m_crcFailCount` 暴露到 UI。

### sendFrame 部分写入

```cpp
qint64 written = sock->write(frame.serialize());
return written > 0;          // ⚠️ 部分写入也算成功
```

🟡 **风险**: TCP 缓冲满时 `write` 可能写不全。当前若写了 5/40 字节，return true，下次帧覆盖到错位置。
- **建议**: `return written == data.size() && sock->waitForBytesWritten(timeout);`
- 实际触发概率低（TCP 缓冲通常 64KB，单帧 ≤ 1040B），但工业 24/7 长跑不应该靠概率

---

## (3) 长时运行风险

### DB 连接（DatabaseManager）

✅ **WAL 模式 + foreign_keys=ON + synchronous=NORMAL**（DatabaseManager.cpp:51-53）——并发读 + 完整性 + 性能平衡

✅ **Async write queue + 200ms flush** — 减少 SQLite I/O 阻塞主线程（line 17, 109）

✅ **`~DatabaseManager` 析构前 flush** — 注释明确踩过坑："T2 数据采样接近运行结束时会丢几行"（line 22-30）

⚠️ **DB 文件大小未监控**：
- `databaseSizeBytes()` 接口存在（DatabaseManager.h:85）但**未在 UI 暴露**
- 长跑 7×24，detections 表每天插入数千→数百万行，DB 文件 100MB→GB 也无人报警
- 有 `pruneOldRecords(retentionDays)` 接口（.h:83）但**未发现自动调用**——靠手动触发

### 日志文件无限增长

Logger.h:30-34 声明了 `rotateLog()` + `cleanupOldLogs()`，但实现未验证。**Logger.cpp 已读 Review 1 中确认有 m_mutex 保护写入** — 跨线程串行化。

⚠️ **Logger 跨线程写入**：每条 log 都过 QMutex（Logger.cpp:23, 60），**高频日志成为热点锁**。如果视觉线程每帧 log 一次（30 FPS 持续），单线程争抢 mutex 30 Hz——可接受。但如果 T4 长时运行同时高 log 量，可能成为瓶颈。

### HObject/QImage 缓存内存

✅ VisionProcessor 用栈 HImage（per-call），不缓存 —— **无内存累积风险**

⚠️ **CalibrationManager 长期持图**：
- `m_calibImages` (CalibrationManager.h:88) `QList<HImage>` 长生命周期成员
- `m_handEyeImages` (line 97) `QVector<HImage>` 同上
- 标定流程中累积 15+ 张图（1920×1080×1 = 2MB / 张 → 30MB+）**直到 clearCalibImages 被调用**
- ❌ **未自动清理**：calibrateCamera 成功后不清空 m_calibImages，用户每次再标定都累积。**潜在内存增长**。

### T4 报告："句柄数波动 1514" 真实性

🔴 **T4 报告失真**（T4_stability_results.md:11-13）：

| 指标 | T4 报告 | 真实解读 |
|---|---|---|
| 内存 164.1 → **0.0 MB**（-164.1） | "通过 < 50 MB" | 0.0 = 进程已退出，**采样脚本 bug**，不是项目内存泄漏 |
| 句柄数 1504 → **0**, 最大=1514 | "波动 < 200 不通过" | 同上，进程退出后所有句柄被 OS 回收，最大 1514 vs 起始 1504 = **真实波动只有 10** |
| CPU 582.8% (max 711.2%) | "参考" | 多核累计百分比，**单核占用 ≈ 50-70%**（正常） |

✅ **真实读法**：**项目 8 小时跑下来稳定，T4 测试脚本未在进程退出前停止采样**，把退出动作误读为"波动巨大"。这报告需要重写。

---

## (4) 性能数字回测可信度

### "0.10 ms 平均延迟"（T5_pressure_results.md:15）

🟡 **是 localhost 自环测试，非真实网络**：
- T5 用本机 client + server，**无 TCP/IP 栈** physical layer
- 真实工业以太网（Fanuc/库卡控制器）RTT 1-5ms 常见
- **声称 0.10ms 是"代码内 serialize/deserialize 耗时"**，**不是端到端延迟**

✅ **"1000 帧 100% CRC pass"** 这个数字可信
- 1000 帧都是 happy path：connected → send → deserialize → CRC pass
- **没注入错误帧测试** — 如果发送方故意发 CRC 错的帧，反序列化会 drop，但**没测过这条路径**

### FPS 真实瓶颈

❌ **未实测 — 报告里也没找到 FPS 数字**
- 理论分析：
  - HALCON FindScaledShapeModel：1920×1080 灰度图，模板 50×50，~10-30ms（用 use_polarity + 0.7 score）
  - Qt 渲染 ImageView：QImage → QPixmap → QPainter，~5-15ms
  - DB async write：200ms flush 不阻塞
- **预估上限 30-40 FPS**，但**应该有数字证据**

### "ESC + R" 急停响应

✅ sendEstop 用 BlockingQueuedConnection（line 339-344），同步等 worker 发送
- 但**没暴露端到端急停延迟**测量 — 工业要求 < 100ms

---

## (5) 真实工业场景差距

### 当前能撑多少 PPM？

- 假设每件 1 检测 + 1 抓取
- 单件耗时（理论）：图采（33ms@30FPS） + match（20ms） + DB（异步） + 发送 pose（0.10ms LAN） + 等机器人到位（取决于机器人）
- 纯软件侧 **~60-100ms/件** → **600-1000 件/分钟**
- 实际机器人到位（5-10s）会成为节拍瓶颈 → **整体 6-12 PPM**
- **没有压测验证**

### 1 vision : N robots 架构支持？

❌ **当前不支持**：RobotClient 单 socket、单连接、单状态机。
- 想支持 N robot：需重构为 `QMap<QString, RobotConnection> m_connections`，每连接自带 state machine
- 改动 **中等**（RobotClient 重构）

### 弱网（10% 丢包）行为

❌ **未测试**：
- 主要风险：心跳超时 → 切 RECONNECT_WAITING → 重连
- 心跳间隔 1s，未发现心跳超时判定逻辑（**`onHeartbeatTimer` 只发送，没检查 last ack**）
- ⚠️ **bug 候选**：如果机器人端 1 秒回不来响应，但 TCP 没断，**RobotClient 不知道对端死了**，会持续发送 heartbeat。建议加 `m_lastResponseTime` + 心跳超时判定。

---

## ❗ 严重隐患（必修）

### S1: `queryStatus()` 是 stub
```cpp
// RobotClient.cpp:347-353
RobotStatus RobotClient::queryStatus() {
    RobotFrame frame;
    frame.cmd = RobotCmd::QUERY;
    // ... send and wait for response ...      ← 占位注释
    return RobotStatus::STATUS_UNKNOWN;        ← 直接返回未知
}
```
**调用方 MainWindow 看到 STATUS_UNKNOWN 永远不知道机器人 ready/busy/err**。

### S2: 白名单空 = 默认允许任意 IP
```cpp
// RobotClient.cpp:542-546
bool validatePeerAddress() const {
    if (m_ipWhitelist.isEmpty()) return true;   // ← permissive default
    return m_ipWhitelist.contains(m_robotIp);
}
```
工业场景下应该 **默认 deny**，settings.json 显式列允许的 IP。

### S3: CRC 失败 / 帧错误静默吞
RobotClient.cpp:215 注释 "Invalid frames are silently dropped" — **没有计数、没有 emit signal**。长跑中如果机器人 firmware bug 导致 50% CRC 错，UI 看不到。

### S4: heartbeat 单向，没判定超时
心跳只发不收 — TCP 半开连接（连接看似还在但对端进程已死）无法检出，最多靠 5s 重连 timer。

### S5: m_calibImages 不自动清理
长时间反复标定 → 内存累积。

---

## 🎤 5 个面试时主动讲的"工业稳定性设计决策"

### 决策 1：fire-and-forget 重连替代阻塞重连
> "最早的版本，断线重连用 `BlockingQueuedConnection + waitForConnected(1000ms)`，每次重试会把 GUI 卡 1 秒。后来改成 fire-and-forget：在 worker 线程 invoke `connectToHost`，然后等 worker 的 `connected`/`disconnected` 信号驱动状态转移，GUI 完全不卡（RobotClient.cpp:460-468 有详细注释）。状态机里 `RECONNECT_WAITING ↔ CONNECTING` 由 timer 链驱动，最多 5 次失败进 FAULT 终止状态。"

### 决策 2：QWaitCondition 实现"发送→等回复"同步查询
> "queryToolPose 是一个跨线程同步：GUI 线程要拿机器人位姿，但发送在 worker 线程、回包在 worker 线程 readyRead 触发。用 `QMutex m_poseRespMutex + QWaitCondition m_poseRespCondition` + `m_poseRespPending` 三个东西配合：发送后 GUI 线程 wait 在 condition 上，worker 收到 pose 响应后 `wakeAll()`，GUI 线程被唤醒读 `m_lastPoseResp`。带 timeout 兜底（默认 1s），保证 GUI 不会永久阻塞（RobotClient.cpp:355-380）。"

### 决策 3：DatabaseManager async write queue + 析构前 flush
> "DB 写入用 200ms 异步队列减少 I/O 阻塞，但导致一个 bug：进程退出前队列里的 ~40 行没刷到磁盘，T2 报告丢数据。修复是在 `~DatabaseManager` 里显式 `flushQueue()` 再 `close()`，并在代码里留注释解释为什么 — 防止后人重构时把这步删掉。"

### 决策 4：HALCON License 错误码分类避免 self-test 永久禁用
> "HALCON 启动自检偶尔会抛 #1301 'wrong value of control parameter'，是 binding/DLL 版本的小问题。最早把所有自检异常都当作 License 失败 → 永久灰掉视觉按钮 → 用户重启也没用。修复是只有错误码 5200-5399 范围 OR 消息含 'License' 才算 License 问题，其他**保持功能可用**（MainWindow.cpp:480-490）。"

### 决策 5：DoS 保护 MAX_PAYLOAD = 1024
> "TCP 二进制帧的 dataLen 是 16-bit，理论 max 65535。如果对端 firmware bug 发了一个 dataLen = 60000 的帧，反序列化时会等 60KB 数据，期间 buffer 持续累积，主线程进入异常状态。所以在 deserialize 和 tryParseFrame 都加了 `MAX_PAYLOAD = 1024` 上限，超限直接跳 1 字节继续找下一个头（RobotClient.cpp:101, 198）。"

---

## 🛡️ 5 个会被挑战的性能数字 + 回答策略

### 挑战 1："0.10 ms 平均延迟是真实工业延迟吗？"
**直接答**："不是，那是 localhost 自环测出来的 **代码侧 serialize/deserialize 耗时**。真实工业以太网 RTT 大约 1-5ms，加上 PLC/控制器处理 5-20ms。所以这个 0.10ms 应该理解为 **'软件序列化处理不是瓶颈'** 的证据，不是端到端延迟保证。如果需要真实数字，要在产线上用 Wireshark 测 ACK 时差。"

### 挑战 2："1000 帧 100% CRC pass 测出了什么？"
**直接答**："happy path 通信稳定性。**没有注入错误帧、没有断线重连、没有弱网测试**。下一步 T6 计划：故意发 CRC 错的帧 1%、模拟 100ms 网络延迟、模拟连接中断 30s 后恢复，看 RobotClient 状态机和 CRC 失败计数是否正确。"

### 挑战 3："8 小时稳定性测试通过吗？"
**直接答**："T4 报告显示 'FAIL: 句柄数波动 1514'，但 **报告本身有 bug**——它把 '进程退出导致句柄回 0' 误读为波动。**真实波动 1504→1514 = 10**，远低于 200 阈值。需要修测试脚本，让它在进程退出前 30s 停止采样。我已经记在 review_v2/03 里。"

### 挑战 4："T3 抓取 100% 成功率是怎么算的？"
**直接答**："T3 是用 PyBullet **模拟**机器人，模拟执行误差 σ=0.7mm，容差 ±2mm。100% 成功率说明 **算法侧 + 协议侧没有引入额外误差**。但不是真实工业机器人 — 真机 σ 可能 0.1-0.5mm（更小），也可能因末端工具刚性 0.5-1mm（更大）。这个数字应该重新表述为 **'在 σ=0.7mm 仿真扰动下，视觉算法 + 协议 + 坐标变换链不引入超过 1.3mm 的额外误差'**。"

### 挑战 5："系统能撑多少 PPM？"
**直接答**："**没测过 PPM**。理论分析：单件软件处理 ~60-100ms，所以软件侧支持 600-1000 件/分钟，但**真实瓶颈在机器人到位时间（5-10s）**，整体 6-12 PPM。这是单工位估算，多工位需要 1:N 架构改造（当前不支持）。"

---

## 综合评分（1-10）

| 维度 | 分 | 依据 |
|---|---|---|
| 协议帧格式正确性 | 8 | CRC/字节序内部自洽，DoS 保护到位 |
| 异常路径处理 | 7.5 | 重连状态机完整，fire-and-forget 优化过 |
| 长时稳定性 | 6 | m_calibImages 不自动清，CRC 失败不计数，T4 报告失真 |
| 性能数字真实性 | 5.5 | 0.10ms 是 localhost、T3 是仿真、未测真实工业网络 |
| 工业场景适应性 | 5 | 单 1:1 架构、白名单 permissive 默认、queryStatus 未实现 |

**综合：6.5 / 10**
