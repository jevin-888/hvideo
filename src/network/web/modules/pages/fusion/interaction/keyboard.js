import { ensureBlendRegion, setActiveRegion, getState } from '../actions.js?v=2.95';
import { clamp } from '../utils/grid.js';
import { getActiveGeometry, getActiveMask, getActiveBlend, getNextRegionId } from '../selectors.js?v=2.95';
import { switchFusionTab } from '../modeSwitch.js?v=2.100';
import { createDefaultBlendRegionState, createDefaultGeometryRegion } from '../state.js?v=2.95';
import {
    applyGeometryMoveLocal,
    applyManagerMovePointLocal,
    applyManagerMoveLineLocal,
    moveGeometrySelection,
    getManagerCornerName,
    moveManagerCornerSelection,
    snapManagerCornerSelection,
    setGeometryGridVisibleAllRegions,
    setInterpolationMode,
    resizeGeometryRegionTo
} from '../geometry/actions.js?v=2.95';
import { renderFusionUi } from '../ui/panel.js?v=2.100';
import { showNotification } from '../../../components/toast.js';
import { scheduleFullMaskSync } from '../ui/bindings.js?v=2.100';
import { pushGeometryUndo, pushMaskUndo, pushBlendUndo, restoreGeometryUndo, restoreMaskUndo, restoreBlendUndo } from '../undo.js';
import {
    syncGeometryGrid,
    syncAllGeometryGuideVisibility,
    syncGeometryGuideVisibilityForRegions,
    syncGeometryRegion,
    syncActiveGeometrySelection,
    persistGeometryRegion,
    resizeGeometryGridByOp,
    syncGeometryMoveOp,
    syncManagerPointMoveOp,
    syncManagerLineMoveOp,
    syncGeometryPoints,
    flushPendingGeometryOps
} from '../sync/syncGeometry.js?v=2.95';
import {
    moveMaskSelection,
    applyMaskMoveLocal,
    setMaskEnabled,
    setMaskInterpolationMode,
    setMaskGridVisible
} from '../mask/actions.js?v=2.95';
import { syncMask, persistMask, syncMaskGuideVisibility, resizeMaskGridByOp, syncMaskMoveOp } from '../sync/syncMask.js?v=2.95';
import { hydrateBlendRegionsFromResponse, loadBlendRegion, resizeBlendGridsByOp, syncBlendManagerMode, syncBlendRegion, persistBlend, resetFusionConfig } from '../sync/syncBlend.js?v=2.95';
import { setActiveBlendCorner, setActiveBlendSide, setBlendAutoEdges, setBlendManagerMode, setBlendMasterEnabled } from '../blend/actions.js?v=2.95';
import { persistFusionConfig, setGridVisualStyle, setMergeGapBrightness, setMaskEnabledState, setBlendMaster } from '../api.js?v=2.95';
import { applyGridVisualStyle, getGridVisualStyle } from '../canvas/visualStyle.js?v=2.81';

// ─── 状态 ───────────────────────────────────────────────────────────────────────

let lastGeometryUndoAt = 0;
let geometryResizePending = false;
let fusionModeToggleBusy = false;
let lastFusionModeToggleAt = 0;
const FUSION_MODE_TOGGLE_COOLDOWN_MS = 700;
const NORMAL_NUDGE_PX = 2;
const FAST_NUDGE_PX = 5;

// ─── 工具函数 ───────────────────────────────────────────────────────────────────

function traceNowMs() {
    return (window.performance && typeof window.performance.now === 'function')
        ? window.performance.now()
        : Date.now();
}

function createFusionTraceId(prefix = 'fusion') {
    return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
}

function logFusionTrace(traceId, stage, details = {}) {
    if (!traceId) return;
    console.info(`[FusionICloseTrace] trace=${traceId} stage=${stage}`, details);
}

function warnFusionTrace(traceId, stage, details = {}) {
    if (!traceId) return;
    console.warn(`[FusionICloseTrace] trace=${traceId} stage=${stage}`, details);
}

async function traceAsyncStage(traceId, stage, task) {
    const start = traceNowMs();
    logFusionTrace(traceId, `${stage}.begin`);
    try {
        const result = await task();
        logFusionTrace(traceId, `${stage}.end`, { cost_ms: Math.round(traceNowMs() - start) });
        return result;
    } catch (error) {
        warnFusionTrace(traceId, `${stage}.error`, {
            cost_ms: Math.round(traceNowMs() - start),
            message: error && error.message ? error.message : String(error)
        });
        throw error;
    }
}

function isTypingTarget(target) {
    if (!target) return false;
    const tag = target.tagName ? target.tagName.toLowerCase() : '';
    if (target.isContentEditable) return true;
    if (tag === 'textarea' || tag === 'select') return true;
    if (tag !== 'input') return false;
    const type = String(target.getAttribute('type') || 'text').toLowerCase();
    return !['button', 'checkbox', 'color', 'file', 'radio', 'range', 'reset', 'submit'].includes(type);
}

function isFusionShortcutKey(event, key) {
    if (isDirectionalEditKey(event, key)) return true;
    if (key === '-' || key === '_' || key === '=' || key === '+') return true;
    if (event.ctrlKey && !event.shiftKey && !event.altKey && ['s', 'z', 'g', 'i', 'm'].includes(key)) return true;
    if (event.ctrlKey && event.shiftKey && !event.altKey && key === 'z') return true;
    if (event.altKey && !event.ctrlKey && !event.shiftKey && key === 'w') return true;
    if (!event.ctrlKey && !event.shiftKey && !event.altKey &&
        ['g', 'm', 'i', 'n', 'h', 'p', 'a', 'd', 'w', 's', 'k'].includes(key)) {
        return true;
    }
    return false;
}

