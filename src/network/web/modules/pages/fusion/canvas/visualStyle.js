const DEFAULT_GRID_LINE_WIDTH = 7.0;
const DEFAULT_GRID_HOTSPOT_RADIUS = 0.005;

function clamp(value, min, max) {
    return Math.max(min, Math.min(max, value));
}

export const GRID_VISUAL_METRICS = {
    gridLineWidth: 1.5,
    maskBoundaryLineWidth: 1.5,
    fusionEdgeLineWidth: 2,
    regionBorderWidth: 1.5,
    activeRegionBorderWidth: 2,
    overviewCellBorderWidth: 1,
    activeOverviewCellBorderWidth: 2,
    selectedPointRadius: 2.6,
    pointRadius: 2.4,
    pointInnerRadius: 2.4,
    pointOuterRadius: 3.8,
    pointHitRadius: 10,
    lineHitRadius: 8,
    blendCurvePointRadius: 3
};

export const GRID_VISUAL_STATE = {
    lineWidth: DEFAULT_GRID_LINE_WIDTH,
    hotspotRadius: DEFAULT_GRID_HOTSPOT_RADIUS
};

export function getGridVisualStyle() {
    return {
        lineWidth: GRID_VISUAL_STATE.lineWidth,
        hotspotRadius: GRID_VISUAL_STATE.hotspotRadius
    };
}

export function applyGridVisualStyle(lineWidth = DEFAULT_GRID_LINE_WIDTH, hotspotRadius = DEFAULT_GRID_HOTSPOT_RADIUS) {
    const normalizedLineWidth = clamp(Number(lineWidth) || DEFAULT_GRID_LINE_WIDTH, 0.5, 12);
    const normalizedHotspotRadius = clamp(Number(hotspotRadius) || DEFAULT_GRID_HOTSPOT_RADIUS, 0.003, 0.05);
    const canvasLineWidth = clamp(normalizedLineWidth * 0.24, 0.8, 4);
    const canvasPointRadius = clamp(normalizedHotspotRadius * 480, 1.8, 8);

    GRID_VISUAL_STATE.lineWidth = normalizedLineWidth;
    GRID_VISUAL_STATE.hotspotRadius = normalizedHotspotRadius;
    GRID_VISUAL_METRICS.gridLineWidth = canvasLineWidth;
    GRID_VISUAL_METRICS.maskBoundaryLineWidth = canvasLineWidth;
    GRID_VISUAL_METRICS.fusionEdgeLineWidth = clamp(canvasLineWidth + 0.35, 1.2, 5);
    GRID_VISUAL_METRICS.selectedPointRadius = clamp(canvasPointRadius + 0.2, 2, 8.5);
    GRID_VISUAL_METRICS.pointRadius = canvasPointRadius;
    GRID_VISUAL_METRICS.pointInnerRadius = canvasPointRadius;
    GRID_VISUAL_METRICS.pointOuterRadius = clamp(canvasPointRadius + 1.1, 3, 10);
    GRID_VISUAL_METRICS.pointHitRadius = clamp(canvasPointRadius + 7, 8, 18);
    GRID_VISUAL_METRICS.lineHitRadius = clamp(canvasLineWidth + 6, 7, 18);

    return getGridVisualStyle();
}

applyGridVisualStyle(DEFAULT_GRID_LINE_WIDTH, DEFAULT_GRID_HOTSPOT_RADIUS);
