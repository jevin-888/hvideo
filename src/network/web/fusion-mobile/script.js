import '../modules/core/interactionGuards.js';
import { backgroundApi, fusionApi, layerApi, peripheralApi, systemApi, videoApi } from './modules/api.js?v=94';
import {
    activeBlend,
    activeColor,
    activeGeometry,
    getNextRegionId,
    getRegionIds,
    setActiveRegionId,
    SIDE_SHORT,
    state
} from './modules/state.js?v=80';
import {
    clamp,
    createGeometryPointGrid,
    createPointGrid,
    flattenPoints,
    hydrateGeometry,
    hydrateMask,
    maskPayload,
    moveLocal,
    clampSelectionToPerimeter
} from './modules/geometry.js?v=79';
import { geometryPointFromCanvasEvent, maskPointFromCanvasEvent, regionFromCanvasEvent, renderPreview } from './modules/renderer.js?v=113';
import {
    blendPointToCanvas,
    getBlendCurveAlpha,
    getBlendCurveAnchorFromDisplayValue,
    getBlendCurveHandleLayout,
    getBlendPlotRect
} from '../modules/pages/fusion/utils/blendCurve.js';

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));
const BACKGROUND_LAYER_ID = 9060;
const BACKGROUND_LAYER_PRIORITY = 0;
const VIDEO_LAYER_IDS = [1, 2, 3, 4, 10, 11];
const NORMAL_NUDGE_PX = 2;
const FAST_NUDGE_PX = 5;
const SEEK_END_GUARD_SECONDS = 0.25;
const appState = {
    page: 'fusion',
    videoLayerId: null,
    videoStatus: null,
    videoDuration: 0,
    videoSeekClock: {
        position: 0,
        duration: 0,
        playbackRate: 1,
        state: 'stopped',
        updatedAt: 0
    },
    selectedLayerId: null,
    videoSeekDragging: false,
    videoSeekTrace: 0,
    layers: [],
    runtimeLayers: [],
    dmxFlushTimer: null,
    systemTimer: null,
    videoSeekTimer: null,
    videoStatusEventSource: null
};

function numberValue(id, fallback = 0) {
    const el = $(id.startsWith('#') ? id : `#${id}`);
    const value = Number(el?.value);
    return Number.isFinite(value) ? value : fallback;
}

function setValue(id, value) {
    const el = $(id.startsWith('#') ? id : `#${id}`);
    if (!el) return;
    el.value = value ?? '';
    if (el.tagName === 'OUTPUT') el.textContent = value ?? '';
}

function setChecked(id, checked) {
    const el = $(id.startsWith('#') ? id : `#${id}`);
    if (el) el.checked = !!checked;
}

function toast(message, type = 'info') {
    const el = $('#toast');
    if (!el) return;
    el.textContent = message;
    el.dataset.type = type;
    el.classList.add('show');
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => el.classList.remove('show'), 2200);
}

function setBusy(busy) {
    document.body.classList.toggle('is-busy', !!busy);
}

async function withBusy(label, task, successText = null) {
    setBusy(true);
    setText('#statusLine', label);
    try {
        const result = await task();
        if (successText) toast(successText, 'success');
        return result;
    } catch (error) {
        console.error(error);
        toast(error.message || '操作失败', 'error');
        throw error;
    } finally {
        setBusy(false);
        updateStatusLine();
    }
}

async function runGuideToggle(event, labels, task) {
    const checked = event.target.checked;
    try {
        await withBusy(
            checked ? labels.opening : labels.closing,
            () => task(checked),
            checked ? labels.opened : labels.closed
        );
    } catch (error) {
        event.target.checked = !checked;
    } finally {
        renderAll();
    }
}

function updateStatusLine() {
    if (appState.page !== 'fusion') {
        setText('#statusLine', `${appPageLabel(appState.page)} · 已连接`);
        return;
    }
    const targetLabel = state.mode === 'mask'
        ? '输入幕布'
        : `区域 ${state.activeRegionId}`;
    setText('#statusLine', `${targetLabel} · ${modeLabel(state.mode)}`);
}

function modeLabel(mode) {
    return {
        layout: '布局',
        geometry: '几何',
        mask: '遮罩',
        blend: '融合',
        color: '色彩',
        advanced: '高级'
    }[mode] || mode;
}

function appPageLabel(page) {
    return {
        layer: '图层',
        video: '视频',
        fusion: '融合',
        control: '控制',
        system: '系统'
    }[page] || page;
}

function formatLayerType(type) {
    return {
        video: '视频',
        image: '图片',
        text: '文本',
        capture: '采集',
        effect: '特效',
        qr: '二维码'
    }[type] || type || '未知';
}

function formatVideoState(status = {}) {
    const stateText = status.state || status.status || 'unknown';
    return {
        playing: '播放中',
        paused: '已暂停',
        stopped: '已停止',
        loaded: '已加载',
        unknown: '未知'
    }[stateText] || stateText;
}

function fileName(path) {
    if (!path) return '无媒体';
    const normalized = String(path).split('?')[0].split('#')[0].replace(/\\/g, '/');
    return decodeURIComponent(normalized.split('/').filter(Boolean).pop() || normalized);
}

function escapeHtml(value) {
    return String(value ?? '').replace(/[&<>"']/g, (char) => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#39;'
    }[char]));
}

