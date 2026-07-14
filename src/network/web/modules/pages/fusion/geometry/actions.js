import { ensureGeometryRegion, getState } from '../actions.js?v=2.95';
import { clamp, createGeometryPointGrid } from '../utils/grid.js';
import { normalizeFlatPoints } from '../utils/points.js';

function isGeometrySavable(region) {
    return !!(
        region &&
        region.loaded &&
        Number.isInteger(region.rows) && region.rows >= 2 &&
        Number.isInteger(region.cols) && region.cols >= 2 &&
        Array.isArray(region.points) &&
        region.points.length === region.rows * region.cols &&
        region.points.every((point) => Number.isFinite(point.u) && Number.isFinite(point.v))
    );
}

function looksLikeUnitLocalGeometry(points, rows, cols) {
    if (!Array.isArray(points) || points.length !== rows * cols) return false;
    let minU = 1;
    let maxU = 0;
    let minV = 1;
    let maxV = 0;
    const eps = 0.0001;
    for (const point of points) {
        if (point.u < -eps || point.u > 1 + eps || point.v < -eps || point.v > 1 + eps) {
            return false;
        }
        minU = Math.min(minU, point.u);
        maxU = Math.max(maxU, point.u);
        minV = Math.min(minV, point.v);
        maxV = Math.max(maxV, point.v);
    }
    return minU <= eps && minV <= eps && maxU >= 1 - eps && maxV >= 1 - eps;
}

function normalizeGeometryPoints(points, rows, cols) {
    if (!looksLikeUnitLocalGeometry(points, rows, cols)) return points;
    return points.map((point) => ({
        u: point.u * 2 - 1,
        v: 1 - point.v * 2
    }));
}

export function initializeGeometryRegion(regionId, rows = 2, cols = 2) {
    const region = ensureGeometryRegion(regionId);
    region.loaded = false;
    region.rows = rows;
    region.cols = cols;
    region.points = createGeometryPointGrid(rows, cols);
    region.selected = { row: 0, col: 0 };
    region.showGrid = false; // 确保几何网格默认关闭
    return region;
}

export function hydrateGeometryRegion(regionId, payload = {}) {
    const region = ensureGeometryRegion(regionId);
    const rows = clamp(Number(payload.rows) || region.rows || 2, 2, 33);
    const cols = clamp(Number(payload.cols) || region.cols || 2, 2, 33);
    const points = normalizeFlatPoints(payload.corners, rows, cols, {
        geometry: true
    });

    region.rows = rows;
    region.cols = cols;
    region.srcX = Number.isFinite(Number(payload.srcX)) ? Number(payload.srcX) : region.srcX;
    region.srcY = Number.isFinite(Number(payload.srcY)) ? Number(payload.srcY) : region.srcY;
    region.srcWidth = Number.isFinite(Number(payload.srcWidth)) ? Number(payload.srcWidth) : region.srcWidth;
    region.srcHeight = Number.isFinite(Number(payload.srcHeight)) ? Number(payload.srcHeight) : region.srcHeight;
    region.outX = Number.isFinite(Number(payload.outX)) ? Number(payload.outX) : region.outX;
    region.outY = Number.isFinite(Number(payload.outY)) ? Number(payload.outY) : region.outY;
    region.outWidth = Number.isFinite(Number(payload.outWidth)) ? Number(payload.outWidth) : region.outWidth;
    region.outHeight = Number.isFinite(Number(payload.outHeight)) ? Number(payload.outHeight) : region.outHeight;
    region.outputRow = Number.isFinite(Number(payload.output_row)) ? Number(payload.output_row) : region.outputRow;
    region.outputCol = Number.isFinite(Number(payload.output_col)) ? Number(payload.output_col) : region.outputCol;
    region.interpolationMode = payload.interpolation_mode === 1 ? 1 : 0;
    region.showGrid = typeof payload.show_grid === 'boolean' ? payload.show_grid : false; // 默认关闭几何网格

    const payloadRow = Number(payload.selected_row);
    const payloadCol = Number(payload.selected_col);
    const showGrid = region.showGrid;

    region.selected = {
        row: (showGrid && (payloadRow < 0 || !Number.isFinite(payloadRow))) ? 0 : clamp(payloadRow || 0, 0, rows - 1),
        col: (showGrid && (payloadCol < 0 || !Number.isFinite(payloadCol))) ? 0 : clamp(payloadCol || 0, 0, cols - 1),
        axis: region.selected?.axis || 'col'
    };
    if (getState().page.activeTab === 'blend' && getState().blend.managerMode) {
        const corner = getManagerCornerSelection(region);
        region.selected.row = corner.row;
        region.selected.col = corner.col;
    }

    if (points) {
        region.points = normalizeGeometryPoints(points, rows, cols);
        region.loaded = true;
    } else {
        region.points = createGeometryPointGrid(rows, cols);
        region.loaded = false;
    }

    return region;
}

