import { activeRegion, state } from './state.js?v=80';

const GEOMETRY_GRID_COLORS = {
    line: 'rgba(255, 255, 255, 0.74)',
    activeLine: '#ffffff',
    selectedLine: '#00ff00',
    point: '#38d9ff',
    pointStroke: 'rgba(2, 8, 15, .92)',
    selected: '#ff2d2d',
    selectedStroke: '#ffffff'
};

const BLEND_GRID_COLORS = {
    line: '#ffffff',
    activeLine: '#ffff00',
    selectedLine: '#00ff00',
    point: '#00ffff',
    pointStroke: 'rgba(2, 8, 15, .92)',
    selected: '#ff2d2d',
    selectedStroke: '#ffffff'
};

const MASK_GRID_COLORS = {
    line: '#00ffff',
    selectedLine: '#ff00ff',
    point: '#00ffff',
    pointStroke: 'rgba(2, 8, 15, .92)',
    selected: '#ff2d2d',
    selectedStroke: '#ffffff'
};

const EPSILON = 1e-6;

function clear(ctx, canvas) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#10141c';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
}

function fitRect(width, height, aspectRatio, pad = 18) {
    const availableW = Math.max(10, width - pad * 2);
    const availableH = Math.max(10, height - pad * 2);
    let rectW = availableW;
    let rectH = rectW / aspectRatio;
    if (rectH > availableH) {
        rectH = availableH;
        rectW = rectH * aspectRatio;
    }
    return {
        x: (width - rectW) / 2,
        y: (height - rectH) / 2,
        width: rectW,
        height: rectH
    };
}

function drawRoundedRect(ctx, rect, radius = 10) {
    const r = Math.min(radius, rect.width / 2, rect.height / 2);
    ctx.beginPath();
    ctx.moveTo(rect.x + r, rect.y);
    ctx.lineTo(rect.x + rect.width - r, rect.y);
    ctx.quadraticCurveTo(rect.x + rect.width, rect.y, rect.x + rect.width, rect.y + r);
    ctx.lineTo(rect.x + rect.width, rect.y + rect.height - r);
    ctx.quadraticCurveTo(rect.x + rect.width, rect.y + rect.height, rect.x + rect.width - r, rect.y + rect.height);
    ctx.lineTo(rect.x + r, rect.y + rect.height);
    ctx.quadraticCurveTo(rect.x, rect.y + rect.height, rect.x, rect.y + rect.height - r);
    ctx.lineTo(rect.x, rect.y + r);
    ctx.quadraticCurveTo(rect.x, rect.y, rect.x + r, rect.y);
    ctx.closePath();
}