function formatTime(seconds) {
    const safe = Math.max(0, Number(seconds) || 0);
    const total = Math.floor(safe);
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const secs = total % 60;
    if (hours > 0) return `${hours}:${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
    return `${minutes}:${String(secs).padStart(2, '0')}`;
}

function normalizeDuration(value) {
    const duration = Number(value);
    return Number.isFinite(duration) && duration > 0 ? duration : 0;
}

function sliderSecondValue(value) {
    const number = Number(value);
    return Number.isFinite(number) ? String(Math.round(number * 1000) / 1000) : '0';
}

function clampPlaybackPosition(position, duration = 0, endGuardSeconds = 0) {
    const safeDuration = normalizeDuration(duration);
    const maxPosition = safeDuration > 0
        ? Math.max(0, safeDuration - Math.max(0, endGuardSeconds))
        : Number.MAX_SAFE_INTEGER;
    return clamp(Number(position), 0, maxPosition);
}

function displayPositionForDuration(position, duration) {
    const safeDuration = normalizeDuration(duration);
    const safePosition = clampPlaybackPosition(position, safeDuration, 0);
    if (safeDuration > 0 && safeDuration - safePosition <= 0.5) {
        return safeDuration;
    }
    return safePosition;
}

function isPlaybackRunning(stateText) {
    return stateText === 'playing';
}

function getLiveVideoSeekPosition() {
    const clock = appState.videoSeekClock || {};
    const base = displayPositionForDuration(clock.position, clock.duration);
    if (!isPlaybackRunning(clock.state) || !clock.updatedAt || normalizeDuration(clock.duration) <= 0) {
        return base;
    }
    const elapsedSeconds = (performance.now() - clock.updatedAt) / 1000;
    return displayPositionForDuration(base + elapsedSeconds * (Number(clock.playbackRate) || 1), clock.duration);
}

function renderVideoSeekClock() {
    if (appState.page !== 'video' || appState.videoSeekDragging) return;
    const clock = appState.videoSeekClock || {};
    const duration = normalizeDuration(clock.duration);
    if (duration <= 0) return;
    const position = getLiveVideoSeekPosition();
    const seekSlider = $('#videoSeekSlider');
    if (seekSlider) {
        seekSlider.max = sliderSecondValue(duration);
        seekSlider.disabled = false;
        seekSlider.value = sliderSecondValue(clampPlaybackPosition(position, duration, 0));
    }
    setText('#videoSeekValue', `${formatTime(position)} / ${formatTime(duration)}`);
    const status = currentVideoStatus();
    const mediaName = fileName(status.path || status.current_path || status.currentPath);
    setText('#videoStatusText', `${formatVideoState(status)} · ${mediaName} · ${formatTime(position)} / ${formatTime(duration)}`);
}

function syncVideoSeekClock(status = {}, position, duration) {
    appState.videoSeekClock = {
        position: displayPositionForDuration(position, duration),
        duration,
        playbackRate: Math.max(0, Number(status.playbackRate ?? 1) || 1),
        state: status.state || status.status || 'stopped',
        updatedAt: performance.now()
    };
    if (!appState.videoSeekTimer) {
        appState.videoSeekTimer = setInterval(renderVideoSeekClock, 250);
    }
}

function clearVideoSeekClock(status = {}) {
    appState.videoDuration = 0;
    appState.videoSeekClock = {
        position: 0,
        duration: 0,
        playbackRate: 1,
        state: status.state || status.status || 'stopped',
        updatedAt: performance.now()
    };
    const seekSlider = $('#videoSeekSlider');
    if (seekSlider) {
        seekSlider.max = '0';
        seekSlider.value = '0';
        seekSlider.disabled = true;
    }
    setText('#videoSeekValue', '--');
    const mediaName = fileName(status.path || status.current_path || status.currentPath);
    setText('#videoStatusText', `${formatVideoState(status)} · ${mediaName} · 0:00`);
}

function shouldClearVideoSeek(status = {}) {
    const stateText = status.state || status.status;
    return stateText === 'stopped' || stateText === 'capturing' || stateText === 'no_signal_placeholder';
}

function statusPositionFromPayload(status = {}) {
    return status.current_position ?? status.currentPosition ?? status.position;
}

function syncVideoProgressPayload(status = {}) {
    const positionValue = statusPositionFromPayload(status);
    if (appState.videoSeekDragging) return;
    if (status.duration === undefined || positionValue === undefined) {
        if (shouldClearVideoSeek(status)) {
            clearVideoSeekClock(status);
        } else if (status.state || status.status) {
            appState.videoSeekClock = {
                ...appState.videoSeekClock,
                state: status.state || status.status,
                updatedAt: performance.now()
            };
        }
        return;
    }
    const duration = normalizeDuration(status.duration);
    const position = displayPositionForDuration(positionValue, duration);
    if (duration <= 0 && shouldClearVideoSeek(status)) {
        clearVideoSeekClock(status);
        return;
    }
    appState.videoDuration = duration;
    syncVideoSeekClock(status, position, duration);
    renderVideoSeekClock();
}

function handleVideoStatusEvent(event) {
    try {
        const data = JSON.parse(event.data || '{}');
        const layerId = Number(appState.videoLayerId || chooseVideoLayer());
        const statusMap = data.video_status || data.videoStatus;
        const status = statusMap
            ? (statusMap[layerId] || statusMap[String(layerId)])
            : (Number(data.layerId) === layerId ? data : null);
        if (!status) return;
        appState.videoStatus = status;
        syncVideoProgressPayload(status);
    } catch (error) {
        console.warn('[video_status] 解析失败', error);
    }
}

function ensureVideoStatusEvents() {
    if (appState.videoStatusEventSource || typeof EventSource === 'undefined') return;
    appState.videoStatusEventSource = new EventSource('/api/v1/events');
    appState.videoStatusEventSource.addEventListener('video_status', handleVideoStatusEvent);
    appState.videoStatusEventSource.onerror = () => {
        if (appState.videoStatusEventSource?.readyState === EventSource.CLOSED) {
            appState.videoStatusEventSource.close();
            appState.videoStatusEventSource = null;
            setTimeout(ensureVideoStatusEvents, 3000);
        }
    };
}

function formatResolution(status = {}) {
    const width = status.width || status.canvas_in_width || status.canvas_width;
    const height = status.height || status.canvas_in_height || status.canvas_height;
    if (width && height) return `${width}×${height}`;
    if (typeof status.resolution === 'string') return status.resolution.trim().replace(/\s+/, '×');
    return '--';
}

function formatPercent(value) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return '--';
    return `${Math.max(0, Math.min(100, numeric)).toFixed(1)}%`;
}

function setText(selector, value) {
    const el = $(selector);
    if (el) el.textContent = value;
}

function emptyList(message) {
    return `<div class="empty-state">${message}</div>`;
}

function getRuntimeLayer(layerId) {
    return (appState.runtimeLayers || []).find((layer) => Number(layer?.id) === Number(layerId)) || null;
}

function getConfigLayer(layerId) {
    return (appState.layers || []).find((layer) => Number(layer?.id) === Number(layerId)) || null;
}

function mergedLayer(layerId) {
    const config = getConfigLayer(layerId) || {};
    const runtime = getRuntimeLayer(layerId) || {};
    return {
        ...config,
        ...runtime,
        id: Number(config.id ?? runtime.id ?? layerId),
        type: config.type ?? runtime.type,
        position: runtime.position || config.position || { x: runtime.x ?? config.x ?? 0, y: runtime.y ?? config.y ?? 0 },
        size: runtime.size || config.size || { width: runtime.width ?? config.width ?? 0, height: runtime.height ?? config.height ?? 0 },
        visible: runtime.visible ?? config.visible,
        alpha: runtime.alpha ?? config.alpha,
        priority: runtime.priority ?? config.priority,
        rotation: runtime.rotation ?? config.rotation,
        scale: runtime.scale ?? config.scale,
        volume: runtime.volume ?? config.volume
    };
}

function numberOr(value, fallback = 0) {
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
}

function geometryAxisForDirection(dir) {
    return (dir === 'left' || dir === 'right') ? 'col' : 'row';
}

function directionCode(dir) {
    return { up: 0, down: 1, left: 2, right: 3 }[dir] ?? -1;
}

function firstPositiveNumber(...values) {
    for (const value of values) {
        const number = Number(value);
        if (Number.isFinite(number) && number > 0) return number;
    }
    return 0;
}

function activeRegionConfig(regionId = state.activeRegionId) {
    return (state.regions || []).find((region) => Number(region?.id) === Number(regionId)) || null;
}

function pixelDimension(value, total) {
    const number = Number(value);
    const totalNumber = Number(total);
    if (!Number.isFinite(number) || number <= 0) return 0;
    if (number <= 1 && Number.isFinite(totalNumber) && totalNumber > 0) return number * totalNumber;
    return number;
}

function getOutputNudgeSize(regionId = state.activeRegionId) {
    const region = activeRegionConfig(regionId);
    const outputWidth = Number(state.layout.canvas_out_width) || 0;
    const outputHeight = Number(state.layout.canvas_out_height) || 0;
    const rows = Math.max(1, Number(state.layout.grid_out_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_out_cols) || 1);
    const width = firstPositiveNumber(
        pixelDimension(region?.outWidth ?? region?.out_width, outputWidth),
        state.layout.tile_out_width,
        outputWidth > 0 ? outputWidth / cols : 0,
        outputWidth,
        1
    );
    const height = firstPositiveNumber(
        pixelDimension(region?.outHeight ?? region?.out_height, outputHeight),
        state.layout.tile_out_height,
        outputHeight > 0 ? outputHeight / rows : 0,
        outputHeight,
        1
    );
    return { width, height };
}

function getInputNudgeSize() {
    const rows = Math.max(1, Number(state.layout.grid_in_rows) || 1);
    const cols = Math.max(1, Number(state.layout.grid_in_cols) || 1);
    const tileW = Number(state.layout.tile_in_width) || 0;
    const tileH = Number(state.layout.tile_in_height) || 0;
    const width = firstPositiveNumber(
        state.layout.input_total_width,
        tileW > 0 ? tileW * cols : 0,
        state.layout.canvas_in_width,
        tileW,
        cols,
        1
    );
    const height = firstPositiveNumber(
        state.layout.input_total_height,
        tileH > 0 ? tileH * rows : 0,
        state.layout.canvas_in_height,
        tileH,
        rows,
        1
    );
    return { width, height };
}

function nudgeStepPixels(fastMove) {
    return fastMove ? FAST_NUDGE_PX : NORMAL_NUDGE_PX;
}

function geometryDeltaFromPixels(sx, sy, fastMove, regionId = state.activeRegionId) {
    const step = nudgeStepPixels(fastMove);
    const { width, height } = getOutputNudgeSize(regionId);
    return {
        dx: sx * (2 * step / Math.max(1, width)),
        dy: -sy * (2 * step / Math.max(1, height))
    };
}

function maskDeltaFromPixels(sx, sy, fastMove) {
    const step = nudgeStepPixels(fastMove);
    const { width, height } = getInputNudgeSize();
    return {
        dx: sx * (step / Math.max(1, width)),
        dy: sy * (step / Math.max(1, height))
    };
}

function nearestGeometryCorner(geo) {
    if (!geo) return { row: 0, col: 0 };
    const lastRow = Math.max(0, Number(geo.rows || 1) - 1);
    const lastCol = Math.max(0, Number(geo.cols || 1) - 1);
    const currentRow = clamp(Number(geo.selected?.row ?? 0), 0, lastRow);
    const currentCol = clamp(Number(geo.selected?.col ?? 0), 0, lastCol);
    const corners = [
        { row: 0, col: 0 },
        { row: 0, col: lastCol },
        { row: lastRow, col: 0 },
        { row: lastRow, col: lastCol }
    ];
    const selection = corners.reduce((best, item) => {
        const distance = Math.abs(item.row - currentRow) + Math.abs(item.col - currentCol);
        return distance < best.distance ? { ...item, distance } : best;
    }, { ...corners[0], distance: Number.POSITIVE_INFINITY });
    geo.selected = { row: selection.row, col: selection.col };
    return geo.selected;
}

function managerCornerName(geo) {
    const selection = nearestGeometryCorner(geo);
    return `${selection.row === 0 ? 'top' : 'bottom'}-${selection.col === 0 ? 'left' : 'right'}`;
}

function managerSideForDirection(geo, direction) {
    if (!geo) return state.activeSide;
    if (direction === 2 || direction === 3) return (geo.selected?.col ?? 0) < geo.cols / 2 ? 'left' : 'right';
    if (direction === 0 || direction === 1) return (geo.selected?.row ?? 0) < geo.rows / 2 ? 'top' : 'bottom';
    return state.activeSide;
}

function isGeometryPayload(payload) {
    return payload && typeof payload === 'object' && (
        Array.isArray(payload.corners)
        || Array.isArray(payload.points)
        || Number.isFinite(Number(payload.rows))
        || Number.isFinite(Number(payload.cols))
    );
}

async function refreshMovedGeometry(regionId, response) {
    const payload = isGeometryPayload(response) ? response : await fusionApi.loadGeometry(regionId);
    state.geometry[regionId] = hydrateGeometry(regionId, payload || {});
}

async function ensureBlendMasterEnabled() {
    const master = await fusionApi.getMaster().catch(() => null);
    if (master && typeof master.enabled === 'boolean') {
        state.masterEnabled = master.enabled;
        if (typeof master.blend_auto_edges === 'boolean') {
            state.blendAutoEdges = master.blend_auto_edges;
        }
    }
    if (state.masterEnabled) return;
    const response = await fusionApi.setMaster(true);
    state.masterEnabled = response?.enabled !== false;
    if (typeof response?.blend_auto_edges === 'boolean') {
        state.blendAutoEdges = response.blend_auto_edges;
    }
}

function blendNumber(value, min, max) {
    return clamp(Number(value), min, max);
}

function normalizeBlend(payload = {}) {
    if (typeof payload.blend_auto_edges === 'boolean') {
        state.blendAutoEdges = payload.blend_auto_edges;
    }
    if (typeof payload.fusion_master_enabled === 'boolean') {
        state.masterEnabled = payload.fusion_master_enabled;
    }
    const makeSide = (side) => {
        const short = SIDE_SHORT[side];
        return {
            enabled: !!payload[`blend_${side}_enabled`],
            width: Number(payload[`blend_${side}`]) || 0,
            gamma: Number(payload[`edge_${side}_gamma`]) || 1.8,
            slope: Number(payload[`edge_${side}_slope`]) || 1.0,
            stripStart: Number(payload[`strip_start_${short}`] ?? 0),
            stripEnd: Number(payload[`strip_end_${short}`] ?? 255),
            anchor: Number(payload[`anchor_${short}`] ?? 0.5),
            bright: [
                Number(payload[`bright_${short}_r`] ?? 128),
                Number(payload[`bright_${short}_g`] ?? 128),
                Number(payload[`bright_${short}_b`] ?? 128)
            ]
        };
    };
    return {
        gridRows: Number(payload.blend_grid_rows) || 2,
        gridCols: Number(payload.blend_grid_cols) || 2,
        left: makeSide('left'),
        right: makeSide('right'),
        top: makeSide('top'),
        bottom: makeSide('bottom')
    };
}

function cloneBlendSide(side) {
    return {
        enabled: !!side?.enabled,
        width: Number(side?.width ?? 0),
        gamma: Number(side?.gamma ?? 1.8),
        slope: Number(side?.slope ?? 1.0),
        stripStart: Number(side?.stripStart ?? 0),
        stripEnd: Number(side?.stripEnd ?? 255),
        anchor: Number(side?.anchor ?? 0.5),
        bright: [
            Number(side?.bright?.[0] ?? 128),
            Number(side?.bright?.[1] ?? 128),
            Number(side?.bright?.[2] ?? 128)
        ]
    };
}

function getTargetRegionIds() {
    const ids = getRegionIds();
    return ids.length ? ids : [state.activeRegionId];
}

function getDefaultFocusRegionId() {
    const ids = getRegionIds();
    return Number(ids.find((id) => Number(id) > 0) || 1);
}

async function resetDebugModeFocusToDefaultRegion() {
    const regionId = getDefaultFocusRegionId();
    if (Number(state.activeRegionId) !== regionId) {
        setActiveRegionId(regionId);
        await fusionApi.setActiveRegion(regionId).catch(() => {});
        if (!state.geometry[regionId]) await loadActiveRegionDetails(regionId);
    }
    return regionId;
}

function getBlendEditRegionIds() {
    const ids = state.managerMode ? getRegionIds() : [state.activeRegionId];
    return ids.length ? ids : [state.activeRegionId];
}

async function syncAllGeometryGuideVisibility(showGrid, options = {}) {
    const visible = !!showGrid;
    const ids = getTargetRegionIds();
    await Promise.all(ids.map((regionId) => ensureGeometryLoaded(regionId)));
    ids.forEach((regionId) => {
        if (state.geometry[regionId]) state.geometry[regionId].showGrid = visible;
    });
    await fusionApi.showGeometryGridAll(visible);
    if (visible) {
        await fusionApi.setGeometrySelection(
            state.activeRegionId,
            activeGeometry()?.selected?.row ?? 0,
            activeGeometry()?.selected?.col ?? 0
        ).catch(() => {});
    }
    if (options.persist !== false) await fusionApi.persist();
}

async function syncGeometryGuideVisibilityForRegions(ids, showGrid) {
    const regionIds = Array.isArray(ids) && ids.length ? ids : getTargetRegionIds();
    await Promise.all(regionIds.map((regionId) => fusionApi.showGeometryGrid(regionId, !!showGrid)));
}

async function syncMaskGuideVisibility(showGrid, options = {}) {
    const visible = !!showGrid;
    if (state.mask) {
        state.mask.showGrid = visible;
        if (visible && ((state.mask.selected?.row ?? -1) < 0 || (state.mask.selected?.col ?? -1) < 0)) {
            state.mask.selected = { row: 0, col: 0 };
        }
    }
    await fusionApi.setMaskGuideVisibility(visible);
    if (options.persist !== false) await fusionApi.persist();
}

function isBlendSideOn(blend, side) {
    const sideState = blend?.[side];
    return !!sideState?.enabled;
}

function getDefaultBlendWidth(regionId, side) {
    const geometry = state.geometry[regionId];
    const blend = state.blend[regionId];
    const cols = Math.max(2, Number(blend?.gridCols ?? geometry?.cols ?? 2));
    const rows = Math.max(2, Number(blend?.gridRows ?? geometry?.rows ?? 2));
    if (side === 'left' || side === 'right') return cols > 2 ? 1 / (cols - 1) : 0;
    if (side === 'top' || side === 'bottom') return rows > 2 ? 1 / (rows - 1) : 0;
    return 0;
}

async function ensureBlendLoaded(regionId) {
    if (!state.blend[regionId]) {
        state.blend[regionId] = normalizeBlend(await fusionApi.loadBlend(regionId));
    }
    return state.blend[regionId];
}

async function ensureGeometryLoaded(regionId) {
    if (!state.geometry[regionId]) {
        state.geometry[regionId] = hydrateGeometry(regionId, await fusionApi.loadGeometry(regionId));
    }
    return state.geometry[regionId];
}

async function syncBlendGuideCountFromGeometry(ids = getTargetRegionIds()) {
    const regionIds = Array.isArray(ids) && ids.length ? ids : getTargetRegionIds();
    await Promise.all(regionIds.map(async (regionId) => {
        const geometry = await ensureGeometryLoaded(regionId);
        state.blend[regionId] = normalizeBlend(await fusionApi.loadBlend(regionId));
        const blend = state.blend[regionId];
        if (!geometry || !blend) return;
        const rows = Math.max(2, Number(geometry.rows) || 2);
        const cols = Math.max(2, Number(geometry.cols) || 2);
        if (Number(blend.gridRows) === rows && Number(blend.gridCols) === cols) return;
        blend.gridRows = rows;
        blend.gridCols = cols;
        const response = await fusionApi.saveBlend(regionId, {
            blend_grid_rows: rows,
            blend_grid_cols: cols,
            guide_grid_only: true,
            sync_adjacent: false
        });
        state.blend[regionId] = normalizeBlend(response || await fusionApi.loadBlend(regionId));
    }));
}

async function syncGeometryGuideCountFromBlend(ids = getTargetRegionIds()) {
    const regionIds = Array.isArray(ids) && ids.length ? ids : getTargetRegionIds();
    await Promise.all(regionIds.map(async (regionId) => {
        const geometry = await ensureGeometryLoaded(regionId);
        state.blend[regionId] = normalizeBlend(await fusionApi.loadBlend(regionId));
        const blend = state.blend[regionId];
        if (!geometry || !blend) return;
        const rows = Math.max(2, Number(blend.gridRows) || Number(geometry.rows) || 2);
        const cols = Math.max(2, Number(blend.gridCols) || Number(geometry.cols) || 2);
        if (Number(geometry.rows) === rows && Number(geometry.cols) === cols) return;
        const response = await fusionApi.setGeometryGrid(regionId, rows, cols, geometry.interpolationMode || 0, {
            syncBlendGrid: false
        });
        state.geometry[regionId] = hydrateGeometry(regionId, response || await fusionApi.loadGeometry(regionId));
    }));
}

async function syncActiveGeometrySelection(regionId = state.activeRegionId) {
    const geo = await ensureGeometryLoaded(regionId);
    if (!geo) return null;
    const selection = state.mode === 'blend' && state.managerMode
        ? nearestGeometryCorner(geo)
        : (geo.selected || { row: 0, col: 0 });
    await fusionApi.setGeometrySelection(regionId, selection.row ?? 0, selection.col ?? 0);
    return selection;
}

function getDefaultResetBlendWidth(regionId, side) {
    return getDefaultBlendWidth(regionId, side);
}

function createResetBlendForRegion(regionId) {
    const next = createDefaultBlend();
    const geometry = state.geometry[regionId];
    const blend = state.blend[regionId];
    // 融合管理网格独立于几何播放网格；重置融合时也必须优先保留融合网格。
    next.gridRows = Math.max(2, Number(blend?.gridRows ?? geometry?.rows ?? next.gridRows));
    next.gridCols = Math.max(2, Number(blend?.gridCols ?? geometry?.cols ?? next.gridCols));
    ['left', 'right', 'top', 'bottom'].forEach((side) => {
        next[side].enabled = !!blend?.[side]?.enabled;
        next[side].width = getDefaultResetBlendWidth(regionId, side);
    });
    return next;
}

function normalizeBlendForSave(blend, regionId = state.activeRegionId) {
    const geometry = state.geometry[regionId];
    // 融合管理网格不能在保存时被几何 rows/cols 覆盖；I 模式加减线只影响融合带/辅助线。
    const gridRows = Math.max(2, Number(blend?.gridRows ?? geometry?.rows ?? 2));
    const gridCols = Math.max(2, Number(blend?.gridCols ?? geometry?.cols ?? 2));
    const normalized = {
        ...blend,
        gridRows,
        gridCols
    };
    ['left', 'right', 'top', 'bottom'].forEach((side) => {
        const source = blend?.[side] || {};
        const width = Number(source.width ?? 0);
        const fallbackWidth = (side === 'left' || side === 'right')
            ? (gridCols > 2 ? 1 / (gridCols - 1) : 0)
            : (gridRows > 2 ? 1 / (gridRows - 1) : 0);
        normalized[side] = {
            ...source,
            width: source.enabled && width <= 0.001 ? fallbackWidth : width
        };
    });
    return normalized;
}

function blendPayload(blend, regionId = state.activeRegionId) {
    const normalized = normalizeBlendForSave(blend, regionId);
    return {
        blend_grid_rows: normalized.gridRows,
        blend_grid_cols: normalized.gridCols,
        blend_left: normalized.left.width,
        blend_right: normalized.right.width,
        blend_top: normalized.top.width,
        blend_bottom: normalized.bottom.width,
        blend_left_enabled: !!normalized.left.enabled,
        blend_right_enabled: !!normalized.right.enabled,
        blend_top_enabled: !!normalized.top.enabled,
        blend_bottom_enabled: !!normalized.bottom.enabled,
        edge_left_gamma: normalized.left.gamma ?? 1.8,
        edge_left_slope: normalized.left.slope ?? 1.0,
        edge_right_gamma: normalized.right.gamma ?? 1.8,
        edge_right_slope: normalized.right.slope ?? 1.0,
        edge_top_gamma: normalized.top.gamma ?? 1.8,
        edge_top_slope: normalized.top.slope ?? 1.0,
        edge_bottom_gamma: normalized.bottom.gamma ?? 1.8,
        edge_bottom_slope: normalized.bottom.slope ?? 1.0,
        strip_start_l: normalized.left.stripStart ?? 0,
        strip_end_l: normalized.left.stripEnd ?? 255,
        strip_start_r: normalized.right.stripStart ?? 0,
        strip_end_r: normalized.right.stripEnd ?? 255,
        strip_start_t: normalized.top.stripStart ?? 0,
        strip_end_t: normalized.top.stripEnd ?? 255,
        strip_start_b: normalized.bottom.stripStart ?? 0,
        strip_end_b: normalized.bottom.stripEnd ?? 255,
        anchor_l: normalized.left.anchor ?? 0.5,
        anchor_r: normalized.right.anchor ?? 0.5,
        anchor_t: normalized.top.anchor ?? 0.5,
        anchor_b: normalized.bottom.anchor ?? 0.5,
        sync_adjacent: false
    };
}

function createDefaultGeometry(showGrid = false) {
    return {
        regionId: state.activeRegionId,
        rows: 2,
        cols: 2,
        interpolationMode: 0,
        showGrid,
        selected: { row: 0, col: 0 },
        points: createGeometryPointGrid(2, 2)
    };
}

function geometryPointTuples(geometry) {
    const rows = Math.max(2, Number(geometry?.rows) || 2);
    const cols = Math.max(2, Number(geometry?.cols) || 2);
    const fallback = createGeometryPointGrid(rows, cols);
    const source = Array.isArray(geometry?.points) ? geometry.points : fallback;
    const tuples = [];
    for (let row = 0; row < rows; row += 1) {
        for (let col = 0; col < cols; col += 1) {
            const point = source[row * cols + col] || fallback[row * cols + col];
            tuples.push([row, col, Number(point.u) || 0, Number(point.v) || 0]);
        }
    }
    return tuples;
}

function createDefaultMask(showGrid = false) {
    return {
        rows: 2,
        cols: 2,
        enabled: !!showGrid,
        showGrid: !!showGrid,
        interpolationMode: 0,
        selected: { row: 0, col: 0 },
        points: createPointGrid(2, 2)
    };
}

function resetLocalFusionDebugState() {
    state.mode = 'geometry';
    state.activeSide = 'left';
    Object.values(state.geometry || {}).forEach((geometry) => {
        if (geometry) geometry.showGrid = false;
    });
    if (state.mask) {
        state.mask.enabled = false;
        state.mask.showGrid = false;
        state.mask.selected = { row: 0, col: 0 };
    }
    state.managerMode = false;
}

function createDefaultBlend() {
    const geo = activeGeometry();
    const makeSide = () => ({
        enabled: false,
        width: 0,
        gamma: 1.8,
        slope: 1.0,
        stripStart: 0,
        stripEnd: 255,
        anchor: 0.5,
        bright: [128, 128, 128]
    });
    return {
        gridRows: Math.max(2, Number(geo?.rows) || 2),
        gridCols: Math.max(2, Number(geo?.cols) || 2),
        left: makeSide(),
        right: makeSide(),
        top: makeSide(),
        bottom: makeSide()
    };
}

function syncLayoutInputs() {
    setValue('canvasInWidth', state.layout.canvas_in_width || '');
    setValue('canvasInHeight', state.layout.canvas_in_height || '');
    setValue('gridInRows', state.layout.grid_in_rows || 1);
    setValue('gridInCols', state.layout.grid_in_cols || 1);
    setValue('canvasOutWidth', state.layout.canvas_out_width || '');
    setValue('canvasOutHeight', state.layout.canvas_out_height || '');
    setValue('gridOutRows', state.layout.grid_out_rows || 1);
    setValue('gridOutCols', state.layout.grid_out_cols || 1);
    setValue('rotationAngle', state.layout.rotation_angle || 0);
}

function collectLayout() {
    const regionCount = Math.max(1, numberValue('gridInRows', 1) * numberValue('gridInCols', 1));
    const outputCount = Math.max(1, numberValue('gridOutRows', 1) * numberValue('gridOutCols', 1));
    const mappings = [];
    for (let i = 0; i < regionCount; i += 1) {
        const outputIndex = i < outputCount ? i : -1;
        mappings.push({ enabled: outputIndex >= 0, in_id: i + 1, out_idx: outputIndex });
    }
    return {
        canvas_in_width: Math.round(numberValue('canvasInWidth')),
        canvas_in_height: Math.round(numberValue('canvasInHeight')),
        grid_in_rows: Math.round(numberValue('gridInRows', 1)),
        grid_in_cols: Math.round(numberValue('gridInCols', 1)),
        canvas_out_width: Math.round(numberValue('canvasOutWidth')),
        canvas_out_height: Math.round(numberValue('canvasOutHeight')),
        grid_out_rows: Math.round(numberValue('gridOutRows', 1)),
        grid_out_cols: Math.round(numberValue('gridOutCols', 1)),
        rotation_angle: numberValue('rotationAngle', 0),
        split_direction: state.layout.split_direction,
        mappings
    };
}

function renderRegionPicker() {
    const host = $('#regionPicker');
    const ids = getRegionIds();
    host.innerHTML = ids.map((id) => (
        `<button class="region-chip ${id === state.activeRegionId ? 'active' : ''}" data-region-id="${id}" type="button">${id}</button>`
    )).join('');
}

function renderSummary() {
    const geo = activeGeometry();
    const ids = getRegionIds();
    const currentIndex = Math.max(0, ids.indexOf(Number(state.activeRegionId)));
    const projectorCount = ids.length ? `${currentIndex + 1}/${ids.length}` : `${state.activeRegionId}/1`;
    const selectedPoint = geo
        ? `${(geo.selected?.row ?? 0) + 1}-${(geo.selected?.col ?? 0) + 1}`
        : '--';
    const activeRegionLabel = $('#activeRegionLabel');
    if (activeRegionLabel) {
        activeRegionLabel.textContent = state.mode === 'mask'
            ? '输入幕布'
            : `区域 ${state.activeRegionId}`;
    }
    const previewGridBadge = $('#previewGridBadge');
    if (previewGridBadge) previewGridBadge.textContent = `${geo?.rows ?? '--'}行 ${geo?.cols ?? '--'}列`;
    $$('[data-projector-count]').forEach((item) => {
        item.textContent = projectorCount;
    });
    const geometryMoveModeBtn = $('#geometryMoveModeBtn');
    if (geometryMoveModeBtn) geometryMoveModeBtn.textContent = selectedPoint;
    const blendMoveModeBtn = $('#blendMoveModeBtn');
    if (blendMoveModeBtn) blendMoveModeBtn.textContent = selectedPoint;
    setChecked('fusionMasterToggle', state.masterEnabled);
    setChecked('blendMasterToggle', state.masterEnabled);
    renderRegionPicker();
    updateStatusLine();
}

function syncGeometryPanel() {
    const geo = activeGeometry();
    if (!geo) return;
    setChecked('geometryGuideToggle', geo.showGrid);
}

function syncMaskPanel() {
    const mask = state.mask;
    if (!mask) return;
    setChecked('maskEnabledToggle', mask.enabled);
    setChecked('maskGuideToggle', mask.showGrid);
    const maskMoveModeBtn = $('#maskMoveModeBtn');
    if (maskMoveModeBtn) maskMoveModeBtn.textContent = '点';
}

function syncBlendPanel() {
    const blend = activeBlend();
    if (!blend) return;
    const geo = activeGeometry();
    const side = blend[state.activeSide];
    setChecked('blendGuideToggle', state.managerMode);
    setValue('blendGammaInput', side.gamma);
    setValue('blendSlopeInput', side.slope);
    setValue('blendBrightR', side.bright?.[0] ?? 128);
    setValue('blendBrightG', side.bright?.[1] ?? 128);
    setValue('blendBrightB', side.bright?.[2] ?? 128);
    updateBrightnessDragFill($('#blendBrightR'), side.bright?.[0] ?? 128, '239, 68, 68');
    updateBrightnessDragFill($('#blendBrightG'), side.bright?.[1] ?? 128, '34, 197, 94');
    updateBrightnessDragFill($('#blendBrightB'), side.bright?.[2] ?? 128, '59, 130, 246');
    $$('.blend-side-check').forEach((label) => {
        const sideName = label.dataset.side;
        const input = label.querySelector('input[data-blend-side]');
        const enabled = isBlendSideOn(blend, sideName);
        label.classList.toggle('active', enabled);
        label.classList.toggle('selected', state.masterEnabled && sideName === state.activeSide);
        label.classList.toggle('is-enabled', enabled);
        label.classList.toggle('is-disabled', !state.masterEnabled);
        if (input) {
            input.checked = enabled;
            input.disabled = !state.masterEnabled;
        }
    });
    drawBlendCurve();
}

function syncColorPanel() {
    const color = activeColor();
    if (!color) return;
    setValue('colorBrightness', color.brightness);
    setValue('colorContrast', color.contrast);
    setValue('colorSaturation', color.saturation);
    $('#colorBrightnessValue').textContent = Number(color.brightness).toFixed(2);
    $('#colorContrastValue').textContent = Number(color.contrast).toFixed(2);
    $('#colorSaturationValue').textContent = Number(color.saturation).toFixed(2);
}

function syncAdvancedPanel() {
    const correction = state.correction[state.activeRegionId] || {};
    setChecked('matrixCorrectionToggle', correction.enabled);
    setValue('offsetX', correction.offset_x ?? 0);
    setValue('offsetY', correction.offset_y ?? 0);
    setValue('scaleX', correction.scale_x ?? 1);
    setValue('scaleY', correction.scale_y ?? 1);
    setValue('rotateRad', correction.rotate_rad ?? 0);
    setValue('keystoneX', correction.keystone_x ?? 0);
    setValue('keystoneY', correction.keystone_y ?? 0);

    const cave = state.cave[state.activeRegionId] || {};
    setChecked('caveToggle', cave.enabled);
    setValue('wallType', cave.wall_type ?? 0);
    setValue('eyeDistance', cave.eye_distance ?? 0.065);
    setValue('nearPlane', cave.near_plane ?? 0.1);
    setValue('farPlane', cave.far_plane ?? 100);
    ['llx', 'lly', 'llz', 'ulx', 'uly', 'ulz', 'lrx', 'lry', 'lrz'].forEach((id) => setValue(id, cave[id] ?? 0));
}

function drawBlendCurve() {
    const canvas = $('#blendCurveCanvas');
    const side = activeBlend()?.[state.activeSide];
    if (!canvas || !side) return;
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(260, Math.round(rect.width || canvas.clientWidth || 320));
    const height = Math.max(160, Math.round(rect.height || canvas.clientHeight || 180));
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }
    const ctx = canvas.getContext('2d');
    const plot = getBlendPlotRect(canvas);
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#0b1220';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.strokeStyle = '#1e293b';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i += 1) {
        const gx = plot.x + (plot.width / 4) * i;
        const gy = plot.y + (plot.height / 4) * i;
        ctx.beginPath();
        ctx.moveTo(gx, plot.y);
        ctx.lineTo(gx, plot.y + plot.height);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(plot.x, gy);
        ctx.lineTo(plot.x + plot.width, gy);
        ctx.stroke();
    }
    ctx.strokeStyle = '#60a5fa';
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i <= 96; i += 1) {
        const x = i / 96;
        const screen = blendPointToCanvas({ x, y: 1 - getBlendCurveAlpha(side, x) }, plot);
        if (i === 0) ctx.moveTo(screen.x, screen.y);
        else ctx.lineTo(screen.x, screen.y);
    }
    ctx.stroke();
    const layout = getBlendCurveHandleLayout(canvas, side);
    layout.handles.forEach((handle) => {
        ctx.fillStyle = '#f59e0b';
        ctx.beginPath();
        ctx.arc(handle.screen.x, handle.screen.y, handle.id === 'anchor' ? 8 : 7, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = '#f8fafc';
        ctx.lineWidth = 1.5;
        ctx.stroke();
    });
    ctx.fillStyle = '#e2e8f0';
    ctx.font = '12px Consolas, monospace';
    const params = layout.params;
    ctx.fillText(`BLEND ${state.activeSide.toUpperCase()} · gamma=${params.gamma.toFixed(2)} gain=${params.anchor.toFixed(2)} slope=${params.slope.toFixed(2)}`, 12, 14);
}

function updateBrightnessDragFill(input, value, color) {
    const host = input?.closest('.brightness-drag-field');
    if (!host) return;
    const ratio = clamp(Number(value) || 0, 0, 255) / 255;
    host.style.setProperty('--brightness-color', color);
    host.style.setProperty('--brightness-level', `${ratio * 100}%`);
}

function syncPreviewPlacement() {
    const preview = $('#fusionPreviewPanel');
    const activePanel = $(`#panel-${state.mode}`);
    const slot = activePanel?.querySelector('[data-preview-slot]');
    if (!preview) return;
    if (slot && preview.parentElement !== slot) {
        slot.appendChild(preview);
    }
    preview.hidden = !slot;
}