export function setGeometrySelection(regionId, row, col, axis = null) {
    const region = ensureGeometryRegion(regionId);
    region.selected = {
        row: clamp(row, 0, region.rows - 1),
        col: clamp(col, 0, region.cols - 1),
        axis: axis || region.selected?.axis || 'col'
    };
    return region;
}

export function moveGeometrySelection(regionId, dRow, dCol) {
    const region = ensureGeometryRegion(regionId);
    const axis = dCol !== 0 ? 'col' : dRow !== 0 ? 'row' : region.selected.axis;
    return setGeometrySelection(regionId, region.selected.row + dRow, region.selected.col + dCol, axis);
}

function managerCornerRow(region, row = region.selected?.row ?? 0) {
    return row < region.rows / 2 ? 0 : region.rows - 1;
}

function managerCornerCol(region, col = region.selected?.col ?? 0) {
    return col < region.cols / 2 ? 0 : region.cols - 1;
}

export function getManagerCornerSelection(region) {
    if (!region) return { row: 0, col: 0 };
    return {
        row: managerCornerRow(region),
        col: managerCornerCol(region)
    };
}

export function getManagerCornerName(region) {
    const corner = getManagerCornerSelection(region);
    return `${corner.row === 0 ? 'top' : 'bottom'}-${corner.col === 0 ? 'left' : 'right'}`;
}

export function snapManagerCornerSelection(regionId) {
    const region = ensureGeometryRegion(regionId);
    const corner = getManagerCornerSelection(region);
    return setGeometrySelection(regionId, corner.row, corner.col, region.selected?.axis || 'col');
}

export function setManagerCornerSelection(regionId, row, col) {
    const region = ensureGeometryRegion(regionId);
    const cornerRow = managerCornerRow(region, row);
    const cornerCol = managerCornerCol(region, col);
    return setGeometrySelection(regionId, cornerRow, cornerCol, region.selected?.axis || 'col');
}

export function setManagerSideSelection(regionId, side) {
    const region = ensureGeometryRegion(regionId);
    const corner = getManagerCornerSelection(region);
    let row = corner.row;
    let col = corner.col;
    if (side === 'left') col = 0;
    if (side === 'right') col = region.cols - 1;
    if (side === 'top') row = 0;
    if (side === 'bottom') row = region.rows - 1;
    return setGeometrySelection(regionId, row, col, region.selected?.axis || 'col');
}

export function moveManagerCornerSelection(regionId, dRow, dCol) {
    const region = ensureGeometryRegion(regionId);
    const currentRow = managerCornerRow(region);
    const currentCol = managerCornerCol(region);
    const nextRow = dRow < 0 ? 0 : dRow > 0 ? region.rows - 1 : currentRow;
    const nextCol = dCol < 0 ? 0 : dCol > 0 ? region.cols - 1 : currentCol;
    const axis = dCol !== 0 ? 'col' : dRow !== 0 ? 'row' : region.selected?.axis || 'col';
    return setGeometrySelection(regionId, nextRow, nextCol, axis);
}

