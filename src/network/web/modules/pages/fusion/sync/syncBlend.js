import { autoRecalculateBlend, loadBlend, saveBlend, setBlendAutoEdges as apiSetBlendAutoEdges, setManagerMode, persistFusionConfig, resetFusionConfig as apiResetFusionConfig, resizeBlendGridBatchOp, resizeGeometryGridOp } from '../api.js?v=2.95';
import { ensureBlendRegion, getState } from '../actions.js?v=2.95';
import { setBlendAutoEdges, setBlendManagerMode, setBlendMasterEnabled } from '../blend/actions.js?v=2.95';
import { loadGeometryLayout, loadGeometryRegion } from './syncGeometry.js?v=2.95';
import { loadMask } from './syncMask.js?v=2.95';
import { getLastApiError } from '../../../core/api.js';

const BLEND_SIDES = ['left', 'right', 'top', 'bottom'];
let blendMutationQueue = Promise.resolve();

function enqueueBlendMutation(task) {
    const run = blendMutationQueue.catch(() => {}).then(task);
    blendMutationQueue = run.catch(() => {});
    return run;
}

function defaultBlendWidthForGrid(gridCount) {
    const count = Math.max(2, Number(gridCount) || 2);
    return count > 2 ? 1 / (count - 1) : 0;
}

function normalizeRegionIds(regionIds) {
    const source = Array.isArray(regionIds) ? regionIds : [regionIds];
    return Array.from(new Set(source
        .map((id) => Number(id))
        .filter((id) => Number.isFinite(id) && id > 0)));
}

function normalizeBlendRegionToGeometry(regionId) {
    const region = ensureBlendRegion(regionId);
    const geometry = getState().geometry.byRegionId[regionId];
    // 融合管理网格和几何播放网格分开：I 模式手动加减行列只影响融合带/辅助线，
    // 不能在保存融合参数时又被几何 rows/cols 覆盖，否则会回到“加减行列影响画面”的错误路径。
    region.gridRows = Math.max(2, Number(region.gridRows) || Number(geometry?.rows) || 2);
    region.gridCols = Math.max(2, Number(region.gridCols) || Number(geometry?.cols) || 2);
    const blendW = defaultBlendWidthForGrid(region.gridCols);
    const blendH = defaultBlendWidthForGrid(region.gridRows);
    BLEND_SIDES.forEach((side) => {
        const sideState = region[side];
        if (!sideState) return;
        if (sideState.enabled && Number(sideState.width || 0) <= 0.001) {
            sideState.width = (side === 'left' || side === 'right') ? blendW : blendH;
        }
    });
    return region;
}