function renderAll() {
    document.body.dataset.mode = state.mode;
    $$('.mode-tab').forEach((button) => {
        button.classList.toggle('active', button.dataset.mode === state.mode);
    });
    $$('#page-fusion > .panel').forEach((panel) => {
        panel.classList.toggle('active', panel.id === `panel-${state.mode}`);
    });
    syncLayoutInputs();
    renderSummary();
    syncGeometryPanel();
    syncMaskPanel();
    syncBlendPanel();
    syncColorPanel();
    syncAdvancedPanel();
    updateBackgroundInfo();
    syncPreviewPlacement();
    renderPreview();
}

function hasVisibleDebugGuide() {
    return Object.values(state.geometry || {}).some((geometry) => !!geometry?.showGrid)
        || !!state.mask?.showGrid
        || !!state.managerMode;
}

async function enterMode(mode, options = {}) {
    const resetGuides = options.resetGuides === true;
    const preserveActiveRegion = options.preserveActiveRegion === true;
    if (!preserveActiveRegion && ['geometry', 'mask', 'blend'].includes(mode)) {
        await resetDebugModeFocusToDefaultRegion();
    }
    const regionId = state.activeRegionId;

    if (resetGuides) {
        await closeAllDebugGuides();
    }

    if (mode === 'geometry') {
        const regionIds = getRegionIds();
        const targetIds = regionIds.length ? regionIds : [regionId];
        await Promise.all(targetIds.map((id) => ensureGeometryLoaded(id)));
        if (!resetGuides) {
            await syncActiveGeometrySelection(regionId);
        }
    }

    if (mode === 'mask') {
        const regionIds = getRegionIds();
        const targetIds = regionIds.length ? regionIds : [regionId];
        await Promise.all(targetIds.map((id) => ensureGeometryLoaded(id)));
        if (state.mask) {
            clampSelectionToPerimeter(state.mask);
        }
        state.mask = hydrateMask(await fusionApi.loadMask());
        if (state.mask) clampSelectionToPerimeter(state.mask);
    }

    if (mode === 'blend') {
        const regionIds = getRegionIds();
        const targetIds = regionIds.length ? regionIds : [regionId];
        await Promise.all(targetIds.map((id) => ensureGeometryLoaded(id)));
        await Promise.all(targetIds.map(async (id) => {
            state.blend[id] = normalizeBlend(await fusionApi.loadBlend(id));
        }));
        if (!resetGuides) {
            await syncActiveGeometrySelection(regionId);
        }
    }

    await setDebugBackgroundForMode(hasVisibleDebugGuide());
}

async function closeGeometryGuideMode() {
    const ids = getTargetRegionIds();
    await Promise.all(ids.map((id) => ensureGeometryLoaded(id)));
    ids.forEach((id) => {
        if (state.geometry[id]) state.geometry[id].showGrid = false;
    });
    await fusionApi.showGeometryGridAll(false);
}

async function closeMaskGuideMode() {
    if (state.mask) state.mask.showGrid = false;
    await fusionApi.setMaskGuideVisibility(false);
}

async function closeBlendGuideMode() {
    const ids = getTargetRegionIds();
    state.managerMode = false;
    await fusionApi.setManagerMode(false);
    await Promise.all(ids.map(async (id) => {
        state.blend[id] = normalizeBlend(await fusionApi.loadBlend(id));
    }));
}

async function closeAllDebugGuides(except = '') {
    if (except !== 'geometry') await closeGeometryGuideMode();
    if (except !== 'mask') await closeMaskGuideMode();
    if (except !== 'blend' && state.managerMode) await closeBlendGuideMode();
}

async function setGeometryGuideModeVisible(visible) {
    const show = !!visible;
    const regionId = await resetDebugModeFocusToDefaultRegion();
    const ids = getTargetRegionIds();
    if (!show) {
        await closeGeometryGuideMode();
        await setDebugBackgroundForMode(false);
        return;
    }

    await closeAllDebugGuides('geometry');
    state.mode = 'geometry';
    await Promise.all(ids.map((id) => ensureGeometryLoaded(id)));
    ids.forEach((id) => {
        if (state.geometry[id]) state.geometry[id].showGrid = true;
    });
    await syncGeometryGuideVisibilityForRegions(ids, true);
    await fusionApi.setGeometrySelection(
        regionId,
        activeGeometry()?.selected?.row ?? 0,
        activeGeometry()?.selected?.col ?? 0
    );
    await setDebugBackgroundForMode(true);
}