function setDebugBackgroundActive(active, traceId = '') {
    const controller = window.FusionBackground;
    if (!controller || typeof controller.setDebugModeActive !== 'function') return Promise.resolve(false);
    return controller.setDebugModeActive(active, traceId);
}

function hideDebugBackground() {
    const controller = window.FusionBackground;
    if (!controller) return Promise.resolve(false);
    if (typeof controller.hideBackground === 'function') {
        return controller.hideBackground();
    }
    if (typeof controller.setDebugModeActive === 'function') {
        return controller.setDebugModeActive(false);
    }
    return Promise.resolve(false);
}

function normalizeDirectionKey(key) {
    if (key === 'arrowup' || key === 'w') return 'arrowup';
    if (key === 'arrowdown' || key === 's') return 'arrowdown';
    if (key === 'arrowleft' || key === 'a') return 'arrowleft';
    if (key === 'arrowright' || key === 'd') return 'arrowright';
    return '';
}

function isDirectionalEditKey(event, key) {
    if (['arrowup', 'arrowdown', 'arrowleft', 'arrowright'].includes(key)) return true;
    return !!(event.ctrlKey && event.altKey && ['w', 'a', 's', 'd'].includes(key));
}

function pushGeometryUndoThrottled(regionId) {
    const now = Date.now();
    if (now - lastGeometryUndoAt < 250) return;
    lastGeometryUndoAt = now;
    pushGeometryUndo(regionId, getActiveGeometry());
}

function moveGeometryFast(regionId, op, du, dv) {
    applyGeometryMoveLocal(regionId, op, du, dv);
    renderFusionUi();
    void syncGeometryMoveOp(regionId, op, du, dv);
}

function moveMaskFast(op, du, dv) {
    applyMaskMoveLocal(op, du, dv);
    renderFusionUi();
    void syncMaskMoveOp(op, du, dv);
}

function getMaskLineMove(directionKey, du, dv) {
    const mask = getActiveMask();
    const onVerticalEdge = mask.selected.col === 0 || mask.selected.col === mask.cols - 1;
    const onHorizontalEdge = mask.selected.row === 0 || mask.selected.row === mask.rows - 1;
    if ((directionKey === 'arrowleft' || directionKey === 'arrowright') && onVerticalEdge) {
        return { op: 'col', du, dv: 0 };
    }
    if ((directionKey === 'arrowup' || directionKey === 'arrowdown') && onHorizontalEdge) {
        return { op: 'row', du: 0, dv };
    }
    return null;
}

function canEditActiveRegion(state, active) {
    if (state.page.activeTab === 'blend') return !!state.blend.managerMode;
    if (!active) return false;
    if (state.page.activeTab === 'mask') return !!active.showGrid;
    if (state.page.activeTab === 'geometry') return !!active.showGrid;
    return false;
}

function getActiveRegionForMode(state) {
    if (state.page.activeTab === 'geometry') return getActiveGeometry();
    if (state.page.activeTab === 'mask') return getActiveMask();
    if (state.page.activeTab === 'blend') return getActiveGeometry();
    return null;
}

function directionFromArrowKey(key) {
    const directionKey = normalizeDirectionKey(key);
    if (directionKey === 'arrowup') return 0;
    if (directionKey === 'arrowdown') return 1;
    if (directionKey === 'arrowleft') return 2;
    if (directionKey === 'arrowright') return 3;
    return -1;
}

function managerSideFromDirection(region, direction) {
    if (!region) return getState().blend.activeSide;
    if (direction === 2 || direction === 3) {
        return (region.selected?.col ?? 0) < region.cols / 2 ? 'left' : 'right';
    }
    if (direction === 0 || direction === 1) {
        return (region.selected?.row ?? 0) < region.rows / 2 ? 'top' : 'bottom';
    }
    return getState().blend.activeSide;
}

function getTargetRegionIds(regionId) {
    const ids = getState().layout.regionIds;
    const candidates = Array.isArray(ids) && ids.length ? ids : [regionId];
    return candidates.filter((id) => Number(id) > 0);
}

function firstPositiveNumber(...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number) && number > 0) return number;
    }
    return 1;
}

function getOutputNudgeSize() {
    const layout = getState().layout;
    const rows = Math.max(1, Number(layout.rows) || 1);
    const cols = Math.max(1, Number(layout.cols) || Math.max(1, layout.regionIds?.length || 1));
    const outputWidth = Number(layout.outputWidth) || 0;
    const outputHeight = Number(layout.outputHeight) || 0;
    return {
        width: firstPositiveNumber(
            layout.tileOutWidth,
            outputWidth > 0 ? outputWidth / cols : 0,
            outputWidth
        ),
        height: firstPositiveNumber(
            layout.tileOutHeight,
            outputHeight > 0 ? outputHeight / rows : 0,
            outputHeight
        )
    };
}

function getInputNudgeSize() {
    const layout = getState().layout;
    const rows = Math.max(1, Number(layout.inputRows) || 1);
    const cols = Math.max(1, Number(layout.inputCols) || 1);
    const tileW = Number(layout.tileInWidth) || 0;
    const tileH = Number(layout.tileInHeight) || 0;
    return {
        width: firstPositiveNumber(
            layout.inputTotalWidth,
            tileW > 0 ? tileW * cols : 0,
            layout.canvasWidth,
            tileW
        ),
        height: firstPositiveNumber(
            layout.inputTotalHeight,
            tileH > 0 ? tileH * rows : 0,
            layout.canvasHeight,
            tileH
        )
    };
}