function getPoint(region, row, col) {
    return region.points[row * region.cols + col] || {
        u: region.cols > 1 ? -1 + (2 * col) / (region.cols - 1) : 0,
        v: region.rows > 1 ? 1 - (2 * row) / (region.rows - 1) : 0
    };
}

function movePoint(region, row, col, du, dv, weight = 1) {
    const index = row * region.cols + col;
    const point = region.points[index];
    if (!point) return;
    point.u += du * weight;
    point.v += dv * weight;
}

function safeRatio(numerator, denominator) {
    if (Math.abs(denominator) < 1e-6) return 0;
    return clamp(numerator / denominator, 0, 1);
}

function safeWeight(numerator, denominator) {
    if (Math.abs(denominator) < 1e-6) return 0;
    return numerator / denominator;
}

function pointDistance(a, b) {
    const du = a.u - b.u;
    const dv = a.v - b.v;
    return Math.sqrt(du * du + dv * dv);
}

function applyWeightedEdgeMove(region, direction, du, dv) {
    if (!region || region.rows < 2 || region.cols < 2) return region;
    if (direction === 2) {
        for (let row = 0; row < region.rows; row += 1) {
            const left = getPoint(region, row, 0);
            const right = getPoint(region, row, region.cols - 1);
            for (let col = 0; col < region.cols; col += 1) {
                const p = getPoint(region, row, col);
                const weight = safeRatio(p.u - right.u, left.u - right.u);
                movePoint(region, row, col, du, 0, weight);
            }
        }
    } else if (direction === 3) {
        for (let row = 0; row < region.rows; row += 1) {
            const left = getPoint(region, row, 0);
            const right = getPoint(region, row, region.cols - 1);
            for (let col = 0; col < region.cols; col += 1) {
                const p = getPoint(region, row, col);
                const weight = safeRatio(p.u - left.u, right.u - left.u);
                movePoint(region, row, col, du, 0, weight);
            }
        }
    } else if (direction === 0) {
        for (let col = 0; col < region.cols; col += 1) {
            const top = getPoint(region, 0, col);
            const bottom = getPoint(region, region.rows - 1, col);
            for (let row = 0; row < region.rows; row += 1) {
                const p = getPoint(region, row, col);
                const weight = safeRatio(p.v - bottom.v, top.v - bottom.v);
                movePoint(region, row, col, 0, dv, weight);
            }
        }
    } else if (direction === 1) {
        for (let col = 0; col < region.cols; col += 1) {
            const top = getPoint(region, 0, col);
            const bottom = getPoint(region, region.rows - 1, col);
            for (let row = 0; row < region.rows; row += 1) {
                const p = getPoint(region, row, col);
                const weight = safeRatio(p.v - top.v, bottom.v - top.v);
                movePoint(region, row, col, 0, dv, weight);
            }
        }
    }
    return region;
}

function edgeDirectionFromOp(op) {
    if (op === 'edge_top') return 0;
    if (op === 'edge_bottom') return 1;
    if (op === 'edge_left') return 2;
    if (op === 'edge_right') return 3;
    return -1;
}

export function getManagerLineSelection(region) {
    return getManagerCornerSelection(region);
}

