/**
 * @file ProjectionMatrixCalculator.cpp（文件名）
 * @brief 投影校正矩阵计算实现
 *
 * 纯 CPU 实现，无 Vulkan 依赖。矩阵顺序与文档一致。
 */

#include "renderer/ProjectionCalibration.h"
#include <cstring>

namespace hsvj {

namespace {

void mat4Multiply(const ProjMat4& a, const ProjMat4& b, ProjMat4& out) {
  ProjMat4 tmp;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0;
      for (int k = 0; k < 4; k++) {
        sum += a.m[k * 4 + row] * b.m[col * 4 + k];
      }
      tmp.m[col * 4 + row] = sum;
    }
  }
  std::memcpy(out.m, tmp.m, sizeof(out.m));
}

// 透视投影矩阵（列主序），Vulkan Y 轴翻转
void perspective(float fovDeg, float aspect, float near, float far,
                 ProjMat4& out) {
  const float fovRad = fovDeg * 3.14159265f / 180.0f;
  const float tanHalfFov = std::tan(fovRad * 0.5f);
  const float yScale = 1.0f / tanHalfFov;
  const float xScale = yScale / aspect;
  const float nf = 1.0f / (near - far);

  std::memset(out.m, 0, sizeof(out.m));
  out.m[0] = xScale;
  out.m[5] = -yScale;  // Vulkan Y 翻转
  out.m[10] = far * nf;
  out.m[11] = -1.0f;
  out.m[14] = far * near * nf;
}

// 平移矩阵
void translate(float x, float y, float z, ProjMat4& out) {
  std::memset(out.m, 0, sizeof(out.m));
  out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
  out.m[12] = x;
  out.m[13] = y;
  out.m[14] = z;
}

// Z 轴旋转（弧度）
void rotateZ(float angle, ProjMat4& out) {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  std::memset(out.m, 0, sizeof(out.m));
  out.m[0] = c;  out.m[1] = s;
  out.m[4] = -s; out.m[5] = c;
  out.m[10] = 1;
  out.m[15] = 1;
}

// 缩放矩阵
void scale(float x, float y, float z, ProjMat4& out) {
  std::memset(out.m, 0, sizeof(out.m));
  out.m[0] = x;
  out.m[5] = y;
  out.m[10] = z;
  out.m[15] = 1;
}

// 梯形剪切矩阵：keystoneMat[0][1]=kx, keystoneMat[1][0]=ky
void keystone(float kx, float ky, ProjMat4& out) {
  std::memset(out.m, 0, sizeof(out.m));
  out.m[0] = 1;  out.m[1] = ky;  // 第 0 列
  out.m[4] = kx; out.m[5] = 1;   // 第 1 列
  out.m[10] = 1;
  out.m[15] = 1;
}

}  // 命名空间

void calculateCalibratedProjectionMatrix(ProjectorCalibration& config) {
  ProjMat4 baseProj;
  perspective(config.fov, config.aspectRatio, config.nearPlane,
              config.farPlane, baseProj);

  ProjMat4 correction;
  correction.m[0] = correction.m[5] = correction.m[10] = correction.m[15] = 1.0f;

  ProjMat4 T, R, S, K;

  translate(config.offset.x, config.offset.y, 0.0f, T);
  mat4Multiply(T, correction, correction);

  rotateZ(config.rotate, R);
  mat4Multiply(R, correction, correction);

  scale(config.scale.x, config.scale.y, 1.0f, S);
  mat4Multiply(S, correction, correction);

  keystone(config.keystone.x, config.keystone.y, K);
  mat4Multiply(correction, K, correction);  // 字段说明：correction = correction * K

  mat4Multiply(baseProj, correction, config.calibratedProjMatrix);
}

}  // 命名空间 hsvj
