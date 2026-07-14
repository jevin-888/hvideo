import { enrichBounds } from './geometry.js';

const EDGE_POINTS = [
    { key: 'left', axis: 'x' },
    { key: 'centerX', axis: 'x' },
    { key: 'right', axis: 'x' },
    { key: 'top', axis: 'y' },
    { key: 'centerY', axis: 'y' },
    { key: 'bottom', axis: 'y' }
];

function getCanvasThreshold(canvas, dpr, canvasConfig, cssPx = 8) {
    if (!canvas || !canvasConfig?.width || !canvasConfig?.height) return cssPx;
    const logicalWidth = canvas.width / (dpr || 1);
    const logicalHeight = canvas.height / (dpr || 1);
    const scaleX = logicalWidth > 0 ? logicalWidth / canvasConfig.width : 1;
    const scaleY = logicalHeight > 0 ? logicalHeight / canvasConfig.height : 1;
    const scale = Math.max(0.0001, Math.min(scaleX, scaleY));
    return Math.max(1, Math.round(cssPx / scale));
}

function addGuide(list, guide) {
    const exists = list.some(item =>
        item.axis === guide.axis &&
        Math.round(item.value) === Math.round(guide.value) &&
        item.kind === guide.kind
    );
    if (!exists) list.push(guide);
}

function getGridValues(size, divisions) {
    const count = Math.max(1, Number(divisions) || 1);
    const values = [];
    for (let i = 1; i < count; i++) {
        values.push(Math.round((size / count) * i));
    }
    return values;
}

function buildReferenceLines(referenceBoundsList, canvasConfig) {
    const lines = {
        x: [
            { value: 0, kind: 'canvas-edge' },
            { value: canvasConfig.width / 2, kind: 'canvas-center' },
            { value: canvasConfig.width, kind: 'canvas-edge' }
        ],
        y: [
            { value: 0, kind: 'canvas-edge' },
            { value: canvasConfig.height / 2, kind: 'canvas-center' },
            { value: canvasConfig.height, kind: 'canvas-edge' }
        ]
    };

    getGridValues(canvasConfig.width, canvasConfig.cols).forEach(value => {
        lines.x.push({ value, kind: 'grid' });
    });
    getGridValues(canvasConfig.height, canvasConfig.rows).forEach(value => {
        lines.y.push({ value, kind: 'grid' });
    });

    referenceBoundsList.forEach(ref => {
        const bounds = enrichBounds(ref.bounds);
        lines.x.push({ value: bounds.left, kind: 'layer-edge', ref });
        lines.x.push({ value: bounds.centerX, kind: 'layer-center', ref });
        lines.x.push({ value: bounds.right, kind: 'layer-edge', ref });
        lines.y.push({ value: bounds.top, kind: 'layer-edge', ref });
        lines.y.push({ value: bounds.centerY, kind: 'layer-center', ref });
        lines.y.push({ value: bounds.bottom, kind: 'layer-edge', ref });
    });

    return lines;
}

function pickSnap(axis, movingBounds, referenceLines, threshold) {
    let best = null;
    EDGE_POINTS
        .filter(point => point.axis === axis)
        .forEach(point => {
            const movingValue = movingBounds[point.key];
            referenceLines[axis].forEach(line => {
                const distance = line.value - movingValue;
                const abs = Math.abs(distance);
                if (abs > threshold) return;
                if (!best || abs < best.abs) {
                    best = {
                        value: line.value,
                        delta: distance,
                        abs,
                        kind: line.kind,
                        ref: line.ref || null
                    };
                }
            });
        });
    return best;
}

function getGuideSpan(axis, movingBounds, snap, canvasConfig) {
    const refBounds = snap.ref?.bounds ? enrichBounds(snap.ref.bounds) : null;
    if (axis === 'x') {
        const from = refBounds ? Math.min(movingBounds.top, refBounds.top) : 0;
        const to = refBounds ? Math.max(movingBounds.bottom, refBounds.bottom) : canvasConfig.height;
        return { from, to };
    }
    const from = refBounds ? Math.min(movingBounds.left, refBounds.left) : 0;
    const to = refBounds ? Math.max(movingBounds.right, refBounds.right) : canvasConfig.width;
    return { from, to };
}

export function snapBounds(bounds, referenceBoundsList, canvasConfig, options = {}) {
    const movingBounds = enrichBounds(bounds);
    if (!canvasConfig?.width || !canvasConfig?.height || options.disabled) {
        return { delta: { x: 0, y: 0 }, guides: [] };
    }

    const threshold = Number(options.threshold) > 0 ? Number(options.threshold) : 8;
    const lines = buildReferenceLines(referenceBoundsList || [], canvasConfig);
    const xSnap = pickSnap('x', movingBounds, lines, threshold);
    const ySnap = pickSnap('y', movingBounds, lines, threshold);

    const deltaX = xSnap ? xSnap.delta : 0;
    const deltaY = ySnap ? ySnap.delta : 0;
    const snappedBounds = enrichBounds({
        x: movingBounds.x + deltaX,
        y: movingBounds.y + deltaY,
        width: movingBounds.width,
        height: movingBounds.height
    });

    const guides = [];
    if (xSnap) {
        const span = getGuideSpan('x', snappedBounds, xSnap, canvasConfig);
        addGuide(guides, {
            axis: 'x',
            value: xSnap.value,
            from: span.from,
            to: span.to,
            kind: xSnap.kind
        });
    }
    if (ySnap) {
        const span = getGuideSpan('y', snappedBounds, ySnap, canvasConfig);
        addGuide(guides, {
            axis: 'y',
            value: ySnap.value,
            from: span.from,
            to: span.to,
            kind: ySnap.kind
        });
    }

    return {
        delta: { x: deltaX, y: deltaY },
        guides
    };
}

export function getSnapThreshold(canvas, dpr, canvasConfig, cssPx = 8) {
    return getCanvasThreshold(canvas, dpr, canvasConfig, cssPx);
}
