import { apiPost, apiAction } from '../../core/api.js';
import { createFusionApi } from './apiCore.js?v=2.95';
import { flattenPoints } from './utils/points.js';

export function postFusionCommand(param, timeoutMs) {
    const { action, ...params } = param;
    return apiAction('regions', action, params, timeoutMs);
}

const fusionApi = createFusionApi({
    fusionCommand: postFusionCommand,
    apiPost
});

export function persistFusionConfig() {
    return fusionApi.persist();
}

export function resetFusionConfig() {
    return fusionApi.reset();
}

export const loadRegionConfig = fusionApi.loadRegionConfig;
export const loadGeometryState = fusionApi.loadGeometry;
export const saveGeometry = fusionApi.saveGeometry;
export const setGeometryGuideVisibility = fusionApi.showGeometryGrid;
export const setAllGeometryGuideVisibility = fusionApi.showGeometryGridAll;
export const setGeometrySelection = fusionApi.setGeometrySelection;
export const setActiveRegionId = fusionApi.setActiveRegion;
export const setGeometryGrid = fusionApi.setGeometryGrid;
export const resizeGeometryGridOp = fusionApi.resizeGeometry;
export const resizeBlendGridBatchOp = fusionApi.resizeBlendGridBatch;
export const moveGeometryOp = fusionApi.moveGeometry;
export const moveManagerPoint = fusionApi.moveManagerPoint;
export const moveManagerLine = fusionApi.moveManagerLine;
export const setGridVisualStyle = fusionApi.setGridVisualStyle;
export const setGeometryPoint = fusionApi.setGeometryPoint;
export const setGeometryPoints = fusionApi.setGeometryPoints;
export const resizeMaskGridOp = fusionApi.resizeMask;
export const moveMaskOp = fusionApi.moveMask;
export const loadMaskState = fusionApi.loadMask;
export const seedMaskFromGeometryState = fusionApi.seedMaskFromGeometry;
export function updateMaskState(mask) {
    const selected = mask.selected || { row: 0, col: 0 };
    return fusionApi.setMask({
        show_guide: !!mask.showGrid,
        selected_row: selected.row ?? 0,
        selected_col: selected.col ?? 0,
        mask: {
            enabled: !!mask.enabled,
            rows: mask.rows,
            cols: mask.cols,
            interpolation_mode: mask.interpolationMode || 0,
            vertices: flattenPoints(mask.points, { clampPoints: false })
        }
    });
}
export const setMaskEnabledState = (enabled) => fusionApi.setMask({
    mask: { enabled: !!enabled }
});
export const setMaskGuideVisibility = fusionApi.setMaskGuideVisibility;
export const saveMask = fusionApi.saveMask;
export const loadBlend = fusionApi.loadBlend;
export const autoRecalculateBlend = fusionApi.autoRecalculateBlend;

const SIDES = ['left', 'right', 'top', 'bottom'];

function defaultBlendWidthForGrid(gridCount) {
    const count = Math.max(2, Number(gridCount) || 2);
    return count > 2 ? 1 / (count - 1) : 0;
}

function normalizeBlendPayload(regionId, blend) {
    const gridRows = Math.max(2, Number(blend.gridRows) || 2);
    const gridCols = Math.max(2, Number(blend.gridCols) || 2);
    const normalized = {
        ...blend,
        gridRows,
        gridCols
    };
    SIDES.forEach((side) => {
        const source = blend[side] || {};
        const width = Number(source.width ?? 0);
        const fallbackWidth = (side === 'left' || side === 'right')
            ? defaultBlendWidthForGrid(gridCols)
            : defaultBlendWidthForGrid(gridRows);
        normalized[side] = {
            ...source,
            width: source.enabled && width <= 0.001 ? fallbackWidth : width
        };
    });
    return normalized;
}

export function saveBlend(regionId, blend) {
    if (blend?.guide_grid_only === true) {
        const payload = {
            guide_grid_only: true,
            sync_adjacent: blend.sync_adjacent === true
        };
        if (Object.prototype.hasOwnProperty.call(blend, 'blend_grid_rows')) {
            payload.blend_grid_rows = blend.blend_grid_rows;
        }
        if (Object.prototype.hasOwnProperty.call(blend, 'blend_grid_cols')) {
            payload.blend_grid_cols = blend.blend_grid_cols;
        }
        return fusionApi.saveBlend(regionId, payload);
    }
    const normalized = normalizeBlendPayload(regionId, blend || {});
    return fusionApi.saveBlend(regionId, {
        blend_grid_rows: normalized.gridRows,
        blend_grid_cols: normalized.gridCols,
        blend_left: normalized.left?.width ?? 0,
        blend_right: normalized.right?.width ?? 0,
        blend_top: normalized.top?.width ?? 0,
        blend_bottom: normalized.bottom?.width ?? 0,
        blend_left_enabled: !!normalized.left?.enabled,
        blend_right_enabled: !!normalized.right?.enabled,
        blend_top_enabled: !!normalized.top?.enabled,
        blend_bottom_enabled: !!normalized.bottom?.enabled,
        edge_left_gamma: normalized.left?.gamma ?? 1.8,
        edge_left_slope: normalized.left?.slope ?? 1.0,
        edge_right_gamma: normalized.right?.gamma ?? 1.8,
        edge_right_slope: normalized.right?.slope ?? 1.0,
        edge_top_gamma: normalized.top?.gamma ?? 1.8,
        edge_top_slope: normalized.top?.slope ?? 1.0,
        edge_bottom_gamma: normalized.bottom?.gamma ?? 1.8,
        edge_bottom_slope: normalized.bottom?.slope ?? 1.0,
        strip_start_l: normalized.left?.stripStart ?? 0,
        strip_end_l: normalized.left?.stripEnd ?? 255,
        strip_start_r: normalized.right?.stripStart ?? 0,
        strip_end_r: normalized.right?.stripEnd ?? 255,
        strip_start_t: normalized.top?.stripStart ?? 0,
        strip_end_t: normalized.top?.stripEnd ?? 255,
        strip_start_b: normalized.bottom?.stripStart ?? 0,
        strip_end_b: normalized.bottom?.stripEnd ?? 255,
        anchor_l: normalized.left?.anchor ?? 0.5,
        anchor_r: normalized.right?.anchor ?? 0.5,
        anchor_t: normalized.top?.anchor ?? 0.5,
        anchor_b: normalized.bottom?.anchor ?? 0.5,
        sync_adjacent: false
    });
}

export const setBlendCurveParams = fusionApi.setBlendCurveParams;
export const setBlendMaster = fusionApi.setMaster;
export const getFusionMaster = fusionApi.getMaster;
export const setBlendAutoEdges = fusionApi.setBlendAutoEdges;
export const getBlendAutoEdges = fusionApi.getBlendAutoEdges;
export const setManagerMode = fusionApi.setManagerMode;
export const getManagerMode = fusionApi.getManagerMode;
export const setMergeGapBrightness = fusionApi.setMergeGapBrightness;
export const loadColor = fusionApi.loadColor;
export const saveColor = fusionApi.saveColor;
export const loadCorrection = fusionApi.loadCorrection;
export const saveCorrection = fusionApi.saveCorrection;
export const loadCave = fusionApi.loadCave;
export const saveCave = fusionApi.saveCave;
