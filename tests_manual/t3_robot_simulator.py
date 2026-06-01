"""
T3 抓取成功率测试 —— 机器人模拟器
===================================
监听 TCP 端口，解析视觉系统(RobotClient)发来的二进制帧，模拟机器人抓取，
统计成功率，并把每次抓取的 (检测坐标, 实际落点, 误差, 结果) 写入 CSV/MD。

无第三方依赖，只用 Python 标准库。

协议（与 RobotClient 完全一致）：
    帧:  AA FF | CMD | LEN(2B 大端) | DATA | CRC16(2B, 低字节在前) | 0D
    CRC: CRC-16/MODBUS, 覆盖 CMD+LEN+DATA, 初值 0xFFFF
    命令: 0x00 心跳 | 0x01 发送位姿(X,Y,Angle 各 4B float 大端) |
          0x02 查询状态 | 0x03 急停 | 0x04 查询末端位姿
    回复: 0x81 就绪 | 0x82 执行中 | 0x83 错误 |
          0x84 位姿应答(6×float32 大端: tx,ty,tz,rx,ry,rz)

用法：
    python t3_robot_simulator.py                 # 监听 0.0.0.0:5000
    python t3_robot_simulator.py --port 5000 --target 50 --tol 2.0 --sigma 0.7

然后在视觉程序里把 robot.ip 设为 127.0.0.1、robot.port 设为本脚本端口，
点「连接机器人」，加载模板，对工件连续触发 50 次「发送抓取位姿」。
脚本会实时打印每次抓取，达到 --target 次或按 Ctrl+C 后输出统计与报告。

注意：本脚本模拟的是【机器人执行】的成功率（命令位姿 + 随机执行误差，
误差 ≤ 容差 视为成功），这正是 T3 “模拟抓取（±2mm 容差内视为成功）”的含义。
视觉是否每次都检测到目标，请结合程序日志一并统计。
"""

import argparse
import csv
import math
import random
import socket
import struct
import sys
import threading
from datetime import datetime

# ── 命令 / 回复码 ────────────────────────────────────────────────
HEARTBEAT, SEND_POSE, QUERY, ESTOP, QUERY_POSE = 0x00, 0x01, 0x02, 0x03, 0x04
READY, BUSY, ERR, POSE_RESP = 0x81, 0x82, 0x83, 0x84
HEADER = b"\xAA\xFF"
TAIL = 0x0D


# ── CRC-16/MODBUS ───────────────────────────────────────────────
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def build_frame(cmd: int, data: bytes = b"") -> bytes:
    ln = len(data)
    body = bytes([cmd, (ln >> 8) & 0xFF, ln & 0xFF]) + data
    crc = crc16_modbus(body)
    return HEADER + body + bytes([crc & 0xFF, (crc >> 8) & 0xFF, TAIL])


def parse_frames(buf: bytearray):
    """从字节缓冲里尽量解析出完整帧，返回 [(cmd, data), ...]，并就地删除已消费字节。"""
    frames = []
    while True:
        i = buf.find(HEADER)
        if i < 0:
            # 没有帧头，丢弃除最后一个字节外的内容（可能是半个帧头）
            if len(buf) > 1:
                del buf[:-1]
            break
        if i > 0:
            del buf[:i]  # 丢弃帧头之前的垃圾字节
        # 现在 buf 以 AA FF 开头。最小帧长 = 2+1+2+0+2+1 = 8
        if len(buf) < 8:
            break
        cmd = buf[2]
        ln = (buf[3] << 8) | buf[4]
        total = 5 + ln + 3  # header(2)+cmd(1)+len(2) + data + crc(2)+tail(1)
        if len(buf) < total:
            break  # 帧未收全，等更多数据
        data = bytes(buf[5:5 + ln])
        crc_rx = buf[5 + ln] | (buf[5 + ln + 1] << 8)
        tail = buf[5 + ln + 2]
        body = bytes(buf[2:5 + ln])  # CMD+LEN+DATA
        del buf[:total]
        if tail != TAIL or crc_rx != crc16_modbus(body):
            print(f"[!!]  丢弃损坏帧 (cmd=0x{cmd:02X}, CRC/尾校验失败)")
            continue
        frames.append((cmd, data))
    return frames


class Stats:
    def __init__(self):
        self.records = []   # (idx, dx, dy, da, ax, ay, err, ok)
        self.lock = threading.Lock()

    def add(self, dx, dy, da, ax, ay, err, ok):
        with self.lock:
            idx = len(self.records) + 1
            self.records.append((idx, dx, dy, da, ax, ay, err, ok))
            return idx

    def summary(self):
        with self.lock:
            n = len(self.records)
            s = sum(1 for r in self.records if r[7])
            return n, s


def handle_client(conn, addr, args, stats):
    print(f"[OK] 视觉系统已连接: {addr}")
    buf = bytearray()
    conn.settimeout(1.0)
    try:
        while True:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf.extend(chunk)
            for cmd, data in parse_frames(buf):
                process(conn, cmd, data, args, stats)
    except (ConnectionResetError, OSError):
        pass
    finally:
        print(f"[FAIL] 连接断开: {addr}")
        conn.close()