async function setMaskGuideModeVisible(visible) {
    const show = !!visible;
    const regionId = await resetDebugModeFocusToDefaultRegion();
    if (!show) {
        await closeMaskGuideMode();
        await setDebugBackgroundForMode(false);
        return;
    }

    await closeAllDebugGuides('mask');
    state.mode = 'mask';
    if (!state.mask) {
        state.mask = hydrateMask(await fusionApi.loadMask());
    }
    if (state.mask) {
        state.mask.showGrid = true;
        clampSelectionToPerimeter(state.mask);
    }
    state.managerMode = false;
    await fusionApi.setManagerMode(false).catch(() => {});
    await fusionApi.showGeometryGridAll(false);
    await fusionApi.setMask(maskPayload(state.mask));
    state.mask = hydrateMask(await fusionApi.loadMask());
    await setDebugBackgroundForMode(true);
    await fusionApi.setActiveRegion(regionId).catch(() => {});
}

async function setBlendGuideModeVisible(visible) {
    const show = !!visible;
    const regionId = await resetDebugModeFocusToDefaultRegion();
    const ids = getTargetRegionIds();
    if (!show) {
        await closeBlendGuideMode();
        await setDebugBackgroundForMode(false);
        return;
    }

    await closeAllDebugGuides('blend');
    state.mode = 'blend';
    await ensureBlendMasterEnabled();
    await Promise.all(ids.map((id) => ensureGeometryLoaded(id)));
    const geo = activeGeometry();
    const selection = geo ? nearestGeometryCorner(geo) : { row: 0, col: 0 };
    if (state.mask) state.mask.showGrid = false;
    state.managerMode = true;
    if (state.mask) await fusionApi.setMask(maskPayload(state.mask));
    await fusionApi.setManagerMode(true);
    await Promise.all(ids.map(async (id) => {
        state.blend[id] = normalizeBlend(await fusionApi.loadBlend(id));
    }));
    await fusionApi.setGeometrySelection(regionId, selection.row, selection.col);
    state.activeSide = managerSideForDirection(geo, 2);
    await setDebugBackgroundForMode(true);
}

async function hideBackground() {
    state.background.debugModeActive = false;
    state.background.autoDebugVisible = false;
    state.background.userVisible = false;
    await setBackgroundVisible(false).catch(() => {});
    await resumeVideosAfterBackground();
    await removeBackgroundLayer();
    updateBackgroundInfo();
}

async function restoreGeometryOnly(options = {}) {
    const ids = getTargetRegionIds();
    for (const regionId of ids) {
        const showGrid = !!state.geometry[regionId]?.showGrid;
        const geo = createDefaultGeometry(showGrid);
        geo.regionId = regionId;
        state.geometry[regionId] = geo;
        await fusionApi.showGeometryGrid(regionId, showGrid);
        await fusionApi.setGeometryGrid(regionId, geo.rows, geo.cols, geo.interpolationMode);
        await fusionApi.setGeometryPoints(regionId, geometryPointTuples(geo), geo.rows, geo.cols, geo.interpolationMode);
        await fusionApi.saveGeometry(regionId);
        state.geometry[regionId] = hydrateGeometry(regionId, await fusionApi.loadGeometry(regionId));
    }
    await fusionApi.setGeometrySelection(state.activeRegionId, 0, 0);
    if (state.geometry[state.activeRegionId]) {
        state.geometry[state.activeRegionId].selected = { row: 0, col: 0 };
    }
    if (options.hideBackground !== false) {
        await hideBackground();
    }
}

async function restoreMaskOnly() {
    const mask = createDefaultMask(!!state.mask?.showGrid);
    state.mask = mask;
    await fusionApi.setMask({
        show_guide: mask.showGrid,
        selected_row: mask.selected.row,
        selected_col: mask.selected.col,
        mask: {
            enabled: mask.enabled,
            rows: mask.rows,
            cols: mask.cols,
            interpolation_mode: mask.interpolationMode,
            vertices: flattenPoints(mask.points, { clampPoints: false })
        }
    });
    await fusionApi.saveMask();
    state.mask = hydrateMask(await fusionApi.loadMask());
    await hideBackground();
}

async function restoreBlendOnly() {
    await restoreGeometryOnly({ hideBackground: false });
    const ids = getTargetRegionIds();
    await Promise.all(ids.map(async (regionId) => {
        if (!state.geometry[regionId]) {
            state.geometry[regionId] = hydrateGeometry(regionId, await fusionApi.loadGeometry(regionId));
        }
        const blend = createResetBlendForRegion(regionId);
        state.blend[regionId] = blend;
        await fusionApi.saveBlend(regionId, blendPayload(blend, regionId));
        await Promise.all(['left', 'right', 'top', 'bottom'].flatMap((side) => [
            fusionApi.setMergeGapBrightness(regionId, side, 0, 128),
            fusionApi.setMergeGapBrightness(regionId, side, 1, 128),
            fusionApi.setMergeGapBrightness(regionId, side, 2, 128)
        ]));
        await fusionApi.persist();
        state.blend[regionId] = normalizeBlend(await fusionApi.loadBlend(regionId));
    }));
    await hideBackground();
}

async function loadActiveRegionDetails(regionId = state.activeRegionId) {
    const [geometry, blend, color, correction, cave] = await Promise.all([
        fusionApi.loadGeometry(regionId),
        fusionApi.loadBlend(regionId),
        fusionApi.loadColor(regionId),
        fusionApi.loadCorrection(regionId),
        fusionApi.loadCave(regionId)
    ]);
    state.geometry[regionId] = hydrateGeometry(regionId, geometry || {});
    state.blend[regionId] = normalizeBlend(blend || {});
    state.color[regionId] = {
        brightness: Number(color?.brightness ?? 1),
        contrast: Number(color?.contrast ?? 1),
        saturation: Number(color?.saturation ?? 1)
    };
    state.correction[regionId] = {
        enabled: !!correction?.enabled,
        offset_x: Number(correction?.offset_x ?? 0),
        offset_y: Number(correction?.offset_y ?? 0),
        scale_x: Number(correction?.scale_x ?? 1),
        scale_y: Number(correction?.scale_y ?? 1),
        rotate_rad: Number(correction?.rotate_rad ?? 0),
        keystone_x: Number(correction?.keystone_x ?? 0),
        keystone_y: Number(correction?.keystone_y ?? 0)
    };
    state.cave[regionId] = {
        enabled: !!cave?.enabled,
        wall_type: Number(cave?.wall_type ?? 0),
        llx: Number(cave?.llx ?? 0),
        lly: Number(cave?.lly ?? 0),
        llz: Number(cave?.llz ?? 0),
        ulx: Number(cave?.ulx ?? 0),
        uly: Number(cave?.uly ?? 0),
        ulz: Number(cave?.ulz ?? 0),
        lrx: Number(cave?.lrx ?? 0),
        lry: Number(cave?.lry ?? 0),
        lrz: Number(cave?.lrz ?? 0),
        near_plane: Number(cave?.near_plane ?? 0.1),
        far_plane: Number(cave?.far_plane ?? 100),
        eye_distance: Number(cave?.eye_distance ?? 0.065)
    };
}

async function loadAll() {
    await withBusy('正在加载融合状态...', async () => {
        const config = await fusionApi.loadRegionConfig();
        const inputRows = Number(config.grid_in_rows || 1);
        const inputCols = Number(config.grid_in_cols || 1);
        const sourceRegions = Array.isArray(config.regions) ? config.regions : [];
        const firstSourceRegion = sourceRegions.find((region) => Number(region.srcWidth || region.src_width || 0) > 0 && Number(region.srcHeight || region.src_height || 0) > 0) || {};
        const tileInWidth = Number(config.tile_in_width || firstSourceRegion.srcWidth || firstSourceRegion.src_width || 0);
        const tileInHeight = Number(config.tile_in_height || firstSourceRegion.srcHeight || firstSourceRegion.src_height || 0);
        const inputTotalWidth = Number(config.input_total_width) || (tileInWidth > 0 ? tileInWidth * inputCols : Number(config.canvas_in_width || 0));
        const inputTotalHeight = Number(config.input_total_height) || (tileInHeight > 0 ? tileInHeight * inputRows : Number(config.canvas_in_height || 0));
        state.layout = {
            ...state.layout,
            canvas_in_width: Number(config.canvas_in_width || 0),
            canvas_in_height: Number(config.canvas_in_height || 0),
            // 遮罩显示的是输入合成总幕布：优先 input_total_*，缺失时用输入单格 * 输入行列推导。
            input_total_width: inputTotalWidth,
            input_total_height: inputTotalHeight,
            canvas_out_width: Number(config.canvas_out_width || 0),
            canvas_out_height: Number(config.canvas_out_height || 0),
            tile_in_width: tileInWidth,
            tile_in_height: tileInHeight,
            tile_out_width: Number(config.tile_out_width || 0),
            tile_out_height: Number(config.tile_out_height || 0),
            grid_in_rows: inputRows,
            grid_in_cols: inputCols,
            grid_out_rows: Number(config.grid_out_rows || 1),
            grid_out_cols: Number(config.grid_out_cols || 1),
            merge_360: !!config.merge_360,
            rotation_angle: Number(config.rotation_angle || 0),
            split_direction: Number(config.split_direction || 0),
            mappings: Array.isArray(config.mappings) ? config.mappings : []
        };
        state.regions = Array.isArray(config.regions) ? config.regions : [];
        if (!state.regions.length) {
            const count = Math.max(1, state.layout.grid_out_rows * state.layout.grid_out_cols);
            state.regions = Array.from({ length: count }, (_, i) => ({ id: i + 1, output_index: i }));
        }
        state.masterEnabled = config.fusion_master_enabled === true;
        if (typeof config.blend_auto_edges === 'boolean') {
            state.blendAutoEdges = config.blend_auto_edges;
        }
        if (!getRegionIds().includes(state.activeRegionId)) setActiveRegionId(getRegionIds()[0] || 1);

        const [mask, manager] = await Promise.all([
            fusionApi.loadMask(),
            fusionApi.getManagerMode().catch(() => ({ enabled: false }))
        ]);
        state.mask = hydrateMask(mask || {});
        state.managerMode = !!manager?.enabled;
        if (typeof manager?.blend_auto_edges === 'boolean') {
            state.blendAutoEdges = manager.blend_auto_edges;
        }
        await loadActiveRegionDetails(state.activeRegionId);
        if (['geometry', 'mask', 'blend'].includes(state.mode)) {
            await enterMode(state.mode, { resetGuides: true });
        }
    });
    renderAll();
    await loadBackgroundItems();
}

function renderLayerPage() {
    const layers = appState.layers || [];
    const runtimeById = Object.fromEntries((appState.runtimeLayers || []).map((layer) => [Number(layer.id), layer]));
    const visibleCount = layers.filter((layer) => (runtimeById[Number(layer.id)]?.visible ?? layer.visible) !== false).length;
    const videoCount = layers.filter((layer) => layer.type === 'video').length;
    setText('#layerTotalCount', String(layers.length));
    setText('#layerVisibleCount', String(visibleCount));
    setText('#layerVideoCount', String(videoCount));
    const host = $('#layerList');
    if (!host) return;
    if (!layers.length) {
        host.innerHTML = emptyList('暂无图层数据');
        renderLayerEditor();
        return;
    }
    if (!appState.selectedLayerId || !layers.some((layer) => Number(layer.id) === Number(appState.selectedLayerId))) {
        appState.selectedLayerId = Number(layers[0].id);
    }
    host.innerHTML = layers.map((layer) => {
        const runtime = runtimeById[Number(layer.id)] || {};
        const visible = runtime.visible ?? layer.visible;
        const selected = Number(layer.id) === Number(appState.selectedLayerId);
        const title = escapeHtml(layer.name || layer.label || `图层 ${layer.id}`);
        const size = runtime.size || layer.size || {};
        const position = runtime.position || layer.position || {};
        return `
            <div class="mobile-list-item ${selected ? 'selected' : ''}" data-layer-select="${layer.id}">
                <div>
                    <strong>${title}</strong>
                    <span>${formatLayerType(layer.type)} · ID ${layer.id}</span>
                    <small>${Math.round(position.x ?? layer.x ?? 0)}, ${Math.round(position.y ?? layer.y ?? 0)} · ${Math.round(size.width ?? layer.width ?? 0)}×${Math.round(size.height ?? layer.height ?? 0)}</small>
                </div>
                <button class="ghost-btn small-btn" data-layer-visible="${layer.id}" data-visible="${visible !== false}" type="button">${visible !== false ? '隐藏' : '显示'}</button>
            </div>
        `;
    }).join('');
    renderLayerEditor();
}

