import {
    loadRegionConfig,
    loadGeometryState,
    loadBlend,
    saveGeometry,
    setGeometryGrid,
    setGeometryGuideVisibility,
    setAllGeometryGuideVisibility,
    setGeometryPoint,
    setGeometryPoints,
    setGeometrySelection,
    persistFusionConfig,
    resizeGeometryGridOp,
    moveGeometryOp,
    moveManagerPoint,
    moveManagerLine
} from '../api.js?v=2.95';
import { ensureBlendRegion, ensureGeometryRegion, getState, patchLayout } from '../actions.js?v=2.95';
import {
    canSyncGeometryRegion,
    getManagerLineSelection,
    hydrateGeometryRegion,
    resizeGeometryRegionByOp,
    snapManagerCornerSelection
} from '../geometry/actions.js?v=2.95';

const pendingGeometrySyncs = new Map();
let geometrySyncLoopRunning = false;
const pendingGeometryMoves = new Map();
let geometryMoveLoopRunning = false;
let geometryMoveFlushTimer = null;
const pendingManagerPointMoves = new Map();
let managerPointMoveLoopRunning = false;
let managerPointMoveFlushTimer = null;
const pendingManagerLineMoves = new Map();
let managerLineMoveLoopRunning = false;
let managerLineMoveFlushTimer = null;
const pendingGeometryPersists = new Set();
let geometryPersistTimer = null;
let geometryPersistLoopRunning = false;

function getRegionOutputIndex(region) {
    const outputIndex = Number(region.output_index);
    if (!Number.isInteger(outputIndex) || outputIndex < 0) {
        throw new Error(`Region ${region.id} missing output_index`);
    }
    return outputIndex;
}

function getRegionOutputCell(region) {
    const outputRow = Number(region.output_row);
    const outputCol = Number(region.output_col);
    if (!Number.isInteger(outputRow) || outputRow < 0 || !Number.isInteger(outputCol) || outputCol < 0) {
        throw new Error(`Region ${region.id} missing output_row/output_col`);
    }
    return { outputRow, outputCol };
}

function isDraggingGeometry(regionId) {
    const dragging = getState().interaction.dragging;
    return !!(dragging.active && dragging.regionId === regionId &&
        ['geometry-point', 'manager-point', 'manager-line'].includes(dragging.target));
}

async function flushGeometryMoveQueue() {
    if (geometryMoveLoopRunning) return;
    geometryMoveLoopRunning = true;
    try {
        while (pendingGeometryMoves.size > 0) {
            const moves = Array.from(pendingGeometryMoves.values());
            pendingGeometryMoves.clear();
            for (const move of moves) {
                const response = await moveGeometryOp(move.regionId, move.op, move.du, move.dv);
                if (response && typeof response === 'object' && Array.isArray(response.corners) && !isDraggingGeometry(move.regionId)) {
                    hydrateGeometryRegion(move.regionId, response);
                }
            }
        }
    } finally {
        geometryMoveLoopRunning = false;
        if (pendingGeometryMoves.size > 0) void flushGeometryMoveQueue();
    }
}

export async function flushPendingGeometryOps() {
    if (geometryMoveFlushTimer) {
        clearTimeout(geometryMoveFlushTimer);
        geometryMoveFlushTimer = null;
    }
    if (managerPointMoveFlushTimer) {
        clearTimeout(managerPointMoveFlushTimer);
        managerPointMoveFlushTimer = null;
    }
    if (managerLineMoveFlushTimer) {
        clearTimeout(managerLineMoveFlushTimer);
        managerLineMoveFlushTimer = null;
    }

    await flushGeometryMoveQueue();
    await flushManagerPointMoveQueue();
    await flushManagerLineMoveQueue();
    await flushGeometrySyncQueue();

    for (let i = 0; i < 60; i += 1) {
        if (
            pendingGeometryMoves.size === 0 &&
            pendingManagerPointMoves.size === 0 &&
            pendingManagerLineMoves.size === 0 &&
            pendingGeometrySyncs.size === 0 &&
            !geometryMoveLoopRunning &&
            !managerPointMoveLoopRunning &&
            !managerLineMoveLoopRunning &&
            !geometrySyncLoopRunning
        ) {
            return;
        }
        await new Promise((resolve) => setTimeout(resolve, 16));
    }
}

