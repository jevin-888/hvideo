#!/usr/bin/env python3
"""
WaveBeat 模型转换脚本
PyTorch → ONNX → RKNN

使用方法:
1. 安装依赖: pip install torch onnx rknn-toolkit2
2. 下载 WaveBeat 模型:
   wget https://zenodo.org/record/5525120/files/wavebeat_epoch%3D98-step%3D24749.ckpt
3. 运行:
   python convert_wavebeat.py              # 输出 wavebeat_rk3588.rknn (RK3588)
   python convert_wavebeat.py --target rk3568   # 输出 wavebeat_rk3568.rknn (RK3566/RK3568)

注意: RKNN 转换需在 Linux 或 WSL 下运行（rknn-toolkit2 官方支持）。
"""

import os
import sys
import argparse
import torch
import torch.onnx

# ============ 配置（可通过 --target 覆盖） ============
CHECKPOINT_PATH = "wavebeat_epoch=98-step=24749.ckpt"
ONNX_PATH = "wavebeat.onnx"
SAMPLE_RATE = 22050
AUDIO_LENGTH = 22050  # 1秒

def get_target_config():
    """解析命令行 --target，返回 (TARGET_PLATFORM, RKNN_PATH)"""
    parser = argparse.ArgumentParser(description="WaveBeat PyTorch -> ONNX -> RKNN")
    parser.add_argument("--target", choices=["rk3588", "rk3568"], default="rk3588",
                        help="目标 NPU：rk3588 或 rk3568（RK3566/RK3568 设备用 rk3568）")
    args = parser.parse_args()
    target = args.target
    return target, f"wavebeat_{target}.rknn"

def check_dependencies():
    """检查依赖是否安装"""
    try:
        from rknn.api import RKNN
        print("✓ RKNN Toolkit 已安装")
    except ImportError:
        print("✗ 请安装 rknn-toolkit2: pip install rknn-toolkit2")
        sys.exit(1)

def step1_download_model():
    """下载 WaveBeat 模型"""
    if os.path.exists(CHECKPOINT_PATH):
        print(f"✓ 模型文件已存在: {CHECKPOINT_PATH}")
        return

    print("下载 WaveBeat 模型...")
    import urllib.request
    url = "https://zenodo.org/record/5525120/files/wavebeat_epoch%3D98-step%3D24749.ckpt?download=1"
    urllib.request.urlretrieve(url, CHECKPOINT_PATH)
    print(f"✓ 下载完成: {CHECKPOINT_PATH}")

def step2_convert_to_onnx():
    """PyTorch → ONNX"""
    if os.path.exists(ONNX_PATH):
        print(f"✓ ONNX 文件已存在: {ONNX_PATH}")
        return

    print("转换为 ONNX...")
    
    # 加载 WaveBeat 模型
    # 注意：需要 wavebeat 包，或手动定义模型结构
    try:
        from wavebeat.model import WaveBeat
        model = WaveBeat.load_from_checkpoint(CHECKPOINT_PATH)
    except ImportError:
        print("警告: wavebeat 包未安装，尝试直接加载 checkpoint...")
        checkpoint = torch.load(CHECKPOINT_PATH, map_location='cpu')
        # 简化版：假设标准 TCN 结构
        # 实际使用时需要匹配 WaveBeat 模型定义
        print("✗ 需要 wavebeat 包来加载模型。请运行:")
        print("   git clone https://github.com/csteinmetz1/wavebeat.git")
        print("   cd wavebeat && pip install -e .")
        sys.exit(1)
    
    model.eval()
    
    # 创建示例输入
    dummy_input = torch.randn(1, 1, AUDIO_LENGTH)
    
    # 导出 ONNX
    torch.onnx.export(
        model,
        dummy_input,
        ONNX_PATH,
        input_names=["audio"],
        output_names=["beat_probs", "downbeat_probs"],
        dynamic_axes={
            "audio": {2: "samples"},
            "beat_probs": {1: "timesteps"},
            "downbeat_probs": {1: "timesteps"}
        },
        opset_version=12
    )
    
    print(f"✓ ONNX 转换完成: {ONNX_PATH}")

def step3_convert_to_rknn(target_platform, rknn_path):
    """ONNX → RKNN"""
    from rknn.api import RKNN
    
    print(f"转换为 RKNN (目标平台: {target_platform})...")
    
    rknn = RKNN()
    
    # 配置
    rknn.config(
        target_platform=target_platform,
        mean_values=[[0]],
        std_values=[[1]],
        optimization_level=3
    )
    
    # 加载 ONNX（固定输入形状以兼容 RKNN）
    ret = rknn.load_onnx(model=ONNX_PATH, inputs=['audio'], input_size_list=[[1, 1, AUDIO_LENGTH]])
    if ret != 0:
        ret = rknn.load_onnx(model=ONNX_PATH)
    if ret != 0:
        print(f"✗ 加载 ONNX 失败: {ret}")
        sys.exit(1)
    
    # 构建（FP16，稳定）
    ret = rknn.build(do_quantization=False)
    if ret != 0:
        print(f"✗ 构建 RKNN 失败: {ret}")
        sys.exit(1)
    
    # 导出
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        print(f"✗ 导出 RKNN 失败: {ret}")
        sys.exit(1)
    
    print(f"✓ RKNN 转换完成: {rknn_path}")
    
    # 清理
    rknn.release()

def main():
    TARGET_PLATFORM, RKNN_PATH = get_target_config()
    
    print("=" * 50)
    print("WaveBeat 模型转换工具")
    print(f"  目标平台: {TARGET_PLATFORM} -> {RKNN_PATH}")
    print("=" * 50)
    
    check_dependencies()
    step1_download_model()
    step2_convert_to_onnx()
    step3_convert_to_rknn(TARGET_PLATFORM, RKNN_PATH)
    
    print("\n" + "=" * 50)
    print("✓ 转换完成！")
    print(f"  输出文件: {RKNN_PATH}")
    print(f"  部署: 复制到项目 models/ 或 scripts/ 后重新构建 APK")
    print("=" * 50)

if __name__ == "__main__":
    main()
