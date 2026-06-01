"""
屏幕显示标定板生成器
用途：没有打印机时，在显示器/手机上显示标定板图案，供 T1-T2 使用
运行：python gen_caltab_screen.py
依赖：pip install opencv-python numpy

说明：
- 本脚本生成棋盘格图案（与 HALCON caltab 不同，但 USB 摄像头 demo 可用）
- 显示在第二块屏幕或平板/手机上，用 USB 摄像头对准拍摄
- 采集 15 张不同角度的图像，用于相机内参标定
- 注意：屏幕标定精度不如实体打印，重投影误差可能在 0.5-2 px 范围
"""

import cv2
import numpy as np
import os
import sys

# ─── 棋盘格参数 ─────────────────────────────────────────
BOARD_W    = 9      # 内角点列数
BOARD_H    = 6      # 内角点行数
SQUARE_PX  = 80     # 每个格子像素大小（屏幕上约 2-3 cm）
MARGIN_PX  = 40     # 边距
BG_COLOR   = 255    # 白色背景

OUTPUT_DIR = "caltab_images"   # 采集图像保存目录

def generate_chessboard(w=BOARD_W, h=BOARD_H, sq=SQUARE_PX, margin=MARGIN_PX):
    """生成棋盘格图像"""
    cols = w + 1   # 格子数 = 内角点 + 1
    rows = h + 1
    img_w = cols * sq + 2 * margin
    img_h = rows * sq + 2 * margin
    img = np.ones((img_h, img_w, 3), dtype=np.uint8) * BG_COLOR

    for r in range(rows):
        for c in range(cols):
            if (r + c) % 2 == 0:
                x0 = margin + c * sq
                y0 = margin + r * sq
                img[y0:y0+sq, x0:x0+sq] = 0  # 黑格

    return img


def interactive_capture(camera_id=0):
    """交互式采集标定图像"""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    board = generate_chessboard()

    cap = cv2.VideoCapture(camera_id)
    if not cap.isOpened():
        print(f"❌ 无法打开摄像头 ID={camera_id}，请检查连接")
        sys.exit(1)

    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  1920)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)

    count    = 0
    required = 15

    # 窗口1：棋盘格（用于显示给另一屏/平板）
    cv2.namedWindow("棋盘格标定板 - 拖到第二块屏幕", cv2.WINDOW_NORMAL)
    cv2.imshow("棋盘格标定板 - 拖到第二块屏幕", board)
    cv2.resizeWindow("棋盘格标定板 - 拖到第二块屏幕", 900, 600)

    # 窗口2：摄像头预览
    cv2.namedWindow("摄像头预览 - 按 S 采集", cv2.WINDOW_NORMAL)

    print("=" * 60)
    print("操作说明：")
    print("  1. 把「棋盘格标定板」窗口拖到第二块屏幕（或对着平板/手机屏幕）")
    print("  2. 调整摄像头对准标定板")
    print("  3. 确认检测到棋盘格（绿色角点高亮）后按 S 键采集")
    print("  4. 改变摄像头角度/距离，重复采集 15 张")
    print("  5. 采集完成后按 Q 退出")
    print("=" * 60)
    print(f"目标：{required} 张")

    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        gray    = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        found, corners = cv2.findChessboardCorners(
            gray, (BOARD_W, BOARD_H),
            cv2.CALIB_CB_ADAPTIVE_THRESH +
            cv2.CALIB_CB_FAST_CHECK +
            cv2.CALIB_CB_NORMALIZE_IMAGE
        )

        preview = frame.copy()
        if found:
            corners2 = cv2.cornerSubPix(gray, corners, (11,11), (-1,-1), criteria)
            cv2.drawChessboardCorners(preview, (BOARD_W, BOARD_H), corners2, found)
            cv2.putText(preview, f"检测到棋盘格! 按 S 采集 ({count}/{required})",
                        (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 220, 0), 2)
        else:
            cv2.putText(preview, f"未检测到棋盘格 ({count}/{required})",
                        (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 220), 2)

        cv2.imshow("摄像头预览 - 按 S 采集", preview)
        key = cv2.waitKey(1) & 0xFF

        if key == ord('s') or key == ord('S'):
            if found:
                fname = os.path.join(OUTPUT_DIR, f"caltab_{count+1:03d}.png")
                cv2.imwrite(fname, frame)
                count += 1
                print(f"  ✅ 采集 {count}/{required}: {fname}")
                if count >= required:
                    print(f"\n🎉 已采集 {required} 张，可以开始标定了！")
                    break
            else:
                print("  ⚠️  未检测到棋盘格，请调整角度后重试")
        elif key == ord('q') or key == ord('Q'):
            break

    cap.release()
    cv2.destroyAllWindows()

    print(f"\n图像已保存到: {os.path.abspath(OUTPUT_DIR)}/")
    print(f"共采集: {count} 张")
    if count < required:
        print(f"⚠️  不足 {required} 张，建议继续采集")

    return count


