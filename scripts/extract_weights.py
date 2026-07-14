#!/usr/bin/env python3
# 编码声明：-*- coding: utf-8 -*-
"""
使用 pickle 检查从 WaveBeat checkpoint 中提取权重
该方式会完全绕过 PyTorch Lightning 模块
"""

import sys
import torch
import pickle
import io

# 修复 Unicode 编码
if sys.platform == 'win32':
    import io as sysio
    sys.stdout = sysio.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = sysio.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

class WeightExtractor:
    """只提取张量权重，忽略所有 PyTorch Lightning 元数据"""
    
    def __init__(self):
        self.weights = {}
        self.current_key = None
        
    def persistent_load(self, saved_id):
        """处理持久化 ID（storage 引用）"""
        # 返回占位对象，不需要实际加载 storage
        return None
        
    def find_class(self, module, name):
        """覆盖类解析逻辑以处理缺失模块"""
        # 对于 torch 张量和基础类型，使用真实类
        if module.startswith('torch'):
            try:
                return pickle.Unpickler.find_class(self, module, name)
            except:
                # 说明：找不到类时返回占位类
                return type(name, (), {})
        
        # 其他对象（Lightning 类等）返回占位类
        return type(name, (), {})
    
    def load(self, filename):
        """加载 checkpoint 并提取 state_dict"""
        print(f"Opening {filename}...")
        
        # 创建自定义 Unpickler 子类
        extractor = self
        
        class CustomUnpickler(pickle.Unpickler):
            def persistent_load(self, saved_id):
                # persistent load 返回 None，忽略 storage 引用
                return None
                
            def find_class(self, module, name):
                # 对于 torch 张量和基础类型，使用真实类
                if module.startswith('torch') or module.startswith('collections') or module.startswith('numpy'):
                    try:
                        return super().find_class(module, name)
                    except:
                        # 说明：找不到类时返回占位类
                        return type(name, (), {})
                
                # 其他对象（Lightning 类等）返回占位类
                return type(name, (), {})
        
        try:
            with open(filename, 'rb') as f:
                checkpoint = CustomUnpickler(f).load()
            
            print(f"✓ Checkpoint loaded successfully")
            
            # 说明：如果存在 state_dict，则提取它
            if isinstance(checkpoint, dict):
                if 'state_dict' in checkpoint:
                    return checkpoint['state_dict']
                else:
                    print(f"  Available keys: {list(checkpoint.keys())[:10]}")
                    return checkpoint
            else:
                print(f"  Checkpoint type: {type(checkpoint)}")
                return {}
                
        except Exception as e:
            print(f"✗ Failed to extract: {e}")
            import traceback
            traceback.print_exc()
            return None

if __name__ == "__main__":
    extractor = WeightExtractor()
    state_dict = extractor.load("wavebeat_epoch=98-step=24749.ckpt")
    
    if state_dict:
        print(f"\n✓ Extracted {len(state_dict)} items")
        print(f"Sample keys:")
        for i, key in enumerate(list(state_dict.keys())[:10]):
            value = state_dict[key]
            if hasattr(value, 'shape'):
                print(f"  [{i+1}] {key}: {value.shape}")
            else:
                print(f"  [{i+1}] {key}: {type(value)}")
        
        # 保存提取的权重
        torch.save(state_dict, "wavebeat_weights_only.pth")
        print(f"\n✓ Saved weights to wavebeat_weights_only.pth")
    else:
        print(f"\n✗ Failed to extract weights")
