#!/usr/bin/env python3
# 编码声明：-*- coding: utf-8 -*-
"""
RK3568 专用 ONNX -> RKNN 转换脚本
输入：wavebeat_rk3568.onnx
输出：wavebeat_rk3568.rknn

必须在 Linux / WSL 下运行（rknn-toolkit2 官方要求）

用法：
  python convert_onnx_to_rknn_rk3568.py
  # 或量化版（体积更小，有时更稳定）：
  QUANT=1 python convert_onnx_to_rknn_rk3568.py
"""

import os
import sys

if os.name == 'nt':
    print("此脚本需要在 Linux / WSL 下运行")
    print("请在 WSL 中执行：")
    print("  cd /mnt/d/Hvideo/scripts")
    print("  python3 convert_onnx_to_rknn_rk3568.py")
    sys.exit(1)

from rknn.api import RKNN

# ============================================================
# 配置
# ============================================================
ONNX_PATH    = "wavebeat_rk3568.onnx"
RKNN_OUTPUT  = "wavebeat_rk3568.rknn"
PLATFORM     = "rk3568"
INPUT_SHAPE  = [1, 1, 8192]   # 与 export_wavebeat_rk3568.py 一致
QUANT        = os.environ.get("QUANT", "").strip() in ("1", "yes", "true")
DATASET      = "wavebeat_rk3568_dataset.txt"
# ============================================================


def make_calib_dataset(n=20):
    """生成随机校准数据集（INT8 量化用）"""
    import numpy as np
    npy = "wavebeat_rk3568_calib.npy"
    data = np.random.randn(n, *INPUT_SHAPE).astype(np.float32)
    np.save(npy, data)
    with open(DATASET, 'w') as f:
        f.write(npy + '\n')
    print(f"  生成校准数据：{npy}  ({n} 样本)")


def convert():
    if not os.path.exists(ONNX_PATH):
        print(f"找不到 ONNX 文件：{ONNX_PATH}")
        print("请先在 Windows 端运行：python scripts/export_wavebeat_rk3568.py")
        sys.exit(1)

    print("=" * 60)
    print(f"ONNX -> RKNN  平台：{PLATFORM.upper()}")
    print(f"输入：{ONNX_PATH}   输入形状：{INPUT_SHAPE}")
    print(f"量化：{'INT8' if QUANT else 'FP16'}")
    print("=" * 60)

    rknn = RKNN(verbose=False)

    # ── 1. 配置 ──────────────────────────────────────────────
    print("\n[1/4] 配置 RKNN 参数...")
    ret = rknn.config(
        target_platform=PLATFORM,
        mean_values=None,
        std_values=None,
        optimization_level=3,
        # RK3568 NPU v2：关闭多核（只有 1 核），避免调度开销
        single_core_mode=True,
    )
    if ret != 0:
        print(f"  config 失败 ret={ret}")
        sys.exit(1)
    print("  配置完成")

    # ── 2. 加载 ONNX（固定输入形状）─────────────────────────
    print(f"\n[2/4] 加载 ONNX：{ONNX_PATH}")
    ret = rknn.load_onnx(
        model=ONNX_PATH,
        inputs=["audio"],
        input_size_list=[INPUT_SHAPE]
    )
    if ret != 0:
        # 部分版本不支持同时指定 inputs+input_size_list，退而只指定尺寸
        print(f"  load_onnx(inputs+size) 失败({ret})，尝试仅指定 input_size_list...")
        rknn2 = RKNN(verbose=False)
        rknn2.config(
            target_platform=PLATFORM,
            optimization_level=3,
            single_core_mode=True,
        )
        ret = rknn2.load_onnx(model=ONNX_PATH, input_size_list=[INPUT_SHAPE])
        if ret != 0:
            print(f"  load_onnx 失败 ret={ret}")
            sys.exit(1)
        rknn.release()
        rknn = rknn2
    print("  ONNX 加载成功")

    # ── 3. 构建 ──────────────────────────────────────────────
    print(f"\n[3/4] 构建模型（{'INT8 量化' if QUANT else 'FP16'}）...")
    if QUANT:
        if not os.path.exists(DATASET):
            make_calib_dataset()
        ret = rknn.build(do_quantization=True, dataset=DATASET)
    else:
        ret = rknn.build(do_quantization=False)

    if ret != 0:
        print(f"  build 失败 ret={ret}")
        sys.exit(1)
    print("  构建完成")

    # ── 4. 导出 ──────────────────────────────────────────────
    print(f"\n[4/4] 导出 RKNN：{RKNN_OUTPUT}")
    ret = rknn.export_rknn(RKNN_OUTPUT)
    if ret != 0:
        print(f"  export_rknn 失败 ret={ret}")
        sys.exit(1)

    size_kb = os.path.getsize(RKNN_OUTPUT) / 1024
    print(f"  导出成功：{RKNN_OUTPUT}  ({size_kb:.1f} KB)")

    rknn.release()

    print()
    print("=" * 60)
    print("转换完成！")
    print("=" * 60)
    print(f"  输出文件：{RKNN_OUTPUT}")
    print(f"  复制到项目：cp {RKNN_OUTPUT} /mnt/d/Hvideo/models/")
    print("  重新构建 APK 后部署到 RK3568 设备")
    print()
    print("注意：C++ 端需同步修改 BUFFER_SAMPLES = 8192")
    print("  文件：include/effect/WaveBeatDetector.h")
    print("  原：static constexpr int BUFFER_SAMPLES = 22050;")
    print("  改：static constexpr int BUFFER_SAMPLES = 8192;")


if __name__ == "__main__":
    convert()
