"""
合成标定图像生成器
用途：无需真实相机，生成 15 张模拟从不同角度拍摄的棋盘格图像
      用于 T1 标定精度验证 demo
运行：python gen_synthetic_calib.py
依赖：pip install opencv-python numpy
"""

import cv2
import numpy as np
import os

OUTPUT_DIR = "caltab_images"
BOARD_W    = 8     # 内角点列数（与你的打印纸一致）
BOARD_H    = 5     # 内角点行数
SQUARE_MM  = 30.0  # 格子尺寸 mm（打印纸实际格子大小）
IMG_W      = 1280
IMG_H      = 960

# 模拟相机内参（笔记本摄像头典型值）
fx = 900.0
fy = 900.0
cx = IMG_W / 2
cy = IMG_H / 2
K  = np.array([[fx, 0, cx],
               [0, fy, cy],
               [0,  0,  1]], dtype=np.float64)
# 模拟轻微畸变
dist = np.array([-0.25, 0.08, 0.001, -0.001, 0.0], dtype=np.float64)

def make_board_points():
    """生成棋盘格 3D 角点（Z=0 平面）"""
    pts = np.zeros((BOARD_H * BOARD_W, 3), np.float32)
    pts[:, :2] = np.mgrid[0:BOARD_W, 0:BOARD_H].T.reshape(-1, 2)
    pts *= SQUARE_MM
    return pts

def render_board(rvec, tvec, img_size=(IMG_W, IMG_H)):
    """
    给定旋转向量和平移向量，渲染一张棋盘格图像
    """
    w, h   = img_size
    canvas = np.ones((h, w, 3), dtype=np.uint8) * 200  # 灰色背景

    # 棋盘格四个角点
    cols = BOARD_W + 1
    rows = BOARD_H + 1
    sq   = SQUARE_MM

    # 生成棋盘格所有格子的四个顶点
    for r in range(rows):
        for c in range(cols):
            # 3D 顶点（格子四角）
            p3d = np.float32([
                [c * sq,       r * sq,       0],
                [(c + 1) * sq, r * sq,       0],
                [(c + 1) * sq, (r + 1) * sq, 0],
                [c * sq,       (r + 1) * sq, 0],
            ])
            # 投影到图像
            p2d, _ = cv2.projectPoints(p3d, rvec, tvec, K, dist)
            p2d    = p2d.reshape(-1, 2).astype(np.int32)

            # 黑白交替填充
            color = (20, 20, 20) if (r + c) % 2 == 0 else (240, 240, 240)
            cv2.fillConvexPoly(canvas, p2d, color)

    # 加轻微高斯噪声，模拟真实图像
    noise = np.random.normal(0, 3, canvas.shape).astype(np.int16)
    canvas = np.clip(canvas.astype(np.int16) + noise, 0, 255).astype(np.uint8)

    return canvas


