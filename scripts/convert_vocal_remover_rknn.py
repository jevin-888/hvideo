#!/usr/bin/env python3
# 编码声明：-*- coding: utf-8 -*-
"""
消原唱 ONNX → RKNN 转换脚本
必须在 Linux / WSL 下运行

用法：
  python3 convert_vocal_remover_rknn.py rk3588
  python3 convert_vocal_remover_rknn.py rk3568
  QUANT=1 python3 convert_vocal_remover_rknn.py rk3568   # INT8 量化
"""

import os, sys

if os.name == 'nt':
    print("请在 WSL / Linux 下运行")
    sys.exit(1)

from rknn.api import RKNN

CONFIGS = {
    "rk3588": {
        "onnx":  "vocal_remover_rk3588.onnx",
        "rknn":  "vocal_remover_rk3588.rknn",
        "shape": [1, 2, 512, 128],
        "single_core": False,
    },
    "rk3568": {
        "onnx":  "vocal_remover_rk3568.onnx",
        "rknn":  "vocal_remover_rk3568.rknn",
        "shape": [1, 2, 512, 64],
        "single_core": True,    # RK3568 只有 1 个 NPU 核
    },
}


def make_calib(shape, n=20, path="vocal_remover_calib.npy"):
    import numpy as np
    data = np.random.randn(n, *shape).astype("float32")
    np.save(path, data)
    txt = path.replace(".npy", ".txt")
    with open(txt, "w") as f:
        f.write(path + "\n")
    return txt


def convert(platform: str):
    cfg   = CONFIGS[platform]
    quant = os.environ.get("QUANT", "") in ("1", "yes", "true")

    if not os.path.exists(cfg["onnx"]):
        print(f"找不到 ONNX：{cfg['onnx']}")
        print("请先运行：python3 export_vocal_remover.py", platform)
        sys.exit(1)

    print(f"\n{'='*55}")
    print(f"ONNX → RKNN  平台：{platform.upper()}")
    print(f"量化：{'INT8' if quant else 'FP16'}")
    print(f"输入：{cfg['onnx']}  形状：{cfg['shape']}")
    print('='*55)

    rknn = RKNN(verbose=False)

    # 1. 配置
    rknn.config(
        target_platform=platform,
        mean_values=None,
        std_values=None,
        optimization_level=3,
        single_core_mode=cfg["single_core"],
    )

    # 2. 加载 ONNX
    ret = rknn.load_onnx(
        model=cfg["onnx"],
        inputs=["input"],
        input_size_list=[cfg["shape"]],
    )
    if ret != 0:
        # 部分版本不支持同时传 inputs+size，降级重试
        rknn.release()
        rknn = RKNN(verbose=False)
        rknn.config(target_platform=platform, optimization_level=3,
                    single_core_mode=cfg["single_core"])
        ret = rknn.load_onnx(model=cfg["onnx"],
                             input_size_list=[cfg["shape"]])
    if ret != 0:
        print(f"load_onnx 失败 ret={ret}")
        sys.exit(1)
    print("ONNX 加载成功")

    # 3. 构建
    if quant:
        dataset = make_calib(cfg["shape"])
        ret = rknn.build(do_quantization=True, dataset=dataset)
    else:
        ret = rknn.build(do_quantization=False)
    if ret != 0:
        print(f"build 失败 ret={ret}")
        sys.exit(1)
    print("构建完成")

    # 4. 导出
    ret = rknn.export_rknn(cfg["rknn"])
    if ret != 0:
        print(f"export_rknn 失败 ret={ret}")
        sys.exit(1)

    size_kb = os.path.getsize(cfg["rknn"]) / 1024
    print(f"\n导出成功：{cfg['rknn']}  ({size_kb:.1f} KB)")
    print(f"部署：cp {cfg['rknn']} /mnt/d/Hvideo/models/")
    rknn.release()


if __name__ == "__main__":
    targets = sys.argv[1:] if len(sys.argv) > 1 else ["rk3588", "rk3568"]
    for t in targets:
        if t not in CONFIGS:
            print(f"未知平台：{t}")
            continue
        convert(t)
    print("\n全部转换完成！")