function renderLayerEditor() {
    const host = $('#layerEditor');
    if (!host) return;
    if (!appState.selectedLayerId) {
        host.innerHTML = emptyList('选择一个图层进行调试');
        return;
    }
    const layer = mergedLayer(appState.selectedLayerId);
    if (!layer.id) {
        host.innerHTML = emptyList('选择一个图层进行调试');
        return;
    }
    const position = layer.position || {};
    const size = layer.size || {};
    const visible = layer.visible !== false;
    const alpha255 = Math.round(clamp(numberOr(layer.alpha, 1) * 255, 0, 255));
    const title = escapeHtml(layer.name || layer.label || `图层 ${layer.id}`);
    const type = formatLayerType(layer.type);
    const isVideoLike = layer.type === 'video' || layer.type === 'capture';
    const volumePct = Math.round(clamp(numberOr(layer.volume, 1) * 100, 0, 100));
    const blur = Math.round(numberOr(layer.gaussian_blur ?? layer.gaussianBlur, 0));
    const fitMode = Number(layer.fit_mode ?? layer.fitMode ?? 0) > 0 ? 1 : 0;
    const invert = Math.round(numberOr(layer.invert, 0));
    const shapeType = Math.round(numberOr(layer.shape_type ?? layer.shapeType, 0));
    const shapeParam = numberOr(layer.shape_param ?? layer.shapeParam, 0);
    const blackTransparent = !!(layer.black_to_transparent ?? layer.blackToTransparent);
    host.innerHTML = `
        <div class="mobile-editor-head">
            <div>
                <strong>${title}</strong>
                <span>${type} · ID ${layer.id}</span>
            </div>
            <button class="ghost-btn small-btn" data-layer-editor-visible="${layer.id}" data-visible="${visible}" type="button">${visible ? '隐藏' : '显示'}</button>
        </div>
        <div class="mobile-editor-actions">
            <button class="ghost-btn small-btn" data-layer-priority-delta="-1" type="button">下移</button>
            <button class="ghost-btn small-btn" data-layer-priority-delta="1" type="button">上移</button>
            <button class="ghost-btn small-btn" data-layer-fill-screen type="button">铺满</button>
        </div>
        <div class="mobile-field-grid">
            <label class="field"><span>X</span><input data-layer-field="x" type="number" step="1" inputmode="numeric" value="${Math.round(numberOr(position.x ?? layer.x, 0))}"></label>
            <label class="field"><span>Y</span><input data-layer-field="y" type="number" step="1" inputmode="numeric" value="${Math.round(numberOr(position.y ?? layer.y, 0))}"></label>
            <label class="field"><span>宽</span><input data-layer-field="width" type="number" min="1" step="1" inputmode="numeric" value="${Math.round(numberOr(size.width ?? layer.width, 0))}"></label>
            <label class="field"><span>高</span><input data-layer-field="height" type="number" min="1" step="1" inputmode="numeric" value="${Math.round(numberOr(size.height ?? layer.height, 0))}"></label>
            <label class="field"><span>旋转</span><input data-layer-field="rotation" type="number" min="0" max="360" step="1" inputmode="numeric" value="${Math.round(numberOr(layer.rotation, 0))}"></label>
            <label class="field"><span>层级</span><input data-layer-field="priority" type="number" step="1" inputmode="numeric" value="${Math.round(numberOr(layer.priority, layer.id))}"></label>
            <label class="field"><span>缩放</span><input data-layer-field="scale" type="number" min="0.01" step="0.01" inputmode="decimal" value="${numberOr(layer.scale, 1).toFixed(2)}"></label>
        </div>
        <label class="slider-field">
            <span>透明度 <strong id="layerEditorAlphaValue">${alpha255}</strong></span>
            <input data-layer-field="alpha" type="range" min="0" max="255" step="1" value="${alpha255}">
        </label>
        ${isVideoLike ? `
            <label class="slider-field">
                <span>图层音量 <strong id="layerEditorVolumeValue">${volumePct}%</strong></span>
                <input data-layer-field="volume" type="range" min="0" max="100" step="1" value="${volumePct}">
            </label>
            <div class="mobile-field-grid">
                <label class="field"><span>高斯模糊</span><input data-layer-field="gaussian_blur" type="number" min="0" max="10" step="1" inputmode="numeric" value="${blur}"></label>
                <label class="field"><span>填充模式</span><select data-layer-field="fit_mode"><option value="0" ${fitMode === 0 ? 'selected' : ''}>铺满</option><option value="1" ${fitMode === 1 ? 'selected' : ''}>等比</option></select></label>
                <label class="field"><span>反转</span><select data-layer-field="invert"><option value="0" ${invert === 0 ? 'selected' : ''}>关闭</option><option value="1" ${invert === 1 ? 'selected' : ''}>开启</option></select></label>
            </div>
        ` : ''}
        <details class="details-box mobile-layer-details">
            <summary>形状与透明</summary>
            <div class="mobile-field-grid">
                <label class="field"><span>形状</span><select data-layer-field="shape_type">
                    <option value="0" ${shapeType === 0 ? 'selected' : ''}>矩形</option>
                    <option value="1" ${shapeType === 1 ? 'selected' : ''}>圆形</option>
                    <option value="2" ${shapeType === 2 ? 'selected' : ''}>三角</option>
                    <option value="3" ${shapeType === 3 ? 'selected' : ''}>圆角</option>
                    <option value="4" ${shapeType === 4 ? 'selected' : ''}>星形</option>
                    <option value="5" ${shapeType === 5 ? 'selected' : ''}>六边形</option>
                    <option value="6" ${shapeType === 6 ? 'selected' : ''}>菱形</option>
                    <option value="7" ${shapeType === 7 ? 'selected' : ''}>心形</option>
                    <option value="8" ${shapeType === 8 ? 'selected' : ''}>花瓣</option>
                </select></label>
                <label class="field"><span>形状参数</span><input data-layer-field="shape_param" type="number" min="0" max="1" step="0.01" inputmode="decimal" value="${shapeParam.toFixed(2)}"></label>
                <label class="field"><span>黑色透明</span><select data-layer-field="black_to_transparent"><option value="false" ${!blackTransparent ? 'selected' : ''}>关闭</option><option value="true" ${blackTransparent ? 'selected' : ''}>开启</option></select></label>
            </div>
        </details>
    `;
}

function layerEditorValue(field, fallback = 0) {
    const el = $(`#layerEditor [data-layer-field="${field}"]`);
    if (!el) return fallback;
    const value = el.value;
    if (value === 'true') return true;
    if (value === 'false') return false;
    const number = Number(value);
    return Number.isFinite(number) ? number : fallback;
}

function readLayerEditorPatch() {
    const layerId = Number(appState.selectedLayerId);
    const layer = mergedLayer(layerId);
    const patch = {
        position: {
            x: Math.round(layerEditorValue('x', layer.position?.x ?? 0)),
            y: Math.round(layerEditorValue('y', layer.position?.y ?? 0))
        },
        size: {
            width: Math.max(1, Math.round(layerEditorValue('width', layer.size?.width ?? 1))),
            height: Math.max(1, Math.round(layerEditorValue('height', layer.size?.height ?? 1)))
        },
        rotation: clamp(Math.round(layerEditorValue('rotation', layer.rotation ?? 0)), 0, 360),
        priority: Math.round(layerEditorValue('priority', layer.priority ?? layerId)),
        scale: Math.max(0.01, layerEditorValue('scale', layer.scale ?? 1)),
        alpha: clamp(layerEditorValue('alpha', 255), 0, 255) / 255,
        shape_type: Math.round(layerEditorValue('shape_type', layer.shape_type ?? layer.shapeType ?? 0)),
        shape_param: clamp(layerEditorValue('shape_param', layer.shape_param ?? layer.shapeParam ?? 0), 0, 1),
        black_to_transparent: layerEditorValue('black_to_transparent', !!(layer.black_to_transparent ?? layer.blackToTransparent))
    };
    if (layer.type === 'video' || layer.type === 'capture') {
        patch.volume = clamp(layerEditorValue('volume', 100), 0, 100) / 100;
        patch.gaussian_blur = Math.round(clamp(layerEditorValue('gaussian_blur', layer.gaussian_blur ?? layer.gaussianBlur ?? 0), 0, 10));
        patch.fit_mode = Number(layerEditorValue('fit_mode', layer.fit_mode ?? layer.fitMode ?? 0)) > 0 ? 1 : 0;
        patch.invert = Math.round(layerEditorValue('invert', layer.invert ?? 0));
    }
    return patch;
}

function patchLayerCache(layerId, patch) {
    [appState.layers, appState.runtimeLayers].forEach((list) => {
        const item = (list || []).find((layer) => Number(layer?.id) === Number(layerId));
        if (!item) return;
        Object.assign(item, patch);
        if (patch.position) item.position = { ...(item.position || {}), ...patch.position };
        if (patch.size) item.size = { ...(item.size || {}), ...patch.size };
    });
}

async function applyLayerPatch(layerId, patch, options = {}) {
    patchLayerCache(layerId, patch);
    if (options.render !== false) renderLayerPage();
    const results = await Promise.allSettled([
        layerApi.updateRuntime(layerId, patch),
        layerApi.updateConfig(layerId, patch)
    ]);
    if (results.every((result) => result.status === 'rejected')) {
        throw results[0].reason || new Error('图层同步失败');
    }
}

const syncLayerEditorPatch = debounce(async () => {
    if (!appState.selectedLayerId) return;
    const layerId = Number(appState.selectedLayerId);
    const patch = readLayerEditorPatch();
    patchLayerCache(layerId, patch);
    try {
        const results = await Promise.allSettled([
            layerApi.updateRuntime(layerId, patch),
            layerApi.updateConfig(layerId, patch)
        ]);
        if (results.every((result) => result.status === 'rejected')) {
            throw results[0].reason || new Error('图层参数同步失败');
        }
    } catch (error) {
        console.error(error);
        toast(error.message || '图层参数同步失败', 'error');
    }
}, 180);

async function loadLayerPage() {
    await withBusy('正在加载图层...', async () => {
        const [layers, runtimeLayers] = await Promise.all([
            layerApi.list().catch(() => []),
            layerApi.listRuntime().catch(() => [])
        ]);
        appState.layers = Array.isArray(layers) ? layers : [];
        appState.runtimeLayers = Array.isArray(runtimeLayers) ? runtimeLayers : [];
        renderLayerPage();
    });
}

function chooseVideoLayer(layers = appState.layers) {
    const videoLayer = (layers || []).find((layer) => layer.type === 'video') || (layers || [])[0];
    if (!appState.videoLayerId && videoLayer) appState.videoLayerId = Number(videoLayer.id);
    return appState.videoLayerId || Number(videoLayer?.id || 1);
}

function currentVideoStatus() {
    const status = appState.videoStatus || {};
    const layerId = Number(appState.videoLayerId || chooseVideoLayer());
    if (status.video_status && status.video_status[layerId]) return status.video_status[layerId];
    if (status.videoStatus && status.videoStatus[layerId]) return status.videoStatus[layerId];
    return status;
}

function renderVideoLayerPicker() {
    const host = $('#videoLayerPicker');
    if (!host) return;
    const layers = (appState.layers || []).filter((layer) => layer.type === 'video');
    const targetLayers = layers.length ? layers : appState.layers.slice(0, 6);
    host.innerHTML = targetLayers.map((layer) => `
        <button class="region-chip ${Number(layer.id) === Number(appState.videoLayerId) ? 'active' : ''}" data-video-layer="${layer.id}" type="button">${layer.id}</button>
    `).join('');
}

function renderVideoPage() {
    const layerId = chooseVideoLayer();
    const status = currentVideoStatus();
    const layer = mergedLayer(layerId);
    setText('#videoActiveLayerLabel', `图层 ${layerId}`);
    const duration = normalizeDuration(status.duration);
    const position = displayPositionForDuration(status.current_position ?? status.currentPosition ?? status.position, duration);
    appState.videoDuration = duration;
    syncVideoSeekClock(status, position, duration);
    const mediaName = fileName(status.path || status.current_path || status.currentPath);
    const timeText = duration > 0 ? `${formatTime(position)} / ${formatTime(duration)}` : formatTime(position);
    setText('#videoStatusText', `${formatVideoState(status)} · ${mediaName} · ${timeText}`);
    const playing = status.state === 'playing' || status.status === 'playing';
    const systemVolumeValue = Number($('#systemVolumeSlider')?.value);
    const muted = status.muted === true || status.isMuted === true || (Number.isFinite(systemVolumeValue) && systemVolumeValue <= 0);
    setText('#videoToggleBtn', playing ? '暂停' : '播放');
    setText('#videoMuteBtn', muted ? '取消静音' : '静音');
    const volume = clamp(Number(status.volume ?? layer.volume ?? 1), 0, 1);
    const volumePct = Math.round(volume * 100);
    setValue('videoLayerVolumeSlider', volumePct);
    setText('#videoLayerVolumeValue', `${volumePct}%`);
    const seekSlider = $('#videoSeekSlider');
    if (seekSlider && !appState.videoSeekDragging) {
        seekSlider.max = duration > 0 ? sliderSecondValue(duration) : '0';
        seekSlider.disabled = duration <= 0;
        seekSlider.value = sliderSecondValue(clampPlaybackPosition(position, duration, 0));
    }
    setText('#videoSeekValue', duration > 0 ? `${formatTime(position)} / ${formatTime(duration)}` : '--');
    renderVideoLayerPicker();
}

function syncSystemVolumeControl(payload) {
    const value = Number(payload?.volume ?? payload?.system_volume ?? payload?.systemVolume);
    if (!Number.isFinite(value)) return;
    const percent = Math.round(clamp(value, 0, 1) * 100);
    setValue('systemVolumeSlider', percent);
    setText('#systemVolumeValue', `${percent}%`);
}

async function loadVideoPage() {
    await withBusy('正在加载视频状态...', async () => {
        if (!appState.layers.length || !appState.runtimeLayers.length) {
            const [layers, runtimeLayers] = await Promise.all([
                appState.layers.length ? appState.layers : layerApi.list().catch(() => []),
                layerApi.listRuntime().catch(() => [])
            ]);
            appState.layers = Array.isArray(layers) ? layers : [];
            appState.runtimeLayers = Array.isArray(runtimeLayers) ? runtimeLayers : [];
        }
        const layerId = chooseVideoLayer();
        appState.videoStatus = await videoApi.status(layerId).catch(() => ({}));
        const systemVolume = await videoApi.command('getSystemVolume', {}).catch(() => null);
        syncSystemVolumeControl(systemVolume);
        renderVideoPage();
    });
}

function renderDmxGrid(values = []) {
    const host = $('#dmxMiniGrid');
    if (!host) return;
    const normalized = Array.from({ length: 12 }, (_, index) => Number(values[index] || 0));
    host.innerHTML = normalized.map((value, index) => {
        const percent = Math.max(0, Math.min(100, (value / 255) * 100));
        return `<div class="dmx-mini-cell" style="--level:${percent}%"><span>CH${index + 1}</span><strong>${value}</strong></div>`;
    }).join('');
    setValue('dmxMasterSlider', normalized[0] || 0);
    setText('#dmxMasterValue', String(normalized[0] || 0));
}

async function loadControlPage() {
    await withBusy('正在加载外设状态...', async () => {
        const [dmxStatus, dmxChannels, serialPorts] = await Promise.all([
            peripheralApi.dmxStatus().catch(() => null),
            peripheralApi.dmxChannels().catch(() => []),
            peripheralApi.listSerialPorts().catch(() => null)
        ]);
        const enabled = dmxStatus?.enabled ?? dmxStatus?.running ?? dmxStatus?.connected;
        setText('#dmxStatusText', enabled === false ? '未启用' : '可用');
        setText('#dmxModeText', dmxStatus?.mode ? `模式：${dmxStatus.mode}` : '通道实时监看');
        renderDmxGrid(Array.isArray(dmxChannels) ? dmxChannels : dmxChannels?.channels || []);
        const ports = Array.isArray(serialPorts?.ports) ? serialPorts.ports : Array.isArray(serialPorts) ? serialPorts : [];
        setText('#serialPortCount', `${ports.length} 个端口`);
        const serialHost = $('#serialPortList');
        if (serialHost) {
            serialHost.innerHTML = ports.length ? ports.map((port) => `
                <div class="mobile-list-item compact">
                    <div>
                        <strong>${port.name || port.port || port.path || port}</strong>
                        <span>${port.type || port.serial_type || 'serial'}</span>
                    </div>
                </div>
            `).join('') : emptyList('暂无串口端口');
        }
    });
}

async function loadSystemPage() {
    await withBusy('正在加载系统状态...', async () => {
        const [status, resources, network, device] = await Promise.all([
            systemApi.status().catch(() => ({})),
            systemApi.resources().catch(() => ({})),
            systemApi.networkInfo().catch(() => ({})),
            systemApi.deviceInfo().catch(() => ({}))
        ]);
        setText('#systemResolution', formatResolution(status));
        let cpuUsage = Number(resources?.cpu_usage);
        const cores = Number(resources?.cpu_cores || 0);
        if (cores > 0 && cpuUsage > 100) cpuUsage /= cores;
        setText('#systemCpu', formatPercent(cpuUsage));
        setText('#systemMemory', resources?.memory ? formatPercent(resources.memory.usage_percent) : '--');
        setText('#systemDeviceName', network?.device_name || device?.model || 'HVIDEO');
        setText('#systemDeviceDetail', `${network?.primary_ip || device?.ip || '未知IP'} · ${device?.serial || status?.app_version || '设备信息'}`);
        const info = [
            ['设备类型', status?.device_type ?? '--'],
            ['屏幕旋转', `${status?.screen_rotate || 0}°`],
            ['版本', status?.app_version || '--'],
            ['MAC', device?.mac || network?.mac || '--']
        ];
        const host = $('#systemInfoList');
        if (host) {
            host.innerHTML = info.map(([label, value]) => `
                <div class="mobile-list-item compact">
                    <div><strong>${label}</strong><span>${value}</span></div>
                </div>
            `).join('');
        }
    });
}

async function loadAppPage(page = appState.page) {
    if (page === 'layer') return loadLayerPage();
    if (page === 'video') return loadVideoPage();
    if (page === 'control') return loadControlPage();
    if (page === 'system') return loadSystemPage();
    return loadAll();
}

