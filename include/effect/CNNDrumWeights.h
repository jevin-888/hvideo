/**
 * @file CNNDrumWeights.h（文件名）
 * @brief 嵌入式 CNN 权重（占位 / 训练后生成）
 *
 * ─── 状态 ───
 *   当前是 PLACEHOLDER：所有权重为 0，CNN_MODEL_TRAINED = false。
 *   CNNDrumDetector 检测到未训练时会一直输出 0.5 概率，触发被绕过。
 *
 * ─── 生成真权重 ───
 *   1) 准备数据：
 *      - 推荐 MDB-Drums 数据集：https://github.com/CarlSouthall/MDBDrums
 *      - 或自录鼓声样本，配套 onset 时间戳标注
 *   2) 训练 + 导出：
 *      cd scripts && python train_drum_cnn.py --数据 <路径> --out CNNDrumWeights.h
 *   3) 用生成的文件替换本文件，重新编译即可。
 *
 * ─── 数组形状（必须与 CNNDrumDetector.cpp 里前向计算一致）───
 *   示例/字段：CONV1_W:  [CNN_CONV1_OUT][CNN_NUM_MEL][CNN_CONV_KERNEL]    = [20][80][3]
 *   示例/字段：CONV1_B:  [CNN_CONV1_OUT]                                  = [20]
 *   示例/字段：CONV2_W:  [CNN_CONV2_OUT][CNN_CONV1_OUT][CNN_CONV_KERNEL]  = [20][20][3]
 *   示例/字段：CONV2_B:  [CNN_CONV2_OUT]                                  = [20]
 *   示例/字段：DENSE_HIDDEN_W: [CNN_DENSE_HIDDEN][CNN_FLATTEN_DIM]        = [32][220]
 *   示例/字段：DENSE_HIDDEN_B: [CNN_DENSE_HIDDEN]                         = [32]
 *   示例/字段：DENSE_OUT_W:    [CNN_NUM_CLASSES][CNN_DENSE_HIDDEN]        = [3][32]
 *   示例/字段：DENSE_OUT_B:    [CNN_NUM_CLASSES]                          = [3]
 *
 *   字段说明：CNN_FLATTEN_DIM = CNN_CONV2_OUT × (CNN_CONTEXT_FRAMES - 2*(CNN_CONV_KERNEL-1))
 *                   = 20 × (15 - 4) = 20 × 11 = 220
 *
 * 所有权重均为 NCHW 序、float32，与 PyTorch nn.Conv1d 默认布局一致。
 */
#ifndef HSVJ_CNN_DRUM_WEIGHTS_H
#define HSVJ_CNN_DRUM_WEIGHTS_H

#include "effect/CNNDrumDetector.h"

namespace hsvj {

// 推导：valid 卷积每层时间维减 (kernel-1)，两层后 15 - 2*2 = 11
inline constexpr int CNN_TIME_OUT     = CNN_CONTEXT_FRAMES - 2 * (CNN_CONV_KERNEL - 1);
inline constexpr int CNN_FLATTEN_DIM  = CNN_CONV2_OUT * CNN_TIME_OUT;  // = 220

// ─── 模型已训练标志 ───
// 若 false：CNNDrumDetector 输出固定 0.5 概率，触发被外部逻辑绕过。
// 训练脚本生成本文件时会自动置为 true。
inline constexpr bool CNN_MODEL_TRAINED = false;

// ─── 权重数据 ───
// 占位为全零。训练脚本会用 Python 把真权重一次性写入此处。
extern const float CNN_CONV1_W[CNN_CONV1_OUT][CNN_NUM_MEL][CNN_CONV_KERNEL];
extern const float CNN_CONV1_B[CNN_CONV1_OUT];
extern const float CNN_CONV2_W[CNN_CONV2_OUT][CNN_CONV1_OUT][CNN_CONV_KERNEL];
extern const float CNN_CONV2_B[CNN_CONV2_OUT];
extern const float CNN_DENSE_HIDDEN_W[CNN_DENSE_HIDDEN][CNN_FLATTEN_DIM];
extern const float CNN_DENSE_HIDDEN_B[CNN_DENSE_HIDDEN];
extern const float CNN_DENSE_OUT_W[CNN_NUM_CLASSES][CNN_DENSE_HIDDEN];
extern const float CNN_DENSE_OUT_B[CNN_NUM_CLASSES];

} // 命名空间 hsvj

#endif // 结束 HSVJ_CNN_DRUM_WEIGHTS_H