export function hydrateBlendRegion(regionId, response) {
    const region = ensureBlendRegion(regionId);
    if (!response) return region;
    if (typeof response.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(response.blend_auto_edges);
    }
    if (typeof response.fusion_master_enabled === 'boolean') {
        setBlendMasterEnabled(response.fusion_master_enabled);
    }
    region.gridRows = Number(response.blend_grid_rows ?? region.gridRows ?? 2);
    region.gridCols = Number(response.blend_grid_cols ?? region.gridCols ?? 2);
    region.left.width = Number(response.blend_left ?? region.left.width);
    region.left.enabled = !!response.blend_left_enabled;
    region.left.gamma = Number(response.edge_left_gamma ?? region.left.gamma);
    region.left.slope = Number(response.edge_left_slope ?? region.left.slope);
    region.left.stripStart = Number(response.strip_start_l ?? region.left.stripStart);
    region.left.stripEnd = Number(response.strip_end_l ?? region.left.stripEnd);
    region.left.anchor = Number(response.anchor_l ?? region.left.anchor);
    region.left.bright = [
        Number(response.bright_l_r ?? region.left.bright[0]),
        Number(response.bright_l_g ?? region.left.bright[1]),
        Number(response.bright_l_b ?? region.left.bright[2])
    ];
    region.right.width = Number(response.blend_right ?? region.right.width);
    region.right.enabled = !!response.blend_right_enabled;
    region.right.gamma = Number(response.edge_right_gamma ?? region.right.gamma);
    region.right.slope = Number(response.edge_right_slope ?? region.right.slope);
    region.right.stripStart = Number(response.strip_start_r ?? region.right.stripStart);
    region.right.stripEnd = Number(response.strip_end_r ?? region.right.stripEnd);
    region.right.anchor = Number(response.anchor_r ?? region.right.anchor);
    region.right.bright = [
        Number(response.bright_r_r ?? region.right.bright[0]),
        Number(response.bright_r_g ?? region.right.bright[1]),
        Number(response.bright_r_b ?? region.right.bright[2])
    ];
    region.top.width = Number(response.blend_top ?? region.top.width);
    region.top.enabled = !!response.blend_top_enabled;
    region.top.gamma = Number(response.edge_top_gamma ?? region.top.gamma);
    region.top.slope = Number(response.edge_top_slope ?? region.top.slope);
    region.top.stripStart = Number(response.strip_start_t ?? region.top.stripStart);
    region.top.stripEnd = Number(response.strip_end_t ?? region.top.stripEnd);
    region.top.anchor = Number(response.anchor_t ?? region.top.anchor);
    region.top.bright = [
        Number(response.bright_t_r ?? region.top.bright[0]),
        Number(response.bright_t_g ?? region.top.bright[1]),
        Number(response.bright_t_b ?? region.top.bright[2])
    ];
    region.bottom.width = Number(response.blend_bottom ?? region.bottom.width);
    region.bottom.enabled = !!response.blend_bottom_enabled;
    region.bottom.gamma = Number(response.edge_bottom_gamma ?? region.bottom.gamma);
    region.bottom.slope = Number(response.edge_bottom_slope ?? region.bottom.slope);
    region.bottom.stripStart = Number(response.strip_start_b ?? region.bottom.stripStart);
    region.bottom.stripEnd = Number(response.strip_end_b ?? region.bottom.stripEnd);
    region.bottom.anchor = Number(response.anchor_b ?? region.bottom.anchor);
    region.bottom.bright = [
        Number(response.bright_b_r ?? region.bottom.bright[0]),
        Number(response.bright_b_g ?? region.bottom.bright[1]),
        Number(response.bright_b_b ?? region.bottom.bright[2])
    ];
    return region;
}

export function hydrateBlendRegionsFromResponse(response) {
    if (typeof response?.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(response.blend_auto_edges);
    }
    const payload = response?.blend_auto_recalculated || response;
    if (typeof payload?.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(payload.blend_auto_edges);
    }
    const regions = Array.isArray(payload?.regions) ? payload.regions : [];
    let hydrated = false;
    regions.forEach((item) => {
        const regionId = Number(item?.region_id);
        if (!Number.isFinite(regionId) || regionId <= 0) return;
        hydrateBlendRegion(regionId, item);
        hydrated = true;
    });
    return hydrated;
}

export async function syncBlendRegion(regionId) {
    return enqueueBlendMutation(async () => {
        const region = normalizeBlendRegionToGeometry(regionId);
        const response = await saveBlend(regionId, region);
        if (response) {
            hydrateBlendRegion(regionId, response);
        }
        return response;
    });
}

export async function persistBlend() {
    return persistFusionConfig();
}

export async function loadBlendRegion(regionId) {
    const response = await loadBlend(regionId);
    hydrateBlendRegion(regionId, response);
    return response;
}

export async function resizeBlendGridByOp(regionId, op) {
    // I 融合管理模式下，后端 geometry_resize_grid 会走 Fusion管理器::resizeBlendGrid：
    // 只调整融合管理网格和融合带宽度，不改几何播放网格，也不覆盖手动边开关。
    return enqueueBlendMutation(async () => {
        const response = await resizeGeometryGridOp(regionId, op);
        hydrateBlendRegion(regionId, response);
        return response;
    });
}

export async function resizeBlendGridsByOp(regionIds, op) {
    return enqueueBlendMutation(async () => {
        const ids = normalizeRegionIds(regionIds);
        if (!ids.length) return null;
        const response = await resizeBlendGridBatchOp(ids, op);
        if (!hydrateBlendRegionsFromResponse(response) && ids.length === 1) {
            hydrateBlendRegion(ids[0], response);
        }
        return response;
    });
}

