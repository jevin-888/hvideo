import { getState } from '../actions.js?v=2.95';
import { clamp01 } from '../utils/grid.js';
import { getActiveGeometry, getActiveMask } from '../selectors.js?v=2.95';
import { calculateAspectRatioRect, projectionToScreen } from './coords.js';
import { GRID_VISUAL_METRICS } from './visualStyle.js?v=2.81';
import {
    blendPointToCanvas,
    getBlendCurveAlpha,
    getBlendCurveHandleLayout,
    getBlendCurveParams,
    getBlendPlotRect
} from '../utils/blendCurve.js';

export {
    blendPointToCanvas,
    getBlendCurveAlpha,
    getBlendCurveAnchorFromDisplayValue,
    getBlendCurveHandleLayout,
    getBlendCurveParams,
    getBlendPlotRect
} from '../utils/blendCurve.js';

export function getInputCanvasLayoutMetrics() {
    const state = getState();
    const regionCount = Math.max(1, Number(state.layout.regionCount) || state.layout.regionIds?.length || 1);
    let rows = Math.max(0, Number(state.layout.inputRows) || 0);
    let cols = Math.max(0, Number(state.layout.inputCols) || 0);

    if (!rows && cols) rows = Math.max(1, Math.ceil(regionCount / cols));
    if (!cols && rows) cols = Math.max(1, Math.ceil(regionCount / rows));
    if (!rows) rows = 1;
    if (!cols) cols = Math.max(1, regionCount);
    const regions = Object.values(state.geometry.byRegionId || {});
    const sourceRegion = regions.find((region) => Number(region?.srcWidth) > 0 && Number(region?.srcHeight) > 0);
    const tileWidth = Number(state.layout.tileInWidth) || Number(sourceRegion?.srcWidth) || 0;
    const tileHeight = Number(state.layout.tileInHeight) || Number(sourceRegion?.srcHeight) || 0;
    const inferredTotalWidth = tileWidth > 0 ? tileWidth * cols : 0;
    const inferredTotalHeight = tileHeight > 0 ? tileHeight * rows : 0;
    const totalWidth = Number(state.layout.inputTotalWidth) || inferredTotalWidth || Number(state.layout.canvasWidth) || 0;
    const totalHeight = Number(state.layout.inputTotalHeight) || inferredTotalHeight || Number(state.layout.canvasHeight) || 0;

    // 遮罩预览必须使用“输入合成的整个幕布”尺寸。
    // 不要改成 outputWidth/outputHeight 或输出矩阵行列，否则 Web 会再次显示成输出整屏。
    return {
        rows,
        cols,
        canvasWidth: totalWidth,
        canvasHeight: totalHeight,
        tileWidth,
        tileHeight
    };
}

function getOutputCanvasAspectRatio() {
    const state = getState();
    const width = Number(state.layout.outputWidth) || 0;
    const height = Number(state.layout.outputHeight) || 0;
    if (width > 0 && height > 0) return width / height;
    const tileWidth = Number(state.layout.tileOutWidth) || 0;
    const tileHeight = Number(state.layout.tileOutHeight) || 0;
    const cols = Math.max(1, Number(state.layout.cols) || Math.max(1, state.layout.regionIds?.length || 1));
    const rows = Math.max(1, Number(state.layout.rows) || 1);
    if (tileWidth > 0 && tileHeight > 0) return (tileWidth * cols) / (tileHeight * rows);
    return cols / rows;
}

function getInputCanvasAspectRatio() {
    const input = getInputCanvasLayoutMetrics();
    if (input.canvasWidth > 0 && input.canvasHeight > 0) {
        return input.canvasWidth / input.canvasHeight;
    }
    if (input.tileWidth > 0 && input.tileHeight > 0) {
        return (input.tileWidth * input.cols) / (input.tileHeight * input.rows);
    }
    return input.cols / input.rows;
}

function getMatrixCellAspectRatio(totalWidth, totalHeight, rows, cols) {
    // 矩阵布局统一按行×列(rows×cols)：12×1 表示 12 行 1 列。
    // 单个输出格子的尺寸是 totalWidth / cols 与 totalHeight / rows。
    const safeRows = Math.max(1, Number(rows) || 1);
    const safeCols = Math.max(1, Number(cols) || 1);
    return (totalWidth / safeCols) / (totalHeight / safeRows);
}

function isUsableProjectionAspect(aspectRatio) {
    return Number.isFinite(aspectRatio) && aspectRatio >= 0.45 && aspectRatio <= 3.2;
}

function pickProjectionAspect(...candidates) {
    for (const candidate of candidates) {
        const number = Number(candidate);
        if (isUsableProjectionAspect(number)) return number;
    }
    return 16 / 9;
}

function getRegionAspectRatio() {
    const state = getState();
    const tileInWidth = Number(state.layout.tileInWidth) || 0;
    const tileInHeight = Number(state.layout.tileInHeight) || 0;
    const tileWidth = Number(state.layout.tileOutWidth) || 0;
    const tileHeight = Number(state.layout.tileOutHeight) || 0;
    const width = Number(state.layout.outputWidth) || 0;
    const height = Number(state.layout.outputHeight) || 0;
    const cols = Math.max(1, Number(state.layout.cols) || Math.max(1, state.layout.regionIds?.length || 1));
    const rows = Math.max(1, Number(state.layout.rows) || 1);
    return pickProjectionAspect(
        tileInWidth > 0 && tileInHeight > 0 ? tileInWidth / tileInHeight : null,
        tileWidth > 0 && tileHeight > 0 ? tileWidth / tileHeight : null,
        width > 0 && height > 0 ? getMatrixCellAspectRatio(width, height, rows, cols) : null,
        width > 0 && height > 0 ? width / height : null
    );
}

function getRegionAspectRatioById(regionId) {
    const state = getState();
    const region = state.geometry.byRegionId?.[regionId];
    const outputWidth = Number(state.layout.outputWidth) || 0;
    const outputHeight = Number(state.layout.outputHeight) || 0;
    const outWidth = Number(region?.outWidth) || 0;
    const outHeight = Number(region?.outHeight) || 0;
    const srcWidth = Number(region?.srcWidth) || 0;
    const srcHeight = Number(region?.srcHeight) || 0;
    const regionOutputAspect = outputWidth > 0 && outputHeight > 0 && outWidth > 0 && outHeight > 0
        ? (outWidth * outputWidth) / (outHeight * outputHeight)
        : null;
    const sourceAspect = srcWidth > 0 && srcHeight > 0 ? srcWidth / srcHeight : null;

    return pickProjectionAspect(
        regionOutputAspect,
        sourceAspect,
        getRegionAspectRatio()
    );
}

