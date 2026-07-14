/**
 * @file CNNDrumWeights.cpp（文件名）
 * @brief CNN 权重占位实现（全零）。
 *
 * ─ 当前状态 ─
 *   所有权重为 0，搭配 CNNDrumWeights.h 中 CNN_MODEL_TRAINED = false。
 *   CNNDrumDetector 检测到未训练标志后输出中性概率（0.5）并禁用触发，
 *   Effect管理器 自动回退到 DSP kick-band 检测路径。
 *
 * ─ 如何启用 CNN ─
 *   1) 说明：cd scripts
 *   2) python train_drum_cnn.py --data <MDB-Drums 根目录> --epochs 30 \
 *          说明：--out ../src/effect/CNNDrumWeights.cpp
 *   该脚本会：
 *   - 用真权重覆盖本文件（~170KB）
 *   - 把 CNNDrumWeights.h 里 CNN_MODEL_TRAINED 改为 true
 *   3) 重新编译 C++ 项目
 *
 * ─ 数组形状 ─
 *   详见 CNNDrumWeights.h 注释块。所有 extern 声明全部在此文件做零初始化定义。
 */
#include "effect/CNNDrumWeights.h"

namespace hsvj {

// C++ 标准保证 const float[N] = {} 全零
const float CNN_CONV1_W[CNN_CONV1_OUT][CNN_NUM_MEL][CNN_CONV_KERNEL] = {};
const float CNN_CONV1_B[CNN_CONV1_OUT] = {};
const float CNN_CONV2_W[CNN_CONV2_OUT][CNN_CONV1_OUT][CNN_CONV_KERNEL] = {};
const float CNN_CONV2_B[CNN_CONV2_OUT] = {};
const float CNN_DENSE_HIDDEN_W[CNN_DENSE_HIDDEN][CNN_FLATTEN_DIM] = {};
const float CNN_DENSE_HIDDEN_B[CNN_DENSE_HIDDEN] = {};
const float CNN_DENSE_OUT_W[CNN_NUM_CLASSES][CNN_DENSE_HIDDEN] = {};
const float CNN_DENSE_OUT_B[CNN_NUM_CLASSES] = {};

} // 命名空间 hsvj