async function flushGeometryPersistQueue() {
    if (geometryPersistTimer) {
        clearTimeout(geometryPersistTimer);
        geometryPersistTimer = null;
    }
    if (geometryPersistLoopRunning) return;
    geometryPersistLoopRunning = true;
    try {
        await flushPendingGeometryOps();
        while (pendingGeometryPersists.size > 0) {
            const ids = Array.from(pendingGeometryPersists);
            pendingGeometryPersists.clear();
            for (const id of ids) {
                await persistGeometryRegion(id);
            }
        }
    } finally {
        geometryPersistLoopRunning = false;
        if (pendingGeometryPersists.size > 0) {
            scheduleGeometryPersist();
        }
    }
}

export function scheduleGeometryPersist(regionId = null, delayMs = 600) {
    if (Number.isFinite(Number(regionId)) && Number(regionId) > 0) {
        pendingGeometryPersists.add(Number(regionId));
    }
    if (geometryPersistTimer) clearTimeout(geometryPersistTimer);
    geometryPersistTimer = setTimeout(() => {
        geometryPersistTimer = null;
        void flushGeometryPersistQueue().catch((error) => {
            console.warn('fusion geometry autosave failed:', error);
        });
    }, Math.max(120, Number(delayMs) || 600));
}

async function flushManagerPointMoveQueue() {
    if (managerPointMoveLoopRunning) return;
    managerPointMoveLoopRunning = true;
    try {
        while (pendingManagerPointMoves.size > 0) {
            const moves = Array.from(pendingManagerPointMoves.values());
            pendingManagerPointMoves.clear();
            for (const move of moves) {
                const response = await moveManagerPoint(move.regionId, move.direction, move.du, move.dv, move.corner);
                if (response && typeof response === 'object' && Array.isArray(response.corners) && !isDraggingGeometry(move.regionId)) {
                    hydrateGeometryRegion(move.regionId, response);
                }
            }
        }
    } finally {
        managerPointMoveLoopRunning = false;
        if (pendingManagerPointMoves.size > 0) void flushManagerPointMoveQueue();
    }
}

async function flushManagerLineMoveQueue() {
    if (managerLineMoveLoopRunning) return;
    managerLineMoveLoopRunning = true;
    try {
        while (pendingManagerLineMoves.size > 0) {
            const moves = Array.from(pendingManagerLineMoves.values());
            pendingManagerLineMoves.clear();
            for (const move of moves) {
                const response = await moveManagerLine(move.regionId, move.direction, move.du, move.dv, move.selected);
                if (response && typeof response === 'object' && Array.isArray(response.corners) && !isDraggingGeometry(move.regionId)) {
                    hydrateGeometryRegion(move.regionId, response);
                }
            }
        }
    } finally {
        managerLineMoveLoopRunning = false;
        if (pendingManagerLineMoves.size > 0) void flushManagerLineMoveQueue();
    }
}

function mergePending(regionId, patch) {
    const current = pendingGeometrySyncs.get(regionId) || {
        visibility: undefined,
        selection: false,
        grid: null,
        point: null,
        region: false
    };

    if (Object.prototype.hasOwnProperty.call(patch, 'visibility')) {
        current.visibility = patch.visibility;
    }
    if (patch.selection) current.selection = true;
    if (patch.grid) current.grid = patch.grid;
    if (patch.point) current.point = patch.point;
    if (patch.region) current.region = true;

    pendingGeometrySyncs.set(regionId, current);
}

