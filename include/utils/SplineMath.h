#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace hsvj {

/**
 * @brief 高性能样条插值器 (借鉴自 huoshanVJ geo_curve)
 * 专门用于在 CPU 侧生成平滑的几何网格和遮罩边界
 */
class SplineInterpolator {
public:
    struct Point {
        float x, y;
    };

    static std::vector<float> createEvenGridFromCorners(const std::vector<float>& controlPoints, int rows, int cols, int nextRows, int nextCols) {
        auto getCP = [&](int r, int c, float fallbackX, float fallbackY) -> Point {
            int base = (r * cols + c) * 2;
            if (base + 1 < (int)controlPoints.size()) return {controlPoints[base], controlPoints[base + 1]};
            return {fallbackX, fallbackY};
        };
        Point tl = getCP(0, 0, 0.0f, 0.0f);
        Point tr = getCP(0, std::max(0, cols - 1), 1.0f, 0.0f);
        Point bl = getCP(std::max(0, rows - 1), 0, 0.0f, 1.0f);
        Point br = getCP(std::max(0, rows - 1), std::max(0, cols - 1), 1.0f, 1.0f);
        std::vector<float> out;
        out.reserve(nextRows * nextCols * 2);
        for (int r = 0; r < nextRows; ++r) {
            float v = nextRows > 1 ? (float)r / (nextRows - 1) : 0.0f;
            for (int c = 0; c < nextCols; ++c) {
                float u = nextCols > 1 ? (float)c / (nextCols - 1) : 0.0f;
                float x = (tl.x * (1.0f - u) + tr.x * u) * (1.0f - v) + (bl.x * (1.0f - u) + br.x * u) * v;
                float y = (tl.y * (1.0f - u) + tr.y * u) * (1.0f - v) + (bl.y * (1.0f - u) + br.y * u) * v;
                out.push_back(x);
                out.push_back(y);
            }
        }
        return out;
    }

