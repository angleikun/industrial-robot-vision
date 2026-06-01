# 测试执行手册

**项目**：工业机器人视觉引导系统 v3  
**测试者**：[你的姓名]  
**日期**：[填写测试日期]  
**设备**：笔记本 USB 摄像头 + Python 机器人模拟器

---

## 目录结构

```
tests_manual/
├── robot_simulator.py       # T3-T5 机器人模拟器
├── gen_caltab_screen.py     # T1 标定板采集工具
├── t2_positioning_test.py   # T2 定位精度分析
├── t4_stability_monitor.py  # T4 稳定性监控
├── t5_pressure_test.py      # T5 通信压力测试
├── README.md                # 本文档
└── [运行后生成]
    ├── caltab_images/           # T1 采集的标定板图像
    ├── T1_calibration_results.md
    ├── T2_positioning_results.md
    ├── T2_raw_data.csv
    ├── T3_grab_results.csv
    ├── T4_stability_log.csv
    ├── T4_stability_results.md
    ├── T5_pressure_results.md
    └── T5_raw_frames.csv
```

---

## 环境准备（一次性）

```powershell
# 安装依赖
pip install opencv-python numpy psutil

# 把项目 settings.json 中的机器人 IP 改为模拟器地址
# config/settings.json：
#   "robot": {
#     "ip": "127.0.0.1",
#     "port": 8888
#   }
```

---

## T1：标定精度验证（约 30 分钟）

**目标**：重投影误差 < 0.5 px

### 没有打印机的方案

用手机/平板显示棋盘格，笔记本摄像头对准拍摄。

**步骤**：

1. 打开新 PowerShell，进入 tests_manual/：
   ```powershell
   cd <project_root>\tests_manual
   python gen_caltab_screen.py
   ```

2. 程序弹出两个窗口：
   - 「**棋盘格标定板**」窗口：把它拖到手机/平板旁边，或者对着另一块显示器
   - 「**摄像头预览**」窗口：实时显示摄像头画面

3. 采集方法（需要 15 张不同角度）：
   - 摄像头检测到棋盘格后，绿色角点高亮显示
   - 按 **S** 键采集当前帧
   - 每次采集后**改变角度或距离**（换 15 个不同姿态）
   - 推荐：正面 3 张、左倾 3 张、右倾 3 张、上倾 3 张、近距 3 张

4. 采集完 15 张后，脚本自动计算 OpenCV 重投影误差并保存 `T1_calibration_results.md`

5. 在主程序中也执行一次 HALCON 内参标定（使用上述采集的图像），记录 HALCON 输出的重投影误差

> ⚠️ **注意**：手机屏幕会有轻微反光，建议：
> - 屏幕亮度调最高
> - 拍摄时避免强光直射手机屏幕
> - 笔记本摄像头到手机距离保持 30-50 cm

---

## T2：定位精度验证（约 20 分钟）

**目标**：X/Y 标准差 < 0.05 mm

### 步骤

1. 完成 T1 标定后，启动主程序
2. 打开相机采集（USB 摄像头 0）
3. 固定一个有明显特征的物体在摄像头视野中（不能移动）
   > 推荐：一个有文字/图案的杯垫、键帽、硬币
4. 创建该物体的模板
5. 连续触发模板匹配 **20 次**（每次点「检测」按钮）
6. 打开新 PowerShell：
   ```powershell
   cd <project_root>\tests_manual
   python t2_positioning_test.py
   ```
7. 选择「1=从数据库读取」，脚本自动计算标准差
8. 查看输出的 `T2_positioning_results.md`

---

## T3：抓取成功率测试（约 20 分钟）

**目标**：50 次抓取成功率 ≥ 95%

### 步骤

**终端 1**（启动模拟器）：
```powershell
cd <project_root>\tests_manual
python robot_simulator.py
```

**终端 2**（启动主程序）：
```powershell
cd <project_root>\build\Release
.\RobotVisionSystem.exe
```

