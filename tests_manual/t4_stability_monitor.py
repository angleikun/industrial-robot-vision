"""
T4 稳定性测试监控脚本
======================
每分钟采样一次 RobotVisionSystem.exe 的：
  - 内存（Working Set Private，MB）
  - 句柄数（Windows handle count）
  - CPU 占用率（%）

运行 8 小时后（或 Ctrl+C 提前结束）生成：
  - T4_stability_log.csv     逐分钟原始数据
  - T4_stability_results.md  汇总报告（含验收判定）

用法：
  pip install psutil
  python t4_stability_monitor.py
  python t4_stability_monitor.py --duration 8 --interval 60 --proc RobotVisionSystem
"""

import argparse
import csv
import os
import sys
import time
from datetime import datetime, timedelta

try:
    import psutil
except ImportError:
    sys.exit("请先安装 psutil：pip install psutil")


def find_process(name: str):
    """在所有进程里找第一个匹配进程名的（不区分大小写）。"""
    name_lower = name.lower()
    for p in psutil.process_iter(["pid", "name"]):
        if name_lower in p.info["name"].lower():
            return p
    return None


def sample(proc: psutil.Process):
    """采集一次进程指标，返回 (memory_mb, handles, cpu_pct) 或抛异常。"""
    mem  = proc.memory_info().rss / 1024 / 1024   # MB
    # num_handles() 在 Windows 上可用；Linux/macOS 上用 num_fds()
    try:
        handles = proc.num_handles()
    except AttributeError:
        handles = proc.num_fds()
    cpu = proc.cpu_percent(interval=None)
    return mem, handles, cpu


