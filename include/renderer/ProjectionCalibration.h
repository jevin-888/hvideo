/**
 * @file ProjectionCalibration.h（文件名）
 * @brief 投影仪校正参数与矩阵计算
 *
 * 基于 Vulkan 投影融合文档，实现几何校正（平移/缩放/旋转/梯形）与融合参数定义。
 * 无 Vulkan 依赖，可独立测试。
 */

#ifndef HSVJ_PROJECTION_CALIBRATION_H
#define HSVJ_PROJECTION_CALIBRATION_H

#include <cmath>

namespace hsvj {

/** 2D 向量（投影模块自包含，避免循环依赖） */
struct ProjVec2 {
  float x = 0.0f;
  float y = 0.0f;
  ProjVec2() = default;
  ProjVec2(float x_, float y_) : x(x_), y(y_) {}
};

/** 4x4 矩阵，列主序 (column-major)，与 Vulkan/OpenGL 一致 */
struct ProjMat4 {
  float m[16];
  ProjMat4() {
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
  }
};

/**
 * @brief 单个投影仪校正参数
 */
struct ProjectorCalibration {
  float fov = 60.0f;                // 垂直视场角（度）
    float aspectRatio = 16.0f / 9.0f; // 宽高比
    float nearPlane = 0.1f;           // 近裁剪面
    float farPlane = 100.0f;          // 远裁剪面

  ProjVec2 offset{0.0f, 0.0f};      // 平移偏移（归一化 [-1,1]）
  ProjVec2 scale{1.0f, 1.0f};       // 缩放系数
    float rotate = 0.0f;              // 旋转角度（弧度）
  ProjVec2 keystone{0.0f, 0.0f};    // 梯形畸变系数

  ProjVec2 blendArea{0.0f, 0.0f};   // 融合带 [起始位置, 宽度]

  ProjMat4 calibratedProjMatrix;    // 预计算校正矩阵
};

/**
 * @brief 计算带校正的投影矩阵
 *
 * 顺序：baseProj * correction（透视 * 平移 * 旋转 * 缩放 * 梯形剪切）
 * 适配 Vulkan 左手坐标系（Y 轴翻转）
 *
 * @param config 投影仪校正参数，结果写入 calibratedProjMatrix
 */
void calculateCalibratedProjectionMatrix(ProjectorCalibration& config);

}  // 命名空间 hsvj

#endif  // 结束 HSVJ_PROJECTION_CALIBRATION_H