function getDebugStyle(mode) {
    const selectedGridLineColor = '#00ff00';
    const selectedMaskLineColor = '#ff00ff';
    if (mode === 'mask') {
        return {
            lineColor: '#00ffff',
            selectedLineColor: selectedMaskLineColor,
            pointColor: '#00ffff',
            selectedPointColor: '#ff0000',
            pointOutlineColor: '#ffffff',
            ...GRID_VISUAL_METRICS
        };
    }
    if (mode === 'blend') {
        return {
            lineColor: '#ffffff',
            activeLineColor: '#ffff00',
            selectedLineColor: selectedGridLineColor,
            pointColor: '#00ffff',
            pointOutlineColor: '#ffffff',
            ...GRID_VISUAL_METRICS
        };
    }
    return {
        lineColor: 'rgba(255, 255, 255, 0.74)',
        activeLineColor: '#ffffff',
        selectedLineColor: selectedGridLineColor,
        pointColor: '#ff0000',
        pointOutlineColor: '#ffffff',
        ...GRID_VISUAL_METRICS
    };
}

const EPSILON = 1e-6;

function toGridPoint(source, row, col, options = {}) {
    const rows = Math.max(2, Number(source?.rows) || 2);
    const cols = Math.max(2, Number(source?.cols) || 2);
    const r = Math.max(0, Math.min(rows - 1, row));
    const c = Math.max(0, Math.min(cols - 1, col));
    const point = source?.points?.[r * cols + c];
    if (point) return point;
    if (options.geometry === false) {
        return {
            u: cols > 1 ? c / (cols - 1) : 0.5,
            v: rows > 1 ? r / (rows - 1) : 0.5
        };
    }
    return {
        u: cols > 1 ? -1 + (2 * c) / (cols - 1) : 0,
        v: rows > 1 ? 1 - (2 * r) / (rows - 1) : 0
    };
}

function lerpPoint(a, b, t) {
    return {
        u: a.u * (1 - t) + b.u * t,
        v: a.v * (1 - t) + b.v * t
    };
}