export function applyGeometryMoveLocal(regionId, op, du, dv) {
    const region = ensureGeometryRegion(regionId);
    if (op === 'point') {
        movePoint(region, clamp(region.selected.row, 0, region.rows - 1), clamp(region.selected.col, 0, region.cols - 1), du, dv);
    } else if (op === 'row') {
        const row = clamp(region.selected.row, 0, region.rows - 1);
        for (let col = 0; col < region.cols; col += 1) movePoint(region, row, col, du, dv);
    } else if (op === 'col') {
        const col = clamp(region.selected.col, 0, region.cols - 1);
        for (let row = 0; row < region.rows; row += 1) movePoint(region, row, col, du, dv);
    } else if (op === 'all') {
        for (let row = 0; row < region.rows; row += 1) {
            for (let col = 0; col < region.cols; col += 1) movePoint(region, row, col, du, dv);
        }
    } else {
        const edgeDirection = edgeDirectionFromOp(op);
        if (edgeDirection >= 0) {
            applyWeightedEdgeMove(region, edgeDirection, du, dv);
        } else if (op === 'corner_left_top' || op === 'corner_right_top' || op === 'corner_left_bottom' || op === 'corner_right_bottom') {
            for (let row = 0; row < region.rows; row += 1) {
                for (let col = 0; col < region.cols; col += 1) {
                    const p = getPoint(region, row, col);
                    const left = getPoint(region, row, 0);
                    const right = getPoint(region, row, region.cols - 1);
                    const top = getPoint(region, 0, col);
                    const bottom = getPoint(region, region.rows - 1, col);
                    const tx = safeRatio(p.u - left.u, right.u - left.u);
                    const ty = safeRatio(p.v - top.v, bottom.v - top.v);
                    const wx = (op === 'corner_left_top' || op === 'corner_left_bottom') ? 1 - tx : tx;
                    const wy = (op === 'corner_left_top' || op === 'corner_right_top') ? 1 - ty : ty;
                    movePoint(region, row, col, du, dv, wx * wy);
                }
            }
        } else {
            return region;
        }
    }
    return region;
}

export function applyManagerMovePointLocal(regionId, direction, du, dv, corner = null) {
    const region = ensureGeometryRegion(regionId);
    if (region.rows < 2 || region.cols < 2) return region;
    const activeCorner = corner || 'top-left';
    const cornerRow = activeCorner.startsWith('top') ? 0 : region.rows - 1;
    const cornerCol = activeCorner.endsWith('left') ? 0 : region.cols - 1;

    if (direction === 2 || direction === 3) {
        if (cornerCol === 0) {
            const l1 = pointDistance(getPoint(region, 0, 0), getPoint(region, region.rows - 1, 0));
            for (let row = 0; row < region.rows; row += 1) {
                const left = getPoint(region, row, 0);
                const right = getPoint(region, row, region.cols - 1);
                const l2 = cornerRow === 0
                    ? pointDistance(left, getPoint(region, region.rows - 1, 0))
                    : pointDistance(left, getPoint(region, 0, 0));
                for (let col = 0; col < region.cols; col += 1) {
                    const p = getPoint(region, row, col);
                    const wx = safeRatio(p.u - right.u, left.u - right.u);
                    const wy = safeWeight(l2, l1);
                    movePoint(region, row, col, du, 0, wx * wy);
                }
            }
        } else {
            const l1 = pointDistance(getPoint(region, 0, region.cols - 1), getPoint(region, region.rows - 1, region.cols - 1));
            for (let row = 0; row < region.rows; row += 1) {
                const left = getPoint(region, row, 0);
                const right = getPoint(region, row, region.cols - 1);
                const l2 = cornerRow === 0
                    ? pointDistance(right, getPoint(region, region.rows - 1, region.cols - 1))
                    : pointDistance(right, getPoint(region, 0, region.cols - 1));
                for (let col = 0; col < region.cols; col += 1) {
                    const p = getPoint(region, row, col);
                    const wx = safeRatio(p.u - left.u, right.u - left.u);
                    const wy = safeWeight(l2, l1);
                    movePoint(region, row, col, du, 0, wx * wy);
                }
            }
        }
    } else if (direction === 0 || direction === 1) {
        if (cornerRow === 0) {
            const l1 = pointDistance(getPoint(region, 0, 0), getPoint(region, 0, region.cols - 1));
            for (let col = 0; col < region.cols; col += 1) {
                const top = getPoint(region, 0, col);
                const bottom = getPoint(region, region.rows - 1, col);
                const l2 = cornerCol === 0
                    ? pointDistance(top, getPoint(region, 0, region.cols - 1))
                    : pointDistance(top, getPoint(region, 0, 0));
                for (let row = 0; row < region.rows; row += 1) {
                    const p = getPoint(region, row, col);
                    const wy = safeRatio(p.v - bottom.v, top.v - bottom.v);
                    const wx = safeWeight(l2, l1);
                    movePoint(region, row, col, 0, dv, wx * wy);
                }
            }
        } else {
            const l1 = pointDistance(getPoint(region, region.rows - 1, 0), getPoint(region, region.rows - 1, region.cols - 1));
            for (let col = 0; col < region.cols; col += 1) {
                const top = getPoint(region, 0, col);
                const bottom = getPoint(region, region.rows - 1, col);
                const l2 = cornerCol === 0
                    ? pointDistance(bottom, getPoint(region, region.rows - 1, region.cols - 1))
                    : pointDistance(bottom, getPoint(region, region.rows - 1, 0));
                for (let row = 0; row < region.rows; row += 1) {
                    const p = getPoint(region, row, col);
                    const wy = safeRatio(p.v - top.v, bottom.v - top.v);
                    const wx = safeWeight(l2, l1);
                    movePoint(region, row, col, 0, dv, wx * wy);
                }
            }
        }
    }
    return region;
}

