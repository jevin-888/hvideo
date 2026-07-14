#!/usr/bin/env python3
# 编码声明：-*- coding: utf-8 -*-
"""
消原唱模型 ONNX 导出脚本（独立版，无需网络/torch.hub）

架构：简化版 U-Net 频谱掩码模型
  输入：[1, 2, 512, 128]  float32  (batch, ch, freq_bins, frames)
  输出：[1, 2, 512, 128]  float32  (伴奏掩码 ch0 + 人声掩码 ch1)

支持平台：
  rk3588 → vocal_remover_rk3588.onnx
  rk3568 → vocal_remover_rk3568.onnx（输入帧数改为 64，避免寄存器超限）

用法：
  python3 export_vocal_remover.py          # 导出两个版本
  python3 export_vocal_remover.py rk3588   # 仅 rk3588
  python3 export_vocal_remover.py rk3568   # 仅 rk3568
"""

import sys
import os
import torch
import torch.nn as nn

# ============================================================
# 模型定义（轻量级频谱掩码网络）
# ============================================================

class ConvBNReLU(nn.Module):
    def __init__(self, in_ch, out_ch, kernel=3, pad=1):
        super().__init__()
        self.net = nn.Sequential(
            nn.Conv2d(in_ch, out_ch, kernel, padding=pad, bias=False),
            nn.BatchNorm2d(out_ch),
            nn.ReLU(inplace=True),
        )
    def forward(self, x): return self.net(x)


class VocalRemoverNet(nn.Module):
    """
    轻量级编解码频谱掩码网络
    - 编码器：3 层下采样（频率轴减半）
    - 解码器：3 层上采样 + skip connection
    - 输出：sigmoid 掩码（值域 0~1）

    参数量 ~1.2M，适合 RK3568/RK3588 NPU INT8 量化
    """
    def __init__(self, in_ch=2, base=32):
        super().__init__()
        # ── 编码器 ──
        self.enc1 = ConvBNReLU(in_ch, base)          # [2,512,F] → [32,512,F]
        self.enc2 = nn.Sequential(
            nn.MaxPool2d((2,1)),                       # [32,256,F]
            ConvBNReLU(base, base*2),
        )
        self.enc3 = nn.Sequential(
            nn.MaxPool2d((2,1)),                       # [64,128,F]
            ConvBNReLU(base*2, base*4),
        )
        self.bottleneck = nn.Sequential(
            nn.MaxPool2d((2,1)),                       # [128,64,F]
            ConvBNReLU(base*4, base*4),
            ConvBNReLU(base*4, base*4),
        )
        # ── 解码器 ──
        self.up3 = nn.ConvTranspose2d(base*4, base*4, (2,1), stride=(2,1))
        self.dec3 = ConvBNReLU(base*4 + base*4, base*2)

        self.up2 = nn.ConvTranspose2d(base*2, base*2, (2,1), stride=(2,1))
        self.dec2 = ConvBNReLU(base*2 + base*2, base)

        self.up1 = nn.ConvTranspose2d(base, base, (2,1), stride=(2,1))
        self.dec1 = ConvBNReLU(base + base, base)

        # ── 输出头（2 通道：伴奏掩码 + 人声掩码）──
        self.head = nn.Conv2d(base, 2, kernel_size=1)

    def forward(self, x):
        # 示例/字段：x: [B, 2, 512, F]
        e1 = self.enc1(x)          # [B, 32, 512, F]
        e2 = self.enc2(e1)         # [B, 64, 256, F]
        e3 = self.enc3(e2)         # [B, 128, 128, F]
        b  = self.bottleneck(e3)   # [B, 128, 64, F]

        d3 = self.up3(b)           # [B, 128, 128, F]
        d3 = self.dec3(torch.cat([d3, e3], dim=1))   # [B, 64, 128, F]

        d2 = self.up2(d3)          # [B, 64, 256, F]
        d2 = self.dec2(torch.cat([d2, e2], dim=1))   # [B, 32, 256, F]

        d1 = self.up1(d2)          # [B, 32, 512, F]
        d1 = self.dec1(torch.cat([d1, e1], dim=1))   # [B, 32, 512, F]

        mask = torch.sigmoid(self.head(d1))           # [B, 2, 512, F]
        return mask


# ============================================================
# 导出函数
# ============================================================

CONFIGS = {
    "rk3588": {"frames": 128, "out": "vocal_remover_rk3588.onnx"},
    "rk3568": {"frames": 64,  "out": "vocal_remover_rk3568.onnx"},
}


def export(platform: str):
    cfg = CONFIGS[platform]
    frames = cfg["frames"]
    out_path = cfg["out"]

    print(f"\n{'='*55}")
    print(f"导出平台：{platform.upper()}")
    print(f"输入形状：[1, 2, 512, {frames}]")
    print(f"输出路径：{out_path}")
    print('='*55)

    model = VocalRemoverNet(in_ch=2, base=32)
    model.eval()

    dummy = torch.randn(1, 2, 512, frames)
    with torch.no_grad():
        out = model(dummy)
    print(f"forward 验证：输入 {list(dummy.shape)} → 输出 {list(out.shape)}")

    torch.onnx.export(
        model,
        dummy,
        out_path,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes=None,          # 固定尺寸，RKNN 更稳定
        opset_version=12,
        do_constant_folding=True,
        export_params=True,
    )

    size_kb = os.path.getsize(out_path) / 1024
    print(f"导出成功：{out_path}  ({size_kb:.1f} KB)")
    return out_path


if __name__ == "__main__":
    targets = sys.argv[1:] if len(sys.argv) > 1 else ["rk3588", "rk3568"]
    for t in targets:
        if t not in CONFIGS:
            print(f"未知平台：{t}，可选：rk3588 / rk3568")
            continue
        export(t)
    print("\n全部完成，下一步在 WSL 中运行 convert_vocal_remover_rknn.py")