function getNudgeDelta(directionKey, stepPx = NORMAL_NUDGE_PX) {
    const directionMap = {
        arrowup: [-1, 0, 0, -1],
        arrowdown: [1, 0, 0, 1],
        arrowleft: [0, -1, -1, 0],
        arrowright: [0, 1, 1, 0]
    };
    const [dRow = 0, dCol = 0, sx = 0, sy = 0] = directionMap[directionKey] || [];
    const outputSize = getOutputNudgeSize();
    const inputSize = getInputNudgeSize();
    const pixels = Math.max(0, Number(stepPx) || 0);
    return {
        dRow,
        dCol,
        geometryDU: sx * (2 * pixels / Math.max(1, outputSize.width)),
        geometryDV: -sy * (2 * pixels / Math.max(1, outputSize.height)),
        maskDU: sx * (pixels / Math.max(1, inputSize.width)),
        maskDV: sy * (pixels / Math.max(1, inputSize.height))
    };
}

// ─── 模式切换 ───────────────────────────────────────────────────────────────────

async function openGeometryMode() {
    await switchFusionTab('geometry');
    const regionId = getState().page.activeRegionId;
    const ids = getState().layout.regionIds?.length ? getState().layout.regionIds : [regionId];
    setGeometryGridVisibleAllRegions(ids, true);
    renderFusionUi();
    await syncGeometryGuideVisibilityForRegions(ids, true);
    await syncActiveGeometrySelection(regionId);
}

async function openMaskMode() {
    setMaskEnabled(true);
    setMaskGridVisible(true);
    await switchFusionTab('mask');
    void setMaskEnabledState(true).catch((error) => {
        console.warn('fusion mask enabled sync failed:', error);
    });
    renderFusionUi();
}

async function openBlendMode(traceId = '') {
    await switchFusionTab('blend', traceId);
    renderFusionUi();
    logFusionTrace(traceId, 'openBlendMode.end', {
        tab: getState().page.activeTab,
        managerMode: !!getState().blend.managerMode
    });
}

async function closeBlendMode(regionId, traceId = '') {
    const ids = getState().layout.regionIds?.length ? getState().layout.regionIds : [regionId];
    const startedAt = traceNowMs();
    logFusionTrace(traceId, 'closeBlendMode.begin', {
        regionId,
        regionIds: ids,
        tab: getState().page.activeTab,
        managerMode: !!getState().blend.managerMode
    });
    setBlendManagerMode(false);
    renderFusionUi();
    await traceAsyncStage(traceId, 'debugBackgroundOff', () => setDebugBackgroundActive(false, traceId));
    await traceAsyncStage(traceId, 'syncManagerModeOff', () => syncBlendManagerMode(traceId));
    renderFusionUi();
    logFusionTrace(traceId, 'closeBlendMode.end', {
        cost_ms: Math.round(traceNowMs() - startedAt),
        managerMode: !!getState().blend.managerMode
    });
}

async function closeAllFusionEditModes(regionId, except = '') {
    const state = getState();
    if (except !== 'geometry') {
        const ids = Array.from(new Set([
            ...(state.layout.regionIds?.length ? state.layout.regionIds : [regionId]),
            ...Object.keys(state.geometry.byRegionId || {}).map((id) => Number(id)).filter((id) => Number.isFinite(id) && id > 0)
        ]));
        setGeometryGridVisibleAllRegions(ids, false);
        await syncAllGeometryGuideVisibility(false);
    }
    if (except !== 'mask') {
        setMaskGridVisible(false);
        await syncMaskGuideVisibility(false);
    }
    if (except !== 'blend' && state.blend.managerMode) {
        setBlendManagerMode(false);
        await syncBlendManagerMode();
    }
}

async function closeAllFusionEditModesAfterReset(regionId) {
    const state = getState();
    const ids = Array.from(new Set([
        ...(state.layout.regionIds?.length ? state.layout.regionIds : [regionId]),
        ...Object.keys(state.geometry.byRegionId || {}).map((id) => Number(id)).filter((id) => Number.isFinite(id) && id > 0)
    ]));
    setGeometryGridVisibleAllRegions(ids, false);
    setMaskGridVisible(false);
    setBlendManagerMode(false);
    renderFusionUi();
    await Promise.all([
        syncAllGeometryGuideVisibility(false).catch((error) => console.warn('fusion geometry guide close failed after reset:', error)),
        syncMaskGuideVisibility(false).catch((error) => console.warn('fusion mask guide close failed after reset:', error)),
        syncBlendManagerMode().catch((error) => console.warn('fusion blend manager close failed after reset:', error))
    ]);
    await hideDebugBackground();
}

// ─── 还原 / 初始化 ─────────────────────────────────────────────────────────────

function applyGeometryUndoToAllRegions(sourceRegionId) {
    const state = getState();
    const source = state.geometry.byRegionId[sourceRegionId];
    if (!source) return;
    const ids = state.layout.regionIds?.length ? state.layout.regionIds : [sourceRegionId];
    ids.forEach((id) => {
        const region = state.geometry.byRegionId[id];
        if (!region || id === sourceRegionId) return;
        resizeGeometryRegionTo(id, source.rows, source.cols);
        region.interpolationMode = source.interpolationMode;
        region.showGrid = true;
        region.selected = {
            row: Math.min(source.selected?.row ?? 0, source.rows - 1),
            col: Math.min(source.selected?.col ?? 0, source.cols - 1),
            axis: source.selected?.axis || region.selected?.axis || 'col'
        };
    });
}

function restoreBlendUndoForRegions(regionIds) {
    const restoredIds = [];
    regionIds.forEach((id) => {
        if (restoreBlendUndo(id, ensureBlendRegion(id))) {
            restoredIds.push(id);
        }
    });
    return restoredIds;
}