export function applyManagerMoveLineLocal(regionId, direction, du, dv) {
    const region = ensureGeometryRegion(regionId);
    if (region.rows < 2 || region.cols < 2 || direction < 0 || direction > 3) return region;
    snapManagerCornerSelection(regionId);
    const selected = getManagerLineSelection(region);
    const edgeCol = (selected.col ?? 0) < region.cols / 2 ? 0 : region.cols - 1;
    const edgeRow = (selected.row ?? 0) < region.rows / 2 ? 0 : region.rows - 1;

    if (direction === 2 || direction === 3) {
        for (let row = 0; row < region.rows; row += 1) {
            const left = getPoint(region, row, 0);
            const right = getPoint(region, row, region.cols - 1);
            for (let col = 0; col < region.cols; col += 1) {
                const p = getPoint(region, row, col);
                const weight = edgeCol === 0
                    ? safeRatio(p.u - right.u, left.u - right.u)
                    : safeRatio(p.u - left.u, right.u - left.u);
                movePoint(region, row, col, du, 0, weight);
            }
        }
    } else {
        for (let col = 0; col < region.cols; col += 1) {
            const top = getPoint(region, 0, col);
            const bottom = getPoint(region, region.rows - 1, col);
            for (let row = 0; row < region.rows; row += 1) {
                const p = getPoint(region, row, col);
                const weight = edgeRow === 0
                    ? safeRatio(p.v - bottom.v, top.v - bottom.v)
                    : safeRatio(p.v - top.v, bottom.v - top.v);
                movePoint(region, row, col, 0, dv, weight);
            }
        }
    }
    return region;
}