async function flushGeometrySyncQueue() {
    if (geometrySyncLoopRunning) return;
    geometrySyncLoopRunning = true;

    try {
        while (pendingGeometrySyncs.size > 0) {
            const entries = Array.from(pendingGeometrySyncs.entries());
            pendingGeometrySyncs.clear();

            for (const [regionId, pending] of entries) {
                if (typeof pending.visibility === 'boolean') {
                    await setGeometryGuideVisibility(regionId, pending.visibility);
                }

                if (pending.selection) {
                    const region = ensureGeometryRegion(regionId);
                    await setGeometrySelection(regionId, region.selected.row, region.selected.col);
                }

                if (pending.grid) {
                    await setGeometryGrid(regionId, pending.grid.rows, pending.grid.cols, pending.grid.interpolationMode);
                }

                if (pending.point) {
                    await setGeometryPoint(regionId, pending.point.row, pending.point.col, pending.point.u, pending.point.v);
                }

                if (pending.region) {
                    const region = ensureGeometryRegion(regionId);
                    if (!canSyncGeometryRegion(regionId)) {
                        console.warn(`fusion geometry sync skipped: region ${regionId} is not fully hydrated`);
                    }
                    // region 同步 handled by individual operations (grid, point, etc.)
                }
            }
        }
    } finally {
        geometrySyncLoopRunning = false;
        if (pendingGeometrySyncs.size > 0) {
            void flushGeometrySyncQueue();
        }
    }
}

function enqueueGeometrySync(regionId, patch) {
    mergePending(regionId, patch);
    return flushGeometrySyncQueue();
}

export async function syncGeometryRegion(regionId) {
    return enqueueGeometrySync(regionId, { region: true });
}

export async function persistGeometryRegion(regionId) {
    return saveGeometry(regionId);
}

export async function loadGeometryLayout() {
    const response = await loadRegionConfig();
    if (response && response.regions) {
        const adjacentByRegionId = {};
        response.regions.forEach((region) => {
            if (!region.input_adjacent_region || typeof region.input_adjacent_region !== 'object') {
                throw new Error(`Region ${region.id} missing input_adjacent_region`);
            }
            adjacentByRegionId[region.id] = region.input_adjacent_region;
        });
        // 输出矩阵统一按行×列(rows×cols)解释，12×1 表示 12 行 1 列。
        // 这里先拆出 outRows/outCols，避免水合后把行列含义互换。
        const outRows = Number(response.grid_out_rows) || 1;
        const outCols = Number(response.grid_out_cols) || 1;
        const orderedRegions = response.regions
            .map((region, index) => ({ region, index, outputIndex: getRegionOutputIndex(region) }))
            .sort((a, b) => (a.outputIndex - b.outputIndex) || (a.index - b.index))
            .map((item) => item.region);
        response.regions.forEach((region) => {
            const { outputRow, outputCol } = getRegionOutputCell(region);
            const geometry = ensureGeometryRegion(region.id);
            geometry.srcX = Number(region.srcX);
            geometry.srcY = Number(region.srcY);
            geometry.srcWidth = Number(region.srcWidth);
            geometry.srcHeight = Number(region.srcHeight);
            geometry.outX = Number(region.outX);
            geometry.outY = Number(region.outY);
            geometry.outWidth = Number(region.outWidth);
            geometry.outHeight = Number(region.outHeight);
            geometry.outputRow = outputRow;
            geometry.outputCol = outputCol;
        });
        const inputRows = Number(response.grid_in_rows) || 1;
        const inputCols = Number(response.grid_in_cols) || response.regions.length || 1;
        const firstSourceRegion = response.regions.find((region) => Number(region.srcWidth) > 0 && Number(region.srcHeight) > 0) || {};
        const tileInWidth = Number(response.tile_in_width) || Number(firstSourceRegion.srcWidth) || 0;
        const tileInHeight = Number(response.tile_in_height) || Number(firstSourceRegion.srcHeight) || 0;
        // 遮罩/输入幕布预览必须是输入合成总幕布。后端有 input_total_* 就用它；
        // 旧字段缺失时用输入单格 * 输入行列推导，避免误用输出矩阵比例。
        const inputTotalWidth = Number(response.input_total_width) || (tileInWidth > 0 ? tileInWidth * inputCols : Number(response.canvas_in_width) || 0);
        const inputTotalHeight = Number(response.input_total_height) || (tileInHeight > 0 ? tileInHeight * inputRows : Number(response.canvas_in_height) || 0);
        patchLayout({
            regionIds: orderedRegions.map((region) => region.id),
            regionCount: response.regions.length,
            rows: outRows,
            cols: outCols,
            inputRows,
            inputCols,
            inputTotalWidth,
            inputTotalHeight,
            canvasWidth: inputTotalWidth,
            canvasHeight: inputTotalHeight,
            outputWidth: Number(response.canvas_out_width) || 0,
            outputHeight: Number(response.canvas_out_height) || 0,
            tileInWidth,
            tileInHeight,
            tileOutWidth: Number(response.tile_out_width) || 0,
            tileOutHeight: Number(response.tile_out_height) || 0,
            merge360: !!response.merge_360,
            mirrorMode: Math.max(0, Math.min(6, Number(response.mirror_mode) || 0)),
            adjacentByRegionId
        });
        if (typeof response.blend_auto_edges === 'boolean') {
            getState().blend.autoEdges = response.blend_auto_edges;
        }
    }
    return response;
}