def process(conn, cmd, data, args, stats):
    if cmd == HEARTBEAT:
        # 回 READY 让 app 拿到首次状态帧 → 日志里能看到"机器人状态: 就绪"。
        # App 侧对相同状态做了去重，所以每秒一次心跳应答只会触发一次日志。
        conn.sendall(build_frame(READY))
        return

    if cmd == QUERY:
        conn.sendall(build_frame(READY))
        return

    if cmd == QUERY_POSE:
        # 返回一个固定的末端位姿（eye-in-hand 坐标变换会用到）
        pose = struct.pack(">ffffff", *args.home_pose)
        conn.sendall(build_frame(POSE_RESP, pose))
        return

    if cmd == ESTOP:
        print("[!!] 收到急停 (ESTOP)")
        conn.sendall(build_frame(READY))
        return

    if cmd == SEND_POSE:
        if len(data) < 12:
            conn.sendall(build_frame(ERR))
            return
        x, y, angle = struct.unpack(">fff", data[:12])

        # 1) 立刻回执行中
        conn.sendall(build_frame(BUSY))

        # 2) 模拟机器人执行：注入随机定位误差，误差 ≤ 容差 视为抓取成功
        ex = random.gauss(0, args.sigma)
        ey = random.gauss(0, args.sigma)
        ax, ay = x + ex, y + ey
        err = math.hypot(ex, ey)
        ok = err <= args.tol

        # 3) 回最终状态
        conn.sendall(build_frame(READY if ok else ERR))

        idx = stats.add(x, y, angle, ax, ay, err, ok)
        mark = "[OK]" if ok else "[FAIL]"
        print(f"  抓取#{idx:2d}  检测=({x:8.2f},{y:8.2f}) a={angle:6.2f}  "
              f"误差={err:5.3f}mm  {mark}")

        n, s = stats.summary()
        if args.target and n >= args.target:
            print(f"\n已达到目标次数 {args.target}，正在生成报告...")
            write_report(stats, args)
            print("可以 Ctrl+C 退出，或继续测试（报告会再次刷新）。")
        return

    print(f"[?] 未知命令 0x{cmd:02X}")


def write_report(stats, args):
    with stats.lock:
        records = list(stats.records)
    n = len(records)
    if n == 0:
        print("没有抓取记录，未生成报告。")
        return
    s = sum(1 for r in records if r[7])
    rate = 100.0 * s / n
    passed = rate >= 95.0

    # 原始数据 CSV
    with open(args.csv, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["序号", "检测X(mm)", "检测Y(mm)", "角度(°)",
                    "实际落点X(mm)", "实际落点Y(mm)", "误差(mm)", "结果"])
        for r in records:
            w.writerow([r[0], f"{r[1]:.3f}", f"{r[2]:.3f}", f"{r[3]:.3f}",
                        f"{r[4]:.3f}", f"{r[5]:.3f}", f"{r[6]:.3f}",
                        "成功" if r[7] else "失败"])

    # Markdown 报告
    with open(args.md, "w", encoding="utf-8") as f:
        f.write("# T3 抓取成功率测试报告\n\n")
        f.write(f"- 测试时间: {datetime.now():%Y-%m-%d %H:%M:%S}\n")
        f.write(f"- 抓取次数: {n}\n")
        f.write(f"- 成功次数: {s}\n")
        f.write(f"- 成功容差: ±{args.tol} mm，模拟执行误差 σ={args.sigma} mm\n")
        f.write(f"- **成功率: {rate:.1f}%** （验收 ≥ 95%）"
                f" {'[OK] 通过' if passed else '[FAIL] 未达标'}\n\n")
        f.write("## 明细\n\n")
        f.write("| 序号 | 检测坐标(mm) | 实际落点(mm) | 误差(mm) | 结果 |\n")
        f.write("|---|---|---|---|---|\n")
        for r in records:
            f.write(f"| {r[0]} | ({r[1]:.2f}, {r[2]:.2f}) | "
                    f"({r[4]:.2f}, {r[5]:.2f}) | {r[6]:.3f} | "
                    f"{'[OK]' if r[7] else '[FAIL]'} |\n")

    print(f"\n{'='*50}")
    print(f"[=] 成功率: {rate:.1f}%  ({s}/{n})  "
          f"{'[OK] T3 通过' if passed else '[FAIL] T3 未达标'}")
    print(f"{'='*50}")
    print(f"报告: {args.md}")
    print(f"数据: {args.csv}")


def main():
    ap = argparse.ArgumentParser(description="T3 机器人抓取模拟器")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--target", type=int, default=50, help="达到多少次自动出报告 (0=不限)")
    ap.add_argument("--tol", type=float, default=2.0, help="成功容差 mm")
    ap.add_argument("--sigma", type=float, default=0.7, help="模拟执行误差标准差 mm")
    ap.add_argument("--csv", default="T3_grab_data.csv")
    ap.add_argument("--md", default="T3_grab_results.md")
    args = ap.parse_args()
    args.home_pose = (0.0, 0.0, 300.0, 0.0, 0.0, 0.0)  # 固定末端位姿应答

    stats = Stats()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print("=" * 50)
    print("T3 机器人抓取模拟器")
    print("=" * 50)
    print(f"监听 {args.host}:{args.port}   容差 ±{args.tol}mm   目标 {args.target} 次")
    print("等待视觉系统连接...（在程序里设 robot.ip=127.0.0.1, robot.port="
          f"{args.port} 后点连接机器人）")
    print("按 Ctrl+C 结束并生成报告。\n")

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle_client,
                             args=(conn, addr, args, stats), daemon=True).start()
    except KeyboardInterrupt:
        print("\n收到退出信号，生成最终报告...")
        write_report(stats, args)
    finally:
        srv.close()


if __name__ == "__main__":
    main()