function getDefaultResetBlendWidth(regionId, side) {
    const geometry = getState().geometry.byRegionId[regionId];
    const blend = getState().blend.byRegionId[regionId];
    const cols = Math.max(2, Number(blend?.gridCols ?? geometry?.cols ?? 2));
    const rows = Math.max(2, Number(blend?.gridRows ?? geometry?.rows ?? 2));
    if (side === 'left' || side === 'right') return cols > 2 ? 1 / (cols - 1) : 0;
    return rows > 2 ? 1 / (rows - 1) : 0;
}

function createResetBlendRegion(regionId) {
    const next = createDefaultBlendRegionState();
    const geometry = getState().geometry.byRegionId[regionId];
    const blend = getState().blend.byRegionId[regionId];
    // 融合管理网格独立于几何播放网格；重置融合时也必须优先保留融合网格。
    next.gridRows = Math.max(2, Number(blend?.gridRows ?? geometry?.rows ?? next.gridRows));
    next.gridCols = Math.max(2, Number(blend?.gridCols ?? geometry?.cols ?? next.gridCols));
    ['left', 'right', 'top', 'bottom'].forEach((side) => {
        next[side].enabled = !!blend?.[side]?.enabled;
        next[side].width = getDefaultResetBlendWidth(regionId, side);
    });
    return next;
}

function replaceObject(target, source) {
    Object.keys(target).forEach((key) => delete target[key]);
    Object.assign(target, JSON.parse(JSON.stringify(source)));
}

const GEOMETRY_LAYOUT_KEYS = [
    'srcX',
    'srcY',
    'srcWidth',
    'srcHeight',
    'outX',
    'outY',
    'outWidth',
    'outHeight',
    'outputRow',
    'outputCol'
];

function copyGeometryLayoutFields(target, source) {
    GEOMETRY_LAYOUT_KEYS.forEach((key) => {
        if (Object.prototype.hasOwnProperty.call(source, key)) {
            target[key] = source[key];
        }
    });
}

async function resetGeometryOnly(regionId) {
    const ids = getTargetRegionIds(regionId);
    const showGrid = getState().page.activeTab === 'geometry' || getState().page.activeTab === 'blend';
    ids.forEach((id) => {
        const current = getState().geometry.byRegionId[id] || {};
        const next = createDefaultGeometryRegion();
        next.loaded = true;
        next.showGrid = showGrid;
        copyGeometryLayoutFields(next, current);
        replaceObject(current, next);
        getState().geometry.byRegionId[id] = current;
    });
    renderFusionUi();
    await syncGeometryGuideVisibilityForRegions(ids, showGrid);
    for (const id of ids) {
        await syncGeometryGrid(id);
        await syncGeometryPoints(id);
        await persistGeometryRegion(id);
    }
    await syncActiveGeometrySelection(regionId);
}

async function resetBlendBrightness(regionId) {
    const sides = ['left', 'right', 'top', 'bottom'];
    await Promise.all(sides.flatMap((side) => [0, 1, 2].map((colorId) => setMergeGapBrightness(regionId, side, colorId, 128))));
}

async function resetBlendOnly(regionId) {
    const ids = getTargetRegionIds(regionId);
    ids.forEach((id) => {
        const current = getState().blend.byRegionId[id] || {};
        replaceObject(current, createResetBlendRegion(id));
        getState().blend.byRegionId[id] = current;
    });
    renderFusionUi();
    for (const id of ids) {
        await syncBlendRegion(id);
        await resetBlendBrightness(id);
        await persistBlend(id);
        await loadBlendRegion(id);
    }
}

async function resetMaskOnly() {
    const mask = getActiveMask();
    const keepEnabled = getState().page.activeTab === 'mask';
    const next = {
        enabled: keepEnabled,
        rows: 2,
        cols: 2,
        showGrid: keepEnabled,
        selected: { row: 0, col: 0, axis: mask.selected?.axis || 'col' },
        points: [
            { u: 0, v: 0 },
            { u: 1, v: 0 },
            { u: 0, v: 1 },
            { u: 1, v: 1 }
        ],
        interpolationMode: 0
    };
    replaceObject(mask, next);
    renderFusionUi();
    await syncMask();
    if (!keepEnabled) {
        mask.enabled = false;
        await setMaskEnabledState(false);
    }
    await persistMask();
}

// ─── Ctrl+G / Ctrl+I / Ctrl+M 还原 ────────────────────────────────────────────

function handleRestoreShortcut(event, key, regionId) {
    if (!event.ctrlKey || event.shiftKey || event.altKey || !['g', 'i', 'm'].includes(key)) return false;
    event.preventDefault();
    const handlers = {
        g: { label: '几何', run: () => resetGeometryOnly(regionId) },
        i: { label: '融合', run: () => resetBlendOnly(regionId) },
        m: { label: '遮罩', run: () => resetMaskOnly() }
    };
    const item = handlers[key];
    void item.run().then(() => {
        return hideDebugBackground();
    }).then(() => {
        renderFusionUi();
        showNotification(`${item.label}已还原`, 'success');
    }).catch((error) => {
        console.warn(`fusion ${item.label} restore failed:`, error);
        showNotification(`${item.label}还原失败`, 'error');
    });
    return true;
}

// ─── 方向键统一处理 ─────────────────────────────────────────────────────────────