    /**
     * @brief 一维 Catmull-Rom 插值
     */
    static float catmullRom(float p0, float p1, float p2, float p3, float t) {
        float t2 = t * t;
        float t3 = t2 * t;
        return 0.5f * ((2.0f * p1) +
                       (-p0 + p2) * t +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }

    static Point legacySpline1D(const std::vector<Point>& input, float t, float xyScale) {
        int n = (int)input.size();
        if (n <= 0) return {0.0f, 0.0f};
        if (n == 1) return input[0];
        auto linearAt = [&]() -> Point {
            float clampedT = std::clamp(t, 0.0f, 1.0f);
            float src = clampedT * (n - 1);
            int idx = std::min(n - 2, std::max(0, (int)std::floor(src)));
            float local = src - (float)idx;
            const Point& a = input[idx];
            const Point& b = input[idx + 1];
            return {a.x * (1.0f - local) + b.x * local,
                    a.y * (1.0f - local) + b.y * local};
        };
        if (n == 2) {
            return linearAt();
        }
        float sx = std::abs(xyScale) > 1e-6f ? xyScale : 1.0f;
        std::vector<Point> pts(n);
        for (int i = 0; i < n; ++i) pts[i] = {input[i].x / sx, input[i].y};

        auto dist = [](const Point& a, const Point& b) {
            float dx = a.x - b.x;
            float dy = a.y - b.y;
            return std::sqrt(dx * dx + dy * dy);
        };
        constexpr float eps = 1e-6f;
        const int L = n - 1;
        const int N = n + 1;
        const int M = 4;
        const int curveTot = n + 2;
        std::vector<float> knot(n + 6, 0.0f);
        knot[0] = 0.0f;
        knot[1] = dist(pts[0], pts[1]);
        for (int i = 2; i < n; ++i) {
            knot[i] = knot[i - 1] + dist(pts[i - 1], pts[i]);
        }
        if (knot[L] <= eps) return input[0];

        std::vector<float> delta(n + 1, 0.0f);
        for (int i = 1; i < n; ++i) {
            delta[i] = knot[i] - knot[i - 1];
            if (delta[i] <= eps) return linearAt();
        }

        auto safeDiv = [](float numerator, float denominator) {
            return std::abs(denominator) > 1e-6f ? numerator / denominator : 0.0f;
        };

        std::vector<float> coeffA(n, 0.0f);
        std::vector<float> coeffB(n, 0.0f);
        std::vector<float> coeffC(n, 0.0f);
        std::vector<float> coeffP(n, 0.0f);
        std::vector<float> coeffQ(n, 0.0f);
        std::vector<float> coeffR(n, 0.0f);
        std::vector<float> coeffZ(n, 0.0f);
        std::vector<float> coeffr(n, 0.0f);

        coeffA[1] = safeDiv(delta[2] * delta[2], delta[1] + delta[2]);
        coeffB[0] = coeffB[L] = 1.0f;
        coeffB[1] = safeDiv(delta[2] * delta[1], delta[1] + delta[2]) +
                    safeDiv(delta[1] * (delta[2] + delta[3]), delta[1] + delta[2] + delta[3]);
        coeffC[0] = 0.0f;
        coeffC[1] = safeDiv(delta[1] * delta[1], delta[1] + delta[2] + delta[3]);
        for (int i = 2; i <= L - 1; ++i) {
            coeffA[i] = safeDiv(delta[i + 1] * delta[i + 1], delta[i - 1] + delta[i] + delta[i + 1]);
            coeffB[i] = safeDiv(delta[i + 1] * (delta[i - 1] + delta[i]),
                                delta[i - 1] + delta[i] + delta[i + 1]) +
                        safeDiv(delta[i] * (delta[i + 1] + delta[i + 2]),
                                delta[i] + delta[i + 1] + delta[i + 2]);
            coeffC[i] = safeDiv(delta[i] * delta[i], delta[i] + delta[i + 1] + delta[i + 2]);
        }
        coeffA[L] = 0.0f;

        coeffQ[0] = coeffB[0];
        for (int i = 0; i < L; ++i) coeffR[i] = coeffC[i];
        for (int i = 1; i <= L; ++i) {
            if (std::abs(coeffQ[i - 1]) <= eps) return linearAt();
            coeffP[i] = coeffA[i] / coeffQ[i - 1];
            coeffQ[i] = coeffB[i] - coeffP[i] * coeffC[i - 1];
        }

        Point startVec{};
        float beta = delta[1] + delta[2];
        if (std::abs(beta) <= eps) return linearAt();
        startVec.x = safeDiv(-(2.0f * delta[1] + delta[2]) * pts[0].x, delta[1] * beta) +
                     safeDiv(beta * pts[1].x, delta[1] * delta[2]) -
                     safeDiv(delta[1] * pts[2].x, delta[2] * beta);
        startVec.y = safeDiv(-(2.0f * delta[1] + delta[2]) * pts[0].y, delta[1] * beta) +
                     safeDiv(beta * pts[1].y, delta[1] * delta[2]) -
                     safeDiv(delta[1] * pts[2].y, delta[2] * beta);

        Point endVec{};
        beta = delta[L - 1] + delta[L];
        if (std::abs(beta) <= eps) return linearAt();
        endVec.x = safeDiv(delta[L] * pts[L - 2].x, delta[L - 1] * beta) -
                   safeDiv(beta * pts[L - 1].x, delta[L - 1] * delta[L]) +
                   safeDiv((2.0f * delta[L] + delta[L - 1]) * pts[L].x, beta * delta[L]);
        endVec.y = safeDiv(delta[L] * pts[L - 2].y, delta[L - 1] * beta) -
                   safeDiv(beta * pts[L - 1].y, delta[L - 1] * delta[L]) +
                   safeDiv((2.0f * delta[L] + delta[L - 1]) * pts[L].y, beta * delta[L]);

        float len = std::sqrt(startVec.x * startVec.x + startVec.y * startVec.y);
        if (len <= eps) return linearAt();
        startVec.x /= len;
        startVec.y /= len;
        len = std::sqrt(endVec.x * endVec.x + endVec.y * endVec.y);
        if (len <= eps) return linearAt();
        endVec.x /= len;
        endVec.y /= len;

        std::vector<Point> curvePts(curveTot);
        curvePts[0] = pts[0];
        curvePts[1] = {
            pts[0].x + dist(pts[0], pts[1]) * startVec.x / 3.0f,
            pts[0].y + dist(pts[0], pts[1]) * startVec.y / 3.0f
        };
        curvePts[curveTot - 2] = {
            pts[L].x - dist(pts[L - 1], pts[L]) * endVec.x / 3.0f,
            pts[L].y - dist(pts[L - 1], pts[L]) * endVec.y / 3.0f
        };
        curvePts[N] = pts[L];

        auto solveAxis = [&](bool useX) -> bool {
            auto get = [&](const Point& p) { return useX ? p.x : p.y; };
            auto set = [&](Point& p, float value) {
                if (useX) p.x = value;
                else p.y = value;
            };
            coeffr[0] = get(curvePts[1]);
            for (int i = 1; i < L; ++i) coeffr[i] = (delta[i] + delta[i + 1]) * get(pts[i]);
            coeffr[L] = get(curvePts[L + 1]);

            coeffZ[0] = coeffr[0];
            for (int i = 1; i <= L; ++i) coeffZ[i] = coeffr[i] - coeffP[i] * coeffZ[i - 1];
            if (std::abs(coeffQ[L]) <= eps) return false;
            set(curvePts[L + 1], coeffZ[L] / coeffQ[L]);
            for (int i = L - 1; i >= 0; --i) {
                if (std::abs(coeffQ[i]) <= eps) return false;
                set(curvePts[i + 1], (coeffZ[i] - coeffR[i] * get(curvePts[i + 2])) / coeffQ[i]);
            }
            return true;
        };
        if (!solveAxis(true) || !solveAxis(false)) return linearAt();

        knot[N + M] = knot[N + M - 1] = knot[N + M - 2] = knot[N + M - 3] = knot[L];
        for (int i = L - 1; i > 0; --i) knot[i + 3] = knot[i];
        knot[3] = knot[2] = knot[1] = knot[0] = 0.0f;

        float scaledT = std::clamp(t, 0.0f, 1.0f) * (n - 1);
        int seg = std::min(n - 2, std::max(0, (int)std::floor(scaledT)));
        float local = scaledT - (float)seg;
        int i = 3 + seg;
        float segmentLength = knot[i + 1] - knot[i];
        if (segmentLength <= eps) return linearAt();
        float kt = knot[i] + local * segmentLength;

        float N22 = safeDiv(knot[i + 1] - kt, knot[i + 1] - knot[i]);
        float N32 = safeDiv(kt - knot[i], knot[i + 1] - knot[i]);
        float N13 = safeDiv((knot[i + 1] - kt) * N22, knot[i + 1] - knot[i - 1]);
        float N23 = safeDiv((kt - knot[i - 1]) * N22, knot[i + 1] - knot[i - 1]) +
                    safeDiv((knot[i + 2] - kt) * N32, knot[i + 2] - knot[i]);
        float N33 = safeDiv((kt - knot[i]) * N32, knot[i + 2] - knot[i]);
        float N04 = safeDiv((knot[i + 1] - kt) * N13, knot[i + 1] - knot[i - 2]);
        float N14 = safeDiv((kt - knot[i - 2]) * N13, knot[i + 1] - knot[i - 2]) +
                    safeDiv((knot[i + 2] - kt) * N23, knot[i + 2] - knot[i - 1]);
        float N24 = safeDiv((kt - knot[i - 1]) * N23, knot[i + 2] - knot[i - 1]) +
                    safeDiv((knot[i + 3] - kt) * N33, knot[i + 3] - knot[i]);
        float N34 = safeDiv((kt - knot[i]) * N33, knot[i + 3] - knot[i]);
        float denominator = N04 + N14 + N24 + N34;
        if (std::abs(denominator) <= eps) return linearAt();

        Point out{
            (N04 * curvePts[i - 3].x + N14 * curvePts[i - 2].x +
             N24 * curvePts[i - 1].x + N34 * curvePts[i].x) / denominator,
            (N04 * curvePts[i - 3].y + N14 * curvePts[i - 2].y +
             N24 * curvePts[i - 1].y + N34 * curvePts[i].y) / denominator
        };
        return {out.x * sx, out.y};
    }

    /**
     * @brief 对 2D 网格点进行插值 (支持线性与三阶样条)
     */
    static Point getInterpolatedPoint(const std::vector<float>& controlPoints, int rows, int cols, float u, float v, bool useCubic, float xyScale = 1.0f) {
        if (rows < 2 || cols < 2) return {u, v};

        float srcC = u * (cols - 1);
        float srcR = v * (rows - 1);

        // [[Precision_Fix]] 移除 rows < 4 的强制限制。
        // 只要 useCubic 为 false，无论多少点都必须保持绝对直线。
        if (!useCubic) {
            int c0 = std::min((int)srcC, std::max(0, cols - 2));
            int r0 = std::min((int)srcR, std::max(0, rows - 2));
            float fC = srcC - (float)c0;
            float fR = srcR - (float)r0;
            auto getCP = [&](int r, int c) -> Point {
                r = std::clamp(r, 0, rows - 1);
                c = std::clamp(c, 0, cols - 1);
                int base = (r * cols + c) * 2;
                if (base + 1 < (int)controlPoints.size()) {
                    return {controlPoints[base], controlPoints[base+1]};
                }
                return {(float)c / std::max(1, cols - 1), (float)r / std::max(1, rows - 1)};
            };
            Point p00 = getCP(r0, c0), p10 = getCP(r0, c0+1), p01 = getCP(r0+1, c0), p11 = getCP(r0+1, c0+1);
            return {
                (p00.x * (1.0f-fC) + p10.x * fC)*(1.0f-fR) + (p01.x * (1.0f-fC) + p11.x * fC)*fR,
                (p00.y * (1.0f-fC) + p10.y * fC)*(1.0f-fR) + (p01.y * (1.0f-fC) + p11.y * fC)*fR
            };
        }

        auto getCP = [&](int r, int c) -> Point {
            r = std::clamp(r, 0, rows - 1);
            c = std::clamp(c, 0, cols - 1);
            int base = (r * cols + c) * 2;
            if (base + 1 < (int)controlPoints.size()) {
                return {controlPoints[base], controlPoints[base+1]};
            }
            return {(float)c / std::max(1, cols - 1), (float)r / std::max(1, rows - 1)};
        };
        std::vector<Point> vertical;
        vertical.reserve(cols);
        for (int c = 0; c < cols; ++c) {
            std::vector<Point> column;
            column.reserve(rows);
            for (int r = 0; r < rows; ++r) column.push_back(getCP(r, c));
            vertical.push_back(legacySpline1D(column, v, xyScale));
        }
        return legacySpline1D(vertical, u, xyScale);
    }



    /**
     * @brief 判断点是否在多边形内部 (射线法) - 极致优化版
     */
    static bool isInsidePolygon(Point p, const std::vector<float>& polygon, int rows, int cols) {
        if (rows < 2 || cols < 2) return true;
        
        // --- 快速路径 1：2x2 全屏/矩形遮罩判定 ---
        if (rows == 2 && cols == 2 && polygon.size() >= 8) {
            float x1 = polygon[0], y1 = polygon[1], x2 = polygon[6], y2 = polygon[7];
            float minX = std::min(x1, x2), maxX = std::max(x1, x2);
            float minY = std::min(y1, y2), maxY = std::max(y1, y2);
            // 典型全屏检测
            if (minX < 0.001f && maxX > 0.999f && minY < 0.001f && maxY > 0.999f) return true;
            return (p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY);
        }

        // --- 快速路径 2：多边形 AABB 预过滤 ---
        // 只有网格点在多边形大致范围内才进行射线检测 (通常 4225 个点中大部分在 AABB 之外)
        float bMinX = 1e9, bMinY = 1e9, bMaxX = -1e9, bMaxY = -1e9;
        for (int i = 0; i < (int)polygon.size(); i += 2) {
            bMinX = std::min(bMinX, polygon[i]); bMaxX = std::max(bMaxX, polygon[i]);
            bMinY = std::min(bMinY, polygon[i+1]); bMaxY = std::max(bMaxY, polygon[i+1]);
        }
        if (p.x < bMinX || p.x > bMaxX || p.y < bMinY || p.y > bMaxY) return false;

        int crossings = 0;
        auto getPt = [&](int r, int c) -> Point {
            int base = (r * cols + c) * 2;
            return {polygon[base], polygon[base + 1]};
        };

        // 射线法核心循环 (优化后的内联)
        auto checkEdge = [&](Point p1, Point p2) {
            if (((p1.y <= p.y && p2.y > p.y) || (p2.y <= p.y && p1.y > p.y))) {
                float resX = p1.x + (p.y - p1.y) * (p2.x - p1.x) / (p2.y - p1.y);
                if (resX > p.x) crossings++;
            }
        };

        for (int c = 0; c < cols - 1; c++) {
            checkEdge(getPt(0, c), getPt(0, c + 1));
            checkEdge(getPt(rows - 1, c + 1), getPt(rows - 1, c));
        }
        for (int r = 0; r < rows - 1; r++) {
            checkEdge(getPt(r + 1, 0), getPt(r, 0));
            checkEdge(getPt(r, cols - 1), getPt(r + 1, cols - 1));
        }
        
        return (crossings % 2) == 1;
    }
};

} // 命名空间 hsvj