async function switchAppPage(page) {
    if (page !== 'fusion') {
        await setDebugBackgroundForMode(false).catch(() => {});
    }
    appState.page = page;
    $$('.mobile-page').forEach((item) => item.classList.toggle('active', item.id === `page-${page}`));
    $$('.bottom-nav-item').forEach((item) => item.classList.toggle('active', item.dataset.appPage === page));
    const activePage = $(`#page-${page}`);
    const title = activePage?.dataset.pageTitle || appPageLabel(page);
    setText('#pageTitle', title);
    const saveAllBtn = $('#saveAllBtn');
    if (saveAllBtn) saveAllBtn.style.display = page === 'fusion' ? '' : 'none';
    window.scrollTo(0, 0);
    await loadAppPage(page);
}

async function switchRegion(regionId) {
    setActiveRegionId(regionId);
    await fusionApi.setActiveRegion(state.activeRegionId).catch(() => {});
    if (!state.geometry[state.activeRegionId]) await loadActiveRegionDetails(state.activeRegionId);
    if (['geometry', 'mask', 'blend'].includes(state.mode)) {
        await enterMode(state.mode, { preserveActiveRegion: true });
    }
    renderAll();
}

function bindModeTabs() {
    $$('.mode-tab').forEach((button) => {
        button.addEventListener('click', async () => {
            state.mode = button.dataset.mode;
            $$('.mode-tab').forEach((item) => item.classList.toggle('active', item === button));
            $$('#page-fusion > .panel').forEach((panel) => panel.classList.toggle('active', panel.id === `panel-${state.mode}`));
            await withBusy(`正在进入${modeLabel(state.mode)}模式...`, () => enterMode(state.mode, { resetGuides: true }));
            renderAll();
        });
    });
}

function bindPreview() {
    $('#previewCanvas').addEventListener('click', async (event) => {
        if (state.mode === 'geometry' || state.mode === 'blend') {
            const point = geometryPointFromCanvasEvent(event);
            if (point) {
                const geo = activeGeometry();
                geo.selected = point;
                await fusionApi.setGeometrySelection(state.activeRegionId, point.row, point.col);
                renderAll();
                return;
            }
        }
        if (state.mode === 'mask') {
            const point = maskPointFromCanvasEvent(event);
            if (point && state.mask) {
                state.mask.selected = point;
                await fusionApi.setMask(maskPayload(state.mask));
                renderAll();
                return;
            }
            return;
        }
        const regionId = regionFromCanvasEvent(event);
        if (regionId) await switchRegion(regionId);
    });
}

function bindLayout() {
    $('#regionPicker').addEventListener('click', async (event) => {
        const button = event.target.closest('[data-region-id]');
        if (!button) return;
        await switchRegion(Number(button.dataset.regionId));
    });
    $('#applyLayoutBtn').addEventListener('click', () => withBusy('正在应用矩阵布局...', async () => {
        const layout = collectLayout();
        await fusionApi.setFlexibleMapping(layout);
        await loadAll();
    }, '布局已应用'));
    $('#fusionMasterToggle').addEventListener('change', async (event) => {
        state.masterEnabled = event.target.checked;
        const response = await fusionApi.setMaster(state.masterEnabled);
        if (typeof response?.blend_auto_edges === 'boolean') {
            state.blendAutoEdges = response.blend_auto_edges;
        }
        renderAll();
    });
    $('#blendMasterToggle').addEventListener('change', async (event) => {
        state.masterEnabled = event.target.checked;
        const response = await fusionApi.setMaster(state.masterEnabled);
        if (typeof response?.blend_auto_edges === 'boolean') {
            state.blendAutoEdges = response.blend_auto_edges;
        }
        renderAll();
    });
}

function bindGeometry() {
    $('#geometryMoveModeBtn').addEventListener('click', () => {
        renderAll();
    });
    $('#geometryGuideToggle').addEventListener('change', async (event) => {
        await runGuideToggle(event, {
            opening: '正在打开几何网格...',
            closing: '正在关闭几何网格...',
            opened: '几何网格已显示',
            closed: '几何网格已隐藏'
        }, setGeometryGuideModeVisible);
    });
    $$('#panel-geometry [data-resize]').forEach((button) => {
        button.addEventListener('click', () => withBusy('正在同步几何网格...', async () => {
            const ids = getRegionIds();
            const regionIds = ids.length ? ids : [state.activeRegionId];
            await Promise.all(regionIds.map(async (regionId) => {
                const response = await fusionApi.resizeGeometry(regionId, button.dataset.resize);
                state.geometry[regionId] = hydrateGeometry(regionId, response || await fusionApi.loadGeometry(regionId));
            }));
            renderAll();
        }, '几何网格已同步'));
    });
    $('#restoreGeometryBtn').addEventListener('click', async () => {
        await withBusy('正在还原几何...', restoreGeometryOnly, '几何已还原');
        renderAll();
    });
    $('#saveGeometryBtn').addEventListener('click', () => withBusy('正在保存几何...', () => fusionApi.saveGeometry(state.activeRegionId), '几何已保存'));
}

function bindPreviewGridResize() {
    $$('#fusionPreviewPanel [data-grid-resize]').forEach((button) => {
        button.addEventListener('click', () => withBusy('正在同步网格...', async () => {
            const op = button.dataset.gridResize;
            if (state.mode === 'mask') {
                await fusionApi.resizeMask(op);
                state.mask = hydrateMask(await fusionApi.loadMask());
                renderAll();
                return;
            }

            const ids = getRegionIds();
            const regionIds = ids.length ? ids : [state.activeRegionId];
            await Promise.all(regionIds.map(async (regionId) => {
                if (state.mode === 'blend') {
                    const response = await fusionApi.resizeGeometry(regionId, op);
                    state.blend[regionId] = normalizeBlend(response || await fusionApi.loadBlend(regionId));
                } else {
                    const response = await fusionApi.resizeGeometry(regionId, op);
                    state.geometry[regionId] = hydrateGeometry(regionId, response || await fusionApi.loadGeometry(regionId));
                }
            }));
            renderAll();
        }, '网格已同步'));
    });
}

function bindMask() {
    $('#maskMoveModeBtn').addEventListener('click', () => {
        state.maskOp = 'point';
        renderAll();
    });
    $('#maskEnabledToggle').addEventListener('change', async (event) => {
        state.mask.enabled = event.target.checked;
        await fusionApi.setMask(maskPayload(state.mask));
        renderAll();
    });
    $('#maskGuideToggle').addEventListener('change', async (event) => {
        await runGuideToggle(event, {
            opening: '正在打开遮罩网格...',
            closing: '正在关闭遮罩网格...',
            opened: '遮罩网格已显示',
            closed: '遮罩网格已隐藏'
        }, setMaskGuideModeVisible);
    });
    $$('#panel-mask [data-mask-resize]').forEach((button) => {
        button.addEventListener('click', async () => {
            await fusionApi.resizeMask(button.dataset.maskResize);
            state.mask = hydrateMask(await fusionApi.loadMask());
            renderAll();
        });
    });
    $('#restoreMaskBtn').addEventListener('click', async () => {
        await withBusy('正在还原遮罩...', restoreMaskOnly, '遮罩已还原');
        renderAll();
    });
    $('#saveMaskBtn').addEventListener('click', () => withBusy('正在保存遮罩...', () => fusionApi.saveMask(), '遮罩已保存'));
}

function bindNudgePads() {
    const directionMap = {
        up: [0, -1, 0, -1],
        down: [0, 1, 0, 1],
        left: [-1, 0, -1, 0],
        right: [1, 0, 1, 0]
    };
    const pressState = new WeakMap();

    const runBlendNudge = async (dir, longPress = false) => {
        const geo = activeGeometry();
        if (!geo) return;
        const [, , sx, sy] = directionMap[dir];
        const direction = directionCode(dir);
        if (direction < 0) return;
        const fastMove = $('#blendFastMoveToggle')?.checked === true;
        const wholeMove = $('#blendWholeMoveToggle')?.checked === true;
        const { dx, dy: geometryDy } = geometryDeltaFromPixels(sx, sy, fastMove);
        if (!state.managerMode) {
            state.managerMode = true;
            await fusionApi.setManagerMode(true);
        }
        const selection = nearestGeometryCorner(geo);
        await fusionApi.setGeometrySelection(state.activeRegionId, selection.row, selection.col);

        if (wholeMove) {
            moveLocal(geo, 'all', dx, geometryDy, { clampPoints: false });
            const response = await fusionApi.moveGeometry(state.activeRegionId, 'all', dx, geometryDy);
            if (response) state.geometry[state.activeRegionId] = hydrateGeometry(state.activeRegionId, response);
            renderAll();
            return;
        }

        if (longPress) {
            state.activeSide = managerSideForDirection(geo, direction);
            await fusionApi.moveManagerLine(
                state.activeRegionId,
                direction,
                dx,
                geometryDy,
                { row: selection.row, col: selection.col }
            );
            renderAll();
            return;
        }

        const corner = managerCornerName(geo);
        await fusionApi.moveManagerPoint(
            state.activeRegionId,
            direction,
            dx,
            geometryDy,
            corner
        );
        renderAll();
    };

    const runGeometryNudge = async (dir, longPress = false) => {
        const geo = activeGeometry();
        if (!geo) return;
        const [, , sx, sy] = directionMap[dir];
        const fastMove = $('#geometryFastMoveToggle')?.checked === true;
        const wholeMove = $('#geometryWholeMoveToggle')?.checked === true;
        const { dx, dy: geometryDy } = geometryDeltaFromPixels(sx, sy, fastMove);
        const op = wholeMove ? 'all' : (longPress ? geometryAxisForDirection(dir) : 'point');
        if (longPress) state.geometryLineAxis = op;
        if (wholeMove) {
            moveLocal(geo, op, dx, geometryDy, { clampPoints: false });
            const response = await fusionApi.moveGeometry(state.activeRegionId, op, dx, geometryDy);
            if (response) state.geometry[state.activeRegionId] = hydrateGeometry(state.activeRegionId, response);
            renderAll();
            return;
        }
        moveLocal(geo, op, dx, geometryDy, { clampPoints: false });
        await fusionApi.moveGeometry(state.activeRegionId, op, dx, geometryDy);
        renderAll();
    };
    const runMaskNudge = async (dir, longPress = false) => {
        if (!state.mask) return;
        const [, , sx, sy] = directionMap[dir];
        const fastMove = $('#maskFastMoveToggle')?.checked === true;
        const { dx, dy } = maskDeltaFromPixels(sx, sy, fastMove);
        const op = 'point';
        clampSelectionToPerimeter(state.mask);
        moveLocal(state.mask, op, dx, dy, { clampPoints: false });
        await fusionApi.moveMask(op, dx, dy, state.mask.selected.row, state.mask.selected.col);
        renderAll();
    };
    $$('.nudge-pad button[data-dir]').forEach((button) => {
        const startPress = (event) => {
            const pad = button.closest('.nudge-pad').dataset.pad;
            if (pad !== 'geometry' && pad !== 'blend' && pad !== 'mask') return;
            event.preventDefault();
            const dir = button.dataset.dir;
            const press = { longFired: false, repeat: null };
            press.timer = setTimeout(() => {
                press.longFired = true;
                const runner = pad === 'blend' ? runBlendNudge : pad === 'mask' ? runMaskNudge : runGeometryNudge;
                runner(dir, true).catch((error) => {
                    console.error(error);
                    toast(error.message || '长按移动失败', 'error');
                });
                press.repeat = setInterval(() => {
                    runner(dir, true).catch((error) => {
                        console.error(error);
                        toast(error.message || '长按移动失败', 'error');
                    });
                }, 140);
            }, 620);
            pressState.set(button, press);
            button.setPointerCapture?.(event.pointerId);
        };
        const endPress = (event) => {
            const pad = button.closest('.nudge-pad').dataset.pad;
            if (pad !== 'geometry' && pad !== 'blend' && pad !== 'mask') return;
            event.preventDefault();
            const dir = button.dataset.dir;
            const press = pressState.get(button);
            if (!press) return;
            clearTimeout(press.timer);
            clearInterval(press.repeat);
            pressState.delete(button);
            if (!press.longFired) {
                const runner = pad === 'blend' ? runBlendNudge : pad === 'mask' ? runMaskNudge : runGeometryNudge;
                runner(dir, false).catch((error) => {
                    console.error(error);
                    toast(error.message || '移动失败', 'error');
                });
            }
        };
        button.addEventListener('pointerdown', startPress);
        button.addEventListener('pointerup', endPress);
        button.addEventListener('pointercancel', endPress);
        button.addEventListener('click', async (event) => {
            const pad = button.closest('.nudge-pad').dataset.pad;
            if (pad === 'geometry' || pad === 'blend' || pad === 'mask') {
                event.preventDefault();
            }
        });
    });
}