function handleArrowKeys(state, event, key, regionId) {
    const directionKey = normalizeDirectionKey(key);
    if (!directionKey) return false;

    const isGeometry = state.page.activeTab === 'geometry';
    const isMask = state.page.activeTab === 'mask';
    const isBlend = state.page.activeTab === 'blend';

    const {
        dRow,
        dCol,
        geometryDU,
        geometryDV,
        maskDU,
        maskDV
    } = getNudgeDelta(directionKey, NORMAL_NUDGE_PX);

    const ctrl = event.ctrlKey;
    const shift = event.shiftKey;
    const alt = event.altKey;

    // 裸方向键：选择热点，对齐旧项目 HOT_POINT_CONTROL。
    if (!ctrl && !shift && !alt) {
        if (isGeometry) {
            moveGeometrySelection(regionId, dRow, dCol);
            renderFusionUi();
            void syncActiveGeometrySelection(regionId);
        } else if (isMask) {
            moveMaskSelection(dRow, dCol);
            renderFusionUi();
            scheduleFullMaskSync();
        } else if (isBlend) {
            moveManagerCornerSelection(regionId, dRow, dCol);
            const geometry = getActiveGeometry();
            const side = dCol < 0 ? 'left'
                : dCol > 0 ? 'right'
                    : dRow < 0 ? 'top'
                        : dRow > 0 ? 'bottom'
                            : getState().blend.activeSide;
            setActiveBlendSide(side);
            setActiveBlendCorner(getManagerCornerName(geometry));
            renderFusionUi();
            void syncActiveGeometrySelection(regionId);
        }
        return true;
    }

    // Ctrl+方向：移动当前热点。
    if (ctrl && !shift && !alt) {
        if (isGeometry) {
            pushGeometryUndoThrottled(regionId);
            moveGeometryFast(regionId, 'point', geometryDU, geometryDV);
        } else if (isMask) {
            pushMaskUndo(getActiveMask());
            moveMaskFast('point', maskDU, maskDV);
        } else if (isBlend) {
            const direction = directionFromArrowKey(key);
            if (direction < 0) return true;
            snapManagerCornerSelection(regionId);
            const corner = getManagerCornerName(getActiveGeometry());
            setActiveBlendCorner(corner);
            pushGeometryUndoThrottled(regionId);
            applyManagerMovePointLocal(regionId, direction, geometryDU, geometryDV, corner);
            renderFusionUi();
            void syncManagerPointMoveOp(regionId, direction, geometryDU, geometryDV, corner);
        }
        return true;
    }

    // Ctrl+Alt+方向：快调点 5px；融合模式下对应旧项目 MOVE_MG_POINT。
    if (ctrl && !shift && alt) {
        const fastDelta = getNudgeDelta(directionKey, FAST_NUDGE_PX);
        if (isGeometry) {
            pushGeometryUndoThrottled(regionId);
            moveGeometryFast(regionId, 'point', fastDelta.geometryDU, fastDelta.geometryDV);
        } else if (isMask) {
            pushMaskUndo(getActiveMask());
            moveMaskFast('point', fastDelta.maskDU, fastDelta.maskDV);
        } else if (isBlend) {
            const direction = directionFromArrowKey(directionKey);
            if (direction < 0) return true;
            snapManagerCornerSelection(regionId);
            const corner = getManagerCornerName(getActiveGeometry());
            setActiveBlendCorner(corner);
            pushGeometryUndoThrottled(regionId);
            applyManagerMovePointLocal(regionId, direction, fastDelta.geometryDU, fastDelta.geometryDV, corner);
            renderFusionUi();
            void syncManagerPointMoveOp(regionId, direction, fastDelta.geometryDU, fastDelta.geometryDV, corner);
        }
        return true;
    }

    // Shift+方向：移动整行/整列，对齐旧项目 MOVE_LINE。
    if (!ctrl && shift && !alt) {
        if (isGeometry) {
            pushGeometryUndoThrottled(regionId);
            const op = key === 'arrowleft' || key === 'arrowright' ? 'col' : 'row';
            moveGeometryFast(regionId, op, geometryDU, geometryDV);
        } else if (isMask) {
            const move = getMaskLineMove(directionKey, maskDU, maskDV);
            if (move) {
                pushMaskUndo(getActiveMask());
                moveMaskFast(move.op, move.du, move.dv);
            }
        } else if (isBlend) {
            const direction = directionFromArrowKey(key);
            if (direction < 0) return true;
            snapManagerCornerSelection(regionId);
            const geometry = getActiveGeometry();
            setActiveBlendCorner(getManagerCornerName(geometry));
            setActiveBlendSide(managerSideFromDirection(geometry, direction));
            pushGeometryUndoThrottled(regionId);
            applyManagerMoveLineLocal(regionId, direction, geometryDU, geometryDV);
            renderFusionUi();
            void syncManagerLineMoveOp(regionId, direction, geometryDU, geometryDV);
        }
        return true;
    }

    // Ctrl+Shift+方向：保留为移动整行/整列的等价快捷键。
    if (ctrl && shift && !alt) {
        if (isGeometry) {
            pushGeometryUndoThrottled(regionId);
            const op = key === 'arrowleft' || key === 'arrowright' ? 'col' : 'row';
            moveGeometryFast(regionId, op, geometryDU, geometryDV);
        } else if (isMask) {
            const move = getMaskLineMove(directionKey, maskDU, maskDV);
            if (move) {
                pushMaskUndo(getActiveMask());
                moveMaskFast(move.op, move.du, move.dv);
            }
        } else if (isBlend) {
            const direction = directionFromArrowKey(key);
            if (direction < 0) return true;
            snapManagerCornerSelection(regionId);
            const geometry = getActiveGeometry();
            setActiveBlendCorner(getManagerCornerName(geometry));
            setActiveBlendSide(managerSideFromDirection(geometry, direction));
            pushGeometryUndoThrottled(regionId);
            applyManagerMoveLineLocal(regionId, direction, geometryDU, geometryDV);
            renderFusionUi();
            void syncManagerLineMoveOp(regionId, direction, geometryDU, geometryDV);
        }
        return true;
    }

    // Alt+Shift+方向：融合管理边线。
    if (!ctrl && shift && alt) {
        if (isBlend) {
            const direction = directionFromArrowKey(key);
            if (direction < 0) return true;
            snapManagerCornerSelection(regionId);
            const geometry = getActiveGeometry();
            setActiveBlendCorner(getManagerCornerName(geometry));
            setActiveBlendSide(managerSideFromDirection(geometry, direction));
            pushGeometryUndoThrottled(regionId);
            applyManagerMoveLineLocal(regionId, direction, geometryDU, geometryDV);
            renderFusionUi();
            void syncManagerLineMoveOp(regionId, direction, geometryDU, geometryDV);
        }
        return true;
    }

    // Ctrl+Shift+Alt+方向：整投影区移动，对齐旧项目 cmd_movePan。
    if (ctrl && shift && alt) {
        if (isGeometry) {
            pushGeometryUndoThrottled(regionId);
            moveGeometryFast(regionId, 'all', geometryDU, geometryDV);
        } else if (isMask) {
            pushMaskUndo(getActiveMask());
            moveMaskFast('all', maskDU, maskDV);
        } else if (isBlend) {
            pushGeometryUndoThrottled(regionId);
            moveGeometryFast(regionId, 'all', geometryDU, geometryDV);
        }
        return true;
    }

    return false;
}

