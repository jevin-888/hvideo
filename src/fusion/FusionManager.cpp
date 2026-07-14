#include "fusion/FusionManager.h"
#include "fusion/FusionJson.h"
#include <algorithm>
#include <cmath>

namespace hsvj::fusion {

namespace {

int clampInt(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

float clampFloat(float value, float minValue, float maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

std::vector<Point> createGeometryPointGrid(int rows, int cols) {
  const int safeRows = std::max(2, rows);
  const int safeCols = std::max(2, cols);
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(safeRows * safeCols));
  for (int r = 0; r < safeRows; ++r) {
    for (int c = 0; c < safeCols; ++c) {
      points.push_back({
          safeCols > 1
              ? -1.0f + 2.0f * static_cast<float>(c) /
                            static_cast<float>(safeCols - 1)
              : 0.0f,
          safeRows > 1
              ? 1.0f - 2.0f * static_cast<float>(r) /
                           static_cast<float>(safeRows - 1)
              : 0.0f});
    }
  }
  return points;
}

std::vector<Point> createMaskPointGrid(int rows, int cols) {
  const int safeRows = std::max(2, rows);
  const int safeCols = std::max(2, cols);
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(safeRows * safeCols));
  for (int r = 0; r < safeRows; ++r) {
    for (int c = 0; c < safeCols; ++c) {
      points.push_back({safeCols > 1 ? static_cast<float>(c) /
                                           static_cast<float>(safeCols - 1)
                                      : 0.5f,
                        safeRows > 1 ? static_cast<float>(r) /
                                           static_cast<float>(safeRows - 1)
                                      : 0.5f});
    }
  }
  return points;
}

bool looksLikeUnitLocalGeometry(const GeometryRegionState &region) {
  const size_t expected = static_cast<size_t>(region.rows * region.cols);
  if (region.rows < 2 || region.cols < 2 || region.points.size() != expected) {
    return false;
  }
  float minU = 1.0f;
  float maxU = 0.0f;
  float minV = 1.0f;
  float maxV = 0.0f;
  constexpr float eps = 0.0001f;
  for (const auto &point : region.points) {
    if (point.u < -eps || point.u > 1.0f + eps || point.v < -eps ||
        point.v > 1.0f + eps) {
      return false;
    }
    minU = std::min(minU, point.u);
    maxU = std::max(maxU, point.u);
    minV = std::min(minV, point.v);
    maxV = std::max(maxV, point.v);
  }
  return minU <= eps && minV <= eps && maxU >= 1.0f - eps &&
         maxV >= 1.0f - eps;
}

void convertUnitLocalGeometryToLegacy(GeometryRegionState &region) {
  for (auto &point : region.points) {
    point.u = point.u * 2.0f - 1.0f;
    point.v = 1.0f - point.v * 2.0f;
  }
}

void ensureGeometryPoints(GeometryRegionState &region) {
  region.rows = clampInt(region.rows, 2, 33);
  region.cols = clampInt(region.cols, 2, 33);
  const size_t expected = static_cast<size_t>(region.rows * region.cols);
  if (region.points.size() != expected) {
    region.points = createGeometryPointGrid(region.rows, region.cols);
  } else if (looksLikeUnitLocalGeometry(region)) {
    convertUnitLocalGeometryToLegacy(region);
  }
  region.selected.row = clampInt(region.selected.row, 0, region.rows - 1);
  region.selected.col = clampInt(region.selected.col, 0, region.cols - 1);
}

bool isMaskPerimeterPoint(const MaskState &mask, int row, int col) {
  return row == 0 || col == 0 || row == mask.rows - 1 ||
         col == mask.cols - 1;
}

void clampMaskSelectionToPerimeter(MaskState &mask) {
  mask.selected.row = clampInt(mask.selected.row, 0, mask.rows - 1);
  mask.selected.col = clampInt(mask.selected.col, 0, mask.cols - 1);
  if (isMaskPerimeterPoint(mask, mask.selected.row, mask.selected.col)) return;

  const int row = mask.selected.row;
  const int col = mask.selected.col;
  struct Candidate {
    int row;
    int col;
    int distance;
  };
  const Candidate candidates[] = {
      {0, col, row},
      {mask.rows - 1, col, mask.rows - 1 - row},
      {row, 0, col},
      {row, mask.cols - 1, mask.cols - 1 - col},
  };
  const Candidate *best = &candidates[0];
  for (const Candidate &candidate : candidates) {
    if (candidate.distance < best->distance) best = &candidate;
  }
  mask.selected.row = best->row;
  mask.selected.col = best->col;
}

void ensureMaskPoints(MaskState &mask) {
  mask.rows = clampInt(mask.rows, kMaskMinGrid, kMaskMaxGrid);
  mask.cols = clampInt(mask.cols, kMaskMinGrid, kMaskMaxGrid);
  const size_t expected = static_cast<size_t>(mask.rows * mask.cols);
  if (mask.points.size() != expected) {
    mask.points = createMaskPointGrid(mask.rows, mask.cols);
  }
  mask.selected.row = clampInt(mask.selected.row, 0, mask.rows - 1);
  mask.selected.col = clampInt(mask.selected.col, 0, mask.cols - 1);
  clampMaskSelectionToPerimeter(mask);
}

Point &pointAt(GeometryRegionState &region, int row, int col) {
  ensureGeometryPoints(region);
  return region.points[static_cast<size_t>(row * region.cols + col)];
}

const Point &pointAt(const GeometryRegionState &region, int row, int col) {
  return region.points[static_cast<size_t>(row * region.cols + col)];
}

void movePoint(GeometryRegionState &region, int row, int col, float du,
               float dv, float weight = 1.0f) {
  Point &point = pointAt(region, row, col);
  point.u += du * weight;
  point.v += dv * weight;
}

float safeRatio(float numerator, float denominator) {
  if (std::fabs(denominator) < 0.000001f) return 0.0f;
  return clampFloat(numerator / denominator, 0.0f, 1.0f);
}

float safeWeight(float numerator, float denominator) {
  if (std::fabs(denominator) < 0.000001f) return 0.0f;
  return numerator / denominator;
}

float pointDistance(const Point &a, const Point &b) {
  const float du = a.u - b.u;
  const float dv = a.v - b.v;
  return std::sqrt(du * du + dv * dv);
}

int resizeGridCount(int value, const std::string &op, const char *growOp,
                    const char *shrinkOp, int maxValue) {
  const int current = std::max(2, value);
  if (op == growOp) return std::min(maxValue, (current - 1) * 2 + 1);
  if (op == shrinkOp) return std::max(2, (current - 1) / 2 + 1);
  return current;
}

int resizeSelectedIndex(int index, int oldCount, int nextCount) {
  index = clampInt(index, 0, std::max(0, oldCount - 1));
  if (nextCount == (oldCount - 1) * 2 + 1) {
    return clampInt(index * 2, 0, nextCount - 1);
  }
  if (nextCount == (oldCount - 1) / 2 + 1) {
    return clampInt(index / 2, 0, nextCount - 1);
  }
  if (oldCount > 1 && nextCount > 1) {
    const float scaled = static_cast<float>(index) *
                         static_cast<float>(nextCount - 1) /
                         static_cast<float>(oldCount - 1);
    return clampInt(static_cast<int>(std::round(scaled)), 0, nextCount - 1);
  }
  return 0;
}

std::vector<Point> resizePoints(const GeometryRegionState &region, int nextRows,
                                int nextCols) {
  const int oldRows = region.rows;
  const int oldCols = region.cols;
  if (oldRows < 2 || oldCols < 2 || nextRows < 2 || nextCols < 2) {
    return createGeometryPointGrid(nextRows, nextCols);
  }

  std::vector<Point> afterCols;
  afterCols.reserve(static_cast<size_t>(oldRows * nextCols));
  auto getOld = [&](int r, int c) -> Point {
    return pointAt(region, clampInt(r, 0, oldRows - 1),
                   clampInt(c, 0, oldCols - 1));
  };

  if (nextCols == (oldCols - 1) * 2 + 1) {
    for (int r = 0; r < oldRows; ++r) {
      for (int c = 0; c < nextCols; ++c) {
        if ((c & 1) == 0) {
          afterCols.push_back(getOld(r, c / 2));
        } else {
          const Point a = getOld(r, c / 2);
          const Point b = getOld(r, c / 2 + 1);
          afterCols.push_back({(a.u + b.u) * 0.5f, (a.v + b.v) * 0.5f});
        }
      }
    }
  } else if (nextCols == (oldCols - 1) / 2 + 1) {
    for (int r = 0; r < oldRows; ++r) {
      for (int c = 0; c < nextCols; ++c) {
        const int srcC = c == nextCols - 1 ? oldCols - 1 : c * 2;
        afterCols.push_back(getOld(r, srcC));
      }
    }
  } else if (nextCols == oldCols) {
    afterCols = region.points;
  } else {
    return region.points;
  }

  auto getAfter = [&](int r, int c) -> Point {
    const int rr = clampInt(r, 0, oldRows - 1);
    const int cc = clampInt(c, 0, nextCols - 1);
    return afterCols[static_cast<size_t>(rr * nextCols + cc)];
  };

  std::vector<Point> out;
  out.reserve(static_cast<size_t>(nextRows * nextCols));
  if (nextRows == (oldRows - 1) * 2 + 1) {
    for (int r = 0; r < nextRows; ++r) {
      for (int c = 0; c < nextCols; ++c) {
        if ((r & 1) == 0) {
          out.push_back(getAfter(r / 2, c));
        } else {
          const Point a = getAfter(r / 2, c);
          const Point b = getAfter(r / 2 + 1, c);
          out.push_back({(a.u + b.u) * 0.5f, (a.v + b.v) * 0.5f});
        }
      }
    }
    return out;
  }
  if (nextRows == (oldRows - 1) / 2 + 1) {
    for (int r = 0; r < nextRows; ++r) {
      const int srcR = r == nextRows - 1 ? oldRows - 1 : r * 2;
      for (int c = 0; c < nextCols; ++c) {
        out.push_back(getAfter(srcR, c));
      }
    }
    return out;
  }
  return nextRows == oldRows ? afterCols : region.points;
}

std::vector<Point> resizeUnitPoints(const std::vector<Point> &points,
                                    int oldRows, int oldCols,
                                    int nextRows, int nextCols) {
  if (oldRows < 2 || oldCols < 2 || nextRows < 2 || nextCols < 2 ||
      points.size() != static_cast<size_t>(oldRows * oldCols)) {
    return createMaskPointGrid(nextRows, nextCols);
  }

  std::vector<Point> afterCols;
  afterCols.reserve(static_cast<size_t>(oldRows * nextCols));
  auto getOld = [&](int r, int c) -> Point {
    const int rr = clampInt(r, 0, oldRows - 1);
    const int cc = clampInt(c, 0, oldCols - 1);
    return points[static_cast<size_t>(rr * oldCols + cc)];
  };

  if (nextCols == (oldCols - 1) * 2 + 1) {
    for (int r = 0; r < oldRows; ++r) {
      for (int c = 0; c < nextCols; ++c) {
        if ((c & 1) == 0) {
          afterCols.push_back(getOld(r, c / 2));
        } else {
          const Point a = getOld(r, c / 2);
          const Point b = getOld(r, c / 2 + 1);
          afterCols.push_back({(a.u + b.u) * 0.5f, (a.v + b.v) * 0.5f});
        }
      }
    }
  } else if (nextCols == (oldCols - 1) / 2 + 1) {
    for (int r = 0; r < oldRows; ++r) {
      for (int c = 0; c < nextCols; ++c) {
        const int srcC = c == nextCols - 1 ? oldCols - 1 : c * 2;
        afterCols.push_back(getOld(r, srcC));
      }
    }
  } else if (nextCols == oldCols) {
    afterCols = points;
  } else {
    return points;
  }

  auto getAfter = [&](int r, int c) -> Point {
    const int rr = clampInt(r, 0, oldRows - 1);
    const int cc = clampInt(c, 0, nextCols - 1);
    return afterCols[static_cast<size_t>(rr * nextCols + cc)];
  };

  std::vector<Point> out;
  out.reserve(static_cast<size_t>(nextRows * nextCols));
  if (nextRows == (oldRows - 1) * 2 + 1) {
    for (int r = 0; r < nextRows; ++r) {
      for (int c = 0; c < nextCols; ++c) {
        if ((r & 1) == 0) {
          out.push_back(getAfter(r / 2, c));
        } else {
          const Point a = getAfter(r / 2, c);
          const Point b = getAfter(r / 2 + 1, c);
          out.push_back({(a.u + b.u) * 0.5f, (a.v + b.v) * 0.5f});
        }
      }
    }
    return out;
  }
  if (nextRows == (oldRows - 1) / 2 + 1) {
    for (int r = 0; r < nextRows; ++r) {
      const int srcR = r == nextRows - 1 ? oldRows - 1 : r * 2;
      for (int c = 0; c < nextCols; ++c) {
        out.push_back(getAfter(srcR, c));
      }
    }
    return out;
  }
  return nextRows == oldRows ? afterCols : points;
}

void resizeGeometryTo(GeometryRegionState &region, int nextRows, int nextCols) {
  ensureGeometryPoints(region);
  nextRows = clampInt(nextRows, 2, 33);
  nextCols = clampInt(nextCols, 2, 33);
  if (region.rows == nextRows && region.cols == nextCols &&
      region.points.size() == static_cast<size_t>(nextRows * nextCols)) {
    return;
  }
  region.points = resizePoints(region, nextRows, nextCols);
  region.rows = nextRows;
  region.cols = nextCols;
  region.selected.row = clampInt(region.selected.row, 0, region.rows - 1);
  region.selected.col = clampInt(region.selected.col, 0, region.cols - 1);
}

int edgeDirectionFromOp(const std::string &op) {
  if (op == "edge_top") return 0;
  if (op == "edge_bottom") return 1;
  if (op == "edge_left") return 2;
  if (op == "edge_right") return 3;
  return -1;
}

void applyWeightedEdgeMove(GeometryRegionState &region, int direction, float du,
                           float dv) {
  ensureGeometryPoints(region);
  if (direction == 2) {
    for (int row = 0; row < region.rows; ++row) {
      const Point left = pointAt(region, row, 0);
      const Point right = pointAt(region, row, region.cols - 1);
      for (int col = 0; col < region.cols; ++col) {
        const Point p = pointAt(region, row, col);
        movePoint(region, row, col, du, 0.0f,
                  safeRatio(p.u - right.u, left.u - right.u));
      }
    }
  } else if (direction == 3) {
    for (int row = 0; row < region.rows; ++row) {
      const Point left = pointAt(region, row, 0);
      const Point right = pointAt(region, row, region.cols - 1);
      for (int col = 0; col < region.cols; ++col) {
        const Point p = pointAt(region, row, col);
        movePoint(region, row, col, du, 0.0f,
                  safeRatio(p.u - left.u, right.u - left.u));
      }
    }
  } else if (direction == 0) {
    for (int col = 0; col < region.cols; ++col) {
      const Point top = pointAt(region, 0, col);
      const Point bottom = pointAt(region, region.rows - 1, col);
      for (int row = 0; row < region.rows; ++row) {
        const Point p = pointAt(region, row, col);
        movePoint(region, row, col, 0.0f, dv,
                  safeRatio(p.v - bottom.v, top.v - bottom.v));
      }
    }
  } else if (direction == 1) {
    for (int col = 0; col < region.cols; ++col) {
      const Point top = pointAt(region, 0, col);
      const Point bottom = pointAt(region, region.rows - 1, col);
      for (int row = 0; row < region.rows; ++row) {
        const Point p = pointAt(region, row, col);
        movePoint(region, row, col, 0.0f, dv,
                  safeRatio(p.v - top.v, bottom.v - top.v));
      }
    }
  }
}

void applyGeometryMoveLocal(GeometryRegionState &region, const std::string &op,
                            float du, float dv) {
  ensureGeometryPoints(region);
  if (op == "point") {
    movePoint(region, region.selected.row, region.selected.col, du, dv);
  } else if (op == "row") {
    for (int col = 0; col < region.cols; ++col) {
      movePoint(region, region.selected.row, col, du, dv);
    }
  } else if (op == "col") {
    for (int row = 0; row < region.rows; ++row) {
      movePoint(region, row, region.selected.col, du, dv);
    }
  } else if (op == "all") {
    for (int row = 0; row < region.rows; ++row) {
      for (int col = 0; col < region.cols; ++col) {
        movePoint(region, row, col, du, dv);
      }
    }
  } else {
    const int edgeDirection = edgeDirectionFromOp(op);
    if (edgeDirection >= 0) {
      applyWeightedEdgeMove(region, edgeDirection, du, dv);
    } else if (op == "corner_left_top" || op == "corner_right_top" ||
               op == "corner_left_bottom" || op == "corner_right_bottom") {
      for (int row = 0; row < region.rows; ++row) {
        for (int col = 0; col < region.cols; ++col) {
          const Point p = pointAt(region, row, col);
          const Point left = pointAt(region, row, 0);
          const Point right = pointAt(region, row, region.cols - 1);
          const Point top = pointAt(region, 0, col);
          const Point bottom = pointAt(region, region.rows - 1, col);
          const float tx = safeRatio(p.u - left.u, right.u - left.u);
          const float ty = safeRatio(p.v - top.v, bottom.v - top.v);
          const float wx = (op == "corner_left_top" || op == "corner_left_bottom")
              ? 1.0f - tx
              : tx;
          const float wy = (op == "corner_left_top" || op == "corner_right_top")
              ? 1.0f - ty
              : ty;
          movePoint(region, row, col, du, dv, wx * wy);
        }
      }
    }
  }
}

void applyManagerMovePointLocal(GeometryRegionState &region, int direction,
                                float du, float dv, const std::string &corner) {
  ensureGeometryPoints(region);
  if (region.rows < 2 || region.cols < 2) return;
  const std::string activeCorner = corner.empty() ? "top-left" : corner;
  const int cornerRow = activeCorner.rfind("top", 0) == 0 ? 0 : region.rows - 1;
  const int cornerCol =
      activeCorner.size() >= 4 && activeCorner.substr(activeCorner.size() - 4) == "left"
          ? 0
          : region.cols - 1;

  if (direction == 2 || direction == 3) {
    const bool leftCorner = cornerCol == 0;
    const int edgeCol = leftCorner ? 0 : region.cols - 1;
    const float l1 = pointDistance(pointAt(region, 0, edgeCol),
                                   pointAt(region, region.rows - 1, edgeCol));
    for (int row = 0; row < region.rows; ++row) {
      const Point left = pointAt(region, row, 0);
      const Point right = pointAt(region, row, region.cols - 1);
      const Point edge = pointAt(region, row, edgeCol);
      const Point opposite = cornerRow == 0
          ? pointAt(region, region.rows - 1, edgeCol)
          : pointAt(region, 0, edgeCol);
      const float l2 = pointDistance(edge, opposite);
      for (int col = 0; col < region.cols; ++col) {
        const Point p = pointAt(region, row, col);
        const float wx = leftCorner
            ? safeRatio(p.u - right.u, left.u - right.u)
            : safeRatio(p.u - left.u, right.u - left.u);
        movePoint(region, row, col, du, 0.0f, wx * safeWeight(l2, l1));
      }
    }
  } else if (direction == 0 || direction == 1) {
    const bool topCorner = cornerRow == 0;
    const int edgeRow = topCorner ? 0 : region.rows - 1;
    const float l1 = pointDistance(pointAt(region, edgeRow, 0),
                                   pointAt(region, edgeRow, region.cols - 1));
    for (int col = 0; col < region.cols; ++col) {
      const Point top = pointAt(region, 0, col);
      const Point bottom = pointAt(region, region.rows - 1, col);
      const Point edge = pointAt(region, edgeRow, col);
      const Point opposite = cornerCol == 0
          ? pointAt(region, edgeRow, region.cols - 1)
          : pointAt(region, edgeRow, 0);
      const float l2 = pointDistance(edge, opposite);
      for (int row = 0; row < region.rows; ++row) {
        const Point p = pointAt(region, row, col);
        const float wy = topCorner
            ? safeRatio(p.v - bottom.v, top.v - bottom.v)
            : safeRatio(p.v - top.v, bottom.v - top.v);
        movePoint(region, row, col, 0.0f, dv, wy * safeWeight(l2, l1));
      }
    }
  }
}

void applyManagerMoveLineLocal(GeometryRegionState &region, int direction,
                               float du, float dv, int selectedRow,
                               int selectedCol) {
  ensureGeometryPoints(region);
  if (region.rows < 2 || region.cols < 2 || direction < 0 || direction > 3) return;
  const int edgeCol = selectedCol < region.cols / 2 ? 0 : region.cols - 1;
  const int edgeRow = selectedRow < region.rows / 2 ? 0 : region.rows - 1;
  if (direction == 2 || direction == 3) {
    for (int row = 0; row < region.rows; ++row) {
      const Point left = pointAt(region, row, 0);
      const Point right = pointAt(region, row, region.cols - 1);
      for (int col = 0; col < region.cols; ++col) {
        const Point p = pointAt(region, row, col);
        const float weight = edgeCol == 0
            ? safeRatio(p.u - right.u, left.u - right.u)
            : safeRatio(p.u - left.u, right.u - left.u);
        movePoint(region, row, col, du, 0.0f, weight);
      }
    }
  } else {
    for (int col = 0; col < region.cols; ++col) {
      const Point top = pointAt(region, 0, col);
      const Point bottom = pointAt(region, region.rows - 1, col);
      for (int row = 0; row < region.rows; ++row) {
        const Point p = pointAt(region, row, col);
        const float weight = edgeRow == 0
            ? safeRatio(p.v - bottom.v, top.v - bottom.v)
            : safeRatio(p.v - top.v, bottom.v - top.v);
        movePoint(region, row, col, 0.0f, dv, weight);
      }
    }
  }
}

void moveMaskPoint(MaskState &mask, int row, int col, float du, float dv) {
  row = clampInt(row, 0, mask.rows - 1);
  col = clampInt(col, 0, mask.cols - 1);
  Point &point = mask.points[static_cast<size_t>(row * mask.cols + col)];
  point.u += du;
  point.v += dv;
}

void resizeMaskTo(MaskState &mask, int nextRows, int nextCols) {
  ensureMaskPoints(mask);
  const int oldRows = mask.rows;
  const int oldCols = mask.cols;
  nextRows = clampInt(nextRows, kMaskMinGrid, kMaskMaxGrid);
  nextCols = clampInt(nextCols, kMaskMinGrid, kMaskMaxGrid);
  if (oldRows == nextRows && oldCols == nextCols &&
      mask.points.size() == static_cast<size_t>(nextRows * nextCols)) {
    return;
  }
  mask.points = resizeUnitPoints(mask.points, oldRows, oldCols, nextRows,
                                 nextCols);
  const int selectedRow =
      resizeSelectedIndex(mask.selected.row, oldRows, nextRows);
  const int selectedCol =
      resizeSelectedIndex(mask.selected.col, oldCols, nextCols);
  mask.rows = nextRows;
  mask.cols = nextCols;
  mask.selected.row = selectedRow;
  mask.selected.col = selectedCol;
  clampMaskSelectionToPerimeter(mask);
}

void applyMaskMoveLocal(MaskState &mask, const std::string &op, float du,
                        float dv, int row, int col) {
  ensureMaskPoints(mask);
  if (row >= 0 && col >= 0) {
    mask.selected.row = clampInt(row, 0, mask.rows - 1);
    mask.selected.col = clampInt(col, 0, mask.cols - 1);
  }
  clampMaskSelectionToPerimeter(mask);
  if (op == "row") {
    const int selectedRow = clampInt(mask.selected.row, 0, mask.rows - 1);
    for (int c = 0; c < mask.cols; ++c) {
      moveMaskPoint(mask, selectedRow, c, du, dv);
    }
  } else if (op == "col") {
    const int selectedCol = clampInt(mask.selected.col, 0, mask.cols - 1);
    for (int r = 0; r < mask.rows; ++r) {
      moveMaskPoint(mask, r, selectedCol, du, dv);
    }
  } else if (op == "all") {
    for (int r = 0; r < mask.rows; ++r) {
      for (int c = 0; c < mask.cols; ++c) {
        moveMaskPoint(mask, r, c, du, dv);
      }
    }
  } else {
    moveMaskPoint(mask, mask.selected.row, mask.selected.col, du, dv);
  }
}

void patchSideFromApi(BlendSideState &side, const Json::Value &payload,
                      const char *name, char shortName) {
  const std::string prefix = std::string("blend_") + name;
  const std::string enabled = prefix + "_enabled";
  if (payload.isMember(prefix) && payload[prefix].isNumeric()) {
    side.width = clampFloat(payload[prefix].asFloat(), 0.0f, 1.0f);
  }
  if (payload.isMember(enabled) && payload[enabled].isBool()) {
    side.enabled = payload[enabled].asBool();
  }
  const std::string gamma = std::string("edge_") + name + "_gamma";
  const std::string slope = std::string("edge_") + name + "_slope";
  if (payload.isMember(gamma) && payload[gamma].isNumeric()) {
    side.gamma = std::max(0.001f, payload[gamma].asFloat());
  }
  if (payload.isMember(slope) && payload[slope].isNumeric()) {
    side.slope = std::max(0.001f, payload[slope].asFloat());
  }
  const std::string stripStart = std::string("strip_start_") + shortName;
  const std::string stripEnd = std::string("strip_end_") + shortName;
  const std::string anchor = std::string("anchor_") + shortName;
  const std::string solid = std::string("solid_") + shortName;
  if (payload.isMember(stripStart) && payload[stripStart].isNumeric()) {
    side.stripStart = clampInt(payload[stripStart].asInt(), 0, 255);
  }
  if (payload.isMember(stripEnd) && payload[stripEnd].isNumeric()) {
    side.stripEnd = clampInt(payload[stripEnd].asInt(), 0, 255);
  }
  if (payload.isMember(anchor) && payload[anchor].isNumeric()) {
    side.anchor = clampFloat(payload[anchor].asFloat(), 0.0f, 1.0f);
  }
  if (payload.isMember(solid) && payload[solid].isBool()) {
    side.solid = payload[solid].asBool();
  }
}

float legacyMergeRangeForGrid(int gridCount) {
  return gridCount > 2 ? 1.0f / static_cast<float>(gridCount - 1) : 0.0f;
}

bool hasAdjacentSide(const Json::Value &adjacencyByRegion, int regionId,
                     const char *field) {
  const std::string key = std::to_string(std::max(1, regionId));
  if (!adjacencyByRegion.isObject() || !adjacencyByRegion.isMember(key) ||
      !adjacencyByRegion[key].isObject()) {
    return false;
  }
  const Json::Value &adjacent = adjacencyByRegion[key][field];
  return adjacent.isNumeric() && adjacent.asInt() > 0;
}

void applyAutoBlendEdges(BlendRegionState &region, int regionId,
                         const Json::Value &adjacencyByRegion) {
  region.left.enabled =
      hasAdjacentSide(adjacencyByRegion, regionId, "left_region_id");
  region.right.enabled =
      hasAdjacentSide(adjacencyByRegion, regionId, "right_region_id");
  region.top.enabled =
      hasAdjacentSide(adjacencyByRegion, regionId, "top_region_id");
  region.bottom.enabled =
      hasAdjacentSide(adjacencyByRegion, regionId, "bottom_region_id");
}

void patchCorrectionFromApi(GeometryCorrectionState &correction,
                            const Json::Value &payload) {
  if (payload.isMember("enabled") && payload["enabled"].isBool()) {
    correction.enabled = payload["enabled"].asBool();
  }
  if (payload.isMember("offset_x") && payload["offset_x"].isNumeric()) {
    correction.offsetX = payload["offset_x"].asFloat();
  }
  if (payload.isMember("offset_y") && payload["offset_y"].isNumeric()) {
    correction.offsetY = payload["offset_y"].asFloat();
  }
  if (payload.isMember("scale_x") && payload["scale_x"].isNumeric()) {
    correction.scaleX = payload["scale_x"].asFloat();
  }
  if (payload.isMember("scale_y") && payload["scale_y"].isNumeric()) {
    correction.scaleY = payload["scale_y"].asFloat();
  }
  if (payload.isMember("rotate_rad") && payload["rotate_rad"].isNumeric()) {
    correction.rotateRad = payload["rotate_rad"].asFloat();
  }
  if (payload.isMember("keystone_x") && payload["keystone_x"].isNumeric()) {
    correction.keystoneX = payload["keystone_x"].asFloat();
  }
  if (payload.isMember("keystone_y") && payload["keystone_y"].isNumeric()) {
    correction.keystoneY = payload["keystone_y"].asFloat();
  }
}

void patchCaveWallFromApi(CaveWallState &caveWall,
                          const Json::Value &payload) {
  if (payload.isMember("enabled") && payload["enabled"].isBool()) {
    caveWall.enabled = payload["enabled"].asBool();
  }
  if (payload.isMember("wall_type") && payload["wall_type"].isNumeric()) {
    caveWall.wallType = clampInt(payload["wall_type"].asInt(), 0, 6);
  }
  if (payload.isMember("eye_distance") && payload["eye_distance"].isNumeric()) {
    caveWall.eyeDistance = std::max(0.0f, payload["eye_distance"].asFloat());
  }
  if (payload.isMember("near_plane") && payload["near_plane"].isNumeric()) {
    caveWall.nearPlane = std::max(0.001f, payload["near_plane"].asFloat());
  }
  if (payload.isMember("far_plane") && payload["far_plane"].isNumeric()) {
    caveWall.farPlane = std::max(0.001f, payload["far_plane"].asFloat());
  }
  if (caveWall.farPlane <= caveWall.nearPlane) {
    caveWall.farPlane = caveWall.nearPlane + 0.001f;
  }
  if (payload.isMember("llx") && payload["llx"].isNumeric()) {
    caveWall.llx = payload["llx"].asFloat();
  }
  if (payload.isMember("lly") && payload["lly"].isNumeric()) {
    caveWall.lly = payload["lly"].asFloat();
  }
  if (payload.isMember("llz") && payload["llz"].isNumeric()) {
    caveWall.llz = payload["llz"].asFloat();
  }
  if (payload.isMember("ulx") && payload["ulx"].isNumeric()) {
    caveWall.ulx = payload["ulx"].asFloat();
  }
  if (payload.isMember("uly") && payload["uly"].isNumeric()) {
    caveWall.uly = payload["uly"].asFloat();
  }
  if (payload.isMember("ulz") && payload["ulz"].isNumeric()) {
    caveWall.ulz = payload["ulz"].asFloat();
  }
  if (payload.isMember("lrx") && payload["lrx"].isNumeric()) {
    caveWall.lrx = payload["lrx"].asFloat();
  }
  if (payload.isMember("lry") && payload["lry"].isNumeric()) {
    caveWall.lry = payload["lry"].asFloat();
  }
  if (payload.isMember("lrz") && payload["lrz"].isNumeric()) {
    caveWall.lrz = payload["lrz"].asFloat();
  }
}

} // 命名空间

FusionManager::FusionManager(FusionProjectState &state) : state_(state) {
  ensureMaskPoints(state_.mask);
}

GeometryRegionState &FusionManager::geometry(int regionId) {
  GeometryRegionState &region = state_.geometryByRegion[std::max(1, regionId)];
  ensureGeometryPoints(region);
  return region;
}

BlendRegionState &FusionManager::blend(int regionId) {
  BlendRegionState &region = state_.blendByRegion[std::max(1, regionId)];
  region.gridRows = clampInt(region.gridRows, 2, 33);
  region.gridCols = clampInt(region.gridCols, 2, 33);
  return region;
}

GeometryCorrectionState &FusionManager::correction(int regionId) {
  return state_.correctionByRegion[std::max(1, regionId)];
}

CaveWallState &FusionManager::caveWall(int regionId) {
  CaveWallState &wall = state_.caveWallByRegion[std::max(1, regionId)];
  wall.wallType = clampInt(wall.wallType, 0, 6);
  wall.eyeDistance = std::max(0.0f, wall.eyeDistance);
  wall.nearPlane = std::max(0.001f, wall.nearPlane);
  if (wall.farPlane <= wall.nearPlane) {
    wall.farPlane = wall.nearPlane + 0.001f;
  }
  return wall;
}

void FusionManager::reset(int regionCount) {
  state_ = FusionProjectState();
  state_.mask.points = createMaskPointGrid(state_.mask.rows, state_.mask.cols);
  ensureRegionCount(regionCount);
}

void FusionManager::ensureRegionCount(int regionCount) {
  const int count = std::max(0, regionCount);
  for (int id = 1; id <= count; ++id) {
    geometry(id);
    blend(id);
    correction(id);
    caveWall(id);
  }
}

Json::Value FusionManager::getGeometryApi(int regionId) {
  return geometryToApiJson(geometry(regionId));
}

Json::Value FusionManager::setGeometryDisplay(int regionId, bool showGrid) {
  GeometryRegionState &region = geometry(regionId);
  region.showGrid = showGrid;
  return geometryToApiJson(region);
}

Json::Value FusionManager::setGeometryDisplayAll(bool showGrid) {
  Json::Value out(Json::objectValue);
  Json::Value regions(Json::arrayValue);
  for (auto &entry : state_.geometryByRegion) {
    entry.second.showGrid = showGrid;
    Json::Value item = geometryToApiJson(entry.second);
    item["region_id"] = entry.first;
    regions.append(item);
  }
  out["regions"] = regions;
  out["show_grid"] = showGrid;
  return out;
}

Json::Value FusionManager::setGeometrySelection(int regionId, int row, int col) {
  GeometryRegionState &region = geometry(regionId);
  region.selected.row = clampInt(row, 0, region.rows - 1);
  region.selected.col = clampInt(col, 0, region.cols - 1);
  return geometryToApiJson(region);
}

Json::Value FusionManager::setGeometryGrid(int regionId, int rows, int cols,
                                           int interpolationMode) {
  GeometryRegionState &region = geometry(regionId);
  resizeGeometryTo(region, rows, cols);
  region.interpolationMode = interpolationMode == 1 ? 1 : 0;
  return geometryToApiJson(region);
}

Json::Value FusionManager::resizeGeometryGrid(int regionId,
                                              const std::string &op) {
  GeometryRegionState &region = geometry(regionId);
  int nextRows = region.rows;
  int nextCols = region.cols;
  nextRows = resizeGridCount(nextRows, op, "grow_rows", "shrink_rows", 33);
  nextCols = resizeGridCount(nextCols, op, "grow_cols", "shrink_cols", 33);
  resizeGeometryTo(region, nextRows, nextCols);
  return geometryToApiJson(region);
}

Json::Value FusionManager::resizeBlendGrid(
    int regionId, const std::string &op,
    const Json::Value &adjacencyByRegion) {
  BlendRegionState &region = blend(regionId);
  const int nextRows = resizeGridCount(region.gridRows, op, "grow_rows", "shrink_rows", 33);
  const int nextCols = resizeGridCount(region.gridCols, op, "grow_cols", "shrink_cols", 33);
  region.gridRows = nextRows;
  region.gridCols = nextCols;
  const float rangeX = legacyMergeRangeForGrid(region.gridCols);
  const float rangeY = legacyMergeRangeForGrid(region.gridRows);
  // 加减融合行列只调整融合管理网格和边宽。自动模式下按输入相邻关系回算 enabled，
  // 手动模式下保留用户逐边选择，避免行列调整后回灌旧状态。
  region.left.width = rangeX;
  region.right.width = rangeX;
  region.top.width = rangeY;
  region.bottom.width = rangeY;
  if (state_.blendAutoEdges) {
    applyAutoBlendEdges(region, regionId, adjacencyByRegion);
  }
  return blendToApiJson(region);
}

Json::Value FusionManager::moveGeometry(int regionId, const std::string &op,
                                        float du, float dv) {
  GeometryRegionState &region = geometry(regionId);
  applyGeometryMoveLocal(region, op, du, dv);
  return geometryToApiJson(region);
}

Json::Value FusionManager::moveManagerPoint(int regionId, int direction,
                                            float du, float dv,
                                            const std::string &corner) {
  GeometryRegionState &region = geometry(regionId);
  applyManagerMovePointLocal(region, direction, du, dv, corner);
  return geometryToApiJson(region);
}

Json::Value FusionManager::moveManagerLine(int regionId, int direction,
                                           float du, float dv, int selectedRow,
                                           int selectedCol) {
  GeometryRegionState &region = geometry(regionId);
  selectedRow = selectedRow >= 0 ? selectedRow : region.selected.row;
  selectedCol = selectedCol >= 0 ? selectedCol : region.selected.col;
  applyManagerMoveLineLocal(region, direction, du, dv, selectedRow, selectedCol);
  return geometryToApiJson(region);
}

Json::Value FusionManager::setGeometryPoint(int regionId, int row, int col,
                                            float u, float v) {
  GeometryRegionState &region = geometry(regionId);
  row = clampInt(row, 0, region.rows - 1);
  col = clampInt(col, 0, region.cols - 1);
  Point &point = pointAt(region, row, col);
  point.u = u;
  point.v = v;
  region.selected.row = row;
  region.selected.col = col;
  return geometryToApiJson(region);
}

Json::Value FusionManager::setGeometryPoints(int regionId,
                                             const Json::Value &points,
                                             int rows, int cols,
                                             int interpolationMode) {
  GeometryRegionState &region = geometry(regionId);
  if (rows >= 2 && cols >= 2) {
    region.rows = clampInt(rows, 2, 33);
    region.cols = clampInt(cols, 2, 33);
    region.points = createGeometryPointGrid(region.rows, region.cols);
  }
  if (interpolationMode >= 0) {
    region.interpolationMode = interpolationMode == 1 ? 1 : 0;
  }
  ensureGeometryPoints(region);
  if (points.isArray()) {
    if (points.size() == static_cast<Json::ArrayIndex>(region.rows * region.cols * 2)) {
      for (int i = 0; i < region.rows * region.cols; ++i) {
        if (points[i * 2].isNumeric() && points[i * 2 + 1].isNumeric()) {
          region.points[static_cast<size_t>(i)] =
              {points[i * 2].asFloat(), points[i * 2 + 1].asFloat()};
        }
      }
    } else {
      for (const auto &item : points) {
        if (item.isArray() && item.size() >= 4 && item[0].isNumeric() &&
            item[1].isNumeric() && item[2].isNumeric() && item[3].isNumeric()) {
          const int row = clampInt(item[0].asInt(), 0, region.rows - 1);
          const int col = clampInt(item[1].asInt(), 0, region.cols - 1);
          pointAt(region, row, col) = {item[2].asFloat(), item[3].asFloat()};
        }
      }
    }
  }
  if (looksLikeUnitLocalGeometry(region)) {
    convertUnitLocalGeometryToLegacy(region);
  }
  return geometryToApiJson(region);
}

Json::Value FusionManager::getMaskApi() {
  ensureMaskPoints(state_.mask);
  return maskToApiJson(state_.mask);
}

Json::Value FusionManager::setMaskState(const Json::Value &payload) {
  MaskState &maskState = state_.mask;
  ensureMaskPoints(maskState);
  if (payload.isMember("show_guide") && payload["show_guide"].isBool()) {
    maskState.showGuide = payload["show_guide"].asBool();
  }
  if (payload.isMember("selected_row") && payload["selected_row"].isNumeric()) {
    maskState.selected.row = clampInt(payload["selected_row"].asInt(), 0, maskState.rows - 1);
  }
  if (payload.isMember("selected_col") && payload["selected_col"].isNumeric()) {
    maskState.selected.col = clampInt(payload["selected_col"].asInt(), 0, maskState.cols - 1);
  }
  const Json::Value &maskPayload = payload["mask"];
  const bool hasMaskPayload = maskPayload.isObject();
  const bool shouldReturnFullMask =
      hasMaskPayload &&
      (maskPayload.isMember("rows") || maskPayload.isMember("cols") ||
       maskPayload.isMember("interpolation_mode") ||
       maskPayload.isMember("vertices"));
  if (hasMaskPayload) {
    if (maskPayload.isMember("enabled") && maskPayload["enabled"].isBool()) {
      maskState.enabled = maskPayload["enabled"].asBool();
    }
    const int nextRows = maskPayload.isMember("rows") && maskPayload["rows"].isNumeric()
        ? clampInt(maskPayload["rows"].asInt(), kMaskMinGrid, kMaskMaxGrid)
        : maskState.rows;
    const int nextCols = maskPayload.isMember("cols") && maskPayload["cols"].isNumeric()
        ? clampInt(maskPayload["cols"].asInt(), kMaskMinGrid, kMaskMaxGrid)
        : maskState.cols;
    if (nextRows != maskState.rows || nextCols != maskState.cols) {
      resizeMaskTo(maskState, nextRows, nextCols);
    }
    if (maskPayload.isMember("interpolation_mode") &&
        maskPayload["interpolation_mode"].isNumeric()) {
      maskState.interpolationMode =
          maskPayload["interpolation_mode"].asInt() == 1 ? 1 : 0;
    }
    if (maskPayload.isMember("vertices") && maskPayload["vertices"].isArray() &&
        maskPayload["vertices"].size() >=
            static_cast<Json::ArrayIndex>(maskState.rows * maskState.cols * 2)) {
      for (int i = 0; i < maskState.rows * maskState.cols; ++i) {
        if (maskPayload["vertices"][i * 2].isNumeric() &&
            maskPayload["vertices"][i * 2 + 1].isNumeric()) {
          maskState.points[static_cast<size_t>(i)] = {
              maskPayload["vertices"][i * 2].asFloat(),
              maskPayload["vertices"][i * 2 + 1].asFloat()};
        }
      }
    }
  }
  ensureMaskPoints(maskState);
  if (!shouldReturnFullMask) {
    Json::Value result;
    result["show_guide"] = maskState.showGuide;
    result["selected_row"] = maskState.selected.row;
    result["selected_col"] = maskState.selected.col;
    result["mask_enabled"] = maskState.enabled;
    return result;
  }
  return maskToApiJson(maskState);
}

Json::Value FusionManager::seedMaskFromGeometry(int inputRows, int inputCols) {
  const int rowsIn = std::max(1, inputRows);
  const int colsIn = std::max(1, inputCols);
  const int regionCount = std::max(1, rowsIn * colsIn);

  int regionRows = 2;
  int regionCols = 2;
  int interpolationMode = 0;
  for (int regionId = 1; regionId <= regionCount; ++regionId) {
    GeometryRegionState &geo = geometry(regionId);
    ensureGeometryPoints(geo);
    regionRows = std::max(regionRows, geo.rows);
    regionCols = std::max(regionCols, geo.cols);
    if (geo.interpolationMode == 1) interpolationMode = 1;
  }

  regionRows = clampInt(regionRows > 0 ? regionRows : 2, kMaskMinGrid, kMaskMaxGrid);
  regionCols = clampInt(regionCols > 0 ? regionCols : 2, kMaskMinGrid, kMaskMaxGrid);

  // ZheZhao.dat 是单个输入合成画布遮罩，不按输出或区域分别保存。
  // When mask 模式 opens, copy the geometry hotspot lattice into that canvas:
  // 每个输入 tile 贡献 (rows-1)x(cols-1) 个单元格，共享 tile 边缘
  // 会变成共享遮罩行/列。
  const int maskRows =
      clampInt(rowsIn * (regionRows - 1) + 1, kMaskMinGrid, kMaskMaxGrid);
  const int maskCols =
      clampInt(colsIn * (regionCols - 1) + 1, kMaskMinGrid, kMaskMaxGrid);

  MaskState &maskState = state_.mask;
  maskState.rows = maskRows;
  maskState.cols = maskCols;
  maskState.interpolationMode = interpolationMode;
  maskState.enabled = true;
  maskState.showGuide = true;
  maskState.points.clear();
  maskState.points.reserve(static_cast<size_t>(maskRows * maskCols));
  for (int r = 0; r < maskRows; ++r) {
    const float v =
        maskRows > 1 ? static_cast<float>(r) / static_cast<float>(maskRows - 1)
                     : 0.5f;
    for (int c = 0; c < maskCols; ++c) {
      const float u =
          maskCols > 1 ? static_cast<float>(c) / static_cast<float>(maskCols - 1)
                       : 0.5f;
      maskState.points.push_back({u, v});
    }
  }

  const int activeRegionId = clampInt(state_.activeRegionId, 1, regionCount);
  const int activeIndex = activeRegionId - 1;
  const int activeInputRow = activeIndex / colsIn;
  const int activeInputCol = activeIndex % colsIn;
  GeometryRegionState &activeGeometry = geometry(activeRegionId);
  ensureGeometryPoints(activeGeometry);
  const int localRow = clampInt(activeGeometry.selected.row, 0, regionRows - 1);
  const int localCol = clampInt(activeGeometry.selected.col, 0, regionCols - 1);
  maskState.selected.row =
      clampInt(activeInputRow * (regionRows - 1) + localRow, 0, maskRows - 1);
  maskState.selected.col =
      clampInt(activeInputCol * (regionCols - 1) + localCol, 0, maskCols - 1);
  clampMaskSelectionToPerimeter(maskState);

  return maskToApiJson(maskState);
}

Json::Value FusionManager::resizeMaskGrid(const std::string &op) {
  MaskState &maskState = state_.mask;
  ensureMaskPoints(maskState);
  int nextRows =
      resizeGridCount(maskState.rows, op, "grow_rows", "shrink_rows", kMaskMaxGrid);
  int nextCols =
      resizeGridCount(maskState.cols, op, "grow_cols", "shrink_cols", kMaskMaxGrid);
  resizeMaskTo(maskState, nextRows, nextCols);
  return maskToApiJson(maskState);
}

Json::Value FusionManager::moveMask(const std::string &op, float du, float dv,
                                    int row, int col) {
  applyMaskMoveLocal(state_.mask, op, du, dv, row, col);
  return maskToApiJson(state_.mask);
}

Json::Value FusionManager::getBlendApi(int regionId) {
  return blendToApiJson(blend(regionId));
}

Json::Value FusionManager::setBlendState(int regionId,
                                         const Json::Value &payload,
                                         const Json::Value &adjacencyByRegion) {
  BlendRegionState &region = blend(regionId);
  const bool guideGridOnly =
      payload.get("guide_grid_only", false).asBool();
  if (payload.isMember("blend_grid_rows") && payload["blend_grid_rows"].isNumeric()) {
    region.gridRows = clampInt(payload["blend_grid_rows"].asInt(), 2, 33);
  }
  if (payload.isMember("blend_grid_cols") && payload["blend_grid_cols"].isNumeric()) {
    region.gridCols = clampInt(payload["blend_grid_cols"].asInt(), 2, 33);
  }
  if (guideGridOnly) {
    return blendToApiJson(region);
  }
  patchSideFromApi(region.left, payload, "left", 'l');
  patchSideFromApi(region.right, payload, "right", 'r');
  patchSideFromApi(region.top, payload, "top", 't');
  patchSideFromApi(region.bottom, payload, "bottom", 'b');
  const float rangeX = legacyMergeRangeForGrid(region.gridCols);
  const float rangeY = legacyMergeRangeForGrid(region.gridRows);
  if (region.left.enabled && region.left.width <= 0.001f) region.left.width = rangeX;
  if (region.right.enabled && region.right.width <= 0.001f) region.right.width = rangeX;
  if (region.top.enabled && region.top.width <= 0.001f) region.top.width = rangeY;
  if (region.bottom.enabled && region.bottom.width <= 0.001f) region.bottom.width = rangeY;
  if (state_.blendAutoEdges) {
    applyAutoBlendEdges(region, regionId, adjacencyByRegion);
    if (region.left.enabled && region.left.width <= 0.001f) region.left.width = rangeX;
    if (region.right.enabled && region.right.width <= 0.001f) region.right.width = rangeX;
    if (region.top.enabled && region.top.width <= 0.001f) region.top.width = rangeY;
    if (region.bottom.enabled && region.bottom.width <= 0.001f) region.bottom.width = rangeY;
  }
  return blendToApiJson(region);
}

Json::Value FusionManager::autoRecalculateBlend(
    const Json::Value &adjacencyByRegion) {
  Json::Value out(Json::objectValue);
  Json::Value regions(Json::arrayValue);
  for (auto &entry : state_.geometryByRegion) {
    const int regionId = entry.first;
    GeometryRegionState &geo = geometry(regionId);
    BlendRegionState &region = blend(regionId);
    region.gridRows = geo.rows;
    region.gridCols = geo.cols;

    const float rangeX = legacyMergeRangeForGrid(region.gridCols);
    const float rangeY = legacyMergeRangeForGrid(region.gridRows);
    // 自动重算始终刷新融合网格和宽度。自动模式下按输入相邻关系打开边，
    // 手动模式下保留用户选择，避免加减行列后手动开关表现成“随机回跳”。
    region.left.width = rangeX;
    region.right.width = rangeX;
    region.top.width = rangeY;
    region.bottom.width = rangeY;
    if (state_.blendAutoEdges) {
      applyAutoBlendEdges(region, regionId, adjacencyByRegion);
    }

    Json::Value item = blendToApiJson(region);
    item["region_id"] = regionId;
    item["range_x"] = rangeX;
    item["range_y"] = rangeY;
    regions.append(item);
  }
  out["regions"] = regions;
  return out;
}

Json::Value FusionManager::setMergeGapBrightness(int regionId, FusionSide side,
                                                 int colorId, int value) {
  BlendRegionState &region = blend(regionId);
  BlendSideState &edge = sideState(region, side);
  if (colorId >= 0 && colorId < 3) {
    edge.bright[static_cast<size_t>(colorId)] = clampInt(value, 0, 255);
  }
  return blendToApiJson(region);
}

Json::Value FusionManager::getCorrectionApi(int regionId) {
  return correctionToApiJson(correction(regionId));
}

Json::Value FusionManager::setCorrectionState(int regionId,
                                              const Json::Value &payload) {
  GeometryCorrectionState &state = correction(regionId);
  patchCorrectionFromApi(state, payload);
  return correctionToApiJson(state);
}

Json::Value FusionManager::getCaveWallApi(int regionId) {
  return caveWallToApiJson(caveWall(regionId));
}

Json::Value FusionManager::setCaveWallState(int regionId,
                                            const Json::Value &payload) {
  CaveWallState &state = caveWall(regionId);
  patchCaveWallFromApi(state, payload);
  return caveWallToApiJson(state);
}

} // 命名空间 hsvj::fusion