function sampleLinearGrid(source, u, v, options = {}) {
    const rows = Math.max(2, Number(source?.rows) || 2);
    const cols = Math.max(2, Number(source?.cols) || 2);
    const srcC = clamp01(u) * (cols - 1);
    const srcR = clamp01(v) * (rows - 1);
    const c0 = Math.min(Math.floor(srcC), cols - 2);
    const r0 = Math.min(Math.floor(srcR), rows - 2);
    const c1 = c0 + 1;
    const r1 = r0 + 1;
    const fC = srcC - c0;
    const fR = srcR - r0;
    const top = lerpPoint(toGridPoint(source, r0, c0, options), toGridPoint(source, r0, c1, options), fC);
    const bottom = lerpPoint(toGridPoint(source, r1, c0, options), toGridPoint(source, r1, c1, options), fC);
    return lerpPoint(top, bottom, fR);
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
    for (let i = 2; i < n; i += 1) {
        knot[i] = knot[i - 1] + distance(pts[i - 1], pts[i]);
    }
    if (knot[L] <= EPSILON) return input[0];

    const delta = new Array(n + 1).fill(0);
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

function sampleGridSurface(source, u, v, rect = null, options = {}) {
    const rows = Math.max(2, Number(source?.rows) || 2);
    const cols = Math.max(2, Number(source?.cols) || 2);
    const useCurve = Number(source?.interpolationMode) === 1;
    if (!useCurve) return sampleLinearGrid(source, u, v, options);

    const xyScale = rect && rect.width > 0 ? rect.height / rect.width : 1;
    const vertical = [];
    for (let c = 0; c < cols; c += 1) {
        const column = [];
        for (let r = 0; r < rows; r += 1) column.push(toGridPoint(source, r, c, options));
        vertical.push(legacySpline1D(column, v, xyScale));
    }
    return legacySpline1D(vertical, u, xyScale);
}

function insetCanvasCoord(value, maxValue, inset) {
    if (!Number.isFinite(value) || !Number.isFinite(maxValue) || inset <= 0) return value;
    if (value >= -inset && value < inset) return inset;
    if (value <= maxValue + inset && value > maxValue - inset) return maxValue - inset;
    return value;
}

function insetCanvasPoint(point, inset, canvas = null) {
    if (!point || inset <= 0) return point;
    const targetCanvas = canvas || {};
    const width = Number(targetCanvas.width) || 0;
    const height = Number(targetCanvas.height) || 0;
    if (width <= 0 || height <= 0) return point;
    return {
        ...point,
        x: insetCanvasCoord(point.x, width, inset),
        y: insetCanvasCoord(point.y, height, inset)
    };
}

function drawPolyline(ctx, points) {
    let started = false;
    const inset = Math.max(0.5, (Number(ctx.lineWidth) || 1) * 0.5);
    ctx.beginPath();
    points.forEach((point) => {
        if (!point) return;
        const drawPoint = insetCanvasPoint(point, inset, ctx.canvas);
        if (!started) {
            ctx.moveTo(drawPoint.x, drawPoint.y);
            started = true;
        } else {
            ctx.lineTo(drawPoint.x, drawPoint.y);
        }
    });
    if (started) ctx.stroke();
}

function strokeInsetRect(ctx, rect) {
    if (!rect) return;
    const inset = Math.max(0.5, (Number(ctx.lineWidth) || 1) * 0.5);
    ctx.strokeRect(
        rect.x + inset,
        rect.y + inset,
        Math.max(0, rect.width - inset * 2),
        Math.max(0, rect.height - inset * 2)
    );
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

function localPointToOutputScreen(point, layout, options = {}) {
    if (!point || !layout?.rect || !layout?.outputRect) return null;
    const localRect = layout.localRect || layout.outputRect;
    const cell = layout.rect;
    const local = options.geometry === false ? point : geometryPointToLocal01(point);
    return {
        x: localRect.x + (cell.x - layout.outputRect.x) + local.u * cell.width,
        y: localRect.y + (cell.y - layout.outputRect.y) + local.v * cell.height
    };
}

function sampleRegionToOutputScreen(region, u, v, layout) {
    if (!region || !layout) return null;
    const point = sampleGridSurface(region, u, v, layout.rect);
    return localPointToOutputScreen(point, layout);
}

export function outputScreenToRegionLocal(layout, mx, my) {
    if (!layout?.rect || !layout?.localRect || !layout?.outputRect) return null;
    const cell = layout.rect;
    if (cell.width <= 0 || cell.height <= 0) return null;
    return {
        u: (mx - layout.localRect.x - (cell.x - layout.outputRect.x)) / cell.width,
        v: (my - layout.localRect.y - (cell.y - layout.outputRect.y)) / cell.height
    };
}

export function outputScreenToGeometryPoint(layout, mx, my) {
    const local = outputScreenToRegionLocal(layout, mx, my);
    return local ? local01ToGeometryPoint(local) : null;
}

export function regionPointToOutputScreen(point, layout) {
    return localPointToOutputScreen(point, layout);
}

function drawPoints(ctx, region, layout, style, forceVisible = false) {
    if (!region || !region.points || region.points.length === 0) return;
    // 热点跟随辅助线显示，showGrid 关闭时不显示热点
    if (!forceVisible && !region.showGrid) return;
    const selectedIndex = region.selected.row * region.cols + region.selected.col;
    const point = region.points[selectedIndex];
    if (!point) return;

    const screen = localPointToOutputScreen(point, layout);
    if (!screen) return;
    const inset = style.pointOutlineColor ? style.pointOuterRadius : style.selectedPointRadius;
    const drawScreen = insetCanvasPoint(screen, inset, ctx.canvas);
    if (style.pointOutlineColor) {
        ctx.fillStyle = style.pointOutlineColor;
        ctx.beginPath();
        ctx.arc(drawScreen.x, drawScreen.y, style.pointOuterRadius, 0, Math.PI * 2);
        ctx.fill();
    }

    ctx.fillStyle = style.pointColor;
    ctx.beginPath();
    ctx.arc(drawScreen.x, drawScreen.y, style.pointOutlineColor ? style.pointInnerRadius : style.selectedPointRadius, 0, Math.PI * 2);
    ctx.fill();
}

function drawGridLines(ctx, region, layout, style, isActive = false, forceVisible = false) {
    if (!region || (!forceVisible && !region.showGrid) || !region.points || region.points.length === 0) return;
    ctx.lineWidth = style.gridLineWidth;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';

    const isCurve = region.interpolationMode === 1;
    const segments = isCurve ? Math.max(1, Math.min(16, Math.floor(32 / Math.max(region.rows, region.cols, 1)))) : 1;

    // 横线
    for (let r = 0; r < region.rows; r += 1) {
        ctx.strokeStyle = isActive ? style.activeLineColor : style.lineColor;
        if (isActive && r === region.selected.row) ctx.strokeStyle = style.selectedLineColor;
        const v = region.rows > 1 ? r / (region.rows - 1) : 0.5;
        const steps = Math.max(1, segments * Math.max(1, region.cols - 1));
        const points = [];
        for (let i = 0; i <= steps; i += 1) {
            points.push(sampleRegionToOutputScreen(region, i / steps, v, layout));
        }
        drawPolyline(ctx, points);
    }

    // 竖线
    for (let c = 0; c < region.cols; c += 1) {
        ctx.strokeStyle = isActive ? style.activeLineColor : style.lineColor;
        if (isActive && c === region.selected.col) ctx.strokeStyle = style.selectedLineColor;
        const u = region.cols > 1 ? c / (region.cols - 1) : 0.5;
        const steps = Math.max(1, segments * Math.max(1, region.rows - 1));
        const points = [];
        for (let i = 0; i <= steps; i += 1) {
            points.push(sampleRegionToOutputScreen(region, u, i / steps, layout));
        }
        drawPolyline(ctx, points);
    }
}

function createBlendManagerDisplayGrid(region, blend) {
    if (!region || !blend) return region;
    const rows = Math.max(2, Number(blend.gridRows) || Number(region.rows) || 2);
    const cols = Math.max(2, Number(blend.gridCols) || Number(region.cols) || 2);
    const points = [];
    for (let row = 0; row < rows; row += 1) {
        const v = rows > 1 ? row / (rows - 1) : 0.5;
        for (let col = 0; col < cols; col += 1) {
            const u = cols > 1 ? col / (cols - 1) : 0.5;
            points.push(sampleGridSurface(region, u, v));
        }
    }
    // I 融合管理模式只显示融合网格；这些点是按当前几何曲面采样出来的临时辅助线。
    // 不要把 rows/cols/points 写回 region，否则加减融合行列会改变真实画面。
    return {
        ...region,
        rows,
        cols,
        points,
        selected: {
            row: Math.max(0, Math.min(rows - 1, Number(region.selected?.row) || 0)),
            col: Math.max(0, Math.min(cols - 1, Number(region.selected?.col) || 0)),
            axis: region.selected?.axis || 'col'
        }
    };
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
            points.push(sampleGridSurface(mask, ...sample(t), null, {
                geometry: false
            }));
        }
        return { lineRow, lineCol, points };
    };
    const paths = [];
    [0, rows - 1].forEach((row) => {
        const v = rows > 1 ? row / (rows - 1) : 0.5;
        paths.push(makePath(row, -1, Math.max(96, divCnt * (cols - 1)), (t) => [t, v]));
    });
    [0, cols - 1].forEach((col) => {
        const u = cols > 1 ? col / (cols - 1) : 0.5;
        paths.push(makePath(-1, col, Math.max(96, divCnt * (rows - 1)), (t) => [u, t]));
    });
    return paths;
}

function isMaskPerimeterPoint(mask, row, col) {
    if (!mask) return false;
    return row === 0 || col === 0 || row === mask.rows - 1 || col === mask.cols - 1;
}

function firstFinitePositive(...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number) && number > 0) return number;
    }
    return 0;
}

function firstFiniteNumber(defaultValue, ...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number)) return number;
    }
    return defaultValue;
}

function getRegionSourceRect01(region, state = getState()) {
    const input = getInputCanvasLayoutMetrics();
    const canvasW = firstFinitePositive(input.canvasWidth, state.layout.inputTotalWidth, state.layout.canvasWidth, input.tileWidth, 1);
    const canvasH = firstFinitePositive(input.canvasHeight, state.layout.inputTotalHeight, state.layout.canvasHeight, input.tileHeight, 1);
    const srcX = firstFiniteNumber(0, region?.srcX);
    const srcY = firstFiniteNumber(0, region?.srcY);
    const srcW = firstFinitePositive(region?.srcWidth, input.tileWidth, canvasW);
    const srcH = firstFinitePositive(region?.srcHeight, input.tileHeight, canvasH);
    const minU = Math.max(0, Math.min(1, srcX / canvasW));
    const minV = Math.max(0, Math.min(1, srcY / canvasH));
    const maxU = Math.max(0, Math.min(1, (srcX + srcW) / canvasW));
    const maxV = Math.max(0, Math.min(1, (srcY + srcH) / canvasH));
    return {
        minU: Math.min(minU, maxU),
        minV: Math.min(minV, maxV),
        maxU: Math.max(minU, maxU),
        maxV: Math.max(minV, maxV),
        width: Math.abs(maxU - minU),
        height: Math.abs(maxV - minV)
    };
}

