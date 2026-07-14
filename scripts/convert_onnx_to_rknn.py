#!/usr/bin/env python3
"""
Convert ONNX to RKNN
This script must be run on a Linux system with RKNN Toolkit 2 installed.

Usage:
  pip install rknn-toolkit2 "onnx==1.18.0"   # 需 onnx<=1.18，否则会报 module 'onnx' has no attribute 'mapping'
  python convert_onnx_to_rknn.py              # -> wavebeat_rk3588.rknn
  TARGET=rk3568 python convert_onnx_to_rknn.py  # -> wavebeat_rk3568.rknn (for RK3566/RK3568)
  QUANT=1 TARGET=rk3568 python convert_onnx_to_rknn.py  # RK3568 量化版（需 wavebeat_dataset.txt + 校准 npy）

Input: wavebeat.onnx
Output: wavebeat_rk3588.rknn or wavebeat_rk3568.rknn
"""

from rknn.api import RKNN

def convert_to_rknn(onnx_path="wavebeat.onnx", output_path=None, target_platform="rk3588", do_quantization=False, dataset_path=None):
    """Convert ONNX model to RKNN format. target_platform: rk3588 or rk3568. do_quantization: use INT8 quant (needs dataset_path)."""
    import os
    if output_path is None:
        output_path = f"wavebeat_{target_platform}.rknn"
    
    print("=" * 60)
    print(f"ONNX to RKNN Conversion for {target_platform.upper()}")
    if do_quantization:
        print("  (INT8 quantization enabled)")
    print("=" * 60)
    
    # 创建 RKNN instance
    rknn = RKNN()
    
    # 配置目标平台
    print(f"\nConfiguring for {target_platform.upper()} target platform...")
    ret = rknn.config(
        target_platform=target_platform,
        optimization_level=3,
        mean_values=[[0]],  # No normalization needed, audio is pre-normalized
        std_values=[[1]]
    )
    if ret != 0:
        print(f"✗ Configuration failed: {ret}")
        return False
    print(f"✓ Configuration complete")
    
    # Load ONNX model（wavebeat 输入为 [1,1,samples] 动态维，RKNN 需固定：1 秒 @ 22050Hz = 22050）
    print(f"\nLoading ONNX model: {onnx_path}")
    ret = rknn.load_onnx(
        model=onnx_path,
        inputs=['audio'],
        input_size_list=[[1, 1, 22050]]
    )
    if ret != 0:
        print(f"✗ Failed to load ONNX: {ret}")
        return False
    print(f"✓ ONNX model loaded")
    
    # Build: FP16 or INT8 (quantization for RK3568 can yield smaller and some时间s more stable model)
    if do_quantization and dataset_path and os.path.isfile(dataset_path):
        print(f"\nBuilding RKNN model (INT8 quantization, dataset={dataset_path})...")
        ret = rknn.build(do_quantization=True, dataset=dataset_path)
    else:
        if do_quantization and (not dataset_path or not os.path.isfile(dataset_path)):
            print(f"\nWARNING: QUANT=1 but dataset not found ({dataset_path}), falling back to FP16")
        print(f"\nBuilding RKNN model (FP16 mode)...")
        ret = rknn.build(do_quantization=False)
    if ret != 0:
        print(f"✗ Build failed: {ret}")
        return False
    print(f"✓ Build complete")
    
    # 导出 RKNN 模型
    print(f"\nExporting to: {output_path}")
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        print(f"✗ Export failed: {ret}")
        return False
    print(f"✓ Export complete")
    
    # 获取 file 大小
    import os
    size_mb = os.path.getsize(output_path) / (1024 * 1024)
    print(f"  File size: {size_mb:.2f} MB")
    
    # 清理
    rknn.release()
    
    print("\n" + "=" * 60)
    print("Conversion Complete!")
    print("=" * 60)
    print(f"\nDeployment:")
    print(f"  adb push {output_path} /data/local/tmp/models/")
    print(f"\nC++ Usage:")
    print(f"  WaveBeatDetector detector;")
    print(f"  detector.init(\"/data/local/tmp/models/{output_path}\");")
    print("=" * 60)
    
    return True

if __name__ == "__main__":
    import os
    if os.name == 'nt':
        print("✗ This script requires Linux and RKNN Toolkit 2")
        print("  Please run on a Linux machine or WSL")
        exit(1)
    target = os.environ.get("TARGET", "rk3588")
    if target not in ("rk3588", "rk3568"):
        target = "rk3588"
    quant = os.environ.get("QUANT", "").strip() in ("1", "yes", "true")
    dataset_path = "wavebeat_dataset.txt" if quant else None
    convert_to_rknn(target_platform=target, do_quantization=quant, dataset_path=dataset_path)
