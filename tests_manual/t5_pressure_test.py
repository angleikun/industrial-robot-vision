"""
T5 通信压力测试脚本
用途：向模拟器发送 1000 帧，验证 CRC 全通过、无丢包
运行：先启动 robot_simulator.py，再运行本脚本
依赖：Python 3.8+，无需额外安装
"""

import socket
import struct
import time
import threading
import csv
from datetime import datetime

HOST    = "127.0.0.1"
PORT    = 8888
TOTAL   = 1000       # 发送帧数
INTERVAL_MS = 10     # 帧间隔 10ms（100帧/秒）
RESULT_MD = "T5_pressure_results.md"

# ─── CRC-16/MODBUS ───────────────────────────────────────
_CRC_TABLE = [0] * 256
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xA001 if _c & 1 else _c >> 1
    _CRC_TABLE[_i] = _c

def crc16(data: bytes) -> int:
    v = 0xFFFF
    for b in data:
        v = (v >> 8) ^ _CRC_TABLE[(v ^ b) & 0xFF]
    return v

def build_frame(cmd: int, data: bytes = b"") -> bytes:
    ln   = len(data)
    body = bytes([cmd, (ln >> 8) & 0xFF, ln & 0xFF]) + data
    cv   = crc16(body)
    return bytes([0xAA, 0xFF]) + body + bytes([cv & 0xFF, (cv >> 8) & 0xFF, 0x0D])

def parse_response(buf: bytearray):
    """解析响应帧，返回 (cmd, data) 或 None"""
    for i in range(len(buf) - 1):
        if buf[i] == 0xAA and buf[i+1] == 0xFF:
            if i + 8 > len(buf):
                return None
            data_len = (buf[i+3] << 8) | buf[i+4]
            total = 8 + data_len
            if i + total > len(buf):
                return None
            raw = buf[i:i+total]
            body = bytes(raw[2:-3])
            expected = crc16(body)
            actual   = (raw[-2] << 8) | raw[-3]
            if expected != actual or raw[-1] != 0x0D:
                return None
            return raw[2], bytes(raw[5:-3])
    return None

def make_grab_payload(x: float, y: float, angle: float) -> bytes:
    return struct.pack(">3f", x, y, angle)