def verify_calibration():
    """用采集的图像做 OpenCV 标定验证（仅供参考，最终用 HALCON 标定）"""
    import glob
    images = sorted(glob.glob(os.path.join(OUTPUT_DIR, "*.png")))
    if len(images) < 6:
        print("图像不足 6 张，跳过验证")
        return

    obj_pts = []
    img_pts = []
    sq_mm   = 30.0  # 假设标定板格子 30mm

    objp = np.zeros((BOARD_H * BOARD_W, 3), np.float32)
    objp[:, :2] = np.mgrid[0:BOARD_W, 0:BOARD_H].T.reshape(-1, 2)
    objp *= sq_mm

    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
    img_size = None

    for fname in images:
        img  = cv2.imread(fname)
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        if img_size is None:
            img_size = gray.shape[::-1]
        ret, corners = cv2.findChessboardCorners(gray, (BOARD_W, BOARD_H), None)
        if ret:
            corners2 = cv2.cornerSubPix(gray, corners, (11,11), (-1,-1), criteria)
            obj_pts.append(objp)
            img_pts.append(corners2)

    if len(obj_pts) < 4:
        print("有效图像不足 4 张，无法标定")
        return

    ret, K, dist, rvecs, tvecs = cv2.calibrateCamera(
        obj_pts, img_pts, img_size, None, None)

    total_err = 0
    for i in range(len(obj_pts)):
        proj, _ = cv2.projectPoints(obj_pts[i], rvecs[i], tvecs[i], K, dist)
        err = cv2.norm(img_pts[i], proj, cv2.NORM_L2) / len(proj)
        total_err += err
    mean_err = total_err / len(obj_pts)

    print(f"\n{'='*50}")
    print(f"📐 OpenCV 标定结果（参考用，与 HALCON 结果可能略有差异）")
    print(f"{'='*50}")
    print(f"有效图像数: {len(obj_pts)}")
    print(f"重投影误差: {mean_err:.4f} px", "✅" if mean_err < 0.5 else "⚠️（偏大，可增加采集）")
    print(f"焦距 fx: {K[0,0]:.2f} px")
    print(f"焦距 fy: {K[1,1]:.2f} px")
    print(f"主点 cx: {K[0,2]:.2f} px")
    print(f"主点 cy: {K[1,2]:.2f} px")
    print(f"{'='*50}")

    with open("T1_calibration_results.md", "w", encoding="utf-8") as f:
        f.write("# T1 标定精度验证结果\n\n")
        f.write(f"- 采集图像数: {len(images)}\n")
        f.write(f"- 有效图像数: {len(obj_pts)}\n")
        f.write(f"- 重投影误差: **{mean_err:.4f} px** ")
        f.write("✅ 通过\n\n" if mean_err < 0.5 else "⚠️ 未达标（目标 < 0.5 px）\n\n")
        f.write("## 内参矩阵\n\n")
        f.write("| 参数 | 值 |\n|---|---|\n")
        f.write(f"| fx (像素) | {K[0,0]:.2f} |\n")
        f.write(f"| fy (像素) | {K[1,1]:.2f} |\n")
        f.write(f"| cx (像素) | {K[0,2]:.2f} |\n")
        f.write(f"| cy (像素) | {K[1,2]:.2f} |\n")
        f.write(f"\n验收结论: {'通过' if mean_err < 0.5 else '不通过'}\n")

    print(f"\n结果已保存: T1_calibration_results.md")


if __name__ == "__main__":
    print("=" * 60)
    print("屏幕标定板工具  （USB 摄像头 + 无打印机方案）")
    print("=" * 60)
    print("提示：把笔记本摄像头对准手机/平板屏幕上的棋盘格")
    print("      或把棋盘格窗口拖到外接显示器")
    print()

    cam_id = 0
    if len(sys.argv) > 1:
        cam_id = int(sys.argv[1])

    n = interactive_capture(cam_id)

    if n >= 6:
        print("\n正在用 OpenCV 验证标定精度...")
        verify_calibration()
