import { clamp } from './grid.js';

const MIN_BLEND_GAMMA = 0.1;
const MIN_BLEND_SLOPE = 0.01;

function finiteNumber(value, fallback) {
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
}

export function getBlendPlotRect(canvas) {
    return {
        x: 42,
        y: 22,
        width: Math.max(1, finiteNumber(canvas?.width, 0) - 58),
        height: Math.max(1, finiteNumber(canvas?.height, 0) - 62)
    };
}

export function blendPointToCanvas(point, rect) {
    return {
        x: rect.x + point.x * rect.width,
        y: rect.y + (1 - point.y) * rect.height
    };
}

export function getBlendCurveParams(sideState = {}) {
    const rawStart = Math.round(clamp(finiteNumber(sideState.stripStart, 0), 0, 255));
    const rawEnd = Math.round(clamp(finiteNumber(sideState.stripEnd, 255), 0, 255));
    const stripStart = Math.min(rawStart, rawEnd);
    const stripEnd = Math.max(rawStart, rawEnd);
    return {
        stripStart,
        stripEnd,
        stripStartRatio: stripStart / 255,
        stripEndRatio: stripEnd / 255,
        anchor: clamp(finiteNumber(sideState.anchor, 0.5), 0, 1),
        slope: Math.max(MIN_BLEND_SLOPE, finiteNumber(sideState.slope, 1.0)),
        gamma: Math.max(MIN_BLEND_GAMMA, finiteNumber(sideState.gamma, 1.8))
    };
}

function gammaBlend(anchor, slope, gamma, x) {
    // 与 region.frag 的实时融合 gamma 曲线保持一致：
    // 两个分支都把 gamma 指数作用在整个 x1 上，保证 anchor≠0.5 时 x=0.5 处连续。
    const clampedX = clamp(x, 0, 1);
    if (clampedX <= 0) return 0;
    if (clampedX >= 1) return 1;
    const x1 = clampedX < 0.5
        ? anchor * Math.pow(clampedX * 2, slope)
        : 1 - (1 - anchor) * Math.pow((1 - clampedX) * 2, slope);
    return Math.pow(x1, gamma);
}

function blendCurveValue(anchor, slope, gamma, x) {
    return 1 - gammaBlend(anchor, slope, gamma, 1 - clamp(x, 0, 1));
}

export function getBlendCurveAnchorFromDisplayValue(sideState, displayValue) {
    const params = getBlendCurveParams(sideState);
    const y = clamp(finiteNumber(displayValue, 0), 0, 1);
    if (y <= 0) return 1;
    if (y >= 1) return 0;
    return clamp(Math.pow(1 - y, 1 / params.gamma), 0, 1);
}

export function getBlendCurvePatchFromDisplayPoint(sideState, handleId, displayPoint = {}) {
    const params = getBlendCurveParams(sideState);
    const x = clamp(finiteNumber(displayPoint.x, 0), 0, 1);
    if (handleId === 'stripStart') {
        return {
            stripStart: Math.round(clamp(x * 255, 0, Math.max(0, params.stripEnd - 1))),
            stripEnd: params.stripEnd
        };
    }
    if (handleId === 'stripEnd') {
        return {
            stripStart: params.stripStart,
            stripEnd: Math.round(clamp(x * 255, Math.min(255, params.stripStart + 1), 255))
        };
    }
    return {
        anchor: Number(getBlendCurveAnchorFromDisplayValue(sideState, displayPoint.y).toFixed(3))
    };
}

export function getBlendCurveAlpha(sideState, x) {
    const params = getBlendCurveParams(sideState);
    if (x <= params.stripStartRatio) return 0;
    if (x >= params.stripEndRatio) return 1;
    const span = params.stripEndRatio - params.stripStartRatio;
    if (span < 0.000001) return x >= params.stripEndRatio ? 1 : 0;
    const localX = (x - params.stripStartRatio) / span;
    return blendCurveValue(params.anchor, params.slope, params.gamma, localX);
}

export function getBlendCurveHandleLayout(canvas, sideState) {
    const rect = getBlendPlotRect(canvas);
    const params = getBlendCurveParams(sideState);
    const anchorX = params.stripStartRatio + (params.stripEndRatio - params.stripStartRatio) * 0.5;
    const makeHandle = (id, x, label) => {
        const y = getBlendCurveAlpha(sideState, x);
        return {
            id,
            label,
            x,
            y,
            screen: blendPointToCanvas({ x, y }, rect)
        };
    };
    return {
        rect,
        params,
        handles: [
            makeHandle('stripStart', params.stripStartRatio, '起点'),
            makeHandle('anchor', anchorX, '曲线'),
            makeHandle('stripEnd', params.stripEndRatio, '终点')
        ]
    };
}