**操作**：
1. 主程序中：连接机器人 → 应提示「连接成功」（模拟器终端出现「客户端连接」）
2. 启动相机采集
3. 加载模板
4. 连续触发 **50 次**「发送抓取位姿」
   - 每次需要检测到目标 → 才能发送
   - 可以把物体摆在不同位置（每次稍微移动 1-2 cm），模拟真实抓取场景
5. 观察模拟器终端显示的累计成功率
6. 结束后查看 `T3_grab_results.csv`

> 💡 **成功判定**：模拟器检查发送坐标与「当前末端位姿」偏差 ≤ 2 mm。
> 由于用的是模拟坐标，几乎所有帧都会通过，成功率预计 100%。
> 这验证了通信链路完整性。如有机器人后可重新测真实精度。

---

## T4：8 小时稳定性测试

**目标**：无崩溃，内存增长 < 50 MB

> ⏱️ 需要 **8 小时连续运行**，建议睡前启动、早上看结果

### 步骤

**终端 1**：启动模拟器
```powershell
python robot_simulator.py
```

**终端 2**：启动主程序，做好以下配置：
- 连接相机 + 连接机器人 + 加载模板

**终端 3**（与主程序同时运行）：
```powershell
cd <project_root>\tests_manual
pip install psutil
python t4_stability_monitor.py
```

**让主程序自动循环**：
在主程序中，可以写一个简单的自动化脚本，或手动每隔 30 秒点一次「检测 + 发送」。
更省事的方式：让模拟器每 30 秒主动发一个 QUERY 帧，触发程序保持活跃。

8 小时后查看：
- `T4_stability_log.csv`（每分钟一条采样）
- `T4_stability_results.md`（汇总报告）

---

## T5：通信压力测试（约 3 分钟）

**目标**：1000 帧 CRC 全部通过，0 丢包

### 步骤

**终端 1**（先启动模拟器）：
```powershell
python robot_simulator.py
```

**终端 2**（运行压力测试）：
```powershell
cd <project_root>\tests_manual
python t5_pressure_test.py
```

> ⚠️ T5 直接通过 Python 脚本发帧，**不需要启动主程序**
> 这是纯协议层测试，验证 CRC 计算和帧解析的正确性

等待约 3 分钟（1000 帧 × 10 ms 间隔 + 执行时延）。

查看 `T5_pressure_results.md`。

---

## 测试结果汇总

测试完成后，把所有 `*.md` 结果文件复制到项目 `docs/test_results/`：

```powershell
mkdir -p <project_root>\docs\test_results
cp T1_calibration_results.md <project_root>\docs\test_results\
cp T2_positioning_results.md <project_root>\docs\test_results\
cp T3_grab_results.csv       <project_root>\docs\test_results\
cp T4_stability_results.md   <project_root>\docs\test_results\
cp T5_pressure_results.md    <project_root>\docs\test_results\
```

然后在 README.md 末尾添加：

```markdown
## 测试验收结果

| 测试 | 指标 | 结果 |
|---|---|---|
| T1 标定精度 | 重投影误差 | X px ✅ |
| T2 定位精度 | X/Y 标准差 | X mm ✅ |
| T3 抓取成功率 | 50 次 | XX% ✅ |
| T4 稳定性 | 8 小时内存增量 | X MB ✅ |
| T5 压力测试 | 1000 帧 CRC | 全通过 ✅ |
```

---

## 最后一步：录 demo 视频

完成 T1-T3 后就可以录视频了（T4/T5 后台跑，不影响录制）。

**推荐录制内容**（约 3-5 分钟）：

1. 启动程序，展示 Dock Widget UI 布局（15 秒）
2. 打开相机采集，显示实时视频流 + FPS（30 秒）
3. 打开标定向导，快速演示采集界面（30 秒，不用真的标 15 张）
4. 加载已有模板，检测目标，高亮显示检测结果（30 秒）
5. 启动模拟器，点「发送抓取位姿」，展示模拟器收到坐标的打印（30 秒）
6. 导出 CSV 报表（15 秒）
7. 展示 T2 精度报告 + T5 压力测试报告（15 秒）

**推荐工具**：OBS Studio（免费）或 Windows 自带「Xbox Game Bar」（Win+G）
