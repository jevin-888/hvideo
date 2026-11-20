import {
    blendPointToCanvas,
    getBlendCurveAlpha,
    getBlendCurveHandleLayout
} from '../utils/blendCurve.js';

function clampUnit(value) {
    return Math.max(0, Math.min(1, Number(value) || 0));
}

export function drawBlendCurvePreview(canvas, sideState, options = {}) {
    if (!canvas || !sideState) return;
    const ctx = canvas.getContext('2d');
    const { width, height } = canvas;
    const layout = getBlendCurveHandleLayout(canvas, sideState);
    const { rect, params } = layout;
    const stripSpan = params.stripEndRatio - params.stripStartRatio;
    const plotBottom = rect.y + rect.height;
    const sampleCurve = (x) => {
        const level = clampUnit(getBlendCurveAlpha(sideState, x));
        return {
            x,
            level,
            screen: blendPointToCanvas({ x, y: level }, rect)
        };
    };
    const samples = Array.from({ length: 129 }, (_, index) => sampleCurve(index / 128));
    const drawCurveRange = (fromX, toX, strokeStyle, lineWidth) => {
        if (toX <= fromX) return;
        ctx.strokeStyle = strokeStyle;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        const first = sampleCurve(fromX);
        ctx.moveTo(first.screen.x, first.screen.y);
        for (let index = 1; index <= 48; index += 1) {
            const x = fromX + (toX - fromX) * (index / 48);
            const screen = sampleCurve(x).screen;
            ctx.lineTo(screen.x, screen.y);
        }
        ctx.stroke();
    };

    ctx.save();
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#0b1220';
    ctx.fillRect(0, 0, width, height);

    ctx.save();
    ctx.beginPath();
    ctx.rect(rect.x, rect.y, rect.width, rect.height);
    ctx.clip();

    const luminanceField = ctx.createLinearGradient(0, rect.y, 0, plotBottom);
    luminanceField.addColorStop(0, '#f8fafc');
    luminanceField.addColorStop(0.5, '#64748b');
    luminanceField.addColorStop(1, '#020617');
    ctx.fillStyle = luminanceField;
    ctx.fillRect(rect.x, rect.y, rect.width, rect.height);

    if (params.stripStartRatio > 0) {
        ctx.fillStyle = 'rgba(2, 6, 23, 0.34)';
        ctx.fillRect(rect.x, rect.y, rect.width * params.stripStartRatio, rect.height);
    }
    if (stripSpan > 0) {
        ctx.fillStyle = 'rgba(34, 211, 238, 0.13)';
        ctx.fillRect(rect.x + rect.width * params.stripStartRatio, rect.y, rect.width * stripSpan, rect.height);
    }
    if (params.stripEndRatio < 1) {
        ctx.fillStyle = 'rgba(248, 250, 252, 0.08)';
        ctx.fillRect(rect.x + rect.width * params.stripEndRatio, rect.y, rect.width * (1 - params.stripEndRatio), rect.height);
    }

    ctx.beginPath();
    ctx.moveTo(samples[0].screen.x, plotBottom);
    samples.forEach((point) => ctx.lineTo(point.screen.x, point.screen.y));
    ctx.lineTo(samples[samples.length - 1].screen.x, plotBottom);
    ctx.closePath();
    const levelFill = ctx.createLinearGradient(0, rect.y, 0, plotBottom);
    levelFill.addColorStop(0, 'rgba(125, 211, 252, 0.30)');
    levelFill.addColorStop(1, 'rgba(34, 211, 238, 0.04)');
    ctx.fillStyle = levelFill;
    ctx.fill();

    ctx.strokeStyle = 'rgba(15, 23, 42, 0.30)';
    ctx.lineWidth = 1;
    for (let index = 0; index <= 4; index += 1) {
        const gx = rect.x + (rect.width / 4) * index;
        const gy = rect.y + (rect.height / 4) * index;
        ctx.beginPath();
        ctx.moveTo(gx, rect.y);
        ctx.lineTo(gx, plotBottom);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(rect.x, gy);
        ctx.lineTo(rect.x + rect.width, gy);
        ctx.stroke();
    }

    ctx.save();
    ctx.setLineDash([3, 4]);
    ctx.strokeStyle = 'rgba(15, 23, 42, 0.58)';
    ctx.beginPath();
    ctx.moveTo(rect.x, rect.y + rect.height * 0.5);
    ctx.lineTo(rect.x + rect.width, rect.y + rect.height * 0.5);
    ctx.stroke();
    ctx.restore();

    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    drawCurveRange(0, params.stripStartRatio, 'rgba(186, 230, 253, 0.72)', 2);
    drawCurveRange(params.stripEndRatio, 1, 'rgba(15, 23, 42, 0.82)', 2);
    drawCurveRange(params.stripStartRatio, params.stripEndRatio, 'rgba(2, 6, 23, 0.90)', 5);
    const curveGradient = ctx.createLinearGradient(rect.x, 0, rect.x + rect.width, 0);
    curveGradient.addColorStop(0, '#fbbf24');
    curveGradient.addColorStop(1, '#22d3ee');
    ctx.shadowColor = 'rgba(34, 211, 238, 0.58)';
    ctx.shadowBlur = 5;
    drawCurveRange(params.stripStartRatio, params.stripEndRatio, curveGradient, 3);
    ctx.restore();

    ctx.strokeStyle = 'rgba(226, 232, 240, 0.55)';
    ctx.lineWidth = 1;
    ctx.strokeRect(rect.x + 0.5, rect.y + 0.5, rect.width - 1, rect.height - 1);

    const startX = rect.x + rect.width * params.stripStartRatio;
    const endX = rect.x + rect.width * params.stripEndRatio;
    ctx.save();
    ctx.setLineDash([5, 4]);
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = 'rgba(251, 191, 36, 0.96)';
    ctx.beginPath();
    ctx.moveTo(startX, rect.y);
    ctx.lineTo(startX, plotBottom);
    ctx.stroke();
    ctx.strokeStyle = 'rgba(34, 211, 238, 0.96)';
    ctx.beginPath();
    ctx.moveTo(endX, rect.y);
    ctx.lineTo(endX, plotBottom);
    ctx.stroke();
    ctx.restore();

    const ribbonY = plotBottom + 5;
    const ribbonHeight = 8;
    for (let index = 0; index < samples.length - 1; index += 1) {
        const current = samples[index];
        const next = samples[index + 1];
        const value = Math.round(current.level * 255);
        ctx.fillStyle = `rgb(${value}, ${value}, ${value})`;
        ctx.fillRect(current.screen.x, ribbonY, Math.max(1, next.screen.x - current.screen.x + 0.5), ribbonHeight);
    }
    ctx.strokeStyle = 'rgba(248, 250, 252, 0.66)';
    ctx.strokeRect(rect.x + 0.5, ribbonY + 0.5, rect.width - 1, ribbonHeight - 1);

    ctx.fillStyle = '#e2e8f0';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'right';
    ctx.fillText((globalThis.PlayerI18n?.t('亮 100%') ?? ('亮 100%')), rect.x - 6, rect.y + 4);
    ctx.fillText((globalThis.PlayerI18n?.t('50%') ?? ('50%')), rect.x - 6, rect.y + rect.height * 0.5 + 3);
    ctx.fillText((globalThis.PlayerI18n?.t('暗 0%') ?? ('暗 0%')), rect.x - 6, plotBottom);
    ctx.textAlign = 'left';
    ctx.fillText((globalThis.PlayerI18n?.t('边缘') ?? ('边缘')), rect.x, height - 7);
    ctx.textAlign = 'center';
    ctx.fillText((globalThis.PlayerI18n?.t(`融合带 ${params.stripStart}-${params.stripEnd}`) ?? (`融合带 ${params.stripStart}-${params.stripEnd}`)), rect.x + rect.width * 0.5, height - 7);
    ctx.textAlign = 'right';
    ctx.fillText((globalThis.PlayerI18n?.t('画面') ?? ('画面')), rect.x + rect.width, height - 7);

    const handleRadius = Math.max(3, Number(options.handleRadius) || 6);
    const anchorRadius = Math.max(handleRadius, Number(options.anchorRadius) || handleRadius + 1);
    layout.handles.forEach((handle) => {
        const radius = handle.id === 'anchor' ? anchorRadius : handleRadius;
        ctx.fillStyle = handle.id === 'stripStart'
            ? '#fbbf24'
            : handle.id === 'stripEnd'
                ? '#22d3ee'
                : '#f8fafc';
        ctx.beginPath();
        ctx.arc(handle.screen.x, handle.screen.y, radius, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = '#07111f';
        ctx.lineWidth = 2;
        ctx.stroke();
    });

    const sideLabel = String(options.sideLabel || '').toUpperCase();
    const title = width >= 430
        ? `BLEND ${sideLabel} | gamma=${params.gamma.toFixed(2)} anchor=${params.anchor.toFixed(2)} slope=${params.slope.toFixed(2)}`
        : width >= 290
            ? `${sideLabel} G${params.gamma.toFixed(2)} A${params.anchor.toFixed(2)} S${params.slope.toFixed(2)}`
            : `${sideLabel.charAt(0)} G${params.gamma.toFixed(1)} A${params.anchor.toFixed(1)} S${params.slope.toFixed(1)}`;
    ctx.fillStyle = '#f8fafc';
    ctx.font = '11px Consolas, monospace';
    ctx.textAlign = 'left';
    ctx.fillText((globalThis.PlayerI18n?.t(title) ?? (title)), rect.x, 14, rect.width);
    ctx.restore();
}