// ─── 主入口 ─────────────────────────────────────────────────────────────────────

export function handleKeyboard(state, event) {
    const fusionPage = document.getElementById('fusion-page');
    if (!fusionPage || !fusionPage.classList.contains('active')) return false;

    const key = event.key.toLowerCase();

    if (isTypingTarget(event.target) && !isFusionShortcutKey(event, key)) return false;

    // ─── -/+ 几何线粗细，Ctrl+-/+ 热点大小 ─────────────────────────────────────
    if (key === '-' || key === '_' || key === '=' || key === '+') {
        event.preventDefault();
        const increase = key === '=' || key === '+';
        const currentVisualStyle = getGridVisualStyle();
        let gridVisualLineWidth = currentVisualStyle.lineWidth;
        let gridVisualHotspotRadius = currentVisualStyle.hotspotRadius;
        if (event.ctrlKey) {
            gridVisualHotspotRadius = clamp(gridVisualHotspotRadius + (increase ? 0.002 : -0.002), 0.003, 0.05);
        } else {
            gridVisualLineWidth = clamp(gridVisualLineWidth + (increase ? 0.25 : -0.25), 0.5, 12.0);
        }
        applyGridVisualStyle(gridVisualLineWidth, gridVisualHotspotRadius);
        renderFusionUi();
        void setGridVisualStyle(gridVisualLineWidth, gridVisualHotspotRadius).then((response) => {
            if (response && typeof response === 'object') {
                if (typeof response.line_width === 'number') gridVisualLineWidth = response.line_width;
                if (typeof response.hotspot_radius === 'number') gridVisualHotspotRadius = response.hotspot_radius;
            }
            const syncedStyle = applyGridVisualStyle(gridVisualLineWidth, gridVisualHotspotRadius);
            renderFusionUi();
            showNotification(`辅助线 ${syncedStyle.lineWidth.toFixed(2)} · 热点 ${syncedStyle.hotspotRadius.toFixed(3)}`, 'info');
        }).catch((error) => {
            console.warn('fusion grid visual style sync failed:', error);
        });
        return true;
    }

    // ─── Ctrl+G / Ctrl+I / Ctrl+M 还原 ─────────────────────────────────────────
    if (handleRestoreShortcut(event, key, state.page.activeRegionId)) {
        return true;
    }

    // ─── Ctrl+S 保存 ────────────────────────────────────────────────────────────
    if (event.ctrlKey && !event.shiftKey && !event.altKey && key === 's') {
        event.preventDefault();
        void (async () => {
            await flushPendingGeometryOps();
            if (getState().page.activeTab === 'mask') {
                await syncMask();
            }
            await persistFusionConfig();
            showNotification('融合配置已保存', 'success');
        })().catch((error) => {
            console.warn('fusion config save failed:', error);
            showNotification('融合配置保存失败', 'error');
        });
        return true;
    }

    // ─── Ctrl+Shift+Z 初始化 ────────────────────────────────────────────────────
    if (event.ctrlKey && event.shiftKey && !event.altKey && key === 'z') {
        event.preventDefault();
        const regionId = state.page.activeRegionId;
        void (async () => {
            try {
                await resetFusionConfig();
            } catch (error) {
                console.warn('fusion module reset failed:', error);
                showNotification('融合模块初始化失败', 'error');
                return;
            }
            showNotification('融合模块已初始化（几何 / 遮罩 / 融合）', 'success');
            try {
                await closeAllFusionEditModesAfterReset(regionId);
            } catch (error) {
                console.warn('fusion debug guide close failed after reset:', error);
                showNotification('融合已初始化，调试显示关闭失败', 'warning');
            }
            renderFusionUi();
        })();
        return true;
    }

    // ─── Ctrl+Z 撤销 ────────────────────────────────────────────────────────────
    if (event.ctrlKey && !event.shiftKey && !event.altKey && key === 'z') {
        event.preventDefault();
        const regionId = state.page.activeRegionId;
        const isGeometry = state.page.activeTab === 'geometry';
        const isMask = state.page.activeTab === 'mask';
        const isBlend = state.page.activeTab === 'blend';

        if (isGeometry) {
            if (restoreGeometryUndo(regionId, getActiveGeometry())) {
                const region = getActiveGeometry();
                region.showGrid = true;
                applyGeometryUndoToAllRegions(regionId);
                showNotification('已撤销', 'success');
                renderFusionUi();
                void syncActiveGeometrySelection(regionId);
                void syncGeometryGuideVisibilityForRegions(getTargetRegionIds(regionId), true).then(() => {
                    void syncGeometryGrid(regionId).then(() => {
                        void syncGeometryPoints(regionId).then(() => {
                            void syncGeometryRegion(regionId);
                            renderFusionUi();
                        });
                    });
                });
            } else {
                showNotification('没有可撤销的操作', 'info');
            }
        } else if (isBlend) {
            const geometry = getActiveGeometry();
            if (restoreGeometryUndo(regionId, geometry)) {
                geometry.showGrid = true;
                applyGeometryUndoToAllRegions(regionId);
                showNotification('已撤销', 'success');
                renderFusionUi();
                void syncActiveGeometrySelection(regionId);
                void syncGeometryGuideVisibilityForRegions(getTargetRegionIds(regionId), true).then(() => {
                    void syncGeometryGrid(regionId).then(() => {
                        void syncGeometryPoints(regionId).then(() => {
                            void syncGeometryRegion(regionId);
                            renderFusionUi();
                        });
                    });
                });
            } else {
                const restoredBlendIds = restoreBlendUndoForRegions(getTargetRegionIds(regionId));
                if (restoredBlendIds.length) {
                    showNotification('已撤销', 'success');
                    renderFusionUi();
                    void Promise.all(restoredBlendIds.map((id) => syncBlendRegion(id))).then(() => persistBlend()).then(() => {
                        renderFusionUi();
                    });
                } else {
                    showNotification('没有可撤销的操作', 'info');
                }
            }
        } else if (isMask) {
            if (restoreMaskUndo(getActiveMask())) {
                const mask = getActiveMask();
                mask.showGrid = true;
                showNotification('已撤销', 'success');
                renderFusionUi();
                scheduleFullMaskSync();
            } else {
                showNotification('没有可撤销的操作', 'info');
            }
        }
        return true;
    }

    // ─── Alt+W 融合带开关 ───────────────────────────────────────────────────────
    if (key === 'w' && event.altKey && !event.ctrlKey && !event.shiftKey) {
        event.preventDefault();
        void (async () => {
            const nextEnabled = !getState().blend.masterEnabled;
            const response = await setBlendMaster(nextEnabled);
            if (!response || typeof response.enabled !== 'boolean') {
                throw new Error('融合总开关状态返回无效');
            }
            setBlendMasterEnabled(response.enabled);
            if (typeof response.blend_auto_edges === 'boolean') {
                setBlendAutoEdges(response.blend_auto_edges);
            }
            hydrateBlendRegionsFromResponse(response);
            renderFusionUi();
            showNotification(response.enabled ? '融合带已显示' : '融合带已隐藏', 'success');
        })().catch((error) => {
            console.warn('fusion blend display toggle failed:', error);
            showNotification('融合带显示切换失败', 'error');
        });
        return true;
    }

    // ─── P 移动步长提示 ────────────────────────────────────────────────────────
    if (key === 'p' && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        showNotification(`普通 ${NORMAL_NUDGE_PX}px / 快速 ${FAST_NUDGE_PX}px`, 'info');
        return true;
    }

    // ─── N 下一个 / H 上一个 ────────────────────────────────────────────────────
    if (key === 'n' || key === 'h') {
        event.preventDefault();
        const nextRegionId = getNextRegionId(key === 'n' ? 1 : -1);
        if (nextRegionId) {
            setActiveRegion(nextRegionId);
            renderFusionUi();
            if (state.page.activeTab === 'geometry' || state.page.activeTab === 'blend') {
                void syncActiveGeometrySelection(nextRegionId).then(() => {
                    renderFusionUi();
                }).catch((error) => {
                    console.warn('fusion keyboard region selection sync failed:', error);
                });
            }
        }
        return true;
    }

    // ─── G 几何开关 ─────────────────────────────────────────────────────────────
    if (key === 'g' && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        void (async () => {
            const stateRef = getState();
            const regionId = stateRef.page.activeRegionId;
            const ids = getTargetRegionIds(regionId);
            const geometryModeActive =
                stateRef.page.activeTab === 'geometry' &&
                ids.some((id) => !!stateRef.geometry.byRegionId[id]?.showGrid);

            if (geometryModeActive) {
                await closeAllFusionEditModes(regionId);
                setDebugBackgroundActive(false);
                renderFusionUi();
                showNotification('已关闭几何辅助线', 'success');
            } else {
                await closeAllFusionEditModes(regionId, 'geometry');
                await openGeometryMode();
                showNotification('已打开几何辅助线', 'success');
            }
        })().catch((error) => {
            console.warn('fusion geometry mode toggle failed:', error);
            showNotification('几何模式切换失败', 'error');
        });
        return true;
    }

    // ─── M 遮罩开关 ─────────────────────────────────────────────────────────────
    if (key === 'm' && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        void (async () => {
            const regionId = getState().page.activeRegionId;
            const maskModeActive = getState().page.activeTab === 'mask' && getActiveMask().showGrid;
            if (maskModeActive) {
                await closeAllFusionEditModes(regionId);
                setDebugBackgroundActive(false);
                renderFusionUi();
                showNotification('已关闭遮罩辅助线', 'success');
            } else {
                await closeAllFusionEditModes(regionId, 'mask');
                await openMaskMode();
                showNotification('已打开遮罩辅助线', 'success');
            }
        })().catch((error) => {
            console.warn('fusion mask mode toggle failed:', error);
            showNotification('遮罩模式切换失败', 'error');
        });
        return true;
    }

    // ─── I 融合模式 ─────────────────────────────────────────────────────────────
    if (key === 'i' && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        if (event.repeat) {
            console.info('[FusionICloseTrace] stage=keyI.ignoredRepeat');
            return true;
        }
        const toggleNow = traceNowMs();
        if (toggleNow - lastFusionModeToggleAt < FUSION_MODE_TOGGLE_COOLDOWN_MS) {
            console.info('[FusionICloseTrace] stage=keyI.ignoredCooldown', {
                elapsed_ms: Math.round(toggleNow - lastFusionModeToggleAt)
            });
            return true;
        }
        if (fusionModeToggleBusy) {
            showNotification('融合模式正在切换', 'info');
            return true;
        }
        lastFusionModeToggleAt = toggleNow;
        void (async () => {
            fusionModeToggleBusy = true;
            const stateRef = getState();
            const regionId = stateRef.page.activeRegionId;
            const blendModeActive = !!stateRef.blend.managerMode;
            const traceId = createFusionTraceId(blendModeActive ? 'iclose' : 'iopen');
            logFusionTrace(traceId, 'keyI.begin', {
                regionId,
                blendModeActive,
                tab: stateRef.page.activeTab,
                managerMode: !!stateRef.blend.managerMode
            });
            if (blendModeActive) {
                await closeBlendMode(regionId, traceId);
                showNotification('已关闭融合辅助线', 'success');
            } else {
                await closeAllFusionEditModes(regionId, 'blend');
                await openBlendMode(traceId);
                showNotification('已打开融合辅助线', 'success');
            }
            logFusionTrace(traceId, 'keyI.end', {
                tab: getState().page.activeTab,
                managerMode: !!getState().blend.managerMode
            });
        })().catch((error) => {
            console.warn('fusion blend mode toggle failed:', error);
            showNotification('融合模式切换失败', 'error');
        }).finally(() => {
            fusionModeToggleBusy = false;
        });
        return true;
    }

    // ─── 以下需要当前模式处于编辑状态 ───────────────────────────────────────────
    if (state.page.activeTab === 'color') return false;

    const active = getActiveRegionForMode(state);
    if (!active) return false;

    const regionId = state.page.activeRegionId;
    const isGeometry = state.page.activeTab === 'geometry';
    const isMask = state.page.activeTab === 'mask';
    const isBlend = state.page.activeTab === 'blend';
    const canEditActive = canEditActiveRegion(state, active);

    // ─── A/D/W/S 加减行列，K 直线/曲线 ─────────────────────────────────────────
    if ((isGeometry || isBlend) && ['a', 'd', 'w', 's', 'k'].includes(key) && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        if (!canEditActive) return true;

        const targetIds = state.layout.regionIds?.length ? state.layout.regionIds : [regionId];
        const resizeOpMap = {
            w: 'grow_rows',
            s: 'shrink_rows',
            a: 'grow_cols',
            d: 'shrink_cols'
        };
        const isGridResize = Object.prototype.hasOwnProperty.call(resizeOpMap, key);

        if (key === 'k') {
            pushGeometryUndo(regionId, getActiveGeometry());
            const currentMode = getActiveGeometry()?.interpolationMode === 1 ? 1 : 0;
            const mode = currentMode === 1 ? 0 : 1;
            targetIds.forEach((id) => setInterpolationMode(id, mode));
            showNotification(mode === 1 ? '已切换曲线模式' : '已切换直线模式', 'info');
            renderFusionUi();
            void syncActiveGeometrySelection(regionId);
            void Promise.all(targetIds.map((id) => syncGeometryGrid(id))).then(() => {
                renderFusionUi();
            });
            return true;
        }

        if (isGridResize) {
            if (geometryResizePending) return true;
            geometryResizePending = true;
            if (isBlend && getState().blend.managerMode) {
                targetIds.forEach((id) => pushBlendUndo(id, ensureBlendRegion(id)));
            } else {
                pushGeometryUndo(regionId, getActiveGeometry());
            }
            const resizeTask = isBlend && getState().blend.managerMode
                ? resizeBlendGridsByOp(targetIds, resizeOpMap[key])
                : Promise.all(targetIds.map((id) => resizeGeometryGridByOp(id, resizeOpMap[key])));
            void resizeTask
                .then(async () => {
                    if (isBlend && getState().blend.managerMode) {
                        // 后端已按输入邻接同步融合管理网格和四边融合带，这里只刷新 UI。
                    }
                    renderFusionUi();
                })
                .finally(() => {
                    geometryResizePending = false;
                });
            return true;
        }

        return true;
    }

    if (isMask && ['a', 'd', 'w', 's', 'k'].includes(key) && !event.ctrlKey && !event.shiftKey && !event.altKey) {
        event.preventDefault();
        if (!canEditActive) return true;

        const resizeOpMap = {
            w: 'grow_rows',
            s: 'shrink_rows',
            a: 'grow_cols',
            d: 'shrink_cols'
        };
        const isGridResize = Object.prototype.hasOwnProperty.call(resizeOpMap, key);

        if (key === 'k') {
            pushMaskUndo(getActiveMask());
            const nextMode = getActiveMask().interpolationMode === 1 ? 0 : 1;
            setMaskInterpolationMode(nextMode);
            showNotification(nextMode === 1 ? '已切换遮罩曲线模式' : '已切换遮罩直线模式', 'info');
            renderFusionUi();
            scheduleFullMaskSync();
            return true;
        }

        if (isGridResize) {
            pushMaskUndo(getActiveMask());
            void resizeMaskGridByOp(resizeOpMap[key]).then(() => renderFusionUi());
            return true;
        }

        return true;
    }

    // ─── 方向键组合 ─────────────────────────────────────────────────────────────
    if (!isDirectionalEditKey(event, key)) return false;

    event.preventDefault();
    if (!canEditActive) return true;

    return handleArrowKeys(state, event, key, regionId);
}

