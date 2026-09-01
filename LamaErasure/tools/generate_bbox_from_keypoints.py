#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
generate_bbox_from_keypoints.py

根据已生成的 7 个关键点（keypoint）YOLO Pose 标签，自动计算 BoundingBox。

背景
----
LamaErasure 新的人工辅助流程中，最终标签只由用户确认的 Mask 决定：
   - Q 键从最终 Mask 重新 connectedComponents 提取 7 个 Component 中心
   - 按 (y 升序, 相同则 x 升序) 固定排序后保存为 7 个 keypoint
   - 占位 BoundingBox 由 7 个点 min/max + padding 生成（只为保证格式合法）

本脚本读取这些标签，丢弃占位 bbox，用 7 个关键点的真实 min/max 重新计算 bbox，
写回标签文件（保持 keypoint 部分不变）。

标签格式（与 src/Global.h -> saveYoloLabels 完全一致，YOLO Pose）：
    class cx cy w h  kp1x kp1y vis  kp2x kp2y vis  ...  kp7x kp7y vis
    每行 1 + 4 + 7*3 = 26 个数值；坐标是归一化 [0,1]；vis 固定为 2。

用法
----
    python tools/generate_bbox_from_keypoints.py --labels-dir labels/
    python tools/generate_bbox_from_keypoints.py --labels-dir labels/ --padding-x 0.06 --padding-y 0.05
    python tools/generate_bbox_from_keypoints.py --labels-dir labels/ --dry-run   # 只预览不写回
"""

import argparse
import glob
import os


def recompute_line(tokens, pad_x, pad_y):
    """tokens: 数值字符串列表。返回新的 bbox 部分 + 原 keypoint 部分拼成的整行，或 None（跳过）。"""
    if len(tokens) < 5:
        return None
    cls = tokens[0]
    kp_tokens = tokens[5:]
    if len(kp_tokens) < 21 or (len(kp_tokens) % 3) != 0:
        # 关键点数量不对（不是 7 个），不处理这一行
        return None

    pts = []
    for i in range(0, len(kp_tokens), 3):
        try:
            x = float(kp_tokens[i])
            y = float(kp_tokens[i + 1])
            vis = kp_tokens[i + 2]
        except ValueError:
            return None
        pts.append((x, y, vis))

    min_x = min(p[0] for p in pts)
    min_y = min(p[1] for p in pts)
    max_x = max(p[0] for p in pts)
    max_y = max(p[1] for p in pts)

    # 加 padding（归一化单位），并裁剪到 [0,1]
    min_x = max(0.0, min_x - pad_x)
    min_y = max(0.0, min_y - pad_y)
    max_x = min(1.0, max_x + pad_x)
    max_y = min(1.0, max_y + pad_y)

    cx = (min_x + max_x) / 2.0
    cy = (min_y + max_y) / 2.0
    bw = max_x - min_x
    bh = max_y - min_y

    out = [cls,
           f"{cx:.6f}", f"{cy:.6f}", f"{bw:.6f}", f"{bh:.6f}"]
    for (x, y, vis) in pts:
        out.append(f"{x:.6f}")
        out.append(f"{y:.6f}")
        out.append(vis)
    return " ".join(out)


def process_file(path, pad_x, pad_y, dry_run):
    with open(path, "r", encoding="utf-8") as f:
        lines = [ln.strip() for ln in f if ln.strip()]

    new_lines = []
    changed = 0
    for ln in lines:
        toks = ln.split()
        new = recompute_line(toks, pad_x, pad_y)
        if new is None:
            print(f"  [SKIP] {os.path.basename(path)}: 行格式异常，保留原样: {ln[:60]}...")
            new_lines.append(ln)
            continue
        if new != ln:
            changed += 1
        new_lines.append(new)

    if not dry_run and changed > 0:
        with open(path, "w", encoding="utf-8") as f:
            for ln in new_lines:
                f.write(ln + "\n")
    return changed


def main():
    ap = argparse.ArgumentParser(description="从 7 个 keypoint 重新计算 YOLO Pose 标签的 BoundingBox")
    ap.add_argument("--labels-dir", required=True, help="标签文件所在目录（含 *.txt）")
    ap.add_argument("--padding-x", type=float, default=0.05, help="bbox 在 X 方向的 padding（归一化单位）")
    ap.add_argument("--padding-y", type=float, default=0.05, help="bbox 在 Y 方向的 padding（归一化单位）")
    ap.add_argument("--recursive", action="store_true", help="递归搜索子目录")
    ap.add_argument("--dry-run", action="store_true", help="只预览计算结果，不写回文件")
    args = ap.parse_args()

    pattern = "**/*.txt" if args.recursive else "*.txt"
    files = sorted(glob.glob(os.path.join(args.labels_dir, pattern), recursive=args.recursive))
    if not files:
        print(f"未找到标签文件: {args.labels_dir}")
        return

    print(f"处理标签目录: {args.labels_dir}  (padding_x={args.padding_x}, padding_y={args.padding_y})"
          f"{'  [DRY-RUN]' if args.dry_run else ''}")
    total = 0
    for fp in files:
        c = process_file(fp, args.padding_x, args.padding_y, args.dry_run)
        total += c
        if c > 0:
            print(f"  [OK] {fp}: 重算 {c} 行 bbox")
    print(f"完成。共重算 {total} 行。")


if __name__ == "__main__":
    main()