export async function loadGeometryRegion(regionId) {
    const response = await loadGeometryState(regionId);
    if (response) {
        hydrateGeometryRegion(regionId, response);
    }
    return response;
}

export async function syncGeometryGuideVisibility(activeRegionId, showGuide) {
    return enqueueGeometrySync(activeRegionId, { visibility: !!showGuide });
}

export async function syncGeometryGuideVisibilityForRegions(regionIds, showGuide) {
    const ids = Array.isArray(regionIds) && regionIds.length ? regionIds : [];
    return Promise.all(ids.map((id) => enqueueGeometrySync(id, { visibility: !!showGuide })));
}

export async function syncAllGeometryGuideVisibility(showGuide) {
    return setAllGeometryGuideVisibility(!!showGuide);
}

export async function syncActiveGeometrySelection(regionId) {
    const state = getState();
    const region = state.page.activeTab === 'blend' && state.blend.managerMode
        ? snapManagerCornerSelection(regionId)
        : ensureGeometryRegion(regionId);
    await setGeometrySelection(regionId, region.selected.row, region.selected.col);
}

export async function syncGeometryGrid(regionId) {
    const region = ensureGeometryRegion(regionId);
    return enqueueGeometrySync(regionId, {
        grid: {
            rows: region.rows,
            cols: region.cols,
            interpolationMode: region.interpolationMode
        }
    });
}

function normalizeRegionIds(regionIds) {
    const source = Array.isArray(regionIds) ? regionIds : [regionIds];
    return Array.from(new Set(source
        .map((id) => Number(id))
        .filter((id) => Number.isFinite(id) && id > 0)));
}

async function ensureBlendGuideCountLoaded(regionId) {
    const region = ensureBlendRegion(regionId);
    const response = await loadBlend(regionId);
    if (response) {
        region.gridRows = Math.max(2, Number(response.blend_grid_rows) || Number(region.gridRows) || 2);
        region.gridCols = Math.max(2, Number(response.blend_grid_cols) || Number(region.gridCols) || 2);
    }
    return region;
}