function resizePoints(region, nextRows, nextCols) {
    const oldRows = region.rows;
    const oldCols = region.cols;
    if (oldRows < 2 || oldCols < 2 || nextRows < 2 || nextCols < 2) return region.points.slice();

    let afterCols = [];
    if (nextCols === (oldCols - 1) * 2 + 1) {
        for (let r = 0; r < oldRows; r += 1) {
            for (let c = 0; c < nextCols; c += 1) {
                if ((c & 1) === 0) {
                    const p = getPoint(region, r, c / 2);
                    afterCols.push({ u: p.u, v: p.v });
                } else {
                    const a = getPoint(region, r, Math.floor(c / 2));
                    const b = getPoint(region, r, Math.floor(c / 2) + 1);
                    afterCols.push({ u: (a.u + b.u) * 0.5, v: (a.v + b.v) * 0.5 });
                }
            }
        }
    } else if (nextCols === Math.floor((oldCols - 1) / 2) + 1) {
        for (let r = 0; r < oldRows; r += 1) {
            for (let c = 0; c < nextCols; c += 1) {
                const srcC = c === nextCols - 1 ? oldCols - 1 : c * 2;
                const p = getPoint(region, r, srcC);
                afterCols.push({ u: p.u, v: p.v });
            }
        }
    } else if (nextCols === oldCols) {
        afterCols = region.points.map((p) => ({ u: p.u, v: p.v }));
    } else {
        return region.points.slice();
    }

    const getAfter = (r, c) => afterCols[r * nextCols + c] || {
        u: nextCols > 1 ? -1 + (2 * c) / (nextCols - 1) : 0,
        v: oldRows > 1 ? 1 - (2 * r) / (oldRows - 1) : 0
    };
    const out = [];
    if (nextRows === (oldRows - 1) * 2 + 1) {
        for (let r = 0; r < nextRows; r += 1) {
            for (let c = 0; c < nextCols; c += 1) {
                if ((r & 1) === 0) {
                    const p = getAfter(r / 2, c);
                    out.push({ u: p.u, v: p.v });
                } else {
                    const a = getAfter(Math.floor(r / 2), c);
                    const b = getAfter(Math.floor(r / 2) + 1, c);
                    out.push({ u: (a.u + b.u) * 0.5, v: (a.v + b.v) * 0.5 });
                }
            }
        }
        return out;
    }
    if (nextRows === Math.floor((oldRows - 1) / 2) + 1) {
        for (let r = 0; r < nextRows; r += 1) {
            const srcR = r === nextRows - 1 ? oldRows - 1 : r * 2;
            for (let c = 0; c < nextCols; c += 1) {
                const p = getAfter(srcR, c);
                out.push({ u: p.u, v: p.v });
            }
        }
        return out;
    }
    return nextRows === oldRows ? afterCols : region.points.slice();
}

export function resizeGeometryRegionTo(regionId, rows, cols) {
    const region = ensureGeometryRegion(regionId);
    const nextRows = clamp(Math.floor(Number(rows) || region.rows || 2), 2, 33);
    const nextCols = clamp(Math.floor(Number(cols) || region.cols || 2), 2, 33);
    if (region.rows === nextRows && region.cols === nextCols && region.points.length === nextRows * nextCols) {
        return region;
    }
    region.points = resizePoints(region, nextRows, nextCols);
    region.rows = nextRows;
    region.cols = nextCols;
    region.selected = {
        row: clamp(region.selected?.row ?? 0, 0, nextRows - 1),
        col: clamp(region.selected?.col ?? 0, 0, nextCols - 1),
        axis: region.selected?.axis || 'col'
    };
    region.loaded = true;
    return region;
}

export function resizeGeometryRegionByOp(regionId, op) {
    const region = ensureGeometryRegion(regionId);
    let nextRows = region.rows;
    let nextCols = region.cols;
    if (op === 'grow_rows') nextRows = (region.rows - 1) * 2 + 1;
    else if (op === 'shrink_rows') nextRows = Math.floor((region.rows - 1) / 2) + 1;
    else if (op === 'grow_cols') nextCols = (region.cols - 1) * 2 + 1;
    else if (op === 'shrink_cols') nextCols = Math.floor((region.cols - 1) / 2) + 1;
    else return region;

    return resizeGeometryRegionTo(regionId, nextRows, nextCols);
}

export function setGeometryGridVisibleAllRegions(regionIds, showGrid) {
    if (!Array.isArray(regionIds)) return;
    regionIds.forEach((id) => {
        const region = ensureGeometryRegion(id);
        region.showGrid = !!showGrid;
    });
}

export function setInterpolationMode(regionId, mode) {
    const region = ensureGeometryRegion(regionId);
    region.interpolationMode = mode === 1 ? 1 : 0;
    return region;
}

export function canSyncGeometryRegion(regionId) {
    return isGeometrySavable(ensureGeometryRegion(regionId));
}
