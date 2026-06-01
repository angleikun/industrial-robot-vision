"""
T2 定位精度验证脚本（已修正版）
================================
用途：分析模板匹配的重复定位精度，输出标准差报告
运行：python t2_positioning_test.py
依赖：pip install numpy

修正点（相对原版）：
1. 数据库路径：原来写死 "../data/robot_vision.db"，换个目录运行就找不到。
   现在自动在多个常见位置查找，也支持命令行/环境变量覆盖。
2. 单位：数据库里的 x, y 是【像素】(FindShapeModel 列/行)，不是毫米。
   现在用 PIXEL_MM 比例换算成毫米后再算标准差并判定（验收标准是 mm）。
3. 标准差：改用样本标准差 (ddof=1)，更符合"测量重复性"的统计口径。
4. 去掉了用不到的 opencv 依赖。

用法示例：
    python t2_positioning_test.py
    python t2_positioning_test.py --db build/Release/data/robot_vision.db --scale 0.05 --n 20
"""

import argparse
import csv
import os
import sqlite3
import sys
from datetime import datetime

import numpy as np

RESULT_MD  = "T2_positioning_results.md"
RESULT_CSV = "T2_raw_data.csv"
REQUIRED_SAMPLES = 20

# mm/像素 比例，对应 settings.json 的 calibration.pixel_equivalent_mm
PIXEL_MM = 0.05

# 自动查找数据库的候选路径（按你从哪个目录运行）
DB_CANDIDATES = [
    "../build/Release/data/robot_vision.db",                # 脚本放在 build/Release/tests_manual/
    "data/robot_vision.db",                   # 在 build/Release/ 下运行
    "build/Release/data/robot_vision.db",     # 在项目根目录下运行
    "../../data/robot_vision.db",
]


def find_database(explicit=None):
    """返回第一个存在的数据库路径，找不到返回 None。"""
    if explicit:
        return explicit if os.path.exists(explicit) else None
    env = os.environ.get("ROBOT_VISION_DB")
    if env and os.path.exists(env):
        return env
    for p in DB_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def load_from_database(db_path, scale, n=REQUIRED_SAMPLES):
    """从数据库读取最近 n 次检测结果，并把像素坐标换算为毫米。"""
    if not db_path:
        print("❌ 找不到数据库。已查找以下位置：")
        for p in DB_CANDIDATES:
            print(f"    - {os.path.abspath(p)}")
        print("   可用 --db 指定正确路径，例如：")
        print("   python t2_positioning_test.py --db build/Release/data/robot_vision.db")
        return None

    print(f"📂 读取数据库: {os.path.abspath(db_path)}")
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute(
        """
        SELECT x, y, angle, score, timestamp
        FROM detections
        WHERE result = 'PASS'
        ORDER BY id DESC
        LIMIT ?
        """,
        (n,),
    )
    rows = cur.fetchall()
    conn.close()

    if len(rows) < 6:
        print(f"⚠️  有效检测记录不足（当前 {len(rows)} 条，建议 {n} 条）")
        print("    请先在程序中对同一目标、保持静止触发至少 20 次匹配。")
        return None

    if len(rows) < n:
        print(f"⚠️  只取到 {len(rows)} 条（少于 {n}），结果仅供参考。")

    # 像素 → 毫米
    rows = [(x * scale, y * scale, angle, score, ts)
            for (x, y, angle, score, ts) in rows]
    return rows


def manual_input(n=REQUIRED_SAMPLES):
    """手动输入模式（直接输入毫米值，不再换算）。"""
    print(f"\n手动输入模式：请输入 {n} 次匹配结果")
    print("格式：x(mm) y(mm) angle(°)，例如：123.45 67.89 0.12\n")
    data = []
    for i in range(n):
        while True:
            try:
                parts = input(f"第 {i + 1:2d}/{n} 次：").strip().split()
                if len(parts) >= 2:
                    x, y = float(parts[0]), float(parts[1])
                    angle = float(parts[2]) if len(parts) >= 3 else 0.0
                    data.append((x, y, angle, 1.0, datetime.now().isoformat()))
                    break
                print("  格式错误，请重新输入")
            except ValueError:
                print("  请输入数字")
    return data


def analyze(rows):
    """计算定位精度指标（单位：毫米 / 度）。样本标准差 ddof=1。"""
    xs = np.array([r[0] for r in rows])
    ys = np.array([r[1] for r in rows])
    angles = np.array([r[2] for r in rows])
    scores = np.array([r[3] for r in rows])

    ddof = 1 if len(rows) > 1 else 0
    return {
        "n": len(rows),
        "mean_x": float(np.mean(xs)),
        "mean_y": float(np.mean(ys)),
        "std_x": float(np.std(xs, ddof=ddof)),
        "std_y": float(np.std(ys, ddof=ddof)),
        "max_err_x": float(np.max(np.abs(xs - np.mean(xs)))),
        "max_err_y": float(np.max(np.abs(ys - np.mean(ys)))),
        "std_angle": float(np.std(angles, ddof=ddof)),
        "mean_score": float(np.mean(scores)),
        "min_score": float(np.min(scores)),
        "xs": xs, "ys": ys, "angles": angles,
    }


