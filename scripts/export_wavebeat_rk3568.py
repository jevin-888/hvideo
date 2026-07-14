#!/usr/bin/env python3
# 编码声明：-*- coding: utf-8 -*-
"""
专用于 RK3568 的 WaveBeat ONNX 导出脚本

根本原因：RK3568 NPU v2 寄存器位宽限制 0x1fff（8191）
  - 原始输入 22050 采样 → AvgPool1d(256) → 86 帧
  - dilated Conv1d 展开后某层宽度超过硬件限制 → REGTASK 报错

修复方案：
  - 输入长度：22050 → 8192（2的幂次，NPU 友好）
  - AvgPool 步长：256 → 128，输出帧数 = 8192/128 = 64（安全）
  - 最大 dilation=8 时，padding 最宽 = 64帧，远低于 8191

C++ 端同步修改：
  BUFFER_SAMPLES = 8192   （原来 22050）
  TARGET_SAMPLE_RATE = 22050 不变，但喂入的采样点数改为 8192

用法（Windows/Linux 均可导出 ONNX，仅 RKNN 转换需 Linux）：
  python export_wavebeat_rk3568.py
  # 输出：wavebeat_rk3568.onnx
"""

import sys
import os
import torch
import torch.nn as nn

if sys.platform == 'win32':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

# ============================================================
# RK3568 专用参数
# ============================================================
INPUT_SAMPLES  = 8192   # 必须是 2 的幂次，且 / POOL_STRIDE 后 < 8191
POOL_STRIDE    = 128    # 8192 / 128 = 64 帧（安全）
ONNX_OUTPUT    = "wavebeat_rk3568.onnx"
# ============================================================


class WaveBeatRK3568(nn.Module):
    """
    WaveBeat 节拍检测模型 —— RK3568 适配版

    架构与原版完全一致（TCN + pool + beat/downbeat head），
    仅池化步长从 256 改为 128 以适配硬件寄存器限制。

    输入  : [1, 1, 8192]  float32
    输出  : beat_probs   [1, 1, 64]  float32  (sigmoid)
            downbeat_probs [1, 1, 64]  float32  (sigmoid)
    """

    def __init__(self):
        super().__init__()

        # 下采样池化（步长 128，输出 64 帧）
        self.pool = nn.AvgPool1d(kernel_size=POOL_STRIDE, stride=POOL_STRIDE)

        # TCN 层（dilation 序列：1, 2, 4, 8）
        # 最大有效感受野 = kernel=5, dilation=8 → padding=16，远小于 8191
        self.conv1 = nn.Conv1d(1,  32, kernel_size=5, padding=2,  dilation=1)
        self.conv2 = nn.Conv1d(32, 32, kernel_size=5, padding=4,  dilation=2)
        self.conv3 = nn.Conv1d(32, 32, kernel_size=5, padding=8,  dilation=4)
        self.conv4 = nn.Conv1d(32, 32, kernel_size=5, padding=16, dilation=8)

        self.bn = nn.ModuleList([nn.BatchNorm1d(32) for _ in range(4)])

        # 输出头
        self.beat_out     = nn.Conv1d(32, 1, kernel_size=1)
        self.downbeat_out = nn.Conv1d(32, 1, kernel_size=1)

    def forward(self, x):
        # 示例/字段：x: [B, 1, 8192]
        x = self.pool(x)                              # [B, 1, 64]
        x = torch.relu(self.bn[0](self.conv1(x)))     # [B, 32, 64]
        x = torch.relu(self.bn[1](self.conv2(x)))     # [B, 32, 64]
        x = torch.relu(self.bn[2](self.conv3(x)))     # [B, 32, 64]
        x = torch.relu(self.bn[3](self.conv4(x)))     # [B, 32, 64]

        beat      = torch.sigmoid(self.beat_out(x))      # [B, 1, 64]
        downbeat  = torch.sigmoid(self.downbeat_out(x))  # [B, 1, 64]
        return beat, downbeat


def load_weights_from_checkpoint(model, ckpt_path):
    """尝试从官方 checkpoint 加载权重（可选）"""
    if not os.path.exists(ckpt_path):
        print(f"  checkpoint 不存在，使用随机权重：{ckpt_path}")
        return

    print(f"  加载 checkpoint：{ckpt_path}")
    try:
        ckpt = torch.load(ckpt_path, map_location='cpu', weights_only=False)
        sd = ckpt.get('state_dict', ckpt)
        model_keys = set(model.state_dict().keys())
        loaded = {}
        for k, v in sd.items():
            clean = k.replace('model.', '').replace('detector.', '')
            if clean in model_keys:
                loaded[clean] = v
        if loaded:
            model.load_state_dict(loaded, strict=False)
            print(f"  加载了 {len(loaded)} 个权重张量")
        else:
            print("  未找到匹配权重，使用随机初始化")
    except Exception as e:
        print(f"  checkpoint 加载失败（{e}），使用随机初始化")


def export_onnx():
    print("=" * 60)
    print("WaveBeat RK3568 专用 ONNX 导出")
    print(f"  输入长度：{INPUT_SAMPLES} 采样点（@22050Hz = {INPUT_SAMPLES/22050*1000:.0f}ms）")
    print(f"  池化步长：{POOL_STRIDE}  →  {INPUT_SAMPLES//POOL_STRIDE} 帧（< 8191 RK3568 NPU 限制）")
    print("=" * 60)

    model = WaveBeatRK3568()

    # 尝试加载预训练权重
    CKPT = "wavebeat_epoch=98-step=24749.ckpt"
    load_weights_from_checkpoint(model, CKPT)

    model.eval()

    # 验证 forward
    dummy = torch.randn(1, 1, INPUT_SAMPLES)
    with torch.no_grad():
        beat, db = model(dummy)
    print(f"  forward 验证通过")
    print(f"  输入: {list(dummy.shape)}")
    print(f"  beat_probs:     {list(beat.shape)}")
    print(f"  downbeat_probs: {list(db.shape)}")

    # 导出 ONNX（固定尺寸，不使用 dynamic_axes，RKNN 转换更稳定）
    print(f"\n导出 ONNX → {ONNX_OUTPUT} ...")
    torch.onnx.export(
        model,
        dummy,
        ONNX_OUTPUT,
        input_names=["audio"],
        output_names=["beat_probs", "downbeat_probs"],
        dynamic_axes=None,           # 固定尺寸：RKNN rk3568 必须固定
        opset_version=12,
        do_constant_folding=True,
        export_params=True,
        verbose=False
    )

    size_kb = os.path.getsize(ONNX_OUTPUT) / 1024
    print(f"  导出成功：{ONNX_OUTPUT}  ({size_kb:.1f} KB)")
    print()
    print("下一步（WSL / Linux）：")
    print(f"  TARGET=rk3568 python convert_onnx_to_rknn_rk3568.py")
    print("  # 输出：wavebeat_rk3568.rknn")


if __name__ == "__main__":
    export_onnx()