export async function syncGeometryGridCountFromBlend(regionIds = null) {
    const state = getState();
    const ids = normalizeRegionIds(regionIds?.length
        ? regionIds
        : (state.layout.regionIds?.length ? state.layout.regionIds : [state.page.activeRegionId]));
    await Promise.all(ids.map(async (regionId) => {
        if (!ensureGeometryRegion(regionId).loaded) {
            await loadGeometryRegion(regionId);
        }
        const geometry = ensureGeometryRegion(regionId);
        const blend = await ensureBlendGuideCountLoaded(regionId);
        const rows = Math.max(2, Number(blend.gridRows) || Number(geometry.rows) || 2);
        const cols = Math.max(2, Number(blend.gridCols) || Number(geometry.cols) || 2);
        if (Number(geometry.rows) === rows && Number(geometry.cols) === cols) return;
        const response = await setGeometryGrid(regionId, rows, cols, geometry.interpolationMode, {
            syncBlendGrid: false
        });
        if (response) hydrateGeometryRegion(regionId, response);
    }));
    return ids;
}

export async function resizeGeometryGridByOp(regionId, op) {
    const response = await resizeGeometryGridOp(regionId, op);
    if (response) hydrateGeometryRegion(regionId, response);
}

export async function syncGeometryMoveOp(regionId, op, du, dv) {
    const key = `${regionId}:${op}`;
    const current = pendingGeometryMoves.get(key) || { regionId, op, du: 0, dv: 0 };
    current.du += du;
    current.dv += dv;
    pendingGeometryMoves.set(key, current);
    if (!geometryMoveFlushTimer) {
        geometryMoveFlushTimer = setTimeout(() => {
            geometryMoveFlushTimer = null;
            void flushGeometryMoveQueue();
        }, 16);
    }
    scheduleGeometryPersist(regionId);
}

export async function syncManagerPointMoveOp(regionId, direction, du, dv, corner = null) {
    const key = `${regionId}:${direction}:${corner || ''}`;
    const current = pendingManagerPointMoves.get(key) || { regionId, direction, du: 0, dv: 0, corner };
    current.du += du;
    current.dv += dv;
    pendingManagerPointMoves.set(key, current);
    if (!managerPointMoveFlushTimer) {
        managerPointMoveFlushTimer = setTimeout(() => {
            managerPointMoveFlushTimer = null;
            void flushManagerPointMoveQueue();
        }, 16);
    }
    scheduleGeometryPersist(regionId);
}

export async function syncManagerLineMoveOp(regionId, direction, du, dv) {
    const region = getState().geometry.byRegionId[regionId];
    const selected = region ? getManagerLineSelection(region) : null;
    const key = `${regionId}:${direction}:${selected?.row ?? ''}:${selected?.col ?? ''}`;
    const current = pendingManagerLineMoves.get(key) || { regionId, direction, du: 0, dv: 0, selected };
    current.du += du;
    current.dv += dv;
    current.selected = selected;
    pendingManagerLineMoves.set(key, current);
    if (!managerLineMoveFlushTimer) {
        managerLineMoveFlushTimer = setTimeout(() => {
            managerLineMoveFlushTimer = null;
            void flushManagerLineMoveQueue();
        }, 16);
    }
    scheduleGeometryPersist(regionId);
}

export async function syncGeometryPoint(regionId) {
    const region = ensureGeometryRegion(regionId);
    const index = region.selected.row * region.cols + region.selected.col;
    const point = region.points[index];
    if (!point) return Promise.resolve();
    return enqueueGeometrySync(regionId, {
        point: {
            row: region.selected.row,
            col: region.selected.col,
            u: point.u,
            v: point.v
        }
    });
}

export async function syncGeometryPoints(regionId) {
    const region = ensureGeometryRegion(regionId);
    const points = [];
    for (let row = 0; row < region.rows; row++) {
        for (let col = 0; col < region.cols; col++) {
            const index = row * region.cols + col;
            const point = region.points[index];
            if (point) {
                points.push([row, col, point.u, point.v]);
            }
        }
    }
    if (points.length === 0) return Promise.resolve();
    return setGeometryPoints(regionId, points, region.rows, region.cols, region.interpolationMode);
}