def write_report(rows, baseline_mem, csv_path, md_path, duration_h, interval_s):
    if not rows:
        print("没有采样数据，未生成报告。")
        return

    mems    = [r["mem_mb"]  for r in rows]
    handles = [r["handles"] for r in rows]
    cpus    = [r["cpu_pct"] for r in rows]

    mem_growth   = mems[-1] - mems[0]
    max_handles  = max(handles)
    min_handles  = min(handles)
    max_cpu      = max(cpus)
    pass_mem     = mem_growth < 50.0
    # 句柄稳定：最大 - 最小 < 200（经验阈值）
    pass_handles = (max_handles - min_handles) < 200
    passed       = pass_mem and pass_handles

    with open(csv_path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=["elapsed_min", "timestamp",
                                          "mem_mb", "handles", "cpu_pct"])
        w.writeheader()
        w.writerows(rows)

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("# T4 稳定性测试报告\n\n")
        f.write(f"- 测试时间: {datetime.now():%Y-%m-%d %H:%M}\n")
        f.write(f"- 计划时长: {duration_h} 小时，采样间隔: {interval_s} 秒\n")
        f.write(f"- 实际采样点: {len(rows)}\n\n")

        f.write("## 统计结果\n\n")
        f.write("| 指标 | 初始值 | 最终值 | 变化量 | 验收标准 | 结论 |\n")
        f.write("|---|---|---|---|---|---|\n")
        f.write(f"| 内存 (MB) | {mems[0]:.1f} | {mems[-1]:.1f} | "
                f"**{mem_growth:+.1f}** | < 50 MB | "
                f"{'[OK] 通过' if pass_mem else '[FAIL] 未达标'} |\n")
        f.write(f"| 句柄数 | {handles[0]} | {handles[-1]} | "
                f"最大={max_handles}, 最小={min_handles} | 波动 < 200 | "
                f"{'[OK] 稳定' if pass_handles else '[!!] 波动大'} |\n")
        f.write(f"| CPU (%) | {cpus[0]:.1f} | {cpus[-1]:.1f} | "
                f"最大={max_cpu:.1f} | 参考 | — |\n")

        f.write(f"\n## 综合结论\n\n**{'[OK] T4 通过' if passed else '[FAIL] T4 未达标'}**\n\n")
        if not pass_mem:
            f.write(f"> [!!]  内存增长 {mem_growth:.1f} MB，超过 50 MB 阈值。"
                    "建议检查 HALCON 句柄泄漏、QImage 大对象缓存、DB 写队列积压。\n\n")
        if not pass_handles:
            f.write(f"> [!!]  句柄数波动 {max_handles - min_handles}，"
                    "建议检查 QTcpSocket 是否正常释放。\n\n")

        f.write("## 内存趋势（每小时代表点）\n\n```\n")
        step = max(1, len(rows) // 8)
        for r in rows[::step]:
            bar = "█" * int(r["mem_mb"] / max(mems) * 20)
            f.write(f"  {r['elapsed_min']:5.0f} min  {r['mem_mb']:7.1f} MB  {bar}\n")
        f.write("```\n")

    print(f"\n{'='*55}")
    print(f"[=] T4 稳定性测试结果")
    print(f"{'='*55}")
    print(f"  内存增长: {mem_growth:+.1f} MB  {'[OK]' if pass_mem else '[FAIL] (> 50MB)'}")
    print(f"  句柄波动: {max_handles - min_handles}  {'[OK]' if pass_handles else '[!!]'}")
    print(f"  最大 CPU: {max_cpu:.1f}%")
    print(f"{'='*55}")
    print(f"  结论: {'[OK] T4 通过' if passed else '[FAIL] T4 未达标'}")
    print(f"{'='*55}")
    print(f"  报告: {os.path.abspath(md_path)}")
    print(f"  数据: {os.path.abspath(csv_path)}")


def main():
    ap = argparse.ArgumentParser(description="T4 稳定性监控")
    ap.add_argument("--duration", type=float, default=8.0,   help="测试时长（小时）")
    ap.add_argument("--interval", type=int,   default=60,    help="采样间隔（秒）")
    ap.add_argument("--proc",     default="RobotVisionSystem", help="进程名关键字")
    ap.add_argument("--csv",      default="T4_stability_log.csv")
    ap.add_argument("--md",       default="T4_stability_results.md")
    args = ap.parse_args()

    total_s   = args.duration * 3600
    end_time  = time.time() + total_s
    rows      = []

    print("=" * 55)
    print("T4 稳定性监控")
    print("=" * 55)
    print(f"  进程: {args.proc}")
    print(f"  时长: {args.duration} 小时   采样: 每 {args.interval} 秒")
    print(f"  预计结束: {datetime.now() + timedelta(seconds=total_s):%H:%M}")
    print("  Ctrl+C 可提前结束并生成报告")
    print()

    # 等待目标进程启动
    proc = None
    while proc is None:
        proc = find_process(args.proc)
        if proc is None:
            print(f"  等待 {args.proc} 启动... (先启动主程序)", end="\r")
            time.sleep(3)

    # 初始 CPU 预热读数（第一次读总是 0）
    try:
        proc.cpu_percent(interval=None)
    except Exception:
        pass
    time.sleep(1)

    print(f"\n  [OK] 已找到进程 PID={proc.pid}，开始监控\n")

    start_time    = time.time()
    baseline_mem  = None
    next_sample   = time.time()

    try:
        while time.time() < end_time:
            now = time.time()
            if now >= next_sample:
                try:
                    mem, handles, cpu = sample(proc)
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    print(f"\n  [FAIL] 进程已消失！（崩溃或被关闭）at "
                          f"{datetime.now():%H:%M:%S}，共运行 "
                          f"{(now - start_time)/3600:.2f} h")
                    rows.append({"elapsed_min": (now - start_time) / 60,
                                 "timestamp":   datetime.now().isoformat(),
                                 "mem_mb": 0, "handles": 0, "cpu_pct": 0})
                    break

                if baseline_mem is None:
                    baseline_mem = mem

                elapsed_min = (now - start_time) / 60
                rows.append({"elapsed_min": elapsed_min,
                             "timestamp":   datetime.now().isoformat(),
                             "mem_mb":      mem,
                             "handles":     handles,
                             "cpu_pct":     cpu})

                remain_h = (end_time - now) / 3600
                growth   = mem - baseline_mem
                print(f"  [{elapsed_min:5.1f} min]  "
                      f"MEM {mem:7.1f} MB ({growth:+.1f})  "
                      f"HDL {handles:5d}  CPU {cpu:5.1f}%  "
                      f"剩余 {remain_h:.2f}h")

                next_sample += args.interval

            time.sleep(0.5)

    except KeyboardInterrupt:
        print("\n\n  收到退出信号，生成报告…")

    write_report(rows, baseline_mem,
                 args.csv, args.md,
                 args.duration, args.interval)


if __name__ == "__main__":
    main()
