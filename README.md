# 工业机器人视觉引导系�?

基于 Qt 6.11 + HALCON 24.11 + OpenCV 4.8 的工业级视觉引导平台�?

## 功能特�?

- 多后端相机采集（USB / Basler / 海康�?
- HALCON 模板匹配定位
- 相机内参标定 + Eye-in-hand 手眼标定
- 5 种视觉测量（长度 / 圆直�?/ 距离 / 角度 / 面积�?
- TCP 二进制协议机器人通信（CRC-16/MODBUS�?
- SQLite 持久�?+ CSV / PDF 报表导出

## 系统要求

- Windows 10/11 x64
- Qt 6.11（MSVC 2022 编译�?
- HALCON 24.11（需有效 License�?
- OpenCV 4.8
- CMake 3.25+
- Visual Studio 2022

## 快速开�?

### 1. 配置环境变量

```powershell
setx HALCONROOT "<HALCON_install_path>"
```

### 2. 编译

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="<Qt_install_path>/6.11.0/msvc2022_64" `
  -DHALCON_ROOT="<HALCON_install_path>" `
  -DOpenCV_DIR="<OpenCV_install_path>/build/x64/vc16/lib"

cmake --build build --config Release
```

### 3. 运行

```powershell
.\run.bat
```

或直接双击项目根目录下的 `run.bat`�?

## 使用流程

1. **启动相机**：选择相机 �?点击「开始采集�?
2. **相机内参标定**：菜单「标�?�?相机内参标定」→ 在向导中采集 �?5 张标定板图像
3. **手眼标定**：菜单「标�?�?手眼标定」→ 移动机器人采�?�?5 组位�?
4. **加载模板**：「加载模板」选择 .shm 文件，或先「创建模板�?
5. **连接机器�?*：配�?settings.json �?robot.ip �?点击「连接机器人�?
6. **检�?+ 抓取**：图像中检测到目标后点击「发送抓取位姿�?

## 配置说明

主配置文�?`config/settings.json`�?

| 配置�?| 关键字段 |
|--------|---------|
| `camera` | 后端选择、分辨率、FPS、采集超�?|
| `halcon.match` | 得分阈值、最大目标数、角度范�?|
| `calibration` | 标定板规格、像素当量、最少位姿数 |
| `robot` | IP、端口、白名单、工作空间边�?|
| `database` | 数据库路径、保留天�?|
| `report` | 报表导出路径 |

## 项目结构

```
RobotVisionSystem/
├── include/          # 头文�?
├── src/              # 源文�?
├── config/           # 配置
├── resources/        # HALCON 模板等资�?
├── calib/            # 标定输出
├── data/             # SQLite 数据库（运行时生成）
├── logs/             # 日志（运行时生成�?
├── exports/          # 报表导出（运行时生成�?
└── docs/             # 文档
```

## 已知限制

- Basler / Hikvision 相机后端需要额外集�?Pylon SDK / MVS SDK
- Excel 导出未实现（待集�?libxlsxwriter�?
- 机器人通信协议不含加密认证，适用于隔离工业内�?