function bindBlend() {
    const reloadBlendTargets = async (ids) => {
        await Promise.all(ids.map(async (regionId) => {
            state.blend[regionId] = normalizeBlend(await fusionApi.loadBlend(regionId));
        }));
    };
    const syncCurvePatch = async (patch, reload = true) => {
        if (!patch || !Object.keys(patch).length) return;
        const ids = getBlendEditRegionIds();
        await Promise.all(ids.map(async (regionId) => {
            const blend = await ensureBlendLoaded(regionId);
            Object.assign(blend[state.activeSide], patch);
        }));
        syncBlendPanel();
        await Promise.all(ids.map((regionId) => fusionApi.setBlendCurveParams(regionId, state.activeSide, patch)));
        if (reload) await reloadBlendTargets(ids);
        renderAll();
    };
    const updateCurveInput = async (key) => {
        const patch = {};
        if (key === 'gamma') patch.gamma = blendNumber($('#blendGammaInput').value, 1, 3);
        if (key === 'slope') patch.slope = blendNumber($('#blendSlopeInput').value, 1, 3);
        await syncCurvePatch(patch);
    };
    const updateSideEnabled = async (sideName, nextEnabled) => {
        const blend = activeBlend();
        if (!blend) return;
        state.activeSide = sideName;
        const side = blend[sideName];
        side.enabled = !!nextEnabled;
        if (side.enabled && Number(side.width || 0) <= 0.001) {
            side.width = getDefaultBlendWidth(state.activeRegionId, sideName);
        }
        syncBlendPanel();
        try {
            await fusionApi.saveBlend(state.activeRegionId, blendPayload(blend, state.activeRegionId));
        } catch (error) {
            await reloadBlendTargets([state.activeRegionId]).catch(() => {});
            renderAll();
            throw error;
        }
        await reloadBlendTargets([state.activeRegionId]);
        renderAll();
    };
    const updateBlendBrightness = async (colorId, inputId) => {
        const value = clamp(Math.round(numberValue(inputId, 128)), 0, 255);
        const ids = getBlendEditRegionIds();
        await Promise.all(ids.map(async (regionId) => {
            const blend = await ensureBlendLoaded(regionId);
            const next = Array.isArray(blend[state.activeSide].bright)
                ? blend[state.activeSide].bright.slice(0, 3)
                : [128, 128, 128];
            next[colorId] = value;
            blend[state.activeSide].bright = next;
        }));
        syncBlendPanel();
        await Promise.all(ids.map((regionId) => fusionApi.setMergeGapBrightness(regionId, state.activeSide, colorId, value)));
        await reloadBlendTargets(ids);
        renderAll();
    };
    const bindBrightnessDragControl = (input, colorId, color, updateBrightness) => {
        const host = input.closest('.brightness-drag-field') || input;
        if (!host || host.dataset.brightnessDragBound) return;
        host.dataset.brightnessDragBound = 'true';
        let pressTimer = null;
        let dragState = null;
        let pressPoint = null;
        const clearPressTimer = () => {
            if (pressTimer) {
                clearTimeout(pressTimer);
                pressTimer = null;
            }
            pressPoint = null;
        };
        const renderInlineDrag = (value) => updateBrightnessDragFill(input, value, color);
        host._renderBrightnessLevel = renderInlineDrag;
        const valueFromPointer = (event) => {
            const rect = host.getBoundingClientRect();
            const ratio = clamp((event.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
            return Math.round(ratio * 255);
        };
        const updateDrag = (event, final = false) => {
            if (!dragState || dragState.pointerId !== event.pointerId) return;
            const value = valueFromPointer(event);
            setValue(input.id, value);
            renderInlineDrag(value);
            updateBrightness(value, { reload: final });
            event.preventDefault();
        };
        const startDrag = (event) => {
            const value = clamp(parseInt(input.value || 128, 10), 0, 255);
            input.blur();
            dragState = { pointerId: event.pointerId };
            renderInlineDrag(value);
            host.classList.add('is-brightness-dragging');
            host.setPointerCapture?.(event.pointerId);
            event.preventDefault();
            updateDrag(event, false);
        };
        const finishDrag = (event) => {
            clearPressTimer();
            if (!dragState || dragState.pointerId !== event.pointerId) return;
            updateDrag(event, true);
            host.classList.remove('is-brightness-dragging');
            host.releasePointerCapture?.(event.pointerId);
            dragState = null;
        };
        host.addEventListener('pointerdown', (event) => {
            if (event.button !== undefined && event.button !== 0) return;
            if (event.target && event.target.closest('button')) return;
            clearPressTimer();
            pressPoint = { x: event.clientX, y: event.clientY };
            pressTimer = setTimeout(() => {
                pressTimer = null;
                startDrag(event);
            }, 320);
        });
        host.addEventListener('pointermove', (event) => {
            if (dragState) {
                updateDrag(event, false);
                return;
            }
            if (pressPoint && Math.hypot(event.clientX - pressPoint.x, event.clientY - pressPoint.y) > 8) {
                clearPressTimer();
            }
        });
        host.addEventListener('pointerup', finishDrag);
        host.addEventListener('pointercancel', finishDrag);
        host.addEventListener('pointerleave', clearPressTimer);
        renderInlineDrag(clamp(parseInt(input.value || 128, 10), 0, 255));
    };
    const bindCurveCanvas = () => {
        const canvas = $('#blendCurveCanvas');
        if (!canvas) return;
        let drag = null;
        const canvasPoint = (event) => {
            const rect = canvas.getBoundingClientRect();
            const scaleX = canvas.width / Math.max(1, rect.width);
            const scaleY = canvas.height / Math.max(1, rect.height);
            return {
                x: (event.clientX - rect.left) * scaleX,
                y: (event.clientY - rect.top) * scaleY
            };
        };
        const valuesFromPoint = (point) => {
            const layout = getBlendCurveHandleLayout(canvas, activeBlend()?.[state.activeSide] || {});
            return {
                x: clamp((point.x - layout.rect.x) / layout.rect.width, 0, 1),
                y: clamp(1 - ((point.y - layout.rect.y) / layout.rect.height), 0, 1)
            };
        };
        const patchFromDrag = (handleId, values) => {
            const side = activeBlend()?.[state.activeSide] || {};
            const stripStart = clamp(side.stripStart ?? 0, 0, 255);
            const stripEnd = clamp(side.stripEnd ?? 255, 0, 255);
            if (handleId === 'stripStart') {
                return { stripStart: Math.round(clamp(values.x * 255, 0, Math.max(0, stripEnd - 1))) };
            }
            if (handleId === 'stripEnd') {
                return { stripEnd: Math.round(clamp(values.x * 255, Math.min(255, stripStart + 1), 255)) };
            }
            return { anchor: Number(getBlendCurveAnchorFromDisplayValue(side, values.y).toFixed(3)) };
        };
        const applyDrag = (event, reload) => {
            if (!drag) return;
            const patch = patchFromDrag(drag.handleId, valuesFromPoint(canvasPoint(event)));
            const ids = getBlendEditRegionIds();
            ids.forEach((regionId) => {
                if (state.blend[regionId]?.[state.activeSide]) {
                    Object.assign(state.blend[regionId][state.activeSide], patch);
                }
            });
            drawBlendCurve();
            void syncCurvePatch(patch, reload).catch((error) => {
                console.error(error);
                toast(error.message || '融合曲线同步失败', 'error');
            });
        };
        canvas.addEventListener('pointerdown', (event) => {
            const side = activeBlend()?.[state.activeSide];
            if (!side) return;
            const point = canvasPoint(event);
            const layout = getBlendCurveHandleLayout(canvas, side);
            const inside = point.x >= layout.rect.x
                && point.x <= layout.rect.x + layout.rect.width
                && point.y >= layout.rect.y
                && point.y <= layout.rect.y + layout.rect.height;
            const nearest = layout.handles.reduce((best, handle) => {
                const distance = Math.hypot(handle.screen.x - point.x, handle.screen.y - point.y);
                return distance < best.distance ? { handle, distance } : best;
            }, { handle: null, distance: Number.POSITIVE_INFINITY });
            if (!inside && nearest.distance > 24) return;
            drag = {
                handleId: nearest.distance <= 24 ? nearest.handle.id : 'anchor',
                pointerId: event.pointerId
            };
            canvas.classList.add('is-dragging');
            canvas.setPointerCapture?.(event.pointerId);
            event.preventDefault();
            applyDrag(event, false);
        });
        canvas.addEventListener('pointermove', (event) => {
            if (!drag || drag.pointerId !== event.pointerId) return;
            event.preventDefault();
            applyDrag(event, false);
        });
        const endDrag = (event) => {
            if (!drag || drag.pointerId !== event.pointerId) return;
            event.preventDefault();
            applyDrag(event, true);
            canvas.releasePointerCapture?.(event.pointerId);
            canvas.classList.remove('is-dragging');
            drag = null;
        };
        canvas.addEventListener('pointerup', endDrag);
        canvas.addEventListener('pointercancel', endDrag);
    };

    $$('input[data-blend-side]').forEach((input) => {
        input.addEventListener('change', async () => {
            if (!state.masterEnabled) {
                toast('请先打开融合带', 'error');
                input.checked = false;
                return;
            }
            const sideName = input.dataset.blendSide;
            await updateSideEnabled(sideName, input.checked);
        });
    });
    $('#blendGuideToggle').addEventListener('change', async (event) => {
        await runGuideToggle(event, {
            opening: '正在打开融合网格...',
            closing: '正在关闭融合网格...',
            opened: '融合网格已显示',
            closed: '融合网格已隐藏'
        }, setBlendGuideModeVisible);
    });
    $('#blendMoveModeBtn').addEventListener('click', () => {
        renderAll();
    });
    $$('#panel-blend [data-blend-resize]').forEach((button) => {
        button.addEventListener('click', () => withBusy('正在同步融合网格...', async () => {
            const ids = getRegionIds();
            const regionIds = ids.length ? ids : [state.activeRegionId];
            await Promise.all(regionIds.map(async (regionId) => {
                const response = await fusionApi.resizeGeometry(regionId, button.dataset.blendResize);
                state.blend[regionId] = normalizeBlend(response || await fusionApi.loadBlend(regionId));
            }));
            renderAll();
        }, '融合网格已同步'));
    });
    [
        ['blendGammaDecrease', 'blendGammaIncrease', 'blendGammaReset', 'blendGammaInput', 'gamma', 1.8],
        ['blendSlopeDecrease', 'blendSlopeIncrease', 'blendSlopeReset', 'blendSlopeInput', 'slope', 1.0]
    ].forEach(([decreaseId, increaseId, resetId, inputId, key, defaultValue]) => {
        const input = $(`#${inputId}`);
        input.addEventListener('input', debounce(() => updateCurveInput(key), 120));
        [
            [decreaseId, 'decrease'],
            [increaseId, 'increase'],
            [resetId, 'reset']
        ].forEach(([buttonId, mode]) => {
            $(`#${buttonId}`).addEventListener('click', async () => {
                const min = Number(input.min || 0);
                const max = Number(input.max || 1);
                const step = Number(input.step || 0.1);
                const current = Number(input.value || defaultValue);
                let next = defaultValue;
                if (mode === 'decrease') next = current - step;
                if (mode === 'increase') next = current + step;
                const decimals = String(step).includes('.') ? String(step).split('.')[1].length : 0;
                input.value = String(Number(clamp(next, min, max).toFixed(decimals)));
                await updateCurveInput(key);
            });
        });
    });
    [
        ['blendBrightR', 'blendBrightRDecrease', 'blendBrightRIncrease', 'blendBrightRReset', 0],
        ['blendBrightG', 'blendBrightGDecrease', 'blendBrightGIncrease', 'blendBrightGReset', 1],
        ['blendBrightB', 'blendBrightBDecrease', 'blendBrightBIncrease', 'blendBrightBReset', 2]
    ].forEach(([inputId, decreaseId, increaseId, resetId, colorId]) => {
        const input = $(`#${inputId}`);
        const colors = ['239, 68, 68', '34, 197, 94', '59, 130, 246'];
        const syncBrightness = debounce(() => updateBlendBrightness(colorId, inputId), 120);
        const dragBrightness = (value, options = {}) => {
            input.value = String(clamp(Math.round(Number(value) || 0), 0, 255));
            updateBrightnessDragFill(input, input.value, colors[colorId]);
            if (options.reload) {
                void updateBlendBrightness(colorId, inputId);
            } else {
                syncBrightness();
            }
        };
        [
            [decreaseId, -1],
            [increaseId, 1],
            [resetId, 0]
        ].forEach(([buttonId, delta]) => {
            $(`#${buttonId}`).addEventListener('click', async () => {
                const current = parseInt(input.value || 128, 10);
                setValue(inputId, delta === 0 ? 128 : clamp(current + delta, 0, 255));
                await updateBlendBrightness(colorId, inputId);
            });
        });
        bindBrightnessDragControl(input, colorId, colors[colorId], dragBrightness);
    });
    bindCurveCanvas();
    $('#restoreBlendBtn').addEventListener('click', async () => {
        await withBusy('正在还原融合...', restoreBlendOnly, '融合已还原');
        renderAll();
    });
    $('#saveBlendBtn').addEventListener('click', () => withBusy('正在保存融合...', async () => {
        if (activeGeometry()) await fusionApi.saveGeometry(state.activeRegionId);
        if (activeBlend()) await fusionApi.saveBlend(state.activeRegionId, blendPayload(activeBlend(), state.activeRegionId));
        await fusionApi.persist();
    }, '融合参数已保存'));
}

function bindColor() {
    const update = debounce(async () => {
        const color = activeColor();
        color.brightness = Number($('#colorBrightness').value);
        color.contrast = Number($('#colorContrast').value);
        color.saturation = Number($('#colorSaturation').value);
        syncColorPanel();
        await fusionApi.saveColor(state.activeRegionId, color);
    }, 120);
    ['colorBrightness', 'colorContrast', 'colorSaturation'].forEach((id) => $(`#${id}`).addEventListener('input', update));
    $('#resetColorBtn').addEventListener('click', async () => {
        state.color[state.activeRegionId] = { brightness: 1, contrast: 1, saturation: 1 };
        syncColorPanel();
        await fusionApi.saveColor(state.activeRegionId, activeColor());
    });
    $('#saveColorBtn').addEventListener('click', () => withBusy('正在保存色彩...', () => fusionApi.persist(), '色彩已保存'));
}

function bindAdvanced() {
    $('#saveCorrectionBtn').addEventListener('click', () => withBusy('正在应用矩阵校正...', async () => {
        const correction = {
            enabled: $('#matrixCorrectionToggle').checked,
            offset_x: numberValue('offsetX', 0),
            offset_y: numberValue('offsetY', 0),
            scale_x: numberValue('scaleX', 1),
            scale_y: numberValue('scaleY', 1),
            rotate_rad: numberValue('rotateRad', 0),
            keystone_x: numberValue('keystoneX', 0),
            keystone_y: numberValue('keystoneY', 0)
        };
        state.correction[state.activeRegionId] = correction;
        await fusionApi.saveCorrection(state.activeRegionId, correction);
        await fusionApi.persist();
    }, '矩阵校正已应用'));

    $('#saveCaveBtn').addEventListener('click', () => withBusy('正在应用 CAVE 配置...', async () => {
        const cave = {
            enabled: $('#caveToggle').checked,
            wall_type: Math.round(numberValue('wallType', 0)),
            eye_distance: numberValue('eyeDistance', 0.065),
            near_plane: numberValue('nearPlane', 0.1),
            far_plane: numberValue('farPlane', 100),
            llx: numberValue('llx', 0),
            lly: numberValue('lly', 0),
            llz: numberValue('llz', 0),
            ulx: numberValue('ulx', 0),
            uly: numberValue('uly', 0),
            ulz: numberValue('ulz', 0),
            lrx: numberValue('lrx', 0),
            lry: numberValue('lry', 0),
            lrz: numberValue('lrz', 0)
        };
        state.cave[state.activeRegionId] = cave;
        await fusionApi.saveCave(state.activeRegionId, cave);
        await fusionApi.persist();
    }, 'CAVE 配置已应用'));

    $('#resetFusionBtn').addEventListener('click', async () => {
        if (!confirm('确定初始化融合配置？当前几何、遮罩、融合参数会重置。')) return;
        await withBusy('正在初始化融合...', async () => {
            await fusionApi.reset();
            resetLocalFusionDebugState();
            await Promise.all([
                fusionApi.showGeometryGridAll(false).catch(() => {}),
                fusionApi.setMaskGuideVisibility(false).catch(() => {}),
                fusionApi.setManagerMode(false).catch(() => {})
            ]);
            await loadAll();
            await hideBackground();
        }, '融合已初始化');
    });
}

function imageFileFromPath(filePath) {
    const normalized = String(filePath || '').replace(/\\/g, '/');
    const marker = '/image/';
    const idx = normalized.toLowerCase().indexOf(marker);
    if (idx >= 0) return normalized.substring(idx + marker.length);
    return normalized.split('/').pop();
}

async function loadBackgroundItems() {
    try {
        let items = await backgroundApi.listImages();
        if (!Array.isArray(items) || !items.length) {
            const allItems = await backgroundApi.listAllImages().catch(() => []);
            items = Array.isArray(allItems)
                ? allItems.filter((item) => {
                    const text = String(item?.path || item?.name || '').replace(/\\/g, '/').toLowerCase();
                    return text.includes('/gb_fusion/') || text.startsWith('gb_fusion/');
                })
                : [];
        }
        if (!Array.isArray(items) || !items.length) {
            await backgroundApi.refreshMaterials().catch(() => {});
            for (let i = 0; i < 20; i += 1) {
                const status = await backgroundApi.indexStatus().catch(() => null);
                if (!status || status.scanning === false) break;
                await new Promise((resolve) => setTimeout(resolve, 250));
            }
            items = await backgroundApi.listImages().catch(() => []);
            if (!Array.isArray(items) || !items.length) {
                const allItems = await backgroundApi.listAllImages().catch(() => []);
                items = Array.isArray(allItems)
                    ? allItems.filter((item) => {
                        const text = String(item?.path || item?.name || '').replace(/\\/g, '/').toLowerCase();
                        return text.includes('/gb_fusion/') || text.startsWith('gb_fusion/');
                    })
                    : [];
            }
        }
        state.background.items = Array.isArray(items)
            ? items.sort((a, b) => String(a.name || a.path || '').localeCompare(String(b.name || b.path || ''), undefined, { numeric: true }))
            : [];
        updateBackgroundInfo();
    } catch (error) {
        updateBackgroundInfo();
    }
}

async function ensureBackgroundLayer() {
    const runtimeLayers = await backgroundApi.listRuntimeLayers().catch(() => []);
    const exists = runtimeLayers
        .some((layer) => Number(layer?.id) === BACKGROUND_LAYER_ID);
    if (exists) return true;
    await backgroundApi.createRuntimeImageLayer(BACKGROUND_LAYER_ID, BACKGROUND_LAYER_PRIORITY);
    return true;
}

async function canvasSize() {
    const status = await backgroundApi.getCanvasStatus().catch(() => ({}));
    return {
        width: Number(status.canvas_in_width || state.layout.canvas_in_width || 1920),
        height: Number(status.canvas_in_height || state.layout.canvas_in_height || 1080)
    };
}

async function setBackgroundVisible(visible) {
    if (visible) {
        await ensureBackgroundLayer();
    } else {
        const exists = await backgroundApi.getLayer(BACKGROUND_LAYER_ID)
            .then(() => true)
            .catch(() => false);
        if (!exists) {
            state.background.visible = false;
            return;
        }
    }
    const size = await canvasSize();
    await backgroundApi.updateLayer(BACKGROUND_LAYER_ID, {
        visible,
        priority: BACKGROUND_LAYER_PRIORITY,
        position: { x: 0, y: 0 },
        size: { width: Math.round(size.width), height: Math.round(size.height) },
        rotation: 0,
        scale: 1
    });
    state.background.visible = visible;
}

async function removeBackgroundLayer() {
    await backgroundApi.removeRuntimeLayer(BACKGROUND_LAYER_ID)
        .catch((error) => {
            console.warn('[FusionMobile] Failed to remove runtime background layer:', error);
        });
    state.background.visible = false;
}

function cleanupBackgroundOnPageHide() {
    fetch('/api/v1/layers/actions/remove_runtime_layer', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ layerId: BACKGROUND_LAYER_ID }),
        keepalive: true
    }).catch(() => {});
}