def save_results(rows, stats, scale):
    # 原始数据 CSV
    with open(RESULT_CSV, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["序号", "X(mm)", "Y(mm)", "Angle(°)", "Score", "时间",
                    "X偏差(mm)", "Y偏差(mm)"])
        for i, r in enumerate(rows):
            w.writerow([i + 1, f"{r[0]:.4f}", f"{r[1]:.4f}", f"{r[2]:.4f}",
                        f"{r[3]:.4f}", r[4],
                        f"{r[0] - stats['mean_x']:.4f}",
                        f"{r[1] - stats['mean_y']:.4f}"])

    pass_x = stats["std_x"] < 0.05
    pass_y = stats["std_y"] < 0.05
    passed = pass_x and pass_y

    with open(RESULT_MD, "w", encoding="utf-8") as f:
        f.write("# T2 定位精度验证报告\n\n")
        f.write(f"- 测试时间: {datetime.now():%Y-%m-%d %H:%M:%S}\n")
        f.write(f"- 采样次数: {stats['n']}\n")
        f.write(f"- 像素比例: {scale} mm/px（数据库 x,y 为像素，已换算为毫米）\n\n")
        f.write("## 统计结果\n\n")
        f.write("| 指标 | 值 | 验收标准 | 结论 |\n|---|---|---|---|\n")
        f.write(f"| X 标准差 | **{stats['std_x']:.4f} mm** | < 0.05 mm | "
                f"{'✅ 通过' if pass_x else '❌ 未达标'} |\n")
        f.write(f"| Y 标准差 | **{stats['std_y']:.4f} mm** | < 0.05 mm | "
                f"{'✅ 通过' if pass_y else '❌ 未达标'} |\n")
        f.write(f"| 角度标准差 | {stats['std_angle']:.4f}° | < 0.1° | "
                f"{'✅' if stats['std_angle'] < 0.1 else '⚠️'} |\n")
        f.write(f"| X 最大偏差 | {stats['max_err_x']:.4f} mm | — | — |\n")
        f.write(f"| Y 最大偏差 | {stats['max_err_y']:.4f} mm | — | — |\n")
        f.write(f"| 平均匹配得分 | {stats['mean_score']:.4f} | > 0.7 | "
                f"{'✅' if stats['mean_score'] > 0.7 else '⚠️'} |\n")
        f.write(f"| 最低匹配得分 | {stats['min_score']:.4f} | > 0.5 | "
                f"{'✅' if stats['min_score'] > 0.5 else '⚠️'} |\n")
        f.write(f"\n## 综合结论\n\n**{'✅ T2 验收通过' if passed else '❌ T2 未达标，建议改善光照或重新标定'}**\n\n")
        f.write("## 原始数据\n\n")
        f.write("| 序号 | X(mm) | Y(mm) | Angle(°) | X偏差 | Y偏差 |\n|---|---|---|---|---|---|\n")
        for i, r in enumerate(rows):
            f.write(f"| {i + 1} | {r[0]:.4f} | {r[1]:.4f} | {r[2]:.4f} | "
                    f"{r[0] - stats['mean_x']:+.4f} | {r[1] - stats['mean_y']:+.4f} |\n")

    print("\n" + "=" * 50)
    print("📐 T2 定位精度分析结果")
    print("=" * 50)
    print(f"采样次数: {stats['n']}   (比例 {scale} mm/px)")
    print(f"X 标准差: {stats['std_x']:.4f} mm  {'✅' if pass_x else '❌ (目标 < 0.05 mm)'}")
    print(f"Y 标准差: {stats['std_y']:.4f} mm  {'✅' if pass_y else '❌ (目标 < 0.05 mm)'}")
    print(f"角度标准差: {stats['std_angle']:.4f}°")
    print("=" * 50)
    print(f"综合: {'✅ T2 通过' if passed else '❌ T2 未达标'}")
    print("=" * 50)
    print(f"详细报告: {os.path.abspath(RESULT_MD)}")
    print(f"原始数据: {os.path.abspath(RESULT_CSV)}")


def main():
    ap = argparse.ArgumentParser(description="T2 定位精度验证")
    ap.add_argument("--db", default=None, help="数据库路径（默认自动查找）")
    ap.add_argument("--scale", type=float, default=PIXEL_MM, help="mm/像素 比例")
    ap.add_argument("--n", type=int, default=REQUIRED_SAMPLES, help="取最近多少条")
    ap.add_argument("--manual", action="store_true", help="强制手动输入")
    args = ap.parse_args()

    print("=" * 50)
    print("T2 定位精度验证脚本（修正版）")
    print("=" * 50)

    rows = None
    if not args.manual:
        db_path = find_database(args.db)
        rows = load_from_database(db_path, args.scale, args.n)

    if rows is None:
        if sys.stdin.isatty():
            print("切换到手动输入模式...")
            rows = manual_input(args.n)
        else:
            # 非交互环境（如自动化），直接退出而不是卡在 input()
            sys.exit("无法读取数据库且非交互环境，已退出。")

    stats = analyze(rows)
    # 手动输入时数据已是 mm，比例信息仅用于报告说明
    save_results(rows, stats, args.scale if not args.manual else 1.0)


if __name__ == "__main__":
    main()