function clipSegmentToSourceRect(a, b, rect) {
    if (!a || !b || !rect || rect.width <= 0 || rect.height <= 0) return null;
    let t0 = 0;
    let t1 = 1;
    const dx = b.u - a.u;
    const dy = b.v - a.v;
    const clip = (p, q) => {
        if (Math.abs(p) < EPSILON) return q >= 0;
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

function maskCanvasPointToRegionScreen(point, layout) {
    const rect = layout?.sourceRect01;
    if (!point || !layout?.region || !rect || rect.width <= 0 || rect.height <= 0) return null;
    const u = Number(point.u);
    const v = Number(point.v);
    if (!Number.isFinite(u) || !Number.isFinite(v)) return null;
    const eps = 1e-5;
    if (u < rect.minU - eps || u > rect.maxU + eps ||
        v < rect.minV - eps || v > rect.maxV + eps) {
        return null;
    }
    const localU = (u - rect.minU) / rect.width;
    const localV = (v - rect.minV) / rect.height;
    return sampleRegionToOutputScreen(layout.region, localU, localV, layout);
}

function buildProjectedMaskSegments(mask, layouts) {
    const paths = buildMaskGridPaths(mask);
    const segments = [];
    layouts.forEach((layout) => {
        const rect = layout.sourceRect01;
        if (!rect || rect.width <= 0 || rect.height <= 0) return;
        paths.forEach((path) => {
            let run = [];
            const flush = () => {
                if (run.length >= 2) {
                    segments.push({
                        layout,
                        lineRow: path.lineRow,
                        lineCol: path.lineCol,
                        points: run
                    });
                }
                run = [];
            };
            for (let i = 0; i + 1 < path.points.length; i += 1) {
                const clipped = clipSegmentToSourceRect(path.points[i], path.points[i + 1], rect);
                if (!clipped) {
                    flush();
                    continue;
                }
                const a = maskCanvasPointToRegionScreen(clipped.a, layout);
                const b = maskCanvasPointToRegionScreen(clipped.b, layout);
                if (!a || !b) {
                    flush();
                    continue;
                }
                const last = run[run.length - 1];
                if (last && Math.hypot(last.x - a.x, last.y - a.y) > 0.5) flush();
                if (!run.length) run.push(a);
                run.push(b);
            }
            flush();
        });
    });
    return segments;
}

function clipToLayoutOutput(ctx, layout) {
    const rect = layout?.outputRect || layout?.localRect || layout?.rect;
    if (!rect) return false;
    ctx.beginPath();
    ctx.rect(rect.x, rect.y, rect.width, rect.height);
    ctx.clip();
    return true;
}

function drawTransformedMaskBoundaryLines(ctx, mask, layouts, style) {
    if (!mask || !mask.showGrid || !Array.isArray(mask.points) || mask.points.length === 0) return;
    if (!Array.isArray(layouts) || layouts.length === 0) return;
    const inputRect = layouts.find((layout) => layout?.role === 'input' && layout.outputRect)?.outputRect || null;
    if (inputRect) {
        drawContinuousProjectedInputMaskLines(ctx, mask, layouts, inputRect, style);
        return;
    }
    const segments = buildProjectedMaskSegments(mask, layouts);
    const selectedRow = Number(mask.selected?.row);
    const selectedCol = Number(mask.selected?.col);
    const isSelectedMaskLine = (segment) => (
        (segment.lineRow >= 0 && segment.lineRow === selectedRow) ||
        (segment.lineCol >= 0 && segment.lineCol === selectedCol)
    );
    const drawSegment = (segment) => {
        ctx.save();
        clipToLayoutOutput(ctx, segment.layout);
        drawPolyline(ctx, segment.points);
        ctx.restore();
    };

    ctx.save();
    ctx.lineWidth = style.maskBoundaryLineWidth;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.strokeStyle = style.lineColor;
    segments.forEach((segment) => {
        if (!isSelectedMaskLine(segment)) drawSegment(segment);
    });

    // ZheZhao 的遮罩数据只有一份输入合成幕布点阵；Web 只是把这份点阵
    // 分别投到各投影的 sourceRect + 几何曲面上显示。选中行/列必须最后覆盖，
    // 但不能因此引入“每个投影一份遮罩状态”的错误概念。
    ctx.strokeStyle = style.selectedLineColor;
    segments.forEach((segment) => {
        if (isSelectedMaskLine(segment)) drawSegment(segment);
    });
    ctx.restore();
}

function maskCanvasPointToAnyLayoutScreen(point, layouts) {
    if (!point || !Array.isArray(layouts)) return null;
    let fallback = null;
    const eps = 1e-5;
    for (const layout of layouts) {
        const rect = layout?.sourceRect01;
        if (!rect || rect.width <= 0 || rect.height <= 0) continue;
        const u = Number(point.u);
        const v = Number(point.v);
        if (!Number.isFinite(u) || !Number.isFinite(v)) continue;
        if (u < rect.minU - eps || u > rect.maxU + eps ||
            v < rect.minV - eps || v > rect.maxV + eps) {
            continue;
        }
        const screen = maskCanvasPointToRegionScreen(point, layout);
        if (!screen) continue;
        if (u > rect.minU + eps && u < rect.maxU - eps &&
            v > rect.minV + eps && v < rect.maxV - eps) {
            return screen;
        }
        fallback = screen;
    }
    return fallback;
}

function drawContinuousProjectedInputMaskLines(ctx, mask, layouts, inputRect, style) {
    const paths = buildMaskGridPaths(mask);
    const selectedRow = Number(mask.selected?.row);
    const selectedCol = Number(mask.selected?.col);
    const isSelectedMaskLine = (path) => (
        (path.lineRow >= 0 && path.lineRow === selectedRow) ||
        (path.lineCol >= 0 && path.lineCol === selectedCol)
    );
    const drawPath = (path) => {
        const points = path.points
            .map((point) => maskCanvasPointToAnyLayoutScreen(point, layouts))
            .filter(Boolean);
        drawPolyline(ctx, points);
    };

    ctx.save();
    ctx.beginPath();
    ctx.rect(inputRect.x, inputRect.y, inputRect.width, inputRect.height);
    ctx.clip();
    ctx.lineWidth = style.maskBoundaryLineWidth;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.strokeStyle = style.lineColor;
    paths.forEach((path) => {
        if (!isSelectedMaskLine(path)) drawPath(path);
    });
    ctx.strokeStyle = style.selectedLineColor;
    paths.forEach((path) => {
        if (isSelectedMaskLine(path)) drawPath(path);
    });
    ctx.restore();
}

function drawTransformedMaskPoints(ctx, mask, layouts, style) {
    if (!mask || !mask.showGrid || !Array.isArray(mask.points)) return;
    if (!Array.isArray(layouts) || layouts.length === 0) return;
    const inputRect = layouts.find((layout) => layout?.role === 'input' && layout.outputRect)?.outputRect || null;
    if (inputRect) {
        drawProjectedInputMaskPoints(ctx, mask, layouts, inputRect, style);
        return;
    }
    const selectedIndex = mask.selected.row * mask.cols + mask.selected.col;
    ctx.save();
    mask.points.forEach((point, index) => {
        const row = Math.floor(index / mask.cols);
        const col = index % mask.cols;
        if (!isMaskPerimeterPoint(mask, row, col)) return;
        const selected = index === selectedIndex;
        layouts.forEach((layout) => {
            const screen = maskCanvasPointToRegionScreen(point, layout);
            if (!screen) return;
            const inset = selected && style.pointOutlineColor ? style.pointOuterRadius : style.pointRadius;
            const drawScreen = insetCanvasPoint(screen, inset, ctx.canvas);
            ctx.save();
            clipToLayoutOutput(ctx, layout);
            if (selected && style.pointOutlineColor) {
                ctx.fillStyle = style.pointOutlineColor;
                ctx.beginPath();
                ctx.arc(drawScreen.x, drawScreen.y, style.pointOuterRadius, 0, Math.PI * 2);
                ctx.fill();
            }
            // 旧项目 ZheZhao 选中热点是红色点；选中热点所在的线条颜色单独使用紫色。
            ctx.fillStyle = selected ? (style.selectedPointColor || style.selectedLineColor) : style.pointColor;
            ctx.beginPath();
            ctx.arc(drawScreen.x, drawScreen.y, selected && style.pointOutlineColor ? style.pointInnerRadius : style.pointRadius, 0, Math.PI * 2);
            ctx.fill();
            ctx.restore();
        });
    });
    ctx.restore();
}

function drawProjectedInputMaskPoints(ctx, mask, layouts, inputRect, style) {
    const selectedIndex = mask.selected.row * mask.cols + mask.selected.col;
    ctx.save();
    ctx.beginPath();
    ctx.rect(inputRect.x, inputRect.y, inputRect.width, inputRect.height);
    ctx.clip();
    mask.points.forEach((point, index) => {
        const row = Math.floor(index / mask.cols);
        const col = index % mask.cols;
        if (!isMaskPerimeterPoint(mask, row, col)) return;
        const selected = index === selectedIndex;
        const screen = maskCanvasPointToAnyLayoutScreen(point, layouts);
        if (!screen) return;
        const inset = selected && style.pointOutlineColor ? style.pointOuterRadius : style.pointRadius;
        const drawScreen = insetCanvasPoint(screen, inset, ctx.canvas);
        if (selected && style.pointOutlineColor) {
            ctx.fillStyle = style.pointOutlineColor;
            ctx.beginPath();
            ctx.arc(drawScreen.x, drawScreen.y, style.pointOuterRadius, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.fillStyle = selected ? (style.selectedPointColor || style.selectedLineColor) : style.pointColor;
        ctx.beginPath();
        ctx.arc(drawScreen.x, drawScreen.y, selected && style.pointOutlineColor ? style.pointInnerRadius : style.pointRadius, 0, Math.PI * 2);
        ctx.fill();
    });
    ctx.restore();
}

function getGridSceneBounds(canvas) {
    return {
        x: 32,
        y: 72,
        width: canvas.width - 64,
        height: canvas.height - 104
    };
}

export function getCanvasRegionPreviewLayouts(canvas, state = getState()) {
    const layouts = getCanvasOutputRegionLayouts(canvas, state);
    const active = layouts.find((item) => String(item.regionId) === String(state.page.activeRegionId));
    return active ? [toFocusedRegionLayout(active)] : layouts.slice(0, 1).map(toFocusedRegionLayout);
}

function toFocusedRegionLayout(layout) {
    if (!layout) return layout;
    const rect = layout.outputRect || layout.localRect || layout.rect;
    return {
        ...layout,
        cell: rect,
        rect,
        localRect: rect,
        isActive: true,
        role: 'focus'
    };
}

function getCanvasOutputRegionLayouts(canvas, state = getState()) {
    if (!canvas || !state.layout.regionIds.length) return [];
    const bounds = getGridSceneBounds(canvas);
    const outputRect = calculateAspectRatioRect(bounds.width, bounds.height, getOutputCanvasAspectRatio());
    outputRect.x += bounds.x;
    outputRect.y += bounds.y;
    const rows = Math.max(1, state.layout.rows || 1);
    const cols = Math.max(1, state.layout.cols || Math.max(1, state.layout.regionIds.length));
    const outputPixelWidth = Math.max(1, Math.round(Number(state.layout.outputWidth) || cols));
    const outputPixelHeight = Math.max(1, Math.round(Number(state.layout.outputHeight) || rows));

    return (state.layout.regionIds || []).map((regionId) => {
        const region = state.geometry.byRegionId[regionId];
        if (!Number.isInteger(region?.outputRow) || !Number.isInteger(region?.outputCol)) return null;
        const outputRow = Math.max(0, Math.min(rows - 1, region.outputRow));
        const outputCol = Math.max(0, Math.min(cols - 1, region.outputCol));
        const pxLeft = Math.floor(outputCol * outputPixelWidth / cols);
        const pxRight = outputCol === cols - 1
            ? outputPixelWidth
            : Math.floor((outputCol + 1) * outputPixelWidth / cols);
        const pyTop = Math.floor(outputRow * outputPixelHeight / rows);
        const pyBottom = outputRow === rows - 1
            ? outputPixelHeight
            : Math.floor((outputRow + 1) * outputPixelHeight / rows);
        const rect = {
            x: outputRect.x + (pxLeft / outputPixelWidth) * outputRect.width,
            y: outputRect.y + (pyTop / outputPixelHeight) * outputRect.height,
            width: ((pxRight - pxLeft) / outputPixelWidth) * outputRect.width,
            height: ((pyBottom - pyTop) / outputPixelHeight) * outputRect.height
        };
        const localRect = {
            x: outputRect.x,
            y: outputRect.y,
            width: outputRect.width,
            height: outputRect.height
        };
        const isActive = String(regionId) === String(state.page.activeRegionId);
        const sourceRect01 = getRegionSourceRect01(region, state);
        return {
            regionId,
            region,
            cell: rect,
            rect,
            localRect,
            outputRect,
            sourceRect01,
            isActive,
            role: 'output'
        };
    }).filter(Boolean);
}

function getCanvasInputMaskRegionLayouts(canvas, state = getState()) {
    if (!canvas || !state.layout.regionIds.length) return [];
    const bounds = getGridSceneBounds(canvas);
    const inputRect = calculateAspectRatioRect(bounds.width, bounds.height, getInputCanvasAspectRatio());
    inputRect.x += bounds.x;
    inputRect.y += bounds.y;

    const input = getInputCanvasLayoutMetrics();
    const rows = Math.max(1, Number(input.rows) || 1);
    const cols = Math.max(1, Number(input.cols) || Math.max(1, state.layout.regionIds.length));
    const tileWidth = Math.max(1, Number(input.tileWidth) || ((Number(input.canvasWidth) || cols) / cols));
    const tileHeight = Math.max(1, Number(input.tileHeight) || ((Number(input.canvasHeight) || rows) / rows));
    const regionIdSet = new Set((state.layout.regionIds || []).map((id) => Number(id)).filter((id) => Number.isFinite(id) && id > 0));
    const sourceRegions = Array.from({ length: rows * cols }, (_, index) => {
        const regionId = index + 1;
        const inputRow = Math.floor(index / cols);
        const inputCol = index % cols;
        const region = state.geometry.byRegionId?.[regionId];
        if (regionIdSet.has(regionId) &&
            region &&
            Number(region.srcWidth) > 0 &&
            Number(region.srcHeight) > 0) {
            return { ...region, id: regionId };
        }
        return regionIdSet.has(regionId) ? {
            id: regionId,
            srcX: inputCol * tileWidth,
            srcY: inputRow * tileHeight,
            srcWidth: tileWidth,
            srcHeight: tileHeight,
            rows: 2,
            cols: 2,
            points: [
                { u: -1, v: 1 },
                { u: 1, v: 1 },
                { u: -1, v: -1 },
                { u: 1, v: -1 }
            ]
        } : null;
    }).filter(Boolean);

    return sourceRegions.map((region) => {
        const regionId = Number(region.id);
        const sourceRect01 = getRegionSourceRect01(region, state);
        const rect = {
            x: inputRect.x + sourceRect01.minU * inputRect.width,
            y: inputRect.y + sourceRect01.minV * inputRect.height,
            width: Math.max(1, sourceRect01.width * inputRect.width),
            height: Math.max(1, sourceRect01.height * inputRect.height)
        };
        const isActive = String(regionId) === String(state.page.activeRegionId);
        return {
            regionId,
            region,
            cell: rect,
            rect,
            localRect: inputRect,
            outputRect: inputRect,
            sourceRect01,
            isActive,
            role: 'input'
        };
    });
}

function getTransformedMaskPointScreens(canvas, state, mask) {
    if (!mask || !Array.isArray(mask.points)) return [];
    const layouts = getCanvasInputMaskRegionLayouts(canvas, state);
    if (!layouts.length) return [];
    const hits = [];
    mask.points.forEach((point, index) => {
        const row = Math.floor(index / mask.cols);
        const col = index % mask.cols;
        if (!isMaskPerimeterPoint(mask, row, col)) return;
        layouts.forEach((layout) => {
            const screen = maskCanvasPointToRegionScreen(point, layout);
            if (!screen) return;
            hits.push({ pointIndex: index, ...layout, screen });
        });
    });
    return hits;
}

export function hitTestTransformedMaskPoint(canvas, state, mask, mx, my, radius = GRID_VISUAL_METRICS.pointHitRadius) {
    let best = null;
    getTransformedMaskPointScreens(canvas, state, mask).forEach((hit) => {
        const distance = Math.hypot(mx - hit.screen.x, my - hit.screen.y);
        if (distance <= radius && (!best || distance < best.distance)) {
            best = { ...hit, distance };
        }
    });
    return best;
}

export function screenToTransformedMaskCanvasPoint(canvas, state, regionId, mx, my) {
    const layouts = getCanvasInputMaskRegionLayouts(canvas, state);
    if (!layouts.length) return null;
    let layout = layouts.find((item) => String(item.regionId) === String(regionId)) || null;
    let local = layout ? outputScreenToRegionLocal(layout, mx, my) : null;
    const insideLocal = (point) => point &&
        point.u >= -0.02 && point.u <= 1.02 &&
        point.v >= -0.02 && point.v <= 1.02;
    if (!insideLocal(local)) {
        layout = layouts.find((item) => insideLocal(outputScreenToRegionLocal(item, mx, my))) || layout;
        local = layout ? outputScreenToRegionLocal(layout, mx, my) : null;
    }
    const rect = layout?.sourceRect01;
    if (!local || !rect || rect.width <= 0 || rect.height <= 0) return null;
    return {
        u: rect.minU + clamp01(local.u) * rect.width,
        v: rect.minV + clamp01(local.v) * rect.height
    };
}

function drawFusionEdgeLines(ctx, region, layout, blend, style) {
    if (!region || !region.points || region.points.length === 0 || !blend) return;
    const isSideOn = (targetBlend, side) => {
        const edge = targetBlend?.[side];
        return !!edge?.enabled;
    };
    const hasEffectiveSide = (side) => {
        const state = getState();
        return !!state.blend.masterEnabled && isSideOn(blend, side);
    };
    ctx.save();
    ctx.strokeStyle = 'rgba(245, 158, 11, 0.9)';
    ctx.lineWidth = style.fusionEdgeLineWidth;
    ctx.setLineDash([6, 4]);

    const drawEdge = (points) => {
        let started = false;
        ctx.beginPath();
        points.forEach((point) => {
            if (!point) return;
            const screen = localPointToOutputScreen(point, layout);
            if (!screen) return;
            if (!started) {
                ctx.moveTo(screen.x, screen.y);
                started = true;
            } else {
                ctx.lineTo(screen.x, screen.y);
            }
        });
        if (started) ctx.stroke();
    };

    if (hasEffectiveSide('top')) {
        const t = clamp01(blend.top?.width);
        drawEdge(Array.from({ length: region.cols }, (_, c) => {
            const top = region.points[c];
            const bottom = region.points[(region.rows - 1) * region.cols + c];
            return top && bottom ? lerpPoint(top, bottom, t) : top || bottom;
        }));
    }
    if (hasEffectiveSide('bottom')) {
        const t = 1 - clamp01(blend.bottom?.width);
        drawEdge(Array.from({ length: region.cols }, (_, c) => {
            const top = region.points[c];
            const bottom = region.points[(region.rows - 1) * region.cols + c];
            return top && bottom ? lerpPoint(top, bottom, t) : top || bottom;
        }));
    }
    if (hasEffectiveSide('left')) {
        const t = clamp01(blend.left?.width);
        drawEdge(Array.from({ length: region.rows }, (_, r) => {
            const left = region.points[r * region.cols];
            const right = region.points[r * region.cols + region.cols - 1];
            return left && right ? lerpPoint(left, right, t) : left || right;
        }));
    }
    if (hasEffectiveSide('right')) {
        const t = 1 - clamp01(blend.right?.width);
        drawEdge(Array.from({ length: region.rows }, (_, r) => {
            const left = region.points[r * region.cols];
            const right = region.points[r * region.cols + region.cols - 1];
            return left && right ? lerpPoint(left, right, t) : left || right;
        }));
    }
    ctx.restore();
}

function buildRegionBoundaryPath(ctx, region, layout, resetPath = true) {
    if (!region || !region.points || region.points.length === 0) return false;
    const getPoint = (row, col) => region.points[row * region.cols + col];
    let started = false;
    const addPoint = (point) => {
        if (!point) return;
        const screen = localPointToOutputScreen(point, layout);
        if (!screen) return;
        if (!started) {
            ctx.moveTo(screen.x, screen.y);
            started = true;
        } else {
            ctx.lineTo(screen.x, screen.y);
        }
    };

    if (resetPath) ctx.beginPath();
    for (let c = 0; c < region.cols; c += 1) addPoint(getPoint(0, c));
    for (let r = 1; r < region.rows; r += 1) addPoint(getPoint(r, region.cols - 1));
    for (let c = region.cols - 2; c >= 0; c -= 1) addPoint(getPoint(region.rows - 1, c));
    for (let r = region.rows - 2; r > 0; r -= 1) addPoint(getPoint(r, 0));
    if (started) ctx.closePath();
    return started;
}

function drawRegionPreviewContent(ctx, region, layout, isActive) {
    const rect = layout.localRect || layout.rect;
    ctx.save();
    if (buildRegionBoundaryPath(ctx, region, layout)) {
        ctx.clip();
    } else {
        ctx.beginPath();
        ctx.rect(rect.x, rect.y, rect.width, rect.height);
        ctx.clip();
    }

    const gradient = ctx.createLinearGradient(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
    gradient.addColorStop(0, isActive ? 'rgba(59, 130, 246, 0.22)' : 'rgba(30, 41, 59, 0.65)');
    gradient.addColorStop(1, isActive ? 'rgba(14, 165, 233, 0.12)' : 'rgba(15, 23, 42, 0.45)');
    ctx.fillStyle = gradient;
    ctx.fillRect(rect.x, rect.y, rect.width, rect.height);

    ctx.strokeStyle = 'rgba(148, 163, 184, 0.18)';
    ctx.lineWidth = 1;
    const step = Math.max(24, Math.min(rect.width, rect.height) / 5);
    for (let x = rect.x - rect.height; x < rect.x + rect.width; x += step) {
        ctx.beginPath();
        ctx.moveTo(x, rect.y + rect.height);
        ctx.lineTo(x + rect.height, rect.y);
        ctx.stroke();
    }
    ctx.restore();
}

function drawRegionCenterNumber(ctx, regionId, rect, isActive, role) {
    const size = role === 'focus'
        ? Math.max(42, Math.min(118, Math.min(rect.width, rect.height) * 0.26))
        : Math.max(18, Math.min(32, Math.min(rect.width, rect.height) * 0.34));
    const text = String(regionId);
    ctx.save();
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.font = `bold ${size}px Arial, sans-serif`;
    ctx.lineWidth = Math.max(3, size * 0.09);
    ctx.strokeStyle = 'rgba(2, 6, 23, 0.76)';
    ctx.fillStyle = isActive ? 'rgba(255, 255, 255, 0.92)' : 'rgba(226, 232, 240, 0.58)';
    ctx.strokeText(text, rect.x + rect.width / 2, rect.y + rect.height / 2);
    ctx.fillText(text, rect.x + rect.width / 2, rect.y + rect.height / 2);
    ctx.restore();
}

export function resizeCanvasToDisplaySize(canvas, fallbackWidth = 960, fallbackHeight = 540) {
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(1, Math.round(rect.width || fallbackWidth));
    const height = Math.max(1, Math.round(rect.height || fallbackHeight));
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }
}

export function drawGridScene(canvas) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const state = getState();
    if (!state.page.initialized) return;
    const isMask = state.page.activeTab === 'mask';
    const isBlendPage = state.page.activeTab === 'blend';
    const isBlendManager = state.page.activeTab === 'blend' && !!state.blend.managerMode;
    const active = isMask ? getActiveMask() : getActiveGeometry();
    if (!active) return;
    
    const style = getDebugStyle(isMask ? 'mask' : (isBlendManager ? 'blend' : 'geometry'));
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#0b1020';
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    if (!state.layout.regionIds.length) return;
    const regionLayouts = isMask
        ? getCanvasInputMaskRegionLayouts(canvas, state)
        : getCanvasRegionPreviewLayouts(canvas, state);

    if (isMask) {
        const inputRect = regionLayouts.find((layout) => layout.outputRect)?.outputRect;
        if (inputRect) {
            ctx.fillStyle = 'rgba(15, 23, 42, 0.72)';
            ctx.fillRect(inputRect.x, inputRect.y, inputRect.width, inputRect.height);
            ctx.strokeStyle = 'rgba(148, 163, 184, 0.46)';
            ctx.lineWidth = style.regionBorderWidth;
            strokeInsetRect(ctx, inputRect);
        }
        drawTransformedMaskBoundaryLines(ctx, active, regionLayouts, style);
        drawTransformedMaskPoints(ctx, active, regionLayouts, style);
        return;
    }

    regionLayouts.forEach((layout) => {
        const { region, cell, rect, localRect, isActive, role } = layout;
        ctx.fillStyle = role === 'output'
            ? 'rgba(15, 23, 42, 0.72)'
            : role === 'focus'
                ? 'rgba(15, 23, 42, 0.88)'
                : isActive
                    ? 'rgba(30, 41, 59, 0.92)'
                    : 'rgba(15, 23, 42, 0.72)';
        ctx.fillRect(cell.x, cell.y, cell.width, cell.height);
        ctx.globalAlpha = role === 'thumb' ? 0.78 : 1;
        if (!isMask) drawRegionPreviewContent(ctx, region, layout, isActive);
        ctx.globalAlpha = 1;

        const showRegionFrame = isMask || !!region?.showGrid;
        if (showRegionFrame) {
            ctx.strokeStyle = role === 'output'
                ? 'rgba(148, 163, 184, 0.26)'
                : isActive
                    ? '#60a5fa'
                    : 'rgba(148, 163, 184, 0.46)';
            ctx.lineWidth = role === 'output'
                ? style.regionBorderWidth
                : isActive
                    ? style.activeRegionBorderWidth
                    : style.regionBorderWidth;
            const frameRect = localRect || rect;
            strokeInsetRect(ctx, frameRect);
        }
    });

    const drawRegionGrid = (layout) => {
        const { regionId, region, isActive } = layout;
        if (region) {
            if (isBlendManager) {
                const displayRegion = createBlendManagerDisplayGrid(region, state.blend.byRegionId[regionId]);
                if (region.showGrid || state.blend.managerMode) {
                    drawGridLines(ctx, displayRegion, layout, style, isActive, true);
                }
                if (state.blend.masterEnabled) {
                    drawFusionEdgeLines(ctx, region, layout, state.blend.byRegionId[regionId], style);
                }
                if (isActive) drawPoints(ctx, displayRegion, layout, style, true);
            } else if (isBlendPage) {
                if (region.showGrid) {
                    drawGridLines(ctx, region, layout, style, isActive);
                    if (isActive) drawPoints(ctx, region, layout, style, true);
                }
            } else if (region.showGrid) {
                drawGridLines(ctx, region, layout, style, isActive);
                if (isActive) drawPoints(ctx, region, layout, style, true);
            }
        }
    };

    regionLayouts.filter((item) => !item.isActive).forEach(drawRegionGrid);
    regionLayouts.filter((item) => item.isActive).forEach(drawRegionGrid);

    regionLayouts.forEach(({ regionId, rect, isActive, role }) => {
        drawRegionCenterNumber(ctx, regionId, rect, isActive, role);
    });

    regionLayouts.forEach(({ regionId, cell, isActive, role }) => {
        ctx.fillStyle = isActive ? '#e2e8f0' : 'rgba(226, 232, 240, 0.72)';
        ctx.font = role === 'focus' ? 'bold 14px Consolas, monospace' : 'bold 11px Consolas, monospace';
        const label = isBlendPage ? `融合 投影${regionId}` : `几何 投影${regionId}`;
        ctx.fillText(isActive && role === 'focus' ? `${label} · 当前` : label, cell.x + (role === 'focus' ? 18 : 10), cell.y + (role === 'focus' ? 28 : 17));
    });
}

export function drawBlendScene(canvas) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const { width, height } = canvas;
    const state = getState();
    const regionId = state.page.activeRegionId;
    const side = state.blend.activeSide;
    const sideState = state.blend.byRegionId[regionId]?.[side] || state.blend.byRegionId[state.page.activeRegionId]?.[side];
    if (!sideState) return;
    const rect = getBlendPlotRect(canvas);

    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#0b1220';
    ctx.fillRect(0, 0, width, height);

    ctx.strokeStyle = '#1e293b';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i += 1) {
        const gx = rect.x + (rect.width / 4) * i;
        const gy = rect.y + (rect.height / 4) * i;
        ctx.beginPath();
        ctx.moveTo(gx, rect.y);
        ctx.lineTo(gx, rect.y + rect.height);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(rect.x, gy);
        ctx.lineTo(rect.x + rect.width, gy);
        ctx.stroke();
    }

    const layout = getBlendCurveHandleLayout(canvas, sideState);
    const params = layout.params;

    const drawCurveRange = (fromX, toX, style, lineWidth) => {
        if (toX <= fromX) return;
        ctx.strokeStyle = style;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        for (let i = 0; i <= 96; i += 1) {
            const t = i / 96;
            const x = fromX + (toX - fromX) * t;
            const screen = blendPointToCanvas({ x, y: 1 - getBlendCurveAlpha(sideState, x) }, rect);
            if (i === 0) ctx.moveTo(screen.x, screen.y);
            else ctx.lineTo(screen.x, screen.y);
        }
        ctx.stroke();
    };

    drawCurveRange(0, params.stripStartRatio, 'rgba(96, 165, 250, 0.28)', 1.5);
    drawCurveRange(params.stripEndRatio, 1, 'rgba(96, 165, 250, 0.28)', 1.5);
    drawCurveRange(params.stripStartRatio, params.stripEndRatio, '#60a5fa', 2.5);

    ctx.strokeStyle = 'rgba(248, 250, 252, 0.18)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(layout.handles[0].screen.x, rect.y);
    ctx.lineTo(layout.handles[0].screen.x, rect.y + rect.height);
    ctx.moveTo(layout.handles[2].screen.x, rect.y);
    ctx.lineTo(layout.handles[2].screen.x, rect.y + rect.height);
    ctx.stroke();

    layout.handles.forEach((handle) => {
        const radius = handle.id === 'anchor'
            ? GRID_VISUAL_METRICS.blendCurvePointRadius + 1
            : GRID_VISUAL_METRICS.blendCurvePointRadius;
        ctx.fillStyle = '#f59e0b';
        ctx.beginPath();
        ctx.arc(handle.screen.x, handle.screen.y, radius, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = '#f8fafc';
        ctx.lineWidth = 1.5;
        ctx.stroke();
    });

    ctx.fillStyle = '#e2e8f0';
    ctx.font = '12px Consolas, monospace';
    ctx.fillText(`BLEND ${side.toUpperCase()} · gamma=${params.gamma.toFixed(2)} gain=${params.anchor.toFixed(2)} slope=${params.slope.toFixed(2)}`, 24, 14);
}