async function loadVideoLayerIds() {
    if (state.background.videoLayerIds.length) return state.background.videoLayerIds;
    const [runtimeLayers, configLayers] = await Promise.all([
        backgroundApi.listRuntimeLayers().catch(() => []),
        backgroundApi.listLayers().catch(() => [])
    ]);
    const ids = [...runtimeLayers, ...configLayers]
        .filter((layer) => isVideoLayer(layer))
        .map((layer) => Number(layer?.id))
        .filter((id) => id && id !== BACKGROUND_LAYER_ID);
    state.background.videoLayerIds = [...new Set(ids)].length ? [...new Set(ids)] : VIDEO_LAYER_IDS;
    return state.background.videoLayerIds;
}

function isVideoLayer(layer) {
    if (!layer) return false;
    return layer.type === 1 || String(layer.type || '').toLowerCase() === 'video';
}

async function pauseVideosForBackground() {
    if (state.background.pausedVideoLayerIds.length) return;
    const ids = await loadVideoLayerIds();
    state.background.pausedVideoLayerIds = [];
    await Promise.all(ids.map(async (id) => {
        try {
            const status = await backgroundApi.videoCommand(id, 'getStatus');
            if (status?.state === 'playing') {
                state.background.pausedVideoLayerIds.push(id);
                await backgroundApi.videoCommand(id, 'pause');
            }
        } catch (error) {
            // 说明：即使某个图层状态获取失败，也保持背景可用。
        }
    }));
}

async function resumeVideosAfterBackground() {
    const ids = state.background.pausedVideoLayerIds.slice();
    state.background.pausedVideoLayerIds = [];
    await Promise.all(ids.map((id) => backgroundApi.videoCommand(id, 'resume').catch(() => {})));
}

function queueDebugBackgroundOperation(task) {
    state.background.debugOperation = state.background.debugOperation
        .catch(() => null)
        .then(task)
        .catch((error) => {
            console.warn('[FusionMobile] Debug background operation failed:', error);
            return false;
        });
    return state.background.debugOperation;
}

async function applyBackgroundAt(index, { silent = false, markUserVisible = false } = {}) {
    if (!state.background.items.length) {
        if (!silent) toast('gb_fusion 下没有背景图片', 'error');
        return false;
    }
    state.background.currentIndex = (index + state.background.items.length) % state.background.items.length;
    const apply = async () => {
        await setBackgroundVisible(true);
        await pauseVideosForBackground();
        const item = state.background.items[state.background.currentIndex];
        await backgroundApi.loadImage(BACKGROUND_LAYER_ID, imageFileFromPath(item.path));
        if (markUserVisible) state.background.userVisible = true;
        return true;
    };
    const result = silent
        ? await apply().catch(() => false)
        : await withBusy('正在切换调试背景...', apply, '背景已显示');
    updateBackgroundInfo();
    return result;
}

async function showBackgroundAt(index) {
    return applyBackgroundAt(index, { markUserVisible: !state.background.autoDebugVisible });
}

async function setDebugBackgroundForMode(active) {
    state.background.debugModeActive = !!active;
    return queueDebugBackgroundOperation(async () => {
        if (!active) {
            const shouldHide = state.background.autoDebugVisible && !state.background.userVisible;
            state.background.autoDebugVisible = false;
            if (!shouldHide) return false;
            await setBackgroundVisible(false).catch(() => {});
            await resumeVideosAfterBackground();
            await removeBackgroundLayer();
            updateBackgroundInfo();
            return true;
        }

        if (!state.background.items.length) {
            await loadBackgroundItems();
        }
        if (!state.background.items.length || !state.background.debugModeActive) return false;
        if (!state.background.visible) {
            state.background.autoDebugVisible = true;
        }
        return applyBackgroundAt(state.background.currentIndex, { silent: true });
    });
}

function updateBackgroundInfo() {
    const total = state.background.items.length;
    $('#backgroundInfo').textContent = total
        ? `${state.background.currentIndex + 1} / ${total}`
        : 'gb_fusion 无图片';
    $('#bgToggleBtn').textContent = state.background.visible ? '隐藏' : '显示';
}

function bindBackground() {
    $('#bgPrevBtn').addEventListener('click', () => showBackgroundAt(state.background.currentIndex - 1));
    $('#bgNextBtn').addEventListener('click', () => showBackgroundAt(state.background.currentIndex + 1));
    $('#bgToggleBtn').addEventListener('click', async () => {
        if (state.background.visible) {
            state.background.userVisible = false;
            state.background.autoDebugVisible = false;
            state.background.debugModeActive = false;
            await withBusy('正在隐藏背景...', async () => {
                await setBackgroundVisible(false);
                await resumeVideosAfterBackground();
                await removeBackgroundLayer();
            }, '背景已隐藏');
        } else {
            const shown = await showBackgroundAt(state.background.currentIndex);
            if (!shown) state.background.userVisible = false;
        }
        updateBackgroundInfo();
    });
}

function bindMobilePages() {
    $('#refreshLayerPageBtn')?.addEventListener('click', () => loadLayerPage());
    $('#refreshVideoPageBtn')?.addEventListener('click', () => loadVideoPage());
    $('#refreshControlPageBtn')?.addEventListener('click', () => loadControlPage());
    $('#refreshSystemPageBtn')?.addEventListener('click', () => loadSystemPage());

    $('#layerList')?.addEventListener('click', (event) => {
        const button = event.target.closest('[data-layer-visible]');
        if (button) {
            const layerId = Number(button.dataset.layerVisible);
            const nextVisible = button.dataset.visible !== 'true';
            void withBusy('正在切换图层显示...', async () => {
                await applyLayerPatch(layerId, { visible: nextVisible });
            }, nextVisible ? '图层已显示' : '图层已隐藏');
            return;
        }
        const item = event.target.closest('[data-layer-select]');
        if (!item) return;
        appState.selectedLayerId = Number(item.dataset.layerSelect);
        renderLayerPage();
    });

    $('#layerEditor')?.addEventListener('input', (event) => {
        const field = event.target.closest('[data-layer-field]');
        if (!field) return;
        if (field.dataset.layerField === 'alpha') setText('#layerEditorAlphaValue', String(Math.round(Number(field.value) || 0)));
        if (field.dataset.layerField === 'volume') setText('#layerEditorVolumeValue', `${Math.round(Number(field.value) || 0)}%`);
        syncLayerEditorPatch();
    });

    $('#layerEditor')?.addEventListener('change', (event) => {
        if (event.target.closest('[data-layer-field]')) syncLayerEditorPatch();
    });

    $('#layerEditor')?.addEventListener('click', (event) => {
        const visibleButton = event.target.closest('[data-layer-editor-visible]');
        if (visibleButton) {
            const layerId = Number(visibleButton.dataset.layerEditorVisible);
            const nextVisible = visibleButton.dataset.visible !== 'true';
            void withBusy('正在切换图层显示...', async () => {
                await applyLayerPatch(layerId, { visible: nextVisible });
            }, nextVisible ? '图层已显示' : '图层已隐藏');
            return;
        }
        const priorityButton = event.target.closest('[data-layer-priority-delta]');
        if (priorityButton && appState.selectedLayerId) {
            const input = $('#layerEditor [data-layer-field="priority"]');
            const next = Math.round(Number(input?.value || appState.selectedLayerId) + Number(priorityButton.dataset.layerPriorityDelta || 0));
            if (input) input.value = String(next);
            void withBusy('正在调整图层层级...', async () => {
                await applyLayerPatch(Number(appState.selectedLayerId), { priority: next });
            }, '图层层级已调整');
            return;
        }
        const fillButton = event.target.closest('[data-layer-fill-screen]');
        if (fillButton && appState.selectedLayerId) {
            void withBusy('正在铺满图层...', async () => {
                const size = await canvasSize();
                const patch = { position: { x: 0, y: 0 }, size: { width: Math.round(size.width), height: Math.round(size.height) } };
                await applyLayerPatch(Number(appState.selectedLayerId), patch);
            }, '图层已铺满');
        }
    });

    $('#videoLayerPicker')?.addEventListener('click', (event) => {
        const button = event.target.closest('[data-video-layer]');
        if (!button) return;
        appState.videoLayerId = Number(button.dataset.videoLayer);
        void loadVideoPage();
    });

    $$('[data-video-action]').forEach((button) => {
        button.addEventListener('click', () => {
            const action = button.dataset.videoAction;
            const layerId = chooseVideoLayer();
            void withBusy('正在发送视频控制...', async () => {
                let response = null;
                if (action === 'toggle') {
                    const status = currentVideoStatus();
                    const playing = status?.state === 'playing' || status?.status === 'playing';
                    response = await videoApi.command(playing ? 'pause' : 'resume', { layerId });
                } else if (action === 'mute') {
                    response = await videoApi.command('muteToggle', { layerId });
                } else {
                    response = await videoApi.command(action, { layerId });
                }
                syncSystemVolumeControl(response);
                appState.videoStatus = await videoApi.status(layerId).catch(() => ({}));
                renderVideoPage();
            }, '视频控制已发送');
        });
    });

    $('#systemVolumeSlider')?.addEventListener('input', debounce(async (event) => {
        const value = Number(event.target.value || 0);
        setText('#systemVolumeValue', `${value}%`);
        await videoApi.command('setSystemVolume', { volume: value / 100 });
    }, 160));

    $('#videoLayerVolumeSlider')?.addEventListener('input', debounce(async (event) => {
        const value = clamp(Number(event.target.value || 0), 0, 100);
        const layerId = chooseVideoLayer();
        setText('#videoLayerVolumeValue', `${Math.round(value)}%`);
        const volume = value / 100;
        const results = await Promise.allSettled([
            videoApi.command('setVolume', { layerId, volume, suppress_hint: true }),
            layerApi.updateConfig(layerId, { volume })
        ]);
        if (results.every((result) => result.status === 'rejected')) {
            toast('图层音量同步失败', 'error');
            return;
        }
        patchLayerCache(layerId, { volume });
    }, 120));

    const seekSlider = $('#videoSeekSlider');
    seekSlider?.addEventListener('pointerdown', () => {
        appState.videoSeekDragging = true;
    });
    seekSlider?.addEventListener('input', (event) => {
        const position = displayPositionForDuration(event.target.value, appState.videoDuration);
        setText('#videoSeekValue', appState.videoDuration > 0
            ? `${formatTime(position)} / ${formatTime(appState.videoDuration)}`
            : formatTime(position));
    });
    seekSlider?.addEventListener('change', async (event) => {
        const layerId = chooseVideoLayer();
        const position = clampPlaybackPosition(event.target.value, appState.videoDuration, SEEK_END_GUARD_SECONDS);
        const traceId = `mobile-seek-${Date.now()}-${++appState.videoSeekTrace}-L${layerId}`;
        appState.videoSeekDragging = false;
        event.target.value = sliderSecondValue(position);
        await videoApi.command('seek', { layerId, position, traceId }, 15000).catch((error) => {
            console.error(error);
            toast(error.message || '跳转失败', 'error');
        });
        appState.videoStatus = await videoApi.status(layerId).catch(() => ({}));
        renderVideoPage();
    });
    seekSlider?.addEventListener('pointerup', () => {
        appState.videoSeekDragging = false;
    });
    seekSlider?.addEventListener('pointercancel', () => {
        appState.videoSeekDragging = false;
    });

    $$('.control-subtabs [data-control-tab]').forEach((button) => {
        button.addEventListener('click', () => {
            const tab = button.dataset.controlTab;
            $$('.control-subtabs [data-control-tab]').forEach((item) => item.classList.toggle('active', item === button));
            $$('.control-subpage').forEach((item) => item.classList.toggle('active', item.id === `control-${tab}`));
        });
    });

    $('#dmxMasterSlider')?.addEventListener('input', (event) => {
        const value = clamp(Number(event.target.value || 0), 0, 255);
        setText('#dmxMasterValue', String(value));
        clearTimeout(appState.dmxFlushTimer);
        appState.dmxFlushTimer = setTimeout(() => {
            peripheralApi.setDmxChannel(0, Math.round(value)).catch((error) => {
                console.error(error);
                toast(error.message || 'DMX 同步失败', 'error');
            });
        }, 80);
    });
}

function bindGlobalActions() {
    $('#refreshBtn')?.addEventListener('click', () => loadAppPage(appState.page));
    $('#saveAllBtn')?.addEventListener('click', () => withBusy('正在保存融合配置...', () => fusionApi.persist(), '全部配置已保存'));
    $$('[data-region-switch]').forEach((button) => {
        button.addEventListener('click', () => {
            const nextRegionId = getNextRegionId(Number(button.dataset.regionSwitch || 1));
            void withBusy('正在切换投影...', () => switchRegion(nextRegionId), `已切换到投影 ${nextRegionId}`);
        });
    });
    $$('[data-app-page]').forEach((link) => {
        link.addEventListener('click', (event) => {
            event.preventDefault();
            void switchAppPage(link.dataset.appPage);
        });
    });
    window.addEventListener('resize', debounce(renderPreview, 80));
}

function debounce(fn, delay) {
    let timer = null;
    return (...args) => {
        clearTimeout(timer);
        timer = setTimeout(() => fn(...args), delay);
    };
}

function bindAll() {
    window.addEventListener('pagehide', cleanupBackgroundOnPageHide);
    ensureVideoStatusEvents();
    bindModeTabs();
    bindPreview();
    bindLayout();
    bindGeometry();
    bindPreviewGridResize();
    bindMask();
    bindNudgePads();
    bindBlend();
    bindColor();
    bindAdvanced();
    bindBackground();
    bindMobilePages();
    bindGlobalActions();
}

bindAll();
loadAll().catch(() => {
    setText('#statusLine', '连接失败，请检查 8080 调试服务');
});