def generate_poses():
    """
    生成 15 个不同的相机位姿
    参数：(rx_deg, ry_deg, rz_deg, tx_mm, ty_mm, tz_mm)
    tz_mm：相机到棋盘格距离，越大越远
    """
    # 棋盘格中心偏移（让它居中）
    cx_offset = -(BOARD_W - 1) * SQUARE_MM / 2
    cy_offset = -(BOARD_H - 1) * SQUARE_MM / 2

    poses = [
        # 正面居中，不同距离
        (  0,  0,  0,  cx_offset, cy_offset, 450),
        (  0,  0,  0,  cx_offset, cy_offset, 380),
        (  0,  0,  0,  cx_offset, cy_offset, 530),
        # 左右倾斜
        (  0, 20,  0,  cx_offset - 30, cy_offset, 450),
        (  0,-20,  0,  cx_offset + 30, cy_offset, 450),
        (  0, 35,  0,  cx_offset - 50, cy_offset, 420),
        (  0,-35,  0,  cx_offset + 50, cy_offset, 420),
        # 上下倾斜
        ( 20,  0,  0,  cx_offset, cy_offset - 30, 450),
        (-20,  0,  0,  cx_offset, cy_offset + 30, 450),
        ( 30,  0,  0,  cx_offset, cy_offset - 50, 420),
        # 复合角度
        ( 15, 20,  5,  cx_offset - 20, cy_offset - 20, 460),
        (-15,-20,  5,  cx_offset + 20, cy_offset + 20, 460),
        ( 20,-15, -5,  cx_offset + 10, cy_offset - 30, 440),
        (-10, 25, 10,  cx_offset - 40, cy_offset + 10, 480),
        ( 25, 10,  0,  cx_offset - 10, cy_offset - 40, 420),
    ]
    return poses


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    poses  = generate_poses()
    obj_pts = []
    img_pts = []
    board3d = make_board_points()
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)

    print("=" * 50)
    print("生成合成标定图像")
    print("=" * 50)

    for i, (rx, ry, rz, tx, ty, tz) in enumerate(poses):
        rvec = np.array([np.radians(rx),
                         np.radians(ry),
                         np.radians(rz)], dtype=np.float64)
        tvec = np.array([[tx], [ty], [tz]], dtype=np.float64)

        img   = render_board(rvec, tvec)
        gray  = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        found, corners = cv2.findChessboardCorners(
            gray, (BOARD_W, BOARD_H),
            cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE
        )

        if found:
            corners2 = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
            obj_pts.append(board3d)
            img_pts.append(corners2)
            cv2.drawChessboardCorners(img, (BOARD_W, BOARD_H), corners2, found)
            status = "✅ 检测到角点"
        else:
            status = "⚠️  未检测到（跳过）"

        fname = os.path.join(OUTPUT_DIR, f"synthetic_{i+1:03d}.png")
        cv2.imwrite(fname, img)
        print(f"  图像 {i+1:2d}/15  角度({rx:+.0f}°,{ry:+.0f}°,{rz:+.0f}°)  {status}")

    print()
    if len(obj_pts) < 6:
        print("❌ 有效图像不足，无法标定")
        return

    # 执行标定
    print("正在计算标定参数...")
    ret, K_out, dist_out, rvecs, tvecs = cv2.calibrateCamera(
        obj_pts, img_pts, (IMG_W, IMG_H), None, None
    )

    # 计算重投影误差
    total_err = 0
    for j in range(len(obj_pts)):
        proj, _ = cv2.projectPoints(obj_pts[j], rvecs[j], tvecs[j], K_out, dist_out)
        err = cv2.norm(img_pts[j], proj, cv2.NORM_L2) / len(proj)
        total_err += err
    mean_err = total_err / len(obj_pts)

    # 输出结果
    print()
    print("=" * 50)
    print("📐 标定结果")
    print("=" * 50)
    print(f"有效图像数:   {len(obj_pts)} / {len(poses)}")
    print(f"重投影误差:   {mean_err:.4f} px  {'✅ 通过' if mean_err < 0.5 else '⚠️ 偏大'}")
    print(f"fx:           {K_out[0,0]:.2f} px")
    print(f"fy:           {K_out[1,1]:.2f} px")
    print(f"cx:           {K_out[0,2]:.2f} px")
    print(f"cy:           {K_out[1,2]:.2f} px")
    print(f"畸变 k1:      {dist_out[0,0]:.6f}")
    print(f"畸变 k2:      {dist_out[0,1]:.6f}")
    print("=" * 50)

    # 保存报告
    passed = mean_err < 0.5
    with open("T1_calibration_results.md", "w", encoding="utf-8") as f:
        f.write("# T1 标定精度验证结果\n\n")
        f.write(f"> 说明：使用合成图像模拟标定，用于 demo 验证标定流程\n\n")
        f.write(f"- 图像数量: {len(obj_pts)} 张\n")
        f.write(f"- 重投影误差: **{mean_err:.4f} px** ")
        f.write("✅ 通过\n\n" if passed else "⚠️ 偏大（合成图像误差通常 < 0.1 px）\n\n")
        f.write("## 相机内参矩阵\n\n")
        f.write("| 参数 | 值 |\n|---|---|\n")
        f.write(f"| fx | {K_out[0,0]:.4f} px |\n")
        f.write(f"| fy | {K_out[1,1]:.4f} px |\n")
        f.write(f"| cx | {K_out[0,2]:.4f} px |\n")
        f.write(f"| cy | {K_out[1,2]:.4f} px |\n")
        f.write(f"| k1 | {dist_out[0,0]:.6f} |\n")
        f.write(f"| k2 | {dist_out[0,1]:.6f} |\n\n")
        f.write(f"## 结论\n\n**{'✅ T1 验收通过' if passed else '⚠️ T1 需重新采集'}**\n")

    print(f"结果已保存: T1_calibration_results.md")
    print(f"图像已保存: {OUTPUT_DIR}/")


if __name__ == "__main__":
    main()