function drawRegionTile(ctx, rect, regionId, active = false, options = {}) {
    const showLabel = options.showLabel !== false;
    drawRoundedRect(ctx, rect, 8);
    ctx.fillStyle = active ? 'rgba(66, 153, 225, .28)' : 'rgba(255,255,255,.045)';
    ctx.fill();
    ctx.strokeStyle = active ? '#58a6ff' : 'rgba(255,255,255,.22)';
    ctx.lineWidth = active ? 2 : 1;
    ctx.stroke();

    if (showLabel) {
        ctx.fillStyle = active ? '#ffffff' : '#c3cfdd';
        ctx.font = `${Math.max(13, Math.min(22, rect.width * .11))}px system-ui, sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(String(regionId), rect.x + rect.width / 2, rect.y + rect.height / 2);
    }

}

function isCornerPosition(target, row, col) {
    const lastRow = Math.max(0, Number(target?.rows || 1) - 1);
    const lastCol = Math.max(0, Number(target?.cols || 1) - 1);
    return (row === 0 || row === lastRow) && (col === 0 || col === lastCol);
}

function isPerimeterPosition(target, row, col) {
    const lastRow = Math.max(0, Number(target?.rows || 1) - 1);
    const lastCol = Math.max(0, Number(target?.cols || 1) - 1);
    return row === 0 || row === lastRow || col === 0 || col === lastCol;
}

function geometryPointToLocal01(point) {
    return {
        u: (Number(point?.u) + 1) * 0.5,
        v: (1 - Number(point?.v)) * 0.5
    };
}

function local01ToGeometryPoint(point) {
    return {
        u: Number(point?.u) * 2 - 1,
        v: 1 - Number(point?.v) * 2
    };
}

function lerpPoint(a, b, t) {
    return {
        u: a.u * (1 - t) + b.u * t,
        v: a.v * (1 - t) + b.v * t
    };
}

function sampleGeometryLinear(geometry, u, v) {
    const rows = Math.max(2, Number(geometry?.rows) || 2);
    const cols = Math.max(2, Number(geometry?.cols) || 2);
    const srcC = Math.max(0, Math.min(1, Number(u) || 0)) * (cols - 1);
    const srcR = Math.max(0, Math.min(1, Number(v) || 0)) * (rows - 1);
    const c0 = Math.min(Math.floor(srcC), cols - 2);
    const r0 = Math.min(Math.floor(srcR), rows - 2);
    const c1 = c0 + 1;
    const r1 = r0 + 1;
    const fC = srcC - c0;
    const fR = srcR - r0;
    const pointAt = (row, col) => {
        const point = geometry?.points?.[row * cols + col];
        if (point) return geometryPointToLocal01(point);
        return {
            u: cols > 1 ? col / (cols - 1) : 0.5,
            v: rows > 1 ? row / (rows - 1) : 0.5
        };
    };
    const top = lerpPoint(pointAt(r0, c0), pointAt(r0, c1), fC);
    const bottom = lerpPoint(pointAt(r1, c0), pointAt(r1, c1), fC);
    return local01ToGeometryPoint(lerpPoint(top, bottom, fR));
}

function linearCurvePoint(points, t) {
    const n = points.length;
    if (n <= 0) return { u: 0, v: 0 };
    if (n === 1) return points[0];
    const scaled = clamp01(t) * (n - 1);
    const index = Math.min(n - 2, Math.max(0, Math.floor(scaled)));
    return lerpPoint(points[index], points[index + 1], scaled - index);
}

function legacySpline1D(input, t, xyScale = 1) {
    const n = input.length;
    if (n <= 0) return { u: 0, v: 0 };
    if (n === 1) return input[0];

    const linearAt = () => linearCurvePoint(input, t);
    if (n === 2) return linearAt();

    const scale = Math.abs(xyScale) > EPSILON ? xyScale : 1;
    const pts = input.map((point) => ({ u: point.u / scale, v: point.v }));
    const distance = (a, b) => Math.hypot(a.u - b.u, a.v - b.v);
    const safeDiv = (numerator, denominator) => Math.abs(denominator) > EPSILON ? numerator / denominator : 0;
    const L = n - 1;
    const N = n + 1;
    const M = 4;
    const curveTot = n + 2;
    const knot = new Array(n + 6).fill(0);
    knot[0] = 0;
    knot[1] = distance(pts[0], pts[1]);
    for (let i = 2; i < n; i += 1) knot[i] = knot[i - 1] + distance(pts[i - 1], pts[i]);
    if (knot[L] <= EPSILON) return input[0];

    const delta = new Array(n + 3).fill(0);
    for (let i = 1; i < n; i += 1) {
        delta[i] = knot[i] - knot[i - 1];
        if (delta[i] <= EPSILON) return linearAt();
    }

    const coeffA = new Array(n).fill(0);
    const coeffB = new Array(n).fill(0);
    const coeffC = new Array(n).fill(0);
    const coeffP = new Array(n).fill(0);
    const coeffQ = new Array(n).fill(0);
    const coeffR = new Array(n).fill(0);
    const coeffZ = new Array(n).fill(0);
    const coeffr = new Array(n).fill(0);

    coeffA[1] = safeDiv(delta[2] * delta[2], delta[1] + delta[2]);
    coeffB[0] = 1;
    coeffB[L] = 1;
    coeffB[1] = safeDiv(delta[2] * delta[1], delta[1] + delta[2]) +
        safeDiv(delta[1] * (delta[2] + delta[3]), delta[1] + delta[2] + delta[3]);
    coeffC[1] = safeDiv(delta[1] * delta[1], delta[1] + delta[2] + delta[3]);
    for (let i = 2; i <= L - 1; i += 1) {
        coeffA[i] = safeDiv(delta[i + 1] * delta[i + 1], delta[i - 1] + delta[i] + delta[i + 1]);
        coeffB[i] = safeDiv(delta[i + 1] * (delta[i - 1] + delta[i]), delta[i - 1] + delta[i] + delta[i + 1]) +
            safeDiv(delta[i] * (delta[i + 1] + delta[i + 2]), delta[i] + delta[i + 1] + delta[i + 2]);
        coeffC[i] = safeDiv(delta[i] * delta[i], delta[i] + delta[i + 1] + delta[i + 2]);
    }

    coeffQ[0] = coeffB[0];
    for (let i = 0; i < L; i += 1) coeffR[i] = coeffC[i];
    for (let i = 1; i <= L; i += 1) {
        if (Math.abs(coeffQ[i - 1]) <= EPSILON) return linearAt();
        coeffP[i] = coeffA[i] / coeffQ[i - 1];
        coeffQ[i] = coeffB[i] - coeffP[i] * coeffC[i - 1];
    }

    let beta = delta[1] + delta[2];
    if (Math.abs(beta) <= EPSILON) return linearAt();
    const startVec = {
        u: safeDiv(-(2 * delta[1] + delta[2]) * pts[0].u, delta[1] * beta) +
            safeDiv(beta * pts[1].u, delta[1] * delta[2]) -
            safeDiv(delta[1] * pts[2].u, delta[2] * beta),
        v: safeDiv(-(2 * delta[1] + delta[2]) * pts[0].v, delta[1] * beta) +
            safeDiv(beta * pts[1].v, delta[1] * delta[2]) -
            safeDiv(delta[1] * pts[2].v, delta[2] * beta)
    };

    beta = delta[L - 1] + delta[L];
    if (Math.abs(beta) <= EPSILON) return linearAt();
    const endVec = {
        u: safeDiv(delta[L] * pts[L - 2].u, delta[L - 1] * beta) -
            safeDiv(beta * pts[L - 1].u, delta[L - 1] * delta[L]) +
            safeDiv((2 * delta[L] + delta[L - 1]) * pts[L].u, beta * delta[L]),
        v: safeDiv(delta[L] * pts[L - 2].v, delta[L - 1] * beta) -
            safeDiv(beta * pts[L - 1].v, delta[L - 1] * delta[L]) +
            safeDiv((2 * delta[L] + delta[L - 1]) * pts[L].v, beta * delta[L])
    };

    const normalize = (vec) => {
        const len = Math.hypot(vec.u, vec.v);
        if (len <= EPSILON) return false;
        vec.u /= len;
        vec.v /= len;
        return true;
    };
    if (!normalize(startVec) || !normalize(endVec)) return linearAt();

    const curvePts = Array.from({ length: curveTot }, () => ({ u: 0, v: 0 }));
    curvePts[0] = pts[0];
    curvePts[1] = {
        u: pts[0].u + distance(pts[0], pts[1]) * startVec.u / 3,
        v: pts[0].v + distance(pts[0], pts[1]) * startVec.v / 3
    };
    curvePts[curveTot - 2] = {
        u: pts[L].u - distance(pts[L - 1], pts[L]) * endVec.u / 3,
        v: pts[L].v - distance(pts[L - 1], pts[L]) * endVec.v / 3
    };
    curvePts[N] = pts[L];

    const solveAxis = (key) => {
        coeffr[0] = curvePts[1][key];
        for (let i = 1; i < L; i += 1) coeffr[i] = (delta[i] + delta[i + 1]) * pts[i][key];
        coeffr[L] = curvePts[L + 1][key];
        coeffZ[0] = coeffr[0];
        for (let i = 1; i <= L; i += 1) coeffZ[i] = coeffr[i] - coeffP[i] * coeffZ[i - 1];
        if (Math.abs(coeffQ[L]) <= EPSILON) return false;
        curvePts[L + 1][key] = coeffZ[L] / coeffQ[L];
        for (let i = L - 1; i >= 0; i -= 1) {
            if (Math.abs(coeffQ[i]) <= EPSILON) return false;
            curvePts[i + 1][key] = (coeffZ[i] - coeffR[i] * curvePts[i + 2][key]) / coeffQ[i];
        }
        return true;
    };
    if (!solveAxis('u') || !solveAxis('v')) return linearAt();

    knot[N + M] = knot[N + M - 1] = knot[N + M - 2] = knot[N + M - 3] = knot[L];
    for (let i = L - 1; i > 0; i -= 1) knot[i + 3] = knot[i];
    knot[3] = knot[2] = knot[1] = knot[0] = 0;

    const scaledT = clamp01(t) * (n - 1);
    const seg = Math.min(n - 2, Math.max(0, Math.floor(scaledT)));
    const local = scaledT - seg;
    const i = 3 + seg;
    const segmentLength = knot[i + 1] - knot[i];
    if (segmentLength <= EPSILON) return linearAt();
    const kt = knot[i] + local * segmentLength;

    const N22 = safeDiv(knot[i + 1] - kt, knot[i + 1] - knot[i]);
    const N32 = safeDiv(kt - knot[i], knot[i + 1] - knot[i]);
    const N13 = safeDiv((knot[i + 1] - kt) * N22, knot[i + 1] - knot[i - 1]);
    const N23 = safeDiv((kt - knot[i - 1]) * N22, knot[i + 1] - knot[i - 1]) +
        safeDiv((knot[i + 2] - kt) * N32, knot[i + 2] - knot[i]);
    const N33 = safeDiv((kt - knot[i]) * N32, knot[i + 2] - knot[i]);
    const N04 = safeDiv((knot[i + 1] - kt) * N13, knot[i + 1] - knot[i - 2]);
    const N14 = safeDiv((kt - knot[i - 2]) * N13, knot[i + 1] - knot[i - 2]) +
        safeDiv((knot[i + 2] - kt) * N23, knot[i + 2] - knot[i - 1]);
    const N24 = safeDiv((kt - knot[i - 1]) * N23, knot[i + 2] - knot[i - 1]) +
        safeDiv((knot[i + 3] - kt) * N33, knot[i + 3] - knot[i]);
    const N34 = safeDiv((kt - knot[i]) * N33, knot[i + 3] - knot[i]);
    const denominator = N04 + N14 + N24 + N34;
    if (Math.abs(denominator) <= EPSILON) return linearAt();

    return {
        u: ((N04 * curvePts[i - 3].u + N14 * curvePts[i - 2].u + N24 * curvePts[i - 1].u + N34 * curvePts[i].u) / denominator) * scale,
        v: (N04 * curvePts[i - 3].v + N14 * curvePts[i - 2].v + N24 * curvePts[i - 1].v + N34 * curvePts[i].v) / denominator
    };
}

function sampleGeometrySurface(geometry, u, v, rect = null) {
    if (Number(geometry?.interpolationMode) !== 1) return sampleGeometryLinear(geometry, u, v);
    const rows = Math.max(2, Number(geometry?.rows) || 2);
    const cols = Math.max(2, Number(geometry?.cols) || 2);
    const xyScale = rect && rect.width > 0 ? rect.height / rect.width : 1;
    const vertical = [];
    const pointAt = (row, col) => {
        const point = geometry?.points?.[row * cols + col];
        return point ? geometryPointToLocal01(point) : {
            u: cols > 1 ? col / (cols - 1) : 0.5,
            v: rows > 1 ? row / (rows - 1) : 0.5
        };
    };
    for (let col = 0; col < cols; col += 1) {
        const column = [];
        for (let row = 0; row < rows; row += 1) column.push(pointAt(row, col));
        vertical.push(legacySpline1D(column, v, xyScale));
    }
    return local01ToGeometryPoint(legacySpline1D(vertical, u, xyScale));
}

function createBlendManagerDisplayGrid(geometry, blend) {
    if (!geometry || !blend) return geometry;
    const rows = Math.max(2, Number(blend.gridRows) || Number(geometry.rows) || 2);
    const cols = Math.max(2, Number(blend.gridCols) || Number(geometry.cols) || 2);
    const points = [];
    for (let row = 0; row < rows; row += 1) {
        const v = rows > 1 ? row / (rows - 1) : 0.5;
        for (let col = 0; col < cols; col += 1) {
            const u = cols > 1 ? col / (cols - 1) : 0.5;
            points.push(sampleGeometrySurface(geometry, u, v));
        }
    }
    // I 融合管理模式的显示网格是临时采样网格，不写回 geometry，避免加减线影响画面。
    return {
        ...geometry,
        rows,
        cols,
        points,
        selected: {
            row: Math.max(0, Math.min(rows - 1, Number(geometry.selected?.row) || 0)),
            col: Math.max(0, Math.min(cols - 1, Number(geometry.selected?.col) || 0))
        }
    };
}

function getOrderedRegions() {
    const rows = Math.max(1, Number(state.layout.grid_out_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_out_cols) || 1);
    const regions = state.regions || [];
    const ordered = [];
    for (let index = 0; index < rows * cols; index += 1) {
        const region = regions.find((item) => Number(item.output_index) === index) || regions[index];
        if (!region && index >= regions.length) continue;
        ordered.push({
            ...(region || {}),
            id: Number(region?.id || index + 1),
            output_index: Number(region?.output_index ?? index)
        });
    }
    return ordered;
}

function drawRegionGrid(ctx, rect) {
    const rows = Math.max(1, Number(state.layout.grid_out_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_out_cols) || 1);
    const ordered = getOrderedRegions();
    const cellW = rect.width / cols;
    const cellH = rect.height / rows;
    state.previewRects = [];

    ctx.strokeStyle = 'rgba(255,255,255,.18)';
    ctx.lineWidth = 1;
    ctx.strokeRect(rect.x, rect.y, rect.width, rect.height);

    for (let row = 0; row < rows; row += 1) {
        for (let col = 0; col < cols; col += 1) {
            const index = row * cols + col;
            const region = ordered.find((item) => Number(item.output_index) === index) || ordered[index];
            const regionId = Number(region?.id || index + 1);
            const x = rect.x + col * cellW;
            const y = rect.y + row * cellH;
            const active = Number(state.activeRegionId) === regionId;
            state.previewRects.push({ regionId, x, y, width: cellW, height: cellH });
            drawRegionTile(ctx, { x: x + 2, y: y + 2, width: cellW - 4, height: cellH - 4 }, regionId, active);
        }
    }
}

function firstFiniteNumber(...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number)) return number;
    }
    return 0;
}

function firstFinitePositive(...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number) && number > 0) return number;
    }
    return 0;
}

function clamp01(value) {
    return Math.max(0, Math.min(1, Number(value) || 0));
}

function getOutputCanvasAspect() {
    const width = Number(state.layout.canvas_out_width) || 0;
    const height = Number(state.layout.canvas_out_height) || 0;
    const tileW = Number(state.layout.tile_out_width) || 0;
    const tileH = Number(state.layout.tile_out_height) || 0;
    const rows = Math.max(1, Number(state.layout.grid_out_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_out_cols) || 1);
    if (width > 0 && height > 0) return width / height;
    if (tileW > 0 && tileH > 0) return (tileW * cols) / (tileH * rows);
    return cols / rows;
}

function getInputCanvasAspect() {
    const rows = Math.max(1, Number(state.layout.grid_in_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_in_cols) || Math.max(1, state.regions.length));
    const tileW = Number(state.layout.tile_in_width) || 0;
    const tileH = Number(state.layout.tile_in_height) || 0;
    const width = firstFinitePositive(state.layout.input_total_width, tileW * cols, state.layout.canvas_in_width, cols);
    const height = firstFinitePositive(state.layout.input_total_height, tileH * rows, state.layout.canvas_in_height, rows);
    return width > 0 && height > 0 ? width / height : cols / rows;
}

function getRegionSourceRect01(region) {
    const inputRows = Math.max(1, Number(state.layout.grid_in_rows) || 1);
    const inputCols = Math.max(1, Number(state.layout.grid_in_cols) || 1);
    const tileW = Number(state.layout.tile_in_width) || firstFinitePositive(region?.srcWidth, region?.src_width);
    const tileH = Number(state.layout.tile_in_height) || firstFinitePositive(region?.srcHeight, region?.src_height);
    const canvasW = firstFinitePositive(state.layout.input_total_width, tileW * inputCols, state.layout.canvas_in_width, tileW, 1);
    const canvasH = firstFinitePositive(state.layout.input_total_height, tileH * inputRows, state.layout.canvas_in_height, tileH, 1);
    const srcX = firstFiniteNumber(region?.srcX, region?.src_x, 0);
    const srcY = firstFiniteNumber(region?.srcY, region?.src_y, 0);
    const srcW = firstFinitePositive(region?.srcWidth, region?.src_width, tileW, canvasW);
    const srcH = firstFinitePositive(region?.srcHeight, region?.src_height, tileH, canvasH);
    const minU = clamp01(srcX / canvasW);
    const minV = clamp01(srcY / canvasH);
    const maxU = clamp01((srcX + srcW) / canvasW);
    const maxV = clamp01((srcY + srcH) / canvasH);
    return {
        minU: Math.min(minU, maxU),
        minV: Math.min(minV, maxV),
        maxU: Math.max(minU, maxU),
        maxV: Math.max(minV, maxV),
        width: Math.abs(maxU - minU),
        height: Math.abs(maxV - minV)
    };
}

function getMaskInputLayouts(mainArea, unit) {
    const rows = Math.max(1, Number(state.layout.grid_in_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_in_cols) || Math.max(1, state.regions.length));
    const tileW = Number(state.layout.tile_in_width) || Math.max(1, (Number(state.layout.input_total_width) || Number(state.layout.canvas_in_width) || cols) / cols);
    const tileH = Number(state.layout.tile_in_height) || Math.max(1, (Number(state.layout.input_total_height) || Number(state.layout.canvas_in_height) || rows) / rows);
    const inputRect = fitRect(mainArea.width, mainArea.height, getInputCanvasAspect(), 4 * unit);
    inputRect.x += mainArea.x;
    inputRect.y += mainArea.y;
    const regions = state.regions || [];
    const regionById = new Map(regions.map((region) => [Number(region?.id), region]));
    return Array.from({ length: rows * cols }, (_, index) => {
        const regionId = index + 1;
        const inputRow = Math.floor(index / cols);
        const inputCol = index % cols;
        const region = regionById.get(regionId) || {
            id: regionId,
            srcX: inputCol * tileW,
            srcY: inputRow * tileH,
            srcWidth: tileW,
            srcHeight: tileH
        };
        const sourceRect01 = getRegionSourceRect01(region);
        const rect = {
            x: inputRect.x + sourceRect01.minU * inputRect.width,
            y: inputRect.y + sourceRect01.minV * inputRect.height,
            width: Math.max(1, sourceRect01.width * inputRect.width),
            height: Math.max(1, sourceRect01.height * inputRect.height)
        };
        return {
            region,
            regionId,
            rect,
            outputRect: inputRect,
            sourceRect01
        };
    });
}

function getRegionAspect(region) {
    const tileInWidth = Number(state.layout.tile_in_width) || 0;
    const tileInHeight = Number(state.layout.tile_in_height) || 0;
    const tileOutWidth = Number(state.layout.tile_out_width) || 0;
    const tileOutHeight = Number(state.layout.tile_out_height) || 0;
    const sourceWidth = firstFiniteNumber(region?.srcWidth, region?.src_width);
    const sourceHeight = firstFiniteNumber(region?.srcHeight, region?.src_height);
    const outputWidth = Number(state.layout.canvas_out_width) || 0;
    const outputHeight = Number(state.layout.canvas_out_height) || 0;
    const outWidth = firstFiniteNumber(region?.outWidth, region?.out_width);
    const outHeight = firstFiniteNumber(region?.outHeight, region?.out_height);
    const cols = Math.max(1, Number(state.layout.grid_out_cols) || 1);
    const rows = Math.max(1, Number(state.layout.grid_out_rows) || 1);
    const candidates = [
        outWidth > 0 && outHeight > 0 ? outWidth / outHeight : null,
        sourceWidth > 0 && sourceHeight > 0 ? sourceWidth / sourceHeight : null,
        tileInWidth > 0 && tileInHeight > 0 ? tileInWidth / tileInHeight : null,
        tileOutWidth > 0 && tileOutHeight > 0 ? tileOutWidth / tileOutHeight : null,
        outputWidth > 0 && outputHeight > 0 ? (outputWidth / cols) / (outputHeight / rows) : null,
        outputWidth > 0 && outputHeight > 0 ? outputWidth / outputHeight : null
    ];
    const aspect = candidates.find((value) => Number.isFinite(value) && value >= 0.45 && value <= 3.2);
    if (aspect) return aspect;
    const outputAspect = (outputWidth || 16) / (outputHeight || 9);
    return Math.max(0.4, Math.min(4, outputAspect * rows / cols));
}

function maskPointAt(mask, u, v) {
    const rows = Math.max(2, Number(mask?.rows) || 2);
    const cols = Math.max(2, Number(mask?.cols) || 2);
    const srcC = clamp01(u) * (cols - 1);
    const srcR = clamp01(v) * (rows - 1);
    const c0 = Math.min(Math.floor(srcC), cols - 2);
    const r0 = Math.min(Math.floor(srcR), rows - 2);
    const c1 = c0 + 1;
    const r1 = r0 + 1;
    const fC = srcC - c0;
    const fR = srcR - r0;
    const pointAt = (row, col) => mask?.points?.[row * cols + col] || {
        u: cols > 1 ? col / (cols - 1) : 0.5,
        v: rows > 1 ? row / (rows - 1) : 0.5
    };
    const top = lerpPoint(pointAt(r0, c0), pointAt(r0, c1), fC);
    const bottom = lerpPoint(pointAt(r1, c0), pointAt(r1, c1), fC);
    return lerpPoint(top, bottom, fR);
}

function buildMaskGridPaths(mask) {
    const rows = Math.max(2, Number(mask?.rows) || 2);
    const cols = Math.max(2, Number(mask?.cols) || 2);
    const totalCells = (rows - 1) * (cols - 1);
    const divCnt = totalCells <= 16 ? 8 : (totalCells <= 100 ? 4 : 2);
    const makePath = (lineRow, lineCol, steps, sample) => {
        const points = [];
        for (let i = 0; i <= steps; i += 1) {
            const t = i / Math.max(1, steps);
            points.push(maskPointAt(mask, ...sample(t)));
        }
        return { lineRow, lineCol, points };
    };
    const paths = [];
    [0, rows - 1].forEach((row) => {
        const v = rows > 1 ? row / (rows - 1) : 0.5;
        paths.push(makePath(row, -1, Math.max(64, divCnt * (cols - 1)), (t) => [t, v]));
    });
    [0, cols - 1].forEach((col) => {
        const u = cols > 1 ? col / (cols - 1) : 0.5;
        paths.push(makePath(-1, col, Math.max(64, divCnt * (rows - 1)), (t) => [u, t]));
    });
    return paths;
}

function clipSegmentToSourceRect(a, b, rect) {
    if (!a || !b || !rect || rect.width <= 0 || rect.height <= 0) return null;
    let t0 = 0;
    let t1 = 1;
    const dx = b.u - a.u;
    const dy = b.v - a.v;
    const clip = (p, q) => {
        if (Math.abs(p) < 1e-6) return q >= 0;
        const r = q / p;
        if (p < 0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };
    if (!clip(-dx, a.u - rect.minU) ||
        !clip(dx, rect.maxU - a.u) ||
        !clip(-dy, a.v - rect.minV) ||
        !clip(dy, rect.maxV - a.v)) {
        return null;
    }
    return {
        a: { u: a.u + dx * t0, v: a.v + dy * t0 },
        b: { u: a.u + dx * t1, v: a.v + dy * t1 }
    };
}

function maskPointToLayoutScreen(point, layout) {
    const source = layout?.sourceRect01;
    const geometry = state.geometry[layout?.regionId];
    if (!point || !layout || !geometry || !source || source.width <= 0 || source.height <= 0) return null;
    const u = Number(point.u);
    const v = Number(point.v);
    const eps = 1e-5;
    if (!Number.isFinite(u) || !Number.isFinite(v) ||
        u < source.minU - eps || u > source.maxU + eps ||
        v < source.minV - eps || v > source.maxV + eps) {
        return null;
    }
    const localU = (u - source.minU) / source.width;
    const localV = (v - source.minV) / source.height;
    const deformed = geometryPointToLocal01(sampleGeometrySurface(geometry, localU, localV, layout.rect));
    return {
        x: layout.rect.x + deformed.u * layout.rect.width,
        y: layout.rect.y + deformed.v * layout.rect.height
    };
}

function drawProjectedMask(ctx, layouts, mask) {
    if (!mask || !mask.showGrid || !Array.isArray(mask.points)) return;
    const unit = Math.max(1, window.devicePixelRatio || 1);
    const selectedRow = Number(mask.selected?.row ?? 0);
    const selectedCol = Number(mask.selected?.col ?? 0);
    const paths = buildMaskGridPaths(mask);
    const segments = [];
    layouts.forEach((layout) => {
        const source = layout.sourceRect01;
        paths.forEach((path) => {
            let run = [];
            const flush = () => {
                if (run.length >= 2) segments.push({ ...path, layout, points: run });
                run = [];
            };
            for (let i = 0; i + 1 < path.points.length; i += 1) {
                const clipped = clipSegmentToSourceRect(path.points[i], path.points[i + 1], source);
                if (!clipped) {
                    flush();
                    continue;
                }
                const a = maskPointToLayoutScreen(clipped.a, layout);
                const b = maskPointToLayoutScreen(clipped.b, layout);
                if (!a || !b) {
                    flush();
                    continue;
                }
                if (!run.length) run.push(a);
                run.push(b);
            }
            flush();
        });
    });

    const selectedLine = (segment) => (
        (segment.lineRow >= 0 && segment.lineRow === selectedRow) ||
        (segment.lineCol >= 0 && segment.lineCol === selectedCol)
    );
    const drawSegment = (segment) => {
        ctx.save();
        ctx.beginPath();
        ctx.rect(segment.layout.outputRect.x, segment.layout.outputRect.y, segment.layout.outputRect.width, segment.layout.outputRect.height);
        ctx.clip();
        ctx.beginPath();
        segment.points.forEach((point, index) => {
            if (index === 0) ctx.moveTo(point.x, point.y);
            else ctx.lineTo(point.x, point.y);
        });
        ctx.stroke();
        ctx.restore();
    };

    ctx.lineWidth = 1.4 * unit;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.strokeStyle = MASK_GRID_COLORS.line;
    segments.forEach((segment) => {
        if (!selectedLine(segment)) drawSegment(segment);
    });
    // 移动端同样只显示旧项目 ZheZhao 的一份输入幕布遮罩；
    // 这里按各投影 sourceRect + 几何网格投影显示，不能恢复成独立平面遮罩预览。
    ctx.strokeStyle = MASK_GRID_COLORS.selectedLine;
    segments.forEach((segment) => {
        if (selectedLine(segment)) drawSegment(segment);
    });

    const selectedIndex = selectedRow * mask.cols + selectedCol;
    mask.points.forEach((point, index) => {
        const row = Math.floor(index / mask.cols);
        const col = index % mask.cols;
        if (!isPerimeterPosition(mask, row, col)) return;
        const selected = index === selectedIndex;
        layouts.forEach((layout) => {
            const screen = maskPointToLayoutScreen(point, layout);
            if (!screen) return;
            ctx.beginPath();
            ctx.arc(screen.x, screen.y, selected ? 4.6 * unit : 3.6 * unit, 0, Math.PI * 2);
            ctx.fillStyle = selected ? MASK_GRID_COLORS.selected : MASK_GRID_COLORS.point;
            ctx.fill();
            ctx.lineWidth = selected ? 2 * unit : 1.2 * unit;
            ctx.strokeStyle = selected ? MASK_GRID_COLORS.selectedStroke : MASK_GRID_COLORS.pointStroke;
            ctx.stroke();
        });
    });
}

function drawFocusedPreview(ctx, canvas) {
    const ordered = getOrderedRegions();
    const activeId = Number(state.activeRegionId);
    const active = ordered.find((region) => Number(region.id) === activeId) || activeRegion() || ordered[0];
    const isMaskMode = state.mode === 'mask';
    const otherRegions = [];
    const unit = Math.max(1, window.devicePixelRatio || 1);
    const pad = 6 * unit;
    const thumbGap = 10 * unit;
    const thumbH = 0;
    const footerH = 0;
    const mainArea = {
        x: pad,
        y: pad,
        width: canvas.width - pad * 2,
        height: canvas.height - pad * 2 - footerH
    };
    state.previewRects = [];

    const mask = state.mask;
    if (isMaskMode) {
        const layouts = getMaskInputLayouts(mainArea, unit);
        layouts.forEach((layout) => {
            const activeCell = Number(layout.regionId) === Number(state.activeRegionId);
            drawRegionTile(ctx, layout.rect, layout.regionId, activeCell, { showLabel: false });
            state.previewRects.push({ regionId: layout.regionId, ...layout.rect, layout });
        });
        drawProjectedMask(ctx, layouts, mask);
        return;
    }

    const mainRect = fitRect(mainArea.width, mainArea.height, getRegionAspect(active), 4 * unit);
    mainRect.x += mainArea.x;
    mainRect.y += mainArea.y;
    drawRegionTile(ctx, mainRect, active?.id || activeId, true, { showLabel: false });
    state.previewRects.push({ regionId: active?.id || activeId, ...mainRect });

    const geometry = state.geometry[state.activeRegionId];
    const blend = state.blend[state.activeRegionId];
    if (geometry && state.mode === 'geometry') {
        drawPointGrid(ctx, mainRect, geometry, GEOMETRY_GRID_COLORS, false, { legacyGeometry: true });
    }
    if (state.mode === 'blend') {
        drawBlendEdges(ctx, mainRect, blend);
        if (geometry && state.managerMode) {
            const displayGeometry = createBlendManagerDisplayGrid(geometry, blend);
            drawPointGrid(ctx, mainRect, displayGeometry, BLEND_GRID_COLORS, true, { legacyGeometry: true });
        }
    }

    if (!otherRegions.length) return;
    const thumbAreaW = canvas.width - pad * 2;
    const thumbW = Math.max(78, Math.min(128, (thumbAreaW - thumbGap * (otherRegions.length - 1)) / otherRegions.length));
    const totalW = thumbW * otherRegions.length + thumbGap * (otherRegions.length - 1);
    let x = pad + Math.max(0, (thumbAreaW - totalW) / 2);
    const y = canvas.height - pad - thumbH;
    otherRegions.forEach((region) => {
        const rect = { x, y, width: thumbW, height: thumbH };
        drawRegionTile(ctx, rect, region.id, false);
        state.previewRects.push({ regionId: region.id, ...rect });
        x += thumbW + thumbGap;
    });
}

function drawPointGrid(ctx, rect, target, color, force = false, options = {}) {
    if (!target || !Array.isArray(target.points) || (!target.showGrid && !force)) return;
    const unit = Math.max(1, window.devicePixelRatio || 1);
    const inset = options.inset === 0 ? 0 : Math.max(5 * unit, 8);
    const drawRect = {
        x: rect.x + inset,
        y: rect.y + inset,
        width: Math.max(1, rect.width - inset * 2),
        height: Math.max(1, rect.height - inset * 2)
    };
    const toLocal = (point) => (options.legacyGeometry ? geometryPointToLocal01(point) : point);
    const toX = (point) => drawRect.x + toLocal(point).u * drawRect.width;
    const toY = (point) => drawRect.y + toLocal(point).v * drawRect.height;
    const highlightLine = state.mode === 'geometry' && state.geometryOp === 'line';
    const selectedRow = target.selected?.row ?? 0;
    const selectedCol = target.selected?.col ?? 0;
    const cornersOnly = !!options.cornersOnly;
    const perimeterOnly = !!options.perimeterOnly;
    const showLines = options.showLines !== false;
    const showPoints = options.showPoints !== false;
    const lastRow = Math.max(0, Number(target.rows || 1) - 1);
    const lastCol = Math.max(0, Number(target.cols || 1) - 1);
    const canHighlightLine = state.mode === 'geometry' || state.mode === 'blend' || state.mode === 'mask';

    ctx.save();
    ctx.beginPath();
    ctx.rect(rect.x, rect.y, rect.width, rect.height);
    ctx.clip();

    if (showLines) {
        const drawRowLine = (row) => {
            ctx.beginPath();
            for (let col = 0; col < target.cols; col += 1) {
                const point = target.points[row * target.cols + col];
                if (!point) continue;
                const x = toX(point);
                const y = toY(point);
                if (col === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            ctx.stroke();
        };
        const drawColLine = (col) => {
            ctx.beginPath();
            for (let row = 0; row < target.rows; row += 1) {
                const point = target.points[row * target.cols + col];
                if (!point) continue;
                const x = toX(point);
                const y = toY(point);
                if (row === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            ctx.stroke();
        };
        const lineItems = [];
        for (let row = 0; row < target.rows; row += 1) {
            if (perimeterOnly && row !== 0 && row !== lastRow) continue;
            const selectedLine = canHighlightLine && (highlightLine || row === selectedRow) && row === selectedRow;
            lineItems.push({ selectedLine, draw: () => drawRowLine(row) });
        }
        for (let col = 0; col < target.cols; col += 1) {
            if (perimeterOnly && col !== 0 && col !== lastCol) continue;
            const selectedLine = canHighlightLine && (highlightLine || col === selectedCol) && col === selectedCol;
            lineItems.push({ selectedLine, draw: () => drawColLine(col) });
        }

        ctx.lineWidth = 1.4 * unit;
        lineItems.forEach((item) => {
            if (item.selectedLine) return;
            ctx.strokeStyle = color.activeLine || color.line;
            item.draw();
        });
        // 遮罩是旧项目 ZheZhao 的输入合成幕布层；选中行/列最后覆盖绘制，
        // 不允许被普通边界线按投影区域再次盖成分段颜色。
        lineItems.forEach((item) => {
            if (!item.selectedLine) return;
            ctx.strokeStyle = color.selectedLine || color.line;
            item.draw();
        });
    }

    if (!showPoints) {
        ctx.restore();
        return;
    }

    target.points.forEach((point, index) => {
        const row = Math.floor(index / target.cols);
        const col = index % target.cols;
        if (cornersOnly && !isCornerPosition(target, row, col)) return;
        if (perimeterOnly && !isPerimeterPosition(target, row, col)) return;
        const selected = row === selectedRow && col === selectedCol;
        const x = toX(point);
        const y = toY(point);
        const baseRadius = selected ? 4.6 * unit : 3.6 * unit;
        if (selected && color.selectedHalo) {
            ctx.beginPath();
            ctx.arc(x, y, 8.5 * unit, 0, Math.PI * 2);
            ctx.fillStyle = color.selectedHalo;
            ctx.fill();
        }
        ctx.beginPath();
        ctx.arc(x, y, baseRadius, 0, Math.PI * 2);
        ctx.fillStyle = selected ? (color.selected || color.point) : (color.pointOther || color.point);
        ctx.fill();
        ctx.lineWidth = selected ? 2 * unit : 1.2 * unit;
        ctx.strokeStyle = selected ? (color.selectedStroke || color.pointStroke || '#ffffff') : (color.pointStroke || 'rgba(0,0,0,.8)');
        ctx.stroke();
    });
    ctx.restore();
}

function drawBlendEdges(ctx, rect, blend) {
    if (!blend) return;
    const isSideOn = (targetBlend, side) => {
        const edge = targetBlend?.[side];
        return !!edge?.enabled;
    };
    const hasEffectiveSide = (side) => {
        return !!state.masterEnabled && isSideOn(blend, side);
    };
    const sides = [
        ['left', rect.x, rect.y, rect.x, rect.y + rect.height, rect.width],
        ['right', rect.x + rect.width, rect.y, rect.x + rect.width, rect.y + rect.height, rect.width],
        ['top', rect.x, rect.y, rect.x + rect.width, rect.y, rect.height],
        ['bottom', rect.x, rect.y + rect.height, rect.x + rect.width, rect.y + rect.height, rect.height]
    ];

    sides.forEach(([side, x1, y1, x2, y2, base]) => {
        const edge = blend[side];
        if (!hasEffectiveSide(side)) return;
        const width = Math.max(2, Math.min(18, Number(edge.width || 0) * base));
        ctx.strokeStyle = side === state.activeSide ? 'rgba(245, 158, 11, 0.9)' : 'rgba(245, 158, 11, 0.55)';
        ctx.lineWidth = width;
        ctx.beginPath();
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
        ctx.stroke();
    });
}

export function renderPreview() {
    const canvas = document.getElementById('previewCanvas');
    if (!canvas) return;
    const panel = canvas.closest('.preview-panel');
    if (panel && getComputedStyle(panel).display === 'none') return;
    const bounds = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(320, Math.floor(bounds.width * dpr));
    const height = Math.max(220, Math.floor(bounds.height * dpr));
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }

    const ctx = canvas.getContext('2d');
    clear(ctx, canvas);
    drawFocusedPreview(ctx, canvas);
}

export function regionFromCanvasEvent(event) {
    const canvas = document.getElementById('previewCanvas');
    if (!canvas) return null;
    const bounds = canvas.getBoundingClientRect();
    const scaleX = canvas.width / bounds.width;
    const scaleY = canvas.height / bounds.height;
    const x = (event.clientX - bounds.left) * scaleX;
    const y = (event.clientY - bounds.top) * scaleY;
    const hit = state.previewRects.find((rect) => (
        x >= rect.x && x <= rect.x + rect.width &&
        y >= rect.y && y <= rect.y + rect.height
    ));
    return hit ? hit.regionId : null;
}

export function geometryPointFromCanvasEvent(event) {
    const canvas = document.getElementById('previewCanvas');
    const geometry = state.geometry[state.activeRegionId];
    const rect = state.previewRects.find((item) => Number(item.regionId) === Number(state.activeRegionId));
    if (!canvas || !geometry || !rect || !Array.isArray(geometry.points)) return null;

    const bounds = canvas.getBoundingClientRect();
    const scaleX = canvas.width / bounds.width;
    const scaleY = canvas.height / bounds.height;
    const x = (event.clientX - bounds.left) * scaleX;
    const y = (event.clientY - bounds.top) * scaleY;
    const cornersOnly = state.mode === 'blend';
    const threshold = cornersOnly
        ? Math.max(28, Math.min(rect.width, rect.height) * 0.12)
        : Math.max(18, Math.min(rect.width, rect.height) * 0.08);
    let nearest = null;

    geometry.points.forEach((point, index) => {
        const row = Math.floor(index / geometry.cols);
        const col = index % geometry.cols;
        if (cornersOnly && !isCornerPosition(geometry, row, col)) return;
        const unit = Math.max(1, window.devicePixelRatio || 1);
        const inset = Math.max(5 * unit, 8);
        const local = geometryPointToLocal01(point);
        const px = rect.x + inset + local.u * Math.max(1, rect.width - inset * 2);
        const py = rect.y + inset + local.v * Math.max(1, rect.height - inset * 2);
        const distance = Math.hypot(px - x, py - y);
        if (!nearest || distance < nearest.distance) {
            nearest = {
                row,
                col,
                distance
            };
        }
    });

    return nearest && nearest.distance <= threshold
        ? { row: nearest.row, col: nearest.col }
        : null;
}

export function maskPointFromCanvasEvent(event) {
    const canvas = document.getElementById('previewCanvas');
    const mask = state.mask;
    if (!canvas || !mask || !Array.isArray(mask.points)) return null;

    const bounds = canvas.getBoundingClientRect();
    const scaleX = canvas.width / bounds.width;
    const scaleY = canvas.height / bounds.height;
    const x = (event.clientX - bounds.left) * scaleX;
    const y = (event.clientY - bounds.top) * scaleY;
    let nearest = null;

    mask.points.forEach((point, index) => {
        const row = Math.floor(index / mask.cols);
        const col = index % mask.cols;
        if (!isPerimeterPosition(mask, row, col)) return;
        state.previewRects.forEach((preview) => {
            const layout = preview.layout;
            if (!layout) return;
            const screen = maskPointToLayoutScreen(point, layout);
            if (!screen) return;
            const threshold = Math.max(18, Math.min(preview.width, preview.height) * 0.08);
            const distance = Math.hypot(screen.x - x, screen.y - y);
            if (distance <= threshold && (!nearest || distance < nearest.distance)) {
                nearest = {
                    row,
                    col,
                    distance
                };
            }
        });
    });

    return nearest
        ? { row: nearest.row, col: nearest.col }
        : null;
}
