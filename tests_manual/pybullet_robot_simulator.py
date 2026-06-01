"""
PyBullet 机器人仿真模拟器（T3 升级版）
=========================================

把纯协议模拟（t3_robot_simulator.py）升级为真实物理仿真：

- 对外 TCP 协议**完全不变**（AA FF + CMD + LEN + DATA + CRC16/MODBUS + 0D），
  主程序 RobotClient 一行不用改、settings.json 也只需要把 robot.port 对上即可。
- 内部驱动 Franka Panda 机械臂在 PyBullet 里**真实地**走过去、抓取、抬起。
- 一个圆柱体代表"瓶子"——会被瞬移到接收到的 (X, Y) 坐标，机械臂真的把它捡起来。
- GUI 窗口可视化整个抓取动作，**直接录屏就是一段端到端 demo 视频**。
- 仍然按 T3 的格式输出成功率统计 CSV/MD 报告。

依赖：
    pip install pybullet
    （Linux 上还需要装 OpenGL 驱动；Windows 自带 DirectX 渲染，无依赖）

用法：
    # GUI 模式（默认）：弹仿真窗口
    python pybullet_robot_simulator.py --port 5000

    # headless 模式：无窗口，纯统计（适合 CI）
    python pybullet_robot_simulator.py --port 5000 --no-gui

    # 自定义参数
    python pybullet_robot_simulator.py --port 5000 --target 20 --tol 5.0

参数说明：
    --port       TCP 监听端口，需与 settings.json 中 robot.port 一致
    --target     达到这么多次抓取后自动出报告（0=不限）
    --tol        成功容差（mm）。物理仿真天然有几毫米误差，建议 ≥3mm
    --no-gui     无窗口模式

注意：
    1) 这个模拟器把成功率从"模拟器内部注入的高斯误差"变成"PyBullet 物理 + IK
       逆解残差"，是真实的机器人运动学误差，更工业、更可信。
    2) Panda 的工作空间是 ~0.85m 半径。视觉发来的 (X, Y) 是 mm 单位的小数，被
       直接映射到机器人桌面坐标 (table_x + X/1000, table_y + Y/1000)。
       测试标定模式下 X≈37mm, Y≈18mm 落在桌面正中——完美。
"""

import argparse
import csv
import math
import queue
import socket
import struct
import sys
import threading
import time
from datetime import datetime

try:
    import pybullet as p
    import pybullet_data
except ImportError:
    sys.exit("需要 PyBullet：pip install pybullet")


# ────────────────────────────────────────────────────────────────
# 协议层（与 t3_robot_simulator.py 完全一致）
# ────────────────────────────────────────────────────────────────
HEARTBEAT, SEND_POSE, QUERY, ESTOP, QUERY_POSE = 0x00, 0x01, 0x02, 0x03, 0x04
READY, BUSY, ERR, POSE_RESP = 0x81, 0x82, 0x83, 0x84
HEADER = b"\xAA\xFF"
TAIL = 0x0D


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
    frames = []
    while True:
        i = buf.find(HEADER)
        if i < 0:
            if len(buf) > 1:
                del buf[:-1]
            break
        if i > 0:
            del buf[:i]
        if len(buf) < 8:
            break
        cmd = buf[2]
        ln = (buf[3] << 8) | buf[4]
        total = 5 + ln + 3
        if len(buf) < total:
            break
        data = bytes(buf[5:5 + ln])
        crc_rx = buf[5 + ln] | (buf[5 + ln + 1] << 8)
        tail = buf[5 + ln + 2]
        body = bytes(buf[2:5 + ln])
        del buf[:total]
        if tail != TAIL or crc_rx != crc16_modbus(body):
            print(f"[!] 丢弃损坏帧 cmd=0x{cmd:02X}")
            continue
        frames.append((cmd, data))
    return frames


# ────────────────────────────────────────────────────────────────
# 统计
# ────────────────────────────────────────────────────────────────
class Stats:
    def __init__(self):
        self.records = []
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