class PressureTest:
    def __init__(self):
        self.sent       = 0
        self.received   = 0
        self.crc_pass   = 0
        self.crc_fail   = 0
        self.latencies  = []
        self.lock       = threading.Lock()

    def run(self):
        print("=" * 60)
        print("T5 通信压力测试")
        print("=" * 60)
        print(f"目标帧数:  {TOTAL}")
        print(f"帧间隔:    {INTERVAL_MS} ms")
        print(f"连接:      {HOST}:{PORT}")
        print()

        try:
            sock = socket.create_connection((HOST, PORT), timeout=5)
        except ConnectionRefusedError:
            print("[FAIL] 连接失败！请先启动 robot_simulator.py")
            return

        print(f"[OK] 已连接，开始发送...\n")
        sock.settimeout(2.0)

        buf     = bytearray()
        t_start = time.time()

        with open("T5_raw_frames.csv", "w", newline="") as f:
            csv.writer(f).writerow(["帧序", "CMD", "延迟(ms)", "响应CMD", "CRC"])

        import random
        for i in range(TOTAL):
            # 交替发送不同类型的帧（模拟真实使用场景）
            if i % 20 == 0:
                cmd, data = 0x00, b""           # 心跳
            elif i % 20 == 5:
                cmd, data = 0x04, b""           # 查询位姿
            else:
                # 发送抓取位姿（随机坐标在合理范围内）
                x     = round(random.uniform(250, 350), 2)
                y     = round(random.uniform(100, 200), 2)
                angle = round(random.uniform(-45, 45), 2)
                cmd, data = 0x01, make_grab_payload(x, y, angle)

            frame  = build_frame(cmd, data)
            t_send = time.time()
            try:
                sock.sendall(frame)
                self.sent += 1
            except (BrokenPipeError, ConnectionResetError):
                print(f"  [FAIL] 第 {i+1} 帧发送失败（连接断开）")
                break

            # 等待响应
            resp_cmd = None
            try:
                chunk = sock.recv(4096)
                buf.extend(chunk)
                result = parse_response(buf)
                if result:
                    resp_cmd, _ = result
                    latency = (time.time() - t_send) * 1000
                    self.latencies.append(latency)
                    self.received  += 1
                    self.crc_pass  += 1
                    buf.clear()
                else:
                    self.crc_fail += 1
            except socket.timeout:
                self.crc_fail += 1

            with open("T5_raw_frames.csv", "a", newline="") as f:
                csv.writer(f).writerow([
                    i+1, f"0x{cmd:02X}",
                    f"{latency:.2f}" if self.latencies else "—",
                    f"0x{resp_cmd:02X}" if resp_cmd else "—",
                    "[OK]" if resp_cmd else "[FAIL]"
                ])

            # 进度提示
            if (i + 1) % 100 == 0:
                elapsed = time.time() - t_start
                rate    = self.sent / elapsed
                print(f"  第 {i+1:4d} / {TOTAL} 帧  "
                      f"成功率: {self.received/self.sent*100:.1f}%  "
                      f"速率: {rate:.0f} 帧/s")

            time.sleep(INTERVAL_MS / 1000)

        sock.close()
        elapsed = time.time() - t_start
        self.save_report(elapsed)

    def save_report(self, elapsed):
        pass_rate   = self.received / max(self.sent, 1) * 100
        avg_latency = sum(self.latencies) / len(self.latencies) if self.latencies else 0
        max_latency = max(self.latencies) if self.latencies else 0
        min_latency = min(self.latencies) if self.latencies else 0

        passed = (self.sent == TOTAL and
                  self.crc_fail == 0 and
                  pass_rate >= 99.0)

        with open(RESULT_MD, "w", encoding="utf-8") as f:
            f.write("# T5 通信压力测试报告\n\n")
            f.write(f"- 测试时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"- 总耗时: {elapsed:.1f} 秒\n\n")

            f.write("## 验收结果\n\n")
            f.write("| 指标 | 结果 | 标准 | 结论 |\n|---|---|---|---|\n")
            f.write(f"| 发送帧数 | {self.sent} | {TOTAL} | "
                    f"{'[OK]' if self.sent == TOTAL else '[FAIL]'} |\n")
            f.write(f"| 接收帧数 | {self.received} | = 发送数 | "
                    f"{'[OK]' if self.received == self.sent else '[FAIL]'} |\n")
            f.write(f"| CRC 通过 | {self.crc_pass} | 全部通过 | "
                    f"{'[OK]' if self.crc_fail == 0 else '[FAIL]'} |\n")
            f.write(f"| CRC 失败 | {self.crc_fail} | 0 | "
                    f"{'[OK]' if self.crc_fail == 0 else '[FAIL]'} |\n")
            f.write(f"| 成功率 | {pass_rate:.2f}% | ≥ 99% | "
                    f"{'[OK]' if pass_rate >= 99 else '[FAIL]'} |\n")
            f.write(f"| 平均延迟 | {avg_latency:.2f} ms | — | — |\n")
            f.write(f"| 最大延迟 | {max_latency:.2f} ms | — | — |\n")
            f.write(f"| 最小延迟 | {min_latency:.2f} ms | — | — |\n")
            f.write(f"| 吞吐量 | {self.sent/elapsed:.1f} 帧/s | — | — |\n")

            f.write(f"\n## 综合结论\n\n")
            f.write(f"**{'[OK] T5 验收通过' if passed else '[FAIL] T5 未达标'}**\n")

        print(f"\n{'='*60}")
        print(f"[=] T5 压力测试结果")
        print(f"{'='*60}")
        print(f"发送帧数:  {self.sent} / {TOTAL}")
        print(f"接收帧数:  {self.received}")
        print(f"CRC 通过:  {self.crc_pass}  CRC 失败: {self.crc_fail}")
        print(f"成功率:    {pass_rate:.2f}%")
        print(f"平均延迟:  {avg_latency:.2f} ms")
        print(f"最大延迟:  {max_latency:.2f} ms")
        print(f"吞吐量:    {self.sent/elapsed:.1f} 帧/s")
        print(f"{'='*60}")
        print(f"综合: {'[OK] T5 通过' if passed else '[FAIL] T5 未达标'}")
        print(f"报告: {RESULT_MD}")


if __name__ == "__main__":
    PressureTest().run()