export async function syncBlendGuideCountFromGeometry(regionIds = null) {
    return enqueueBlendMutation(async () => {
        const state = getState();
        const ids = normalizeRegionIds(regionIds?.length
            ? regionIds
            : (state.layout.regionIds?.length ? state.layout.regionIds : [state.page.activeRegionId]));
        await Promise.all(ids.map(async (regionId) => {
            if (!state.geometry.byRegionId[regionId]?.loaded) {
                await loadGeometryRegion(regionId);
            }
            const geometry = state.geometry.byRegionId[regionId];
            const region = ensureBlendRegion(regionId);
            if (!region || !geometry) return;
            const rows = Math.max(2, Number(geometry.rows) || 2);
            const cols = Math.max(2, Number(geometry.cols) || 2);
            if (Number(region.gridRows) === rows && Number(region.gridCols) === cols) return;
            const response = await saveBlend(regionId, {
                blend_grid_rows: rows,
                blend_grid_cols: cols,
                guide_grid_only: true,
                sync_adjacent: false
            });
            if (response) {
                hydrateBlendRegion(regionId, response);
            } else {
                region.gridRows = rows;
                region.gridCols = cols;
            }
        }));
        return ids;
    });
}

export async function syncBlendManagerMode(traceId = '') {
    const response = await setManagerMode(getState().blend.managerMode, traceId);
    if (response && typeof response.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(response.blend_auto_edges);
    }
    hydrateBlendRegionsFromResponse(response);
    return response;
}

export async function syncBlendAutoEdges(enabled) {
    const response = await apiSetBlendAutoEdges(!!enabled);
    if (response && typeof response.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(response.blend_auto_edges);
    } else if (response && typeof response.enabled === 'boolean') {
        setBlendAutoEdges(response.enabled);
    }
    hydrateBlendRegionsFromResponse(response);
    return response;
}

export async function recalculateBlendRegions(regionIds = null) {
    return enqueueBlendMutation(async () => {
        const response = await autoRecalculateBlend();
        if (!hydrateBlendRegionsFromResponse(response)) {
            const ids = Array.isArray(regionIds) && regionIds.length
                ? regionIds
                : (getState().layout.regionIds?.length ? getState().layout.regionIds : [getState().page.activeRegionId]);
            await Promise.all(ids.filter((id) => Number(id) > 0).map((id) => loadBlendRegion(id)));
        }
    });
}

function readResetMasterEnabled(resetResponse) {
    if (!resetResponse || typeof resetResponse.fusion_master_enabled !== 'boolean') {
        throw new Error('融合模块初始化响应缺少 fusion_master_enabled');
    }
    return resetResponse.fusion_master_enabled;
}

async function refreshFusionStateAfterReset() {
    let refreshed = false;
    try {
        const layout = await loadGeometryLayout();
        refreshed = !!layout || refreshed;
    } catch (error) {
        console.warn('fusion layout refresh failed after reset:', error);
    }

    const ids = getState().layout.regionIds || [];
    const refreshTasks = ids.flatMap((id) => [
        loadGeometryRegion(id),
        loadBlendRegion(id)
    ]);
    refreshTasks.push(loadMask());

    const results = await Promise.allSettled(refreshTasks);
    results.forEach((result) => {
        if (result.status === 'fulfilled') {
            refreshed = !!result.value || refreshed;
        } else {
            console.warn('fusion state refresh failed after reset:', result.reason);
        }
    });
    return refreshed;
}

export async function resetFusionConfig() {
    const response = await apiResetFusionConfig();
    const resetApiError = getLastApiError();
    if (response == null) {
        throw new Error(resetApiError?.message || '融合模块初始化失败');
    }
    setBlendManagerMode(false);
    setBlendMasterEnabled(readResetMasterEnabled(response));
    if (typeof response.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(response.blend_auto_edges);
    }
    const stateRefreshed = await refreshFusionStateAfterReset();
    if (!stateRefreshed) {
        throw new Error('融合模块初始化后状态刷新失败');
    }
    return response;
}