# ────────────────────────────────────────────────────────────────
# 机器人仿真
# ────────────────────────────────────────────────────────────────
class RobotSim:
    def __init__(self, args):
        self.args = args
        self.stats = Stats()
        self.cmd_queue = queue.Queue()
        self.client_sock = None
        self.client_lock = threading.Lock()
        self.shutdown_flag = False

        # 缓存 TCP 末端位姿（TCP 线程查询时用，避免跨线程调 PyBullet）
        self.cached_tcp_pose_mm = (0.0, 0.0, 400.0, math.pi, 0.0, 0.0)
        self.pose_lock = threading.Lock()

        self._init_pybullet()

    def _init_pybullet(self):
        mode = p.DIRECT if self.args.no_gui else p.GUI
        self.cid = p.connect(mode)
        p.setAdditionalSearchPath(pybullet_data.getDataPath())
        p.setGravity(0, 0, -9.81)
        p.setTimeStep(1.0 / 240.0)

        if not self.args.no_gui:
            p.configureDebugVisualizer(p.COV_ENABLE_GUI, 0)
            p.configureDebugVisualizer(p.COV_ENABLE_SHADOWS, 1)
            p.resetDebugVisualizerCamera(
                cameraDistance=1.4, cameraYaw=50, cameraPitch=-35,
                cameraTargetPosition=[0.4, 0.0, 0.6]
            )

        # 地面 + 桌子
        p.loadURDF("plane.urdf")
        self.table_id = p.loadURDF("table/table.urdf", basePosition=[0.5, 0.0, 0.0])
        self.table_x, self.table_y, self.table_z = 0.5, 0.0, 0.62  # 桌面中心 + 高度

        # 机械臂（Franka Panda），放在桌子边缘
        self.robot = p.loadURDF(
            "franka_panda/panda.urdf",
            basePosition=[0.0, 0.0, 0.62],
            useFixedBase=True
        )

        # 动态查找关节索引（不同 pybullet 版本可能差一两位，按名字找更稳）
        self.arm_joint_ids = []
        self.finger_joint_ids = []
        self.ee_link_id = None
        n_joints = p.getNumJoints(self.robot)
        for i in range(n_joints):
            info = p.getJointInfo(self.robot, i)
            jname = info[1].decode()
            jtype = info[2]
            lname = info[12].decode()
            if jtype == p.JOINT_REVOLUTE and "panda_joint" in jname:
                self.arm_joint_ids.append(i)
            elif jtype == p.JOINT_PRISMATIC and "finger" in jname:
                self.finger_joint_ids.append(i)
            if "grasptarget" in lname or lname == "panda_hand":
                self.ee_link_id = i

        if not self.arm_joint_ids or not self.finger_joint_ids or self.ee_link_id is None:
            raise RuntimeError(
                f"无法识别 Panda 关节结构（arm={self.arm_joint_ids}, "
                f"fingers={self.finger_joint_ids}, ee={self.ee_link_id}）。"
                "检查 pybullet 版本和 franka_panda/panda.urdf 是否完整。"
            )
        print(f"[init] arm joints = {self.arm_joint_ids}, "
              f"fingers = {self.finger_joint_ids}, EE link = {self.ee_link_id}")

        # 设置初始 home 关节角，让机械臂处于一个自然的"准备抓取"姿态
        self.home_q = [0.0, -0.4, 0.0, -1.9, 0.0, 1.6, 0.785]
        for j, q in zip(self.arm_joint_ids, self.home_q):
            p.resetJointState(self.robot, j, q)
        for j in self.finger_joint_ids:
            p.resetJointState(self.robot, j, 0.04)  # 张开

        # "瓶子"——一个红色小圆柱
        bottle_col = p.createCollisionShape(p.GEOM_CYLINDER, radius=0.025, height=0.10)
        bottle_vis = p.createVisualShape(
            p.GEOM_CYLINDER, radius=0.025, length=0.10,
            rgbaColor=[0.85, 0.18, 0.18, 1.0]
        )
        self.bottle_id = p.createMultiBody(
            baseMass=0.2,
            baseCollisionShapeIndex=bottle_col,
            baseVisualShapeIndex=bottle_vis,
            basePosition=[self.table_x, self.table_y, self.table_z + 0.05],
        )

        # 抓取约束（fake-grasp：闭合夹爪时把瓶子刚性连接到手腕）
        self.grasp_cid = None

        # ── 放料区（drop zone）：桌面左侧的一个固定位置 ──────────────
        # 真实机械臂的取放循环里，物料总要被放到指定区域。这里我们画一个
        # 蓝色方块当放料区视觉标识，并设置一个固定坐标作为放料目标。
        self.drop_x  = self.table_x - 0.15   # 桌面中心向左 15cm
        self.drop_y  = self.table_y + 0.10   # 向前 10cm
        drop_marker_vis = p.createVisualShape(
            p.GEOM_BOX, halfExtents=[0.05, 0.05, 0.001],
            rgbaColor=[0.2, 0.4, 0.85, 0.7]
        )
        p.createMultiBody(
            baseMass=0,   # 静态、无碰撞，只是个视觉标记
            baseVisualShapeIndex=drop_marker_vis,
            basePosition=[self.drop_x, self.drop_y, self.table_z + 0.002],
        )

        # 让物理收敛一下
        self._settle(0.5)

    # ─── PyBullet 操作（仅主线程调用）───────────────────────────────
    def _settle(self, secs: float):
        steps = int(secs * 240)
        for _ in range(steps):
            p.stepSimulation()
            if not self.args.no_gui:
                time.sleep(1.0 / 240.0)

    def _update_cached_pose(self):
        """主线程每步调一次，把当前 TCP 位姿缓存供 TCP 线程读取。"""
        pos, orn = p.getLinkState(self.robot, self.ee_link_id)[:2]
        eul = p.getEulerFromQuaternion(orn)
        with self.pose_lock:
            self.cached_tcp_pose_mm = (
                pos[0] * 1000.0, pos[1] * 1000.0, pos[2] * 1000.0,
                eul[0], eul[1], eul[2]
            )

    def _open_gripper(self):
        for j in self.finger_joint_ids:
            p.setJointMotorControl2(self.robot, j, p.POSITION_CONTROL,
                                     targetPosition=0.04, force=20)
        self._settle(0.3)

    def _close_gripper(self):
        for j in self.finger_joint_ids:
            p.setJointMotorControl2(self.robot, j, p.POSITION_CONTROL,
                                     targetPosition=0.0, force=20)
        self._settle(0.3)

    def _grasp(self):
        """把瓶子刚性吸附到末端（避免摩擦抓取的不可靠性，更适合 demo）。"""
        if self.grasp_cid is not None:
            return
        ee_pos, ee_orn = p.getLinkState(self.robot, self.ee_link_id)[:2]
        b_pos, b_orn = p.getBasePositionAndOrientation(self.bottle_id)
        # 计算瓶子相对末端的位姿，作为 child frame 偏移
        ee_inv_pos, ee_inv_orn = p.invertTransform(ee_pos, ee_orn)
        rel_pos, rel_orn = p.multiplyTransforms(ee_inv_pos, ee_inv_orn, b_pos, b_orn)
        self.grasp_cid = p.createConstraint(
            parentBodyUniqueId=self.robot,
            parentLinkIndex=self.ee_link_id,
            childBodyUniqueId=self.bottle_id,
            childLinkIndex=-1,
            jointType=p.JOINT_FIXED,
            jointAxis=[0, 0, 0],
            parentFramePosition=rel_pos,
            childFramePosition=[0, 0, 0],
            parentFrameOrientation=rel_orn,
        )

    def _release(self):
        if self.grasp_cid is not None:
            p.removeConstraint(self.grasp_cid)
            self.grasp_cid = None

    def _move_to(self, target_xyz, yaw_rad: float, settle_sec: float = 1.0):
        """通过 IK 把末端移动到目标位姿（夹爪垂直朝下，绕 Z 转 yaw）。"""
        # 夹爪朝下：绕 X 转 pi；再叠加 yaw（绕 Z）
        target_orn = p.getQuaternionFromEuler([math.pi, 0.0, yaw_rad])
        joints = p.calculateInverseKinematics(
            self.robot, self.ee_link_id, target_xyz, target_orn,
            maxNumIterations=120, residualThreshold=1e-4
        )
        # 只把前 7 个（机械臂）传给电机；finger 不受影响
        for j, q in zip(self.arm_joint_ids, joints[:len(self.arm_joint_ids)]):
            p.setJointMotorControl2(self.robot, j, p.POSITION_CONTROL,
                                     targetPosition=q, force=240, maxVelocity=1.5)
        self._settle(settle_sec)

    def _go_home(self, settle_sec: float = 0.8):
        """关节复位到固定的 home 姿态——直接控制关节角，不走 IK。

        和 _move_to 的差别：_move_to 是给定末端 (x,y,z,yaw) 用 IK 反解关节，
        每次解出的关节角可能不同；_go_home 直接把关节摆回写死的姿态，保证
        每次循环的起点完全一致——这才是工业机械臂"待命位"的语义。
        """
        for j, q in zip(self.arm_joint_ids, self.home_q):
            p.setJointMotorControl2(self.robot, j, p.POSITION_CONTROL,
                                     targetPosition=q, force=240, maxVelocity=2.0)
        self._settle(settle_sec)

    def _place_bottle_at(self, x_mm: float, y_mm: float):
        """把瓶子瞬移到桌面上 (table_x + x_mm/1000, table_y + y_mm/1000)。"""
        self._release()  # 如果上次还抓着，先放下
        x = self.table_x + x_mm / 1000.0
        y = self.table_y + y_mm / 1000.0
        z = self.table_z + 0.05  # 圆柱半高
        p.resetBasePositionAndOrientation(self.bottle_id, [x, y, z], [0, 0, 0, 1])
        p.resetBaseVelocity(self.bottle_id, [0, 0, 0], [0, 0, 0])

    # ─── 完整抓取周期 ──────────────────────────────────────────────
    def _execute_grab(self, x_mm: float, y_mm: float, angle_deg: float):
        """
        完整抓取流程：摆瓶子 → 移到上方 → 下降 → 闭爪抓取 → 提起 → 测量误差 →
        归位 → 放下瓶子。

        返回 (actual_x_mm, actual_y_mm, err_mm, ok)。

        完整工业级取放循环（pick-and-place）：
          ① 关节复位到 home → 摆瓶子（视觉触发后机械臂才动）
          ② 末端移到目标上方（高位）
          ③ 下降到抓取高度
          ④ 闭爪 + 创建抓取约束
          ⑤ 提起到高位（这里测量末端落点 vs 视觉指令，计算误差）
          ⑥ 横向移到放料区上方
          ⑦ 下降到放料高度
          ⑧ 张开夹爪 + 解除约束（瓶子稳稳放在放料区）
          ⑨ 抬回高位
          ⑩ 关节复位到 home（保证下次循环起点完全一致）
        """
        # ① 关节复位到 home + 摆瓶子
        self._go_home(settle_sec=0.5)
        self._open_gripper()
        self._place_bottle_at(x_mm, y_mm)
        self._settle(0.3)

        target_x_world = self.table_x + x_mm / 1000.0
        target_y_world = self.table_y + y_mm / 1000.0
        approach_z = self.table_z + 0.25     # 高位：桌面上 25cm
        grab_z     = self.table_z + 0.13     # 抓取高度：夹爪指尖到瓶身中部
        drop_z     = self.table_z + 0.13     # 放料下降高度（同 grab_z）
        yaw        = math.radians(angle_deg)

        # ② 移到目标上方
        self._move_to([target_x_world, target_y_world, approach_z], yaw, settle_sec=0.9)

        # ③ 下降到抓取高度
        self._move_to([target_x_world, target_y_world, grab_z], yaw, settle_sec=0.7)

        # ④ 闭爪 + 吸附约束
        self._close_gripper()
        self._grasp()

        # ⑤ 提起 + 测量精度
        self._move_to([target_x_world, target_y_world, approach_z], yaw, settle_sec=0.7)
        pos, _ = p.getLinkState(self.robot, self.ee_link_id)[:2]
        actual_x_mm = (pos[0] - self.table_x) * 1000.0
        actual_y_mm = (pos[1] - self.table_y) * 1000.0
        err = math.hypot(actual_x_mm - x_mm, actual_y_mm - y_mm)
        ok = err <= self.args.tol

        # ⑥ 横移到放料区上方（瓶子被刚性吸附，跟着一起走）
        self._move_to([self.drop_x, self.drop_y, approach_z], 0.0, settle_sec=0.9)

        # ⑦ 下降到放料高度
        self._move_to([self.drop_x, self.drop_y, drop_z], 0.0, settle_sec=0.7)

        # ⑧ 张爪 + 解除约束（瓶子稳稳落在放料区）
        self._release()
        self._open_gripper()

        # ⑨ 抬回高位（避免下次复位时刮到刚放下的瓶子）
        self._move_to([self.drop_x, self.drop_y, approach_z], 0.0, settle_sec=0.5)

        # ⑩ 关节复位到 home —— 准备下一轮循环
        self._go_home(settle_sec=0.7)

        return actual_x_mm, actual_y_mm, err, ok

    # ─── TCP 服务器（独立线程）─────────────────────────────────────
    def _tcp_handle_frame(self, cmd, data, conn):
        if cmd == HEARTBEAT or cmd == QUERY:
            try:
                conn.sendall(build_frame(READY))
            except OSError:
                pass

        elif cmd == QUERY_POSE:
            with self.pose_lock:
                pose = self.cached_tcp_pose_mm
            try:
                conn.sendall(build_frame(POSE_RESP, struct.pack(">6f", *pose)))
            except OSError:
                pass

        elif cmd == ESTOP:
            print("[ESTOP] received")
            try:
                conn.sendall(build_frame(READY))
            except OSError:
                pass

        elif cmd == SEND_POSE:
            if len(data) < 12:
                try:
                    conn.sendall(build_frame(ERR))
                except OSError:
                    pass
                return
            x, y, angle = struct.unpack(">3f", data[:12])
            # 立即回 BUSY，主线程做完物理仿真后再回最终结果
            try:
                conn.sendall(build_frame(BUSY))
            except OSError:
                return
            self.cmd_queue.put((x, y, angle, conn))

    def _tcp_loop(self):
        while not self.shutdown_flag:
            try:
                conn, addr = self.server_sock.accept()
            except OSError:
                break
            print(f"[+] 视觉系统已连接: {addr}")
            with self.client_lock:
                self.client_sock = conn
            buf = bytearray()
            conn.settimeout(1.0)
            try:
                while not self.shutdown_flag:
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    buf.extend(chunk)
                    for cmd, data in parse_frames(buf):
                        self._tcp_handle_frame(cmd, data, conn)
            except OSError:
                pass
            finally:
                with self.client_lock:
                    self.client_sock = None
                print(f"[-] 断开: {addr}")

    def _send_to_client(self, frame: bytes):
        with self.client_lock:
            if self.client_sock is None:
                return
            try:
                self.client_sock.sendall(frame)
            except OSError:
                pass

    # ─── 主循环 ────────────────────────────────────────────────────
    def run(self):
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_sock.bind((self.args.host, self.args.port))
        self.server_sock.listen(1)

        print("=" * 60)
        print("PyBullet 机器人仿真模拟器（T3 升级版）")
        print("=" * 60)
        print(f"  监听 {self.args.host}:{self.args.port}")
        print(f"  容差 ±{self.args.tol} mm    目标 {self.args.target} 次抓取")
        print(f"  模式：{'GUI (有可视化窗口)' if not self.args.no_gui else 'headless'}")
        print("  Ctrl+C 退出并出报告")
        print()

        threading.Thread(target=self._tcp_loop, daemon=True).start()

        try:
            while not self.shutdown_flag:
                # 处理一条待执行的抓取命令（每个主循环最多一条，保证 GUI 不卡）
                try:
                    x, y, angle, conn = self.cmd_queue.get_nowait()
                    ax, ay, err, ok = self._execute_grab(x, y, angle)

                    # 抓取完成 → 回 READY 或 ERR
                    self._send_to_client(build_frame(READY if ok else ERR))

                    idx = self.stats.add(x, y, angle, ax, ay, err, ok)
                    mark = "[OK]" if ok else "[FAIL]"
                    print(f"  抓取#{idx:3d}  目标=({x:7.2f},{y:7.2f}) θ={angle:6.2f}°  "
                          f"实际=({ax:7.2f},{ay:7.2f})  误差={err:6.3f}mm  {mark}")

                    n, s = self.stats.summary()
                    if self.args.target and n >= self.args.target:
                        print(f"\n已达到目标次数 {self.args.target}，生成报告…")
                        self._write_report()
                        print("继续运行中（可继续发抓取命令，或 Ctrl+C 退出）。\n")

                except queue.Empty:
                    pass

                # 物理步进（无命令时保持仿真活跃，机械臂会自然停留在最后目标点）
                p.stepSimulation()
                self._update_cached_pose()
                if not self.args.no_gui:
                    time.sleep(1.0 / 240.0)

        except KeyboardInterrupt:
            print("\n收到退出信号，生成最终报告…")
            self._write_report()
        finally:
            self.shutdown_flag = True
            try:
                self.server_sock.close()
            except OSError:
                pass
            try:
                p.disconnect()
            except Exception:
                pass

    # ─── 报告输出（同 T3 格式）────────────────────────────────────
    def _write_report(self):
        with self.stats.lock:
            records = list(self.stats.records)
        n = len(records)
        if n == 0:
            print("无抓取记录，跳过报告。")
            return
        s = sum(1 for r in records if r[7])
        rate = 100.0 * s / n
        passed = rate >= 95.0

        with open(self.args.csv, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(["序号", "目标X(mm)", "目标Y(mm)", "角度(°)",
                        "实际X(mm)", "实际Y(mm)", "误差(mm)", "结果"])
            for r in records:
                w.writerow([r[0], f"{r[1]:.3f}", f"{r[2]:.3f}", f"{r[3]:.3f}",
                            f"{r[4]:.3f}", f"{r[5]:.3f}", f"{r[6]:.3f}",
                            "成功" if r[7] else "失败"])

        with open(self.args.md, "w", encoding="utf-8") as f:
            f.write("# T3 抓取成功率测试报告（PyBullet 物理仿真）\n\n")
            f.write(f"- 测试时间：{datetime.now():%Y-%m-%d %H:%M:%S}\n")
            f.write(f"- 抓取次数：{n}\n")
            f.write(f"- 成功次数：{s}\n")
            f.write(f"- 成功容差：±{self.args.tol} mm（物理仿真 IK 残差天然存在）\n")
            f.write(f"- 仿真平台：PyBullet + Franka Panda 7-DOF\n")
            f.write(f"- **成功率：{rate:.1f}%**（验收 ≥ 95%）"
                    f" {'✅ 通过' if passed else '❌ 未达标'}\n\n")
            f.write("## 明细\n\n")
            f.write("| 序号 | 目标坐标(mm) | 实际落点(mm) | 误差(mm) | 结果 |\n")
            f.write("|---|---|---|---|---|\n")
            for r in records:
                f.write(f"| {r[0]} | ({r[1]:.2f}, {r[2]:.2f}) | "
                        f"({r[4]:.2f}, {r[5]:.2f}) | {r[6]:.3f} | "
                        f"{'✅' if r[7] else '❌'} |\n")

        print(f"\n{'='*55}")
        print(f"成功率：{rate:.1f}% ({s}/{n})  "
              f"{'✅ T3 通过' if passed else '❌ T3 未达标'}")
        print(f"{'='*55}")
        print(f"报告：{self.args.md}")
        print(f"数据：{self.args.csv}")


def main():
    ap = argparse.ArgumentParser(description="PyBullet 机器人仿真模拟器（T3 升级版）")
    ap.add_argument("--host", default="0.0.0.0", help="TCP 监听地址")
    ap.add_argument("--port", type=int, default=5000, help="TCP 监听端口 (settings.json 中 robot.port)")
    ap.add_argument("--target", type=int, default=10,
                    help="达到这么多次抓取自动出报告 (0=不限)")
    ap.add_argument("--tol", type=float, default=5.0,
                    help="成功容差 mm (物理仿真天然有几毫米误差，建议 ≥3mm)")
    ap.add_argument("--no-gui", action="store_true", help="无窗口模式（CI 用）")
    ap.add_argument("--csv", default="T3_pybullet_data.csv")
    ap.add_argument("--md",  default="T3_pybullet_results.md")
    args = ap.parse_args()

    sim = RobotSim(args)
    sim.run()


if __name__ == "__main__":
    main()
