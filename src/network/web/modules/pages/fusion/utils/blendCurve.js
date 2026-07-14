import { clamp } from './grid.js';

export function getBlendPlotRect(canvas) {
    return {
        x: 24,
        y: 20,
        width: Math.max(80, canvas.width - 48),
        height: Math.max(80, canvas.height - 40)
    };
}

export function blendPointToCanvas(point, rect) {
    return {
        x: rect.x + point.x * rect.width,
        y: rect.y + (1 - point.y) * rect.height
    };
}

export function getBlendCurveParams(sideState = {}) {
    const rawStart = clamp(sideState.stripStart ?? 0, 0, 255);
    const rawEnd = clamp(sideState.stripEnd ?? 255, 0, 255);
    const stripStart = Math.min(rawStart, rawEnd);
    const stripEnd = Math.max(rawStart, rawEnd);
    return {
        stripStart,
        stripEnd,
        stripStartRatio: stripStart / 255,
        stripEndRatio: stripEnd / 255,
        anchor: clamp(sideState.anchor ?? 0.5, 0, 1),
        slope: Math.max(0.01, Number(sideState.slope ?? 1.0)),
        gamma: Math.max(0.1, Number(sideState.gamma ?? 1.8))
    };
}

function gammaBlend(anchor, slope, gamma, x) {
    // 与渲染路径的 blendTexture gamma 曲线保持一致：
    // 两个分支都把 gamma 指数作用在整个 x1 上，保证 anchor≠0.5 时 x=0.5 处连续。
    const clampedX = clamp(x, 0, 1);
    if (clampedX <= 0) return 0;
    if (clampedX >= 1) return 1;
    const x1 = clampedX < 0.5
        ? anchor * Math.pow(clampedX * 2, slope)
        : 1 - (1 - anchor) * Math.pow((1 - clampedX) * 2, slope);
    return Math.pow(x1, gamma);
}

function legacyBlendValue(anchor, slope, gamma, x) {
    const steps = 500;
    const id = Math.floor(clamp(x, 0, 0.999999) * steps);
    const x1 = (steps - id - 1) / steps;
    return 1 - gammaBlend(anchor, slope, gamma, x1);
}

export function getBlendCurveAnchorFromDisplayValue(sideState, displayValue) {
    const params = getBlendCurveParams(sideState);
    const y = clamp(displayValue, 0, 1);
    if (y <= 0) return 0;
    if (y >= 1) return 1;
    const steps = 500;
    const id = Math.floor(0.5 * steps);
    const x1 = (steps - id - 1) / steps;
    const gammaRoot = Math.pow(y, 1 / params.gamma);
    if (x1 < 0.5) {
        const factor = Math.pow(clamp(2 * x1, 0, 1), params.slope);
        if (factor <= 0.000001) return params.anchor;
        return clamp(gammaRoot / factor, 0, 1);
    }
    const factor = Math.pow(clamp(2 * (1 - x1), 0, 1), params.slope);
    if (factor <= 0.000001) return params.anchor;
    return clamp(1 - ((1 - gammaRoot) / factor), 0, 1);
}

export function getBlendCurveAlpha(sideState, x) {
    const params = getBlendCurveParams(sideState);
    if (x <= params.stripStartRatio) return 0;
    if (x >= params.stripEndRatio) return 1;
    const span = params.stripEndRatio - params.stripStartRatio;
    if (span < 0.000001) return x >= params.stripEndRatio ? 1 : 0;
    const localX = (x - params.stripStartRatio) / span;
    return legacyBlendValue(params.anchor, params.slope, params.gamma, localX);
}

export function getBlendCurveHandleLayout(canvas, sideState) {
    const rect = getBlendPlotRect(canvas);
    const params = getBlendCurveParams(sideState);
    const anchorX = params.stripStartRatio + (params.stripEndRatio - params.stripStartRatio) * 0.5;
    const makeHandle = (id, x, label) => {
        const y = 1 - getBlendCurveAlpha(sideState, x);
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
