/**
 * @file CaveProjection.h（文件名）
 * @brief CAVE 多墙沉浸式投影 - 离轴透视计算
 *
 * 参考 EVL Pape 算法，根据墙面角点计算 View 与 Projection 矩阵。
 * 眼位使用固定默认值（墙面中心沿法向后退）。
 */

#ifndef HSVJ_CAVE_PROJECTION_H
#define HSVJ_CAVE_PROJECTION_H

#include "renderer/ProjectionCalibration.h"
#include <cmath>

namespace hsvj {

/** 3D 向量 */
struct CaveVec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  CaveVec3() = default;
  CaveVec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

/** 墙面类型 */
enum class CaveWallType {
  FRONT = 0,
  LEFT = 1,
  RIGHT = 2,
  FLOOR = 3,
  CEILING = 4
};

/** 单墙面几何配置（CAVE space 坐标） */
struct CaveWallConfig {
  CaveWallType wallType = CaveWallType::FRONT;
  CaveVec3 cornerLL;  // 左下角
  CaveVec3 cornerUL;  // 左上角
  CaveVec3 cornerLR;  // 右下角
    float nearPlane = 0.1f;
  float farPlane = 100.0f;
};

/** 离轴投影结果 */
struct CaveProjectionResult {
  ProjMat4 viewMatrix;
  ProjMat4 projectionMatrix;
};

/**
 * @brief 计算 CAVE 离轴投影矩阵
 *
 * 眼位 = 墙面中心 + 沿法向后退 defaultEyeDistance
 *
 * @param wallConfig 墙面几何
 * @param defaultEyeDistance 眼位沿法向后退距离（与角点同单位）
 * @param result 输出 viewMatrix、projectionMatrix
 * @return 成功返回 true
 */
bool computeCaveProjection(const CaveWallConfig& wallConfig,
                           float defaultEyeDistance,
                           CaveProjectionResult& result);

}  // 命名空间 hsvj

#endif  // 结束 HSVJ_CAVE_PROJECTION_H
