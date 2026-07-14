/**
 * @file CaveProjectionCalculator.cpp（文件名）
 * @brief CAVE 离轴投影计算实现
 *
 * 参考 EVL Pape: Computing the CAVE Projection Transformation
 * 说明：https://www.evl.uic.edu/pape/caveproj/
 */

#include "renderer/CaveProjection.h"
#include <cstring>
#include <cmath>

namespace hsvj {

namespace {

static void vec3Sub(const CaveVec3& a, const CaveVec3& b, CaveVec3& out) {
  out.x = a.x - b.x;
  out.y = a.y - b.y;
  out.z = a.z - b.z;
}

static float vec3Dot(const CaveVec3& a, const CaveVec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static void vec3Cross(const CaveVec3& a, const CaveVec3& b, CaveVec3& out) {
  out.x = a.y * b.z - a.z * b.y;
  out.y = a.z * b.x - a.x * b.z;
  out.z = a.x * b.y - a.y * b.x;
}

static float vec3Len(const CaveVec3& v) {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static void vec3Norm(const CaveVec3& v, CaveVec3& out) {
  float len = vec3Len(v);
  if (len < 1e-8f) len = 1.0f;
  out.x = v.x / len;
  out.y = v.y / len;
  out.z = v.z / len;
}

// 3x3 矩阵求逆（用于旋转部分）
static void mat3Inverse(const float* m, float* out) {
  float det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
              m[1] * (m[3] * m[8] - m[5] * m[6]) +
              m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (std::fabs(det) < 1e-10f) det = 1.0f;
  float invDet = 1.0f / det;
  out[0] = (m[4] * m[8] - m[5] * m[7]) * invDet;
  out[1] = (m[2] * m[7] - m[1] * m[8]) * invDet;
  out[2] = (m[1] * m[5] - m[2] * m[4]) * invDet;
  out[3] = (m[5] * m[6] - m[3] * m[8]) * invDet;
  out[4] = (m[0] * m[8] - m[2] * m[6]) * invDet;
  out[5] = (m[2] * m[3] - m[0] * m[5]) * invDet;
  out[6] = (m[3] * m[7] - m[4] * m[6]) * invDet;
  out[7] = (m[1] * m[6] - m[0] * m[7]) * invDet;
  out[8] = (m[0] * m[4] - m[1] * m[3]) * invDet;
}

// 创建 frustum 投影矩阵（列主序）
static void frustum(float left, float right, float bottom, float top,
                    float near, float far, ProjMat4& out) {
  std::memset(out.m, 0, sizeof(out.m));
  out.m[0] = 2.0f * near / (right - left);
  out.m[5] = 2.0f * near / (top - bottom);
  out.m[8] = (right + left) / (right - left);
  out.m[9] = (top + bottom) / (top - bottom);
  out.m[10] = far / (near - far);
  out.m[11] = -1.0f;
  out.m[14] = far * near / (near - far);
}

}  // 命名空间

bool computeCaveProjection(const CaveWallConfig& wallConfig,
                           float defaultEyeDistance,
                           CaveProjectionResult& result) {
  CaveVec3 rightV, upV;
  vec3Sub(wallConfig.cornerLR, wallConfig.cornerLL, rightV);
  vec3Sub(wallConfig.cornerUL, wallConfig.cornerLL, upV);

  float width = vec3Len(rightV);
  float height = vec3Len(upV);
  if (width < 1e-8f || height < 1e-8f)
    return false;

  CaveVec3 Xs, Ys, Zs;
  vec3Norm(rightV, Xs);
  vec3Norm(upV, Ys);
  vec3Cross(Xs, Ys, Zs);
  float zLen = vec3Len(Zs);
  if (zLen < 1e-8f)
    return false;
  Zs.x /= zLen;
  Zs.y /= zLen;
  Zs.z /= zLen;

  CaveVec3 center;
  center.x = wallConfig.cornerLL.x + 0.5f * rightV.x + 0.5f * upV.x;
  center.y = wallConfig.cornerLL.y + 0.5f * rightV.y + 0.5f * upV.y;
  center.z = wallConfig.cornerLL.z + 0.5f * rightV.z + 0.5f * upV.z;

  CaveVec3 eye;
  eye.x = center.x + defaultEyeDistance * Zs.x;
  eye.y = center.y + defaultEyeDistance * Zs.y;
  eye.z = center.z + defaultEyeDistance * Zs.z;

  CaveVec3 eyes;
  vec3Sub(eye, wallConfig.cornerLL, eyes);

  float L = vec3Dot(eyes, Xs);
  float R = width - L;
  float B = vec3Dot(eyes, Ys);
  float T = height - B;
  float distance = vec3Dot(eyes, Zs);
  if (std::fabs(distance) < 1e-8f)
    return false;

  float near = wallConfig.nearPlane;
  float far = wallConfig.farPlane;
  float left = -L * near / distance;
  float right = R * near / distance;
  float bottom = -B * near / distance;
  float top = T * near / distance;

  frustum(left, right, bottom, top, near, far, result.projectionMatrix);

  float rotMat[9] = {Xs.x, Xs.y, Xs.z, Ys.x, Ys.y, Ys.z, Zs.x, Zs.y, Zs.z};
  float rotInv[9];
  mat3Inverse(rotMat, rotInv);

  ProjMat4 view;
  std::memset(view.m, 0, sizeof(view.m));
  view.m[0] = rotInv[0];
  view.m[1] = rotInv[1];
  view.m[2] = rotInv[2];
  view.m[4] = rotInv[3];
  view.m[5] = rotInv[4];
  view.m[6] = rotInv[5];
  view.m[8] = rotInv[6];
  view.m[9] = rotInv[7];
  view.m[10] = rotInv[8];
  view.m[12] = -(rotInv[0] * eye.x + rotInv[3] * eye.y + rotInv[6] * eye.z);
  view.m[13] = -(rotInv[1] * eye.x + rotInv[4] * eye.y + rotInv[7] * eye.z);
  view.m[14] = -(rotInv[2] * eye.x + rotInv[5] * eye.y + rotInv[8] * eye.z);
  view.m[15] = 1.0f;

  result.viewMatrix = view;
  return true;
}

}  // 命名空间 hsvj
