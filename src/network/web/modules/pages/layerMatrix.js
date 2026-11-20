// 图层矩阵页面模块 - 主入口文件
// 整合所有子模块，提供统一的对外接口

// 导入核心依赖
import { apiGet, apiPost, apiPut } from '../core/api.js';
import { addToCommandLog } from '/shared/commandLog.js';

// 导入状态管理
import * as state from './layerMatrix/state.js';

// 导入工具函数
import * as utils from './layerMatrix/utils.js';

// 导入配置管理
import * as config from './layerMatrix/config.js';

// 导入场景管理
import * as sceneManagement from './layerMatrix/sceneManagement.js';

// 导入画布绘制
import * as canvas from './layerMatrix/canvas.js';

// 导入数据加载
import * as dataLoader from './layerMatrix/dataLoader.js?v=layers_payload_v2';

// 导入图层管理
import * as layerManagement from './layerMatrix/layerManagement.js';

// 导入切片管理
import * as sliceManagement from './layerMatrix/sliceManagement.js';

// 导入事件处理
import * as eventHandlers from './layerMatrix/eventHandlers.js';

import * as geometry from './layerMatrix/geometry.js';
import {
    LAYER_MATRIX_DRAG_TYPE,
    getMatrixDropRegionAtPoint,
    getMatrixRegionAtPoint,
    getMatrixSubdivisionKey
} from './layerMatrix/matrixLayout.js';

// 导入UI渲染
import * as uiRenderer from './layerMatrix/uiRenderer.js?v=layout_20260906_2';

// 导入二维码辅助
import * as qrCodeHelpers from './layerMatrix/qrCodeHelpers.js?v=qr_address_2';

// 通用输入读取器（updateLayerFromInputs / applyLayerProperties 共用）
import { readLayerCommonInputs } from './layerMatrix/panels/InputReader.js';

// 右键"快速选择"菜单
import { showQuickAlignMenu, hideQuickAlignMenu } from './layerMatrix/panels/QuickAlignMenu.js';

import { showConfirm } from '../components/toast.js';

const MATRIX_SUBDIVISION_MIN_SIZE = 2;
const MATRIX_SUBDIVISION_MAX_SIZE = 12;

let matrixDropTarget = null;
let matrixSubdivisions = [];
let activeMatrixSubdivisionKey = '';
let pendingMatrixSubdivision = null;
let matrixSubdivisionCustomVisible = false;

function setState(updates) {
    if (updates.selectedLayer !== undefined) state.setSelectedLayer(updates.selectedLayer);
    if (updates.selectedSlice !== undefined) state.setSelectedSlice(updates.selectedSlice);
    if (updates.selectedItems !== undefined) state.setSelectedItems(updates.selectedItems);

    state.setCanvasState(updates.canvas, updates.ctx, updates.dpr);

    if (updates.layers !== undefined) state.setLayers(updates.layers);
    if (updates.layersLoadStatus !== undefined) state.setLayersLoadStatus(updates.layersLoadStatus);
    if (updates.refreshingPromise !== undefined) state.setRefreshingPromise(updates.refreshingPromise);

    state.setDragState(updates);
}

// ==================== 内部函数包装 ====================

// 画布相关函数包装
function initializeCanvas() {
    const result = canvas.initializeCanvas(
        state.canvas, state.ctx, state.dpr,
        state.isDragging, state.isResizing,
        onCanvasMouseDown, onCanvasMouseMove, onCanvasMouseUp, onCanvasDoubleClick,
        updateCursor, canvas.cssToCanvasCoord, updateCanvasSizeByLayout, drawCanvas,
        state.canvasConfig,
        onCanvasContextMenu,
        {
            onDragOver: onCanvasLayerDragOver,
            onDragLeave: onCanvasLayerDragLeave,
            onDrop: onCanvasLayerDrop
        }
    );
    if (result) {
        state.setCanvasState(result.canvas, result.ctx, result.dpr);
    }
}

function updateCanvasSizeByLayout() {
    return canvas.updateCanvasSizeByLayout(
        state.canvas, state.ctx, state.dpr, state.canvasConfig
    );
}

let drawFrameId = null;
function renderCanvasNow() {
    canvas.drawCanvas(
        state.canvas, state.ctx, state.dpr, state.layers,
        state.selectedLayer, state.selectedSlice, state.canvasConfig,
        drawLayerRect, drawLayerSlices, canvas.systemToCanvas,
        canvas.getLayerColor, utils.getLayerDisplayName, state.ALL_AVAILABLE_LAYERS,
        canvas.drawResizeHandles, utils.validateConfig, state.selectedItems, state.snapGuides,
        matrixDropTarget, matrixSubdivisions, activeMatrixSubdivisionKey
    );
}

function drawCanvas(immediate = false) {
    if (immediate) {
        drawCanvasNow();
        return;
    }
    if (drawFrameId !== null) return;
    drawFrameId = requestAnimationFrame(() => {
        drawFrameId = null;
        renderCanvasNow();
    });
}

function drawCanvasNow() {
    if (drawFrameId !== null) {
        cancelAnimationFrame(drawFrameId);
        drawFrameId = null;
    }
    renderCanvasNow();
}

function drawLayerRect(layer, isSelected) {
    canvas.drawLayerRect(
        layer, isSelected, state.ctx, state.canvas, state.dpr, state.canvasConfig,
        canvas.systemToCanvas, canvas.getLayerColor, utils.getLayerDisplayName,
        state.ALL_AVAILABLE_LAYERS, canvas.drawResizeHandles, utils.validateConfig
    );
}

function drawLayerSlices(layer) {
    canvas.drawLayerSlices(
        layer, state.selectedSlice, state.ctx, state.canvas, state.dpr, state.canvasConfig,
        canvas.systemToCanvas, drawSliceRect, utils.validateConfig
    );
}

function drawSliceRect(layer, sliceKey, sliceData, isSelected) {
    canvas.drawSliceRect(
        layer, sliceKey, sliceData, isSelected, state.ctx, state.canvas, state.dpr, state.canvasConfig,
        canvas.systemToCanvas, utils.validateConfig, drawLayerRect, canvas.getLayerColor,
        utils.getLayerDisplayName, state.ALL_AVAILABLE_LAYERS, canvas.drawResizeHandles
    );
}

// 配置相关函数包装
async function loadSystemConfig() {
    return config.loadSystemConfig(state.canvasConfig, updateCanvasSizeByLayout);
}

// 数据加载相关函数包装
async function loadAllAvailableLayers() {
    return dataLoader.loadAllAvailableLayers(state.ALL_AVAILABLE_LAYERS);
}

// 图层管理相关函数包装
function selectLayerInternal(layerId) {
    // [Fix] 同一图层重复点击不触发重型 showLayerProperties (会发 2 个 GET + 重建整个表单 DOM)
    // 之前每次画布按下都会发 GET /layers/X + GET /layers/X/roam，期间 PUT 排队，导致拖动大屏只"偶尔动一下"
    const singleLayerItem = geometry.createLayerItem(layerId);
    if (
        layerId === state.selectedLayer &&
        state.selectedSlice == null &&
        state.selectedItems.length === 1 &&
        geometry.getItemKey(state.selectedItems[0]) === geometry.getItemKey(singleLayerItem)
    ) {
        return;
    }
    state.setSelectedLayer(layerId);
    state.setSelectedSlice(null);
    state.setSelectedItems([singleLayerItem]);
    layerManagement.selectLayer(
        layerId, state.layers,
        moveLayerToTop, updateSelectionUI, updateSliceInfo, showLayerPropertiesInternal, drawCanvas
    );
}

async function createLayerInternal(layerId, layerType, initialBounds = null) {
    return layerManagement.createLayer(
        layerId, layerType, updateLayerMatrix, state.layers, refreshLayerInfo,
        renderLayerMatrix, drawCanvas, updateLayerSelection, initialBounds
    );
}

function normalizeLayerId(layerId) {
    const value = Number(layerId);
    return Number.isInteger(value) ? value : null;
}

function getCreatedLayerIds() {
    return new Set(
        state.layers
            .map(layer => normalizeLayerId(layer && layer.id))
            .filter(layerId => layerId !== null)
    );
}

function getLayerCreateCandidates(layerType) {
    const authorized = state.ALL_AVAILABLE_LAYERS
        .map(layer => {
            const id = normalizeLayerId(layer && layer.id);
            if (id === null) return null;
            return { ...layer, id, type: layer.type || layerType };
        })
        .filter(Boolean)
        .sort((a, b) => a.id - b.id);

    if (authorized.length > 0) return authorized;

    return Array.from({ length: 64 }, (_, index) => ({
        id: index + 1,
        type: layerType
    }));
}

function resolveLayerForCreate(layerId, layerType, direction = 1) {
    const requestedId = normalizeLayerId(layerId);
    if (requestedId === null) return null;

    const createdIds = getCreatedLayerIds();
    const candidates = getLayerCreateCandidates(layerType);
    const requestedTemplate = candidates.find(layer => layer.id === requestedId);

    if (!createdIds.has(requestedId)) {
        return {
            id: requestedId,
            type: layerType || (requestedTemplate && requestedTemplate.type),
            skippedFrom: null
        };
    }

    const stepDirection = direction < 0 ? -1 : 1;
    const availableCandidates = candidates
        .filter(layer => !createdIds.has(layer.id))
        .filter(layer => stepDirection > 0 ? layer.id > requestedId : layer.id < requestedId)
        .sort((a, b) => stepDirection > 0 ? a.id - b.id : b.id - a.id);

    const nextLayer = availableCandidates[0];
    if (!nextLayer) return null;

    return {
        id: nextLayer.id,
        type: nextLayer.type || layerType,
        skippedFrom: requestedId
    };
}

async function deleteLayer(layerId) {
    return layerManagement.deleteLayer(
        layerId, state.layers, state.selectedLayer, updateLayerMatrix,
        (value) => {
            state.setSelectedLayer(value);
            state.setSelectedSlice(null);
            state.setSelectedItems(value === null || value === undefined ? [] : [geometry.createLayerItem(value)]);
        },
        drawCanvas
    );
}

function moveLayerToTop(layerId) {
    layerManagement.moveLayerToTop(layerId, state.layers);
}

async function applyLayerProperties(layerId) {
    return layerManagement.applyLayerProperties(
        layerId, state.layers, state.canvasConfig, utils.getLayerTypeFlags,
        drawCanvas, updateLayerMatrix
    );
}

async function refreshLayerInfo(layerId) {
    return layerManagement.refreshLayerInfo(layerId, state.layers);
}

function updatePropertyInputs(layer) {
    layerManagement.updatePropertyInputs(layer);
}

// 切片管理相关函数包装
function selectSliceInternal(layerId, sliceKey) {
    state.setSelectedLayer(layerId);
    state.setSelectedSlice({ layerId, sliceKey });
    state.setSelectedItems([geometry.createSliceItem(layerId, sliceKey)]);
    return sliceManagement.selectSlice(
        layerId, sliceKey, state.layers,
        moveLayerToTop, updateSelectionUI, updateSliceInfo, showSliceProperties, drawCanvas
    );
}

function syncLegacySelectionFromItems(items, primaryItem = null) {
    const nextPrimary = primaryItem || items[items.length - 1] || null;
    state.setSelectedItems(items);

    if (!nextPrimary) {
        state.setSelectedLayer(null);
        state.setSelectedSlice(null);
        updateSelectionUI();
        drawCanvas();
        return;
    }

    state.setSelectedLayer(nextPrimary.layerId);
    if (nextPrimary.type === 'slice') {
        state.setSelectedSlice({ layerId: nextPrimary.layerId, sliceKey: nextPrimary.sliceKey });
        showSliceProperties(nextPrimary.layerId, nextPrimary.sliceKey);
    } else {
        state.setSelectedSlice(null);
        showLayerPropertiesInternal(nextPrimary.layerId);
    }
    updateSelectionUI();
    updateSliceInfo(nextPrimary.layerId);
    drawCanvas();
}

function toggleLayerSelectionForMultiSelect(layerId) {
    const item = geometry.createLayerItem(layerId);
    const key = geometry.getItemKey(item);
    const existing = state.selectedItems || [];
    const exists = existing.some(entry => geometry.getItemKey(entry) === key);
    const nextItems = exists
        ? existing.filter(entry => geometry.getItemKey(entry) !== key)
        : [...geometry.getLayerItemsFromSelection(existing), item];
    syncLegacySelectionFromItems(nextItems, exists ? (nextItems[nextItems.length - 1] || null) : item);
}

async function createSlice() {
    return sliceManagement.createSlice(
        state.selectedLayer, state.layers, state.canvasConfig, refreshLayerInfo, updateSliceInfo, selectSliceInternal, drawCanvas
    );
}

async function deleteSlice(layerId, sliceKey) {
    const result = await sliceManagement.deleteSlice(
        layerId, sliceKey, state.layers, refreshLayerInfo, updateSliceInfo,
        showSliceProperties, drawCanvas
    );
    if (result && state.selectedSlice &&
        Number(state.selectedSlice.layerId) === Number(layerId) &&
        String(state.selectedSlice.sliceKey) === String(sliceKey)) {
        state.setSelectedSlice(null);
        updateSelectionUI();
        drawCanvas();
    }
    return result;
}

function updateSliceInfo(layerId) {
    sliceManagement.updateSliceInfo(
        layerId, state.layers, state.selectedSlice,
        selectSliceInternal, async (lId, sKey) => await deleteSlice(lId, sKey)
    );
}

async function toggleSliceVisibility(layerId, sliceKey) {
    return sliceManagement.toggleSliceVisibility(
        layerId, sliceKey, state.layers, refreshLayerInfo, updateSliceInfo,
        showSliceProperties, drawCanvas
    );
}

async function applySliceProperties(layerId, sliceKey) {
    return sliceManagement.applySliceProperties(
        layerId, sliceKey, state.layers, drawCanvas
    );
}

// UI渲染相关函数包装
function renderLayerMatrix() {
    uiRenderer.renderLayerMatrix(
        state.layers, state.layersLoadStatus, state.selectedLayer, state.ALL_AVAILABLE_LAYERS,
        selectLayerInternal, addLayerToMatrix
    );
}

function updateLayerSelection() {
    uiRenderer.updateLayerSelection(
        state.layers, state.layersLoadStatus, state.selectedLayer, state.ALL_AVAILABLE_LAYERS,
        selectLayerInternal, deleteLayer
    );
}

function updateSelectionUI() {
    uiRenderer.updateSelectionUI(state.selectedLayer, state.selectedSlice);
}

async function showLayerPropertiesInternal(layerId) {
    return uiRenderer.showLayerProperties(
        layerId, state.layers, state.ALL_AVAILABLE_LAYERS,
        utils.getLayerTypeFlags, utils.parsePosition, utils.parseSize, utils.getLayerDisplayName,
        utils.parseColor, utils.rgbaStringToCssColor, utils.cssColorToRgbaString,
        renderQRCodeDashboard, setupQRCodeDashboardListeners,
        setupQRCodeColorInputs, setupLayer40ColorInputs,
        applyLayerProperties, layerManagement.loadImage, updateLayerFromInputs,
        drawCanvas, toggleLayerVisibility, deleteLayer, addToCommandLog, apiGet, apiPut,
        qrCodeHelpers.drawQRCodeCard, qrCodeHelpers.updateQRCodePreview, generateQRCode,
        layerManagement.fetchAndMergeLayerInfo,
        state.isDragging, state.isResizing
    );
}

async function showSliceProperties(layerId, sliceKey) {
    return uiRenderer.showSliceProperties(
        layerId, sliceKey, state.layers, utils.getLayerTypeFlags, utils.parseSliceCoordinate,
        utils.parseColor, utils.rgbaStringToCssColor, utils.cssColorToRgbaString,
        renderQRCodeDashboard, setupQRCodeDashboardListeners,
        applySliceProperties, toggleSliceVisibility, deleteSlice,
        updateSliceInfo, drawCanvas, layerManagement.loadImage, addToCommandLog, apiGet, apiPost
    );
}

// 事件处理相关函数包装
function normalizeMatrixSubdivisionDimension(value) {
    const size = Math.trunc(Number(value));
    if (!Number.isFinite(size)) return MATRIX_SUBDIVISION_MIN_SIZE;
    return Math.max(MATRIX_SUBDIVISION_MIN_SIZE, Math.min(MATRIX_SUBDIVISION_MAX_SIZE, size));
}

function getCanvasSystemPointFromEvent(event) {
    if (!state.canvas || !utils.validateConfig(state.canvasConfig)) return null;
    const rect = state.canvas.getBoundingClientRect();
    const cssX = event.clientX - rect.left;
    const cssY = event.clientY - rect.top;
    const canvasCoord = canvas.cssToCanvasCoord(cssX, cssY);
    return canvas.canvasToSystem(
        canvasCoord.x,
        canvasCoord.y,
        state.canvas,
        state.dpr,
        state.canvasConfig
    );
}

function getBaseMatrixRegionFromCanvasEvent(event) {
    const systemPoint = getCanvasSystemPointFromEvent(event);
    if (!systemPoint) return null;
    return getMatrixRegionAtPoint(systemPoint.x, systemPoint.y, state.canvasConfig);
}

function getMatrixDropRegionFromCanvasEvent(event) {
    const systemPoint = getCanvasSystemPointFromEvent(event);
    if (!systemPoint) return null;
    return getMatrixDropRegionAtPoint(
        systemPoint.x,
        systemPoint.y,
        state.canvasConfig,
        matrixSubdivisions
    );
}

function setMatrixDropTarget(region) {
    const nextRegion = region ? { ...region } : null;
    if (
        matrixDropTarget?.x === nextRegion?.x &&
        matrixDropTarget?.y === nextRegion?.y &&
        matrixDropTarget?.width === nextRegion?.width &&
        matrixDropTarget?.height === nextRegion?.height
    ) {
        return;
    }
    matrixDropTarget = nextRegion;
    drawCanvas();
}

function syncMatrixSubdivisionControls() {
    const presetSelect = document.getElementById('matrix-subdivision-preset');
    const customToggle = document.getElementById('matrix-subdivision-custom-toggle');
    const customFields = document.getElementById('matrix-subdivision-custom-fields');
    const rowsInput = document.getElementById('matrix-subdivision-rows');
    const colsInput = document.getElementById('matrix-subdivision-cols');

    customToggle?.classList.toggle('active', matrixSubdivisionCustomVisible);
    if (customFields) {
        customFields.classList.toggle('hidden', !matrixSubdivisionCustomVisible);
    }
    if (pendingMatrixSubdivision && matrixSubdivisionCustomVisible) {
        if (rowsInput) rowsInput.value = String(pendingMatrixSubdivision.rows);
        if (colsInput) colsInput.value = String(pendingMatrixSubdivision.cols);
    }
    if (presetSelect) {
        presetSelect.disabled = matrixSubdivisionCustomVisible;
    }

    const applyButton = document.getElementById('matrix-subdivision-apply');
    if (applyButton) {
        applyButton.classList.toggle('active', pendingMatrixSubdivision !== null);
        applyButton.textContent = pendingMatrixSubdivision === null
            ? '\u9009\u62e9\u533a\u57df'
            : '\u7b49\u5f85\u70b9\u51fb\u5e55\u5e03';
    }

    const status = document.getElementById('matrix-subdivision-status');
    if (status) {
        if (pendingMatrixSubdivision) {
            status.textContent = `\u8bf7\u70b9\u51fb\u57fa\u7840\u533a\u57df\uff0c\u5e94\u7528 ${pendingMatrixSubdivision.rows}\u00d7${pendingMatrixSubdivision.cols}`;
        } else if (matrixSubdivisions.length > 0) {
            const active = matrixSubdivisions.find(subdivision =>
                getMatrixSubdivisionKey(subdivision.baseRow, subdivision.baseCol) === activeMatrixSubdivisionKey
            ) || matrixSubdivisions[matrixSubdivisions.length - 1];
            status.textContent = `${matrixSubdivisions.length} \u4e2a\u5206\u5272\u533a\u57df\uff1b\u5f53\u524d\u7b2c ${active.baseRow + 1} \u884c\u7b2c ${active.baseCol + 1} \u5217 ${active.rows}\u00d7${active.cols}`;
        } else {
            status.textContent = '\u672a\u8bbe\u7f6e\u5206\u5272\u533a\u57df';
        }
    }

    state.canvas?.classList.toggle(
        'matrix-subdivision-selecting',
        pendingMatrixSubdivision !== null
    );
}

function startMatrixSubdivisionSelection(rows, cols) {
    pendingMatrixSubdivision = {
        rows: normalizeMatrixSubdivisionDimension(rows),
        cols: normalizeMatrixSubdivisionDimension(cols)
    };
    setMatrixDropTarget(null);
    syncMatrixSubdivisionControls();
}

function cancelMatrixSubdivisionSelection() {
    if (pendingMatrixSubdivision === null) return;
    pendingMatrixSubdivision = null;
    syncMatrixSubdivisionControls();
}

function applyMatrixSubdivisionAtEvent(event) {
    if (pendingMatrixSubdivision === null || event.button !== 0) return false;

    const baseRegion = getBaseMatrixRegionFromCanvasEvent(event);
    if (!baseRegion) return false;

    event.preventDefault();
    event.stopPropagation();
    const { rows, cols } = pendingMatrixSubdivision;
    const key = getMatrixSubdivisionKey(baseRegion.row, baseRegion.col);
    matrixSubdivisions = matrixSubdivisions.filter(subdivision =>
        getMatrixSubdivisionKey(subdivision.baseRow, subdivision.baseCol) !== key
    );
    matrixSubdivisions.push({
        baseRow: baseRegion.row,
        baseCol: baseRegion.col,
        rows,
        cols
    });
    activeMatrixSubdivisionKey = key;
    pendingMatrixSubdivision = null;
    syncMatrixSubdivisionControls();
    drawCanvas();
    addToCommandLog(
        '\u533a\u57df\u5206\u5272',
        'success',
        `\u7b2c ${baseRegion.row + 1} \u884c\u7b2c ${baseRegion.col + 1} \u5217\u5df2\u5206\u5272\u4e3a ${rows}\u00d7${cols}`
    );
    return true;
}

function clearMatrixSubdivisions() {
    const hadSubdivisions = matrixSubdivisions.length > 0 || pendingMatrixSubdivision !== null;
    matrixSubdivisions = [];
    activeMatrixSubdivisionKey = '';
    pendingMatrixSubdivision = null;
    setMatrixDropTarget(null);
    syncMatrixSubdivisionControls();
    drawCanvas();
    if (hadSubdivisions) {
        addToCommandLog(
            '\u533a\u57df\u5206\u5272',
            'info',
            '\u5df2\u6e05\u9664\u5168\u90e8\u533a\u57df\u5206\u5272\u8f85\u52a9\u7ebf'
        );
    }
}

function hasLayerDragPayload(event) {
    return Array.from(event.dataTransfer?.types || []).includes(LAYER_MATRIX_DRAG_TYPE);
}

function onCanvasLayerDragOver(event) {
    if (!hasLayerDragPayload(event)) return;
    event.preventDefault();
    event.stopPropagation();
    if (event.dataTransfer) {
        event.dataTransfer.dropEffect = event.dataTransfer.effectAllowed === 'copy' ? 'copy' : 'move';
    }
    setMatrixDropTarget(getMatrixDropRegionFromCanvasEvent(event));
}

function onCanvasLayerDragLeave(event) {
    if (event.relatedTarget && state.canvas?.contains(event.relatedTarget)) return;
    setMatrixDropTarget(null);
}

async function placeLayerInMatrixRegion(payload, region) {
    const layerId = normalizeLayerId(payload?.layerId);
    const layerType = payload?.layerType;
    if (layerId === null || !layerType || !region) return false;

    const existingLayer = state.layers.find(layer => Number(layer.id) === layerId);
    if (!existingLayer) {
        return addLayerToMatrix(layerId, layerType, region);
    }

    geometry.setItemBounds(
        geometry.createLayerItem(layerId),
        state.layers,
        region,
        state.canvasConfig
    );
    state.setSelectedLayer(null);
    state.setSelectedSlice(null);
    state.setSelectedItems([]);
    selectLayerInternal(layerId);
    updatePropertyInputs(existingLayer);
    drawCanvas();
    await layerManagement.syncLayerToServer(existingLayer);
    return true;
}

async function onCanvasLayerDrop(event) {
    if (!hasLayerDragPayload(event)) return;
    event.preventDefault();
    event.stopPropagation();

    const region = getMatrixDropRegionFromCanvasEvent(event);
    const rawPayload = event.dataTransfer?.getData(LAYER_MATRIX_DRAG_TYPE);
    setMatrixDropTarget(null);
    if (!region || !rawPayload) return;

    try {
        const payload = JSON.parse(rawPayload);
        await placeLayerInMatrixRegion(payload, region);
    } catch (error) {
        addToCommandLog('图层放置', 'error', `拖放图层失败: ${error.message || '未知错误'}`);
    }
}

function onCanvasMouseDown(e) {
    if (applyMatrixSubdivisionAtEvent(e)) return null;

    const result = eventHandlers.onCanvasMouseDown(
        e, state.canvas, state.dpr, canvas.cssToCanvasCoord, getClickedHandle, getClickedSlice, getClickedLayer,
        state.resizeHandle,
        state.dragStartX, state.dragStartY, state.dragLayerStartX, state.dragLayerStartY,
        state.dragLayerStartW, state.dragLayerStartH, state.selectedLayer, state.selectedSlice,
        state.layers, state.canvasConfig, utils.validateConfig, utils.parseSliceCoordinate,
        canvas.canvasToSystem,
        selectSliceInternal, selectLayerInternal,
        state.selectedItems, toggleLayerSelectionForMultiSelect
    );
    if (result) {
        setState(result);
    }
    return result;
}

function onCanvasMouseMove(e) {
    eventHandlers.onCanvasMouseMove(
        e, state.canvas, state.dpr, canvas.cssToCanvasCoord, updateCursor, state.isDragging, state.isResizing,
        state.selectedLayer, state.selectedSlice, state.layers, state.canvasConfig,
        canvas.canvasToSystem, utils.parseSliceCoordinate, utils.validateConfig,
        utils.parsePosition, utils.parseSize, drawCanvas, updatePropertyInputs, state.resizeHandle,
        state.dragStartX, state.dragStartY, state.dragLayerStartX, state.dragLayerStartY,
        state.dragLayerStartW, state.dragLayerStartH,
        getClickedHandle, getClickedLayer,
        layerManagement.syncLayerRectRealTime,
        state.interactionSession,
        (updates) => setState(updates)
    );
}

async function onCanvasMouseUp(e) {
    const wasDragging = state.isDragging;
    const wasResizing = state.isResizing;
    const selectedLayerAtRelease = state.selectedLayer;
    const selectedSliceAtRelease = state.selectedSlice ? { ...state.selectedSlice } : null;
    const layersAtRelease = state.layers;

    if (wasDragging || wasResizing) {
        setState({
            isDragging: false,
            isResizing: false,
            resizeHandle: null,
            interactionSession: null,
            snapGuides: []
        });
    }

    await eventHandlers.onCanvasMouseUp(
        e, wasDragging, wasResizing, selectedLayerAtRelease, selectedSliceAtRelease, layersAtRelease,
        applySliceProperties, layerManagement.syncLayerToServer,
        state.selectedItems
    );
}

function updateCursor(mouseX, mouseY) {
    eventHandlers.updateCursor(
        mouseX, mouseY, state.canvas, state.selectedLayer, state.layers, state.canvasConfig,
        getClickedHandle, getClickedLayer, state.selectedSlice, state.dpr
    );
}

function getClickedHandle(mouseX, mouseY) {
    return eventHandlers.getClickedHandle(
        mouseX, mouseY, state.selectedLayer, state.selectedSlice, state.layers, state.canvasConfig,
        canvas.systemToCanvas, canvas.getHandlePositions, utils.validateConfig, state.canvas, state.dpr
    );
}

function getClickedLayer(mouseX, mouseY, options = {}) {
    return eventHandlers.getClickedLayer(
        mouseX, mouseY, state.selectedLayer, state.layers, state.canvasConfig,
        canvas.systemToCanvas, state.canvas, state.dpr, layerManagement.isPointInLayer, options
    );
}

function getClickedSlice(mouseX, mouseY) {
    return eventHandlers.getClickedSlice(
        mouseX, mouseY, state.selectedLayer, state.layers, state.canvasConfig,
        canvas.canvasToSystem, utils.parseSliceCoordinate, utils.validateConfig, state.canvas, state.dpr
    );
}

function onCanvasDoubleClick(e) {
    eventHandlers.onCanvasDoubleClick(
        e, state.canvas, state.dpr, canvas.cssToCanvasCoord, getClickedLayer, getClickedSlice,
        state.selectedLayer, state.selectedSlice, state.layers, state.canvasConfig, utils.validateConfig,
        utils.parsePosition, utils.parseSize,
        selectLayerInternal, selectSliceInternal, drawCanvas, updatePropertyInputs,
        layerManagement.syncLayerToServer, sliceManagement.syncSliceConfig
    );
}

function onCanvasContextMenu(e) {
    eventHandlers.onCanvasContextMenu(
        e, state.canvas, state.dpr, canvas.cssToCanvasCoord, getClickedLayer, getClickedSlice,
        state.selectedLayer, state.layers, state.canvasConfig, utils.validateConfig,
        utils.parsePosition, utils.parseSize,
        selectLayerInternal, selectSliceInternal, drawCanvas, updatePropertyInputs,
        layerManagement.syncLayerToServer, sliceManagement.syncSliceConfig,
        showQuickAlignMenu
    );
}

// 场景管理相关函数包装

async function handleSceneLoaded() {
    setState({ selectedLayer: null, selectedSlice: null, selectedItems: [] });
    await updateLayerMatrix();
}

async function saveDefaultScene() {
    return sceneManagement.saveDefaultScene(state.layers);
}

async function showSceneSelector() {
    return sceneManagement.showSceneSelector(handleSceneLoaded);
}

async function saveCurrentScene() {
    return sceneManagement.saveCurrentScene(state.layers);
}

// 其他辅助函数
async function addLayerToMatrix(layerId, layerType, initialBounds = null) {
    try {
        const resolvedLayer = resolveLayerForCreate(layerId, layerType, 1);
        if (!resolvedLayer || !resolvedLayer.type) {
            addToCommandLog('添加图层', 'warning', `图层 ${layerId} 已存在，且没有可用的未占用图层ID`);
            return false;
        }

        if (resolvedLayer.skippedFrom !== null) {
            addToCommandLog(
                '添加图层',
                'warning',
                `图层 ${resolvedLayer.skippedFrom} 已存在，已跳到未占用图层 ${resolvedLayer.id}`
            );
        }

        addToCommandLog('添加图层', 'info', `正在添加图层 ${resolvedLayer.id}，类型: ${resolvedLayer.type}`);
        const success = await createLayerInternal(
            resolvedLayer.id,
            resolvedLayer.type,
            initialBounds
        );
        if (success) {
            // 新建后强制选中该图层：先清空再选中，避免 selectLayerInternal 的早退判断
            // （layerId === selectedLayer && selectedSlice == null）在罕见边界场景下跳过刷新。
            state.setSelectedLayer(null);
            state.setSelectedSlice(null);
            selectLayerInternal(resolvedLayer.id);
        }
        return success;
    } catch (error) {
        addToCommandLog('添加图层', 'error', `添加图层失败: ${error.message}`);
        return false;
    }
}

// 字段相等比较（处理浮点近似 / 对象 JSON 比较）
function _layerFieldEquals(key, oldVal, newVal) {
    if (oldVal === newVal) return true;
    if (key === 'alpha' || key === 'volume') {
        const a = oldVal === undefined ? 1.0 : oldVal;
        const b = newVal === undefined ? 1.0 : newVal;
        return Math.abs(a - b) <= 0.001;
    }
    if (key === 'roam_config' || (oldVal && typeof oldVal === 'object') || (newVal && typeof newVal === 'object')) {
        try { return JSON.stringify(oldVal || null) === JSON.stringify(newVal || null); }
        catch (_) { return false; }
    }
    return false;
}

function updateLayerFromInputs(layerId) {
    const layer = state.layers.find(l => l.id === layerId);
    if (!layer) return;

    if (!layer.position) layer.position = { x: 0, y: 0 };
    if (!layer.size) layer.size = { width: 0, height: 0 };

    // 单一来源读取所有通用输入
    const inputs = readLayerCommonInputs(layerId, layer);

    // 比较与应用：position / size 拆分；其余字段按 key 平铺
    let changed = false;
    if (inputs.position) {
        if (layer.position.x !== inputs.position.x) { layer.position.x = inputs.position.x; changed = true; }
        if (layer.position.y !== inputs.position.y) { layer.position.y = inputs.position.y; changed = true; }
    }
    if (inputs.size) {
        if (layer.size.width !== inputs.size.width) { layer.size.width = inputs.size.width; changed = true; }
        if (layer.size.height !== inputs.size.height) { layer.size.height = inputs.size.height; changed = true; }
    }
    Object.keys(inputs).forEach(k => {
        if (k === 'position' || k === 'size') return;
        if (!_layerFieldEquals(k, layer[k], inputs[k])) {
            layer[k] = inputs[k];
            changed = true;
        }
    });

    if (!changed) return;

    if (updateLayerFromInputs.rafId) {
        cancelAnimationFrame(updateLayerFromInputs.rafId);
    }
    updateLayerFromInputs.rafId = requestAnimationFrame(() => {
        drawCanvas();
        updateLayerFromInputs.rafId = null;
    });
}

async function toggleLayerVisibility(layerId) {
    return layerManagement.toggleLayerVisibility(layerId, state.layers, drawCanvas, updateLayerSelection);
}

// 二维码相关函数包装
function renderQRCodeDashboard(layer, idPrefix, coords) {
    return uiRenderer.renderQRCodeDashboard(
        layer, idPrefix, coords, utils.parseColor, utils.rgbaStringToCssColor
    );
}

function setupQRCodeDashboardListeners(layerId, idPrefix, sliceKey = null) {
    const applySliceProps = sliceKey ?
        () => applySliceProperties(layerId, sliceKey) :
        null;
    const applyLayerProps = () => applyLayerProperties(layerId);

    qrCodeHelpers.setupQRCodeDashboardListeners(
        layerId, idPrefix, sliceKey, applySliceProps, applyLayerProps,
        generateQRCode, updateQRCodePreview, setupQRCodeColorInputs
    );
}

function setupQRCodeColorInputs(layerId) {
    qrCodeHelpers.setupQRCodeColorInputs(
        layerId, updateQRCodePreview, qrCodeHelpers.drawQRCodeCard
    );
}

function setupLayer40ColorInputs(layerId) {
    qrCodeHelpers.setupLayer40ColorInputs(layerId);
}

async function generateQRCode(layerId) {
    return qrCodeHelpers.generateQRCode(
        layerId,
        applyLayerProperties,
        drawCanvas,
        refreshLayerInfo,
        showLayerPropertiesInternal,
        updateLayerMatrix
    );
}

async function updateQRCodePreview() {
    return qrCodeHelpers.updateQRCodePreview(qrCodeHelpers.drawQRCodeCard);
}

// 存储清理函数
let eventListenerCleanupFunctions = [];
let resizeTimeout = null;
const ENABLE_CUSTOM_SCENE_DROPDOWN = true;

function ensureSceneDropdownVisible() {
    const sel = document.getElementById('select-template-dropdown');
    if (!sel) return null;
    if (!ENABLE_CUSTOM_SCENE_DROPDOWN) {
        sel.style.display = 'block';
        getSceneCustomDropdown()?.remove();
        return null;
    }
    sel.style.display = 'none';
    return renderSceneCustomDropdown(sel);
}

function getSceneCustomDropdown() {
    return document.getElementById('select-template-dropdown-custom');
}

function getSceneCustomDropdownMenu() {
    return document.getElementById('select-template-dropdown-menu');
}

function getSelectedSceneText(select) {
    const selected = select.selectedOptions?.[0] || Array.from(select.options).find(option => option.value === select.value) || select.options[0];
    return selected?.textContent || '默认配置';
}

function positionSceneCustomOptions(container) {
    const trigger = container.querySelector('.custom-select-trigger');
    const options = getSceneCustomDropdownMenu();
    if (!trigger || !options) return;
    const rect = trigger.getBoundingClientRect();
    options.style.left = `${rect.left}px`;
    options.style.top = `${rect.bottom + 4}px`;
    options.style.minWidth = `${Math.max(rect.width, 220)}px`;
}

function closeSceneCustomDropdown() {
    if (!ENABLE_CUSTOM_SCENE_DROPDOWN) return;
    getSceneCustomDropdown()?.classList.remove('open');
    getSceneCustomDropdownMenu()?.classList.remove('open');
}

async function handleSceneCustomDropdownAction(event) {
    if (!ENABLE_CUSTOM_SCENE_DROPDOWN) return false;
    const customDropdown = getSceneCustomDropdown();
    const customMenu = getSceneCustomDropdownMenu();
    if (!customDropdown) return false;

    const trigger = event.target.closest('#select-template-dropdown-custom .custom-select-trigger');
    if (trigger) {
        event.preventDefault();
        event.stopPropagation();
        const willOpen = !customDropdown.classList.contains('open');
        closeSceneCustomDropdown();
        customDropdown.classList.toggle('open', willOpen);
        customMenu?.classList.toggle('open', willOpen);
        trigger.setAttribute('aria-expanded', willOpen ? 'true' : 'false');
        if (willOpen) positionSceneCustomOptions(customDropdown);
        return true;
    }

    const deleteBtn = event.target.closest('#select-template-dropdown-menu .custom-select-delete-btn');
    const option = event.target.closest('#select-template-dropdown-menu .custom-select-option');
    if (!option) return false;

    const sceneDropdown = document.getElementById('select-template-dropdown');
    const sceneName = option.dataset.value || '';
    if (!sceneDropdown || !sceneName) return false;

    if (deleteBtn) {
        event.preventDefault();
        event.stopPropagation();
        if (sceneManagement.isDefaultSceneName(sceneName)) return true;
        const confirmed = await showConfirm(`确定要删除场景"${sceneName}"吗？`, '删除场景');
        if (!confirmed) return true;

        await sceneManagement.deleteScene(sceneName);
        if (sceneDropdown.value === sceneName) {
            sceneDropdown.value = '默认配置';
            sceneManagement.setCurrentSceneName('默认配置');
            await sceneManagement.loadDefaultScene(handleSceneLoaded);
        }
        await sceneManagement.refreshSceneDropdown('select-template-dropdown');
        return true;
    }

    event.preventDefault();
    event.stopPropagation();
    sceneDropdown.value = sceneName;
    closeSceneCustomDropdown();
    if (sceneManagement.isDefaultSceneName(sceneName)) {
        await sceneManagement.loadDefaultScene(handleSceneLoaded);
    } else {
        await sceneManagement.loadScene(sceneName, handleSceneLoaded);
    }
    sceneManagement.setCurrentSceneName(sceneName);
    renderSceneCustomDropdown(sceneDropdown);
    return true;
}

function renderSceneCustomDropdown(select) {
    if (!ENABLE_CUSTOM_SCENE_DROPDOWN) return null;
    let container = getSceneCustomDropdown();
    let menu = getSceneCustomDropdownMenu();
    if (!container) {
        container = document.createElement('div');
        container.id = 'select-template-dropdown-custom';
        container.className = 'custom-select-container scene-template-select';
        container.innerHTML = `
            <button type="button" class="custom-select-trigger" aria-haspopup="listbox" aria-expanded="false">
                <span class="custom-select-value"></span>
                <span class="custom-select-arrow">▾</span>
            </button>
        `;
        select.insertAdjacentElement('afterend', container);
    }
    if (!menu) {
        menu = document.createElement('div');
        menu.id = 'select-template-dropdown-menu';
        menu.className = 'custom-select-options scene-template-options';
        menu.setAttribute('role', 'listbox');
        document.body.appendChild(menu);
    }

    const valueEl = container.querySelector('.custom-select-value');
    const trigger = container.querySelector('.custom-select-trigger');
    if (valueEl) valueEl.textContent = getSelectedSceneText(select);
    if (trigger) trigger.setAttribute('aria-expanded', container.classList.contains('open') ? 'true' : 'false');
    if (!menu) return container;

    menu.innerHTML = '';
    Array.from(select.options).forEach(option => {
        const isDefaultScene = sceneManagement.isDefaultSceneName(option.value);
        const item = document.createElement('div');
        item.className = `custom-select-option${option.value === select.value ? ' selected' : ''}`;
        item.dataset.value = option.value;
        if (isDefaultScene) {
            item.dataset.defaultScene = 'true';
        }
        item.setAttribute('role', 'option');
        item.setAttribute('aria-selected', option.value === select.value ? 'true' : 'false');

        const text = document.createElement('span');
        text.className = 'custom-select-option-text';
        text.textContent = (globalThis.PlayerI18n?.sourceText(option) ?? option.textContent) || option.value;
        item.appendChild(text);

        if (!isDefaultScene) {
            const deleteBtn = document.createElement('button');
            deleteBtn.type = 'button';
            deleteBtn.className = 'custom-select-delete-btn';
            deleteBtn.title = '删除场景';
            deleteBtn.textContent = '×';
            item.appendChild(deleteBtn);
        }

        menu.appendChild(item);
    });

    if (container.classList.contains('open')) {
        positionSceneCustomOptions(container);
        menu.classList.add('open');
    }
    return container;
}

// 清理事件监听器
function cleanupEventListeners() {
    eventListenerCleanupFunctions.forEach(cleanup => {
        try {
            cleanup();
        } catch (e) {
            // 清理事件监听器时出错，忽略
        }
    });
    eventListenerCleanupFunctions = [];

    if (resizeTimeout) {
        clearTimeout(resizeTimeout);
        resizeTimeout = null;
    }
}

// 初始化事件监听器
async function initializeEventListeners() {
    // 先清理之前的监听器
    cleanupEventListeners();

    const subdivisionPresetSelect = document.getElementById('matrix-subdivision-preset');
    const subdivisionCustomToggle = document.getElementById('matrix-subdivision-custom-toggle');
    const subdivisionRowsInput = document.getElementById('matrix-subdivision-rows');
    const subdivisionColsInput = document.getElementById('matrix-subdivision-cols');
    const subdivisionApplyButton = document.getElementById('matrix-subdivision-apply');
    const subdivisionClearButton = document.getElementById('matrix-subdivision-clear');
    const subdivisionPresetChangeHandler = () => {
        matrixSubdivisionCustomVisible = false;
        cancelMatrixSubdivisionSelection();
        syncMatrixSubdivisionControls();
    };
    const subdivisionCustomToggleHandler = () => {
        matrixSubdivisionCustomVisible = !matrixSubdivisionCustomVisible;
        cancelMatrixSubdivisionSelection();
        syncMatrixSubdivisionControls();
        if (matrixSubdivisionCustomVisible) subdivisionRowsInput?.focus();
    };
    const subdivisionApplyHandler = () => {
        if (matrixSubdivisionCustomVisible) {
            startMatrixSubdivisionSelection(
                subdivisionRowsInput?.value,
                subdivisionColsInput?.value
            );
            return;
        }
        const size = subdivisionPresetSelect?.value;
        startMatrixSubdivisionSelection(size, size);
    };
    const subdivisionInputKeyHandler = (event) => {
        if (event.key !== 'Enter') return;
        event.preventDefault();
        subdivisionApplyHandler();
    };
    const subdivisionEscapeHandler = (event) => {
        if (event.key === 'Escape') cancelMatrixSubdivisionSelection();
    };

    subdivisionPresetSelect?.addEventListener('change', subdivisionPresetChangeHandler);
    subdivisionCustomToggle?.addEventListener('click', subdivisionCustomToggleHandler);
    subdivisionApplyButton?.addEventListener('click', subdivisionApplyHandler);
    subdivisionClearButton?.addEventListener('click', clearMatrixSubdivisions);
    subdivisionRowsInput?.addEventListener('keydown', subdivisionInputKeyHandler);
    subdivisionColsInput?.addEventListener('keydown', subdivisionInputKeyHandler);
    document.addEventListener('keydown', subdivisionEscapeHandler);
    eventListenerCleanupFunctions.push(() => {
        subdivisionPresetSelect?.removeEventListener('change', subdivisionPresetChangeHandler);
        subdivisionCustomToggle?.removeEventListener('click', subdivisionCustomToggleHandler);
        subdivisionApplyButton?.removeEventListener('click', subdivisionApplyHandler);
        subdivisionClearButton?.removeEventListener('click', clearMatrixSubdivisions);
        subdivisionRowsInput?.removeEventListener('keydown', subdivisionInputKeyHandler);
        subdivisionColsInput?.removeEventListener('keydown', subdivisionInputKeyHandler);
        document.removeEventListener('keydown', subdivisionEscapeHandler);
    });
    syncMatrixSubdivisionControls();


    const saveDefaultTemplateBtn = document.getElementById('save-default-template');
    if (saveDefaultTemplateBtn) {
        saveDefaultTemplateBtn.addEventListener('click', saveDefaultScene);
        eventListenerCleanupFunctions.push(() => {
            saveDefaultTemplateBtn.removeEventListener('click', saveDefaultScene);
        });
    }

    const selectTemplateBtn = document.getElementById('select-template');
    if (selectTemplateBtn) {
        selectTemplateBtn.addEventListener('click', showSceneSelector);
        eventListenerCleanupFunctions.push(() => {
            selectTemplateBtn.removeEventListener('click', showSceneSelector);
        });
    }

    // 示例/字段：场景下拉框逻辑（新的下拉框方案）
    const sceneDropdown = document.getElementById('select-template-dropdown');
    if (sceneDropdown) {
        // 说明：初始化填充下拉框
        await sceneManagement.refreshSceneDropdown('select-template-dropdown');

        const loadSceneFromDropdown = async (scene) => {
            if (!scene) return;

            if (sceneManagement.isDefaultSceneName(scene)) {
                await sceneManagement.loadDefaultScene(handleSceneLoaded);
            } else {
                await sceneManagement.loadScene(scene, handleSceneLoaded);
            }
            sceneManagement.setCurrentSceneName(scene);
            renderSceneCustomDropdown(sceneDropdown);
        };

        const dropdownChangeHandler = async function () {
            await loadSceneFromDropdown(this.value);
        };
        sceneDropdown.addEventListener('change', dropdownChangeHandler);
        eventListenerCleanupFunctions.push(() => {
            sceneDropdown.removeEventListener('change', dropdownChangeHandler);
        });

        const attachCustomDropdownHandlers = (customDropdown) => {
            if (!ENABLE_CUSTOM_SCENE_DROPDOWN || !customDropdown || customDropdown.dataset.handlersBound === 'true') {
                return;
            }
            customDropdown.dataset.handlersBound = 'true';

            const outsideClickHandler = (event) => {
                const customMenu = getSceneCustomDropdownMenu();
                const clickedInsideTrigger = customDropdown.contains(event.target);
                const clickedInsideMenu = customMenu ? customMenu.contains(event.target) : false;
                if (!clickedInsideTrigger && !clickedInsideMenu) {
                    closeSceneCustomDropdown();
                }
            };
            document.addEventListener('click', outsideClickHandler);

            const dropdownResizeHandler = () => {
                if (customDropdown.classList.contains('open')) {
                    positionSceneCustomOptions(customDropdown);
                }
            };
            window.addEventListener('resize', dropdownResizeHandler);
            window.addEventListener('scroll', dropdownResizeHandler, true);

            eventListenerCleanupFunctions.push(() => {
                document.removeEventListener('click', outsideClickHandler);
                window.removeEventListener('resize', dropdownResizeHandler);
                window.removeEventListener('scroll', dropdownResizeHandler, true);
                delete customDropdown.dataset.handlersBound;
            });
        };

        const sceneOptionsUpdatedHandler = () => {
            renderSceneCustomDropdown(sceneDropdown);
            attachCustomDropdownHandlers(getSceneCustomDropdown());
        };
        sceneDropdown.addEventListener('scene-options-updated', sceneOptionsUpdatedHandler);
        eventListenerCleanupFunctions.push(() => {
            sceneDropdown.removeEventListener('scene-options-updated', sceneOptionsUpdatedHandler);
        });

        const customDropdown = ensureSceneDropdownVisible();
        attachCustomDropdownHandlers(customDropdown);

        const delegatedSceneDropdownHandler = (event) => {
            void handleSceneCustomDropdownAction(event);
        };
        document.addEventListener('click', delegatedSceneDropdownHandler, true);
        eventListenerCleanupFunctions.push(() => {
            document.removeEventListener('click', delegatedSceneDropdownHandler, true);
        });

        sceneManagement.setCurrentSceneName(sceneDropdown.value || '默认配置');

        const legacyDeleteSceneBtn = document.getElementById('delete-scene-btn');
        if (legacyDeleteSceneBtn) {
            const deleteHandler = async () => {
                const sceneName = sceneDropdown.value;
                if (!sceneName || sceneManagement.isDefaultSceneName(sceneName)) return;

                const confirmed = await showConfirm(`确定要删除场景"${sceneName}"吗？`, '删除场景');
                if (!confirmed) return;

                try {
                    await sceneManagement.deleteScene(sceneName);
                    await sceneManagement.refreshSceneDropdown('select-template-dropdown');
                    sceneDropdown.value = '默认配置';
                    sceneManagement.setCurrentSceneName('默认配置');
                    await sceneManagement.loadDefaultScene(handleSceneLoaded);
                } catch (error) {
                    console.error('删除场景失败:', error);
                }
            };
            legacyDeleteSceneBtn.addEventListener('click', deleteHandler);
            eventListenerCleanupFunctions.push(() => {
                legacyDeleteSceneBtn.removeEventListener('click', deleteHandler);
            });
        }

        const refreshTemplateBtn = document.getElementById('refresh-template-list');
        if (refreshTemplateBtn) {
            const refreshHandler = async () => {
                await sceneManagement.refreshSceneDropdown('select-template-dropdown');
            };
            refreshTemplateBtn.addEventListener('click', refreshHandler);
            eventListenerCleanupFunctions.push(() => {
                refreshTemplateBtn.removeEventListener('click', refreshHandler);
            });
        }
    }

    const saveTemplateBtn = document.getElementById('save-template');
    if (saveTemplateBtn) {
        const saveTemplateHandler = async () => {
            const savedScene = await saveCurrentScene();
            if (!savedScene) return;
            // 保存成功后刷新，保留失败提示并避免取消时发起额外请求
            await sceneManagement.refreshSceneDropdown('select-template-dropdown');
            if (savedScene && sceneDropdown) {
                sceneDropdown.value = savedScene;
            }
        };
        saveTemplateBtn.addEventListener('click', saveTemplateHandler);
        eventListenerCleanupFunctions.push(() => {
            saveTemplateBtn.removeEventListener('click', saveTemplateHandler);
        });
    }

    const slice1Btn = document.getElementById('slice-1');
    if (slice1Btn) {
        const createSliceHandler = () => createSlice();
        slice1Btn.addEventListener('click', createSliceHandler);
        eventListenerCleanupFunctions.push(() => {
            slice1Btn.removeEventListener('click', createSliceHandler);
        });
    }

    const layerTabs = document.querySelectorAll('.layer-tab');
    const tabClickHandlers = [];
    layerTabs.forEach(tab => {
        const handler = function () {
            document.querySelectorAll('.layer-tab').forEach(t => t.classList.remove('active'));
            this.classList.add('active');
            addToCommandLog('图层管理', 'info', `切换到${this.textContent}标签`);
        };
        tab.addEventListener('click', handler);
        tabClickHandlers.push({ tab, handler });
    });
    eventListenerCleanupFunctions.push(() => {
        tabClickHandlers.forEach(({ tab, handler }) => {
            tab.removeEventListener('click', handler);
        });
    });

    const resizeHandler = () => {
        clearTimeout(resizeTimeout);
        resizeTimeout = setTimeout(async () => {
            if (utils.isConfigValid(state.canvasConfig)) {
                await updateCanvasSizeByLayout();
                drawCanvasNow();
            }
        }, 100);
    };
    window.addEventListener('resize', resizeHandler);
    eventListenerCleanupFunctions.push(() => {
        window.removeEventListener('resize', resizeHandler);
        if (resizeTimeout) {
            clearTimeout(resizeTimeout);
            resizeTimeout = null;
        }
    });

}

// ==================== 导出函数 ====================
// 保持与原文件相同的导出接口

export async function initializeLayerMatrix() {
    initializeCanvas();
    await initializeEventListeners();
    await loadSystemConfig();
    await updateLayerMatrix();
    updateLayerSelection();
}

export async function updateLayerMatrix() {
    if (state.isDragging || state.isResizing) return state.refreshingPromise;

    state.setDragState({ interactionSession: null, snapGuides: [] });
    ensureSceneDropdownVisible();
    // 每次切换到图层矩阵页时重新初始化 canvas（确保容器已可见、尺寸正确）
    initializeCanvas();
    // 强制重新加载系统配置（canvasConfig 可能因页面未激活时尺寸为0而未正确初始化）
    state.canvasConfig.loaded = false;
    
    const promise = dataLoader.updateLayerMatrix(
        state.layers, state.setLayersLoadStatus, state.ALL_AVAILABLE_LAYERS, state.canvasConfig,
        loadSystemConfig, loadAllAvailableLayers, renderLayerMatrix,
        updateCanvasSizeByLayout, drawCanvas, updateLayerSelection,
        state.isDragging, state.isResizing
    );
    state.setRefreshingPromise(promise);
    return promise;
}

export function selectLayer(layerId) {
    selectLayerInternal(layerId);
}

export async function showLayerProperties(layerId) {
    return showLayerPropertiesInternal(layerId);
}

export function getSelectedLayer() {
    return layerManagement.getSelectedLayer(state.selectedLayer);
}

export async function createLayer(layerId, layerType) {
    return addLayerToMatrix(layerId, layerType);
}

// 导出清理函数
export function cleanupLayerMatrix() {
    cleanupEventListeners();
    canvas.cleanupCanvas();
    if (drawFrameId !== null) {
        cancelAnimationFrame(drawFrameId);
        drawFrameId = null;
    }
    qrCodeHelpers.cleanupQRCodeListeners();
    qrCodeHelpers.cleanupLayer40Listeners();
    qrCodeHelpers.cleanupQRCodePreview();
    hideQuickAlignMenu();
    state.canvas?.classList.remove('matrix-subdivision-selecting');
    matrixDropTarget = null;
    matrixSubdivisions = [];
    activeMatrixSubdivisionKey = '';
    pendingMatrixSubdivision = null;
    matrixSubdivisionCustomVisible = false;
    state.setSelectedItems([]);
    state.setDragState({ interactionSession: null, snapGuides: [] });

    // 自动保存已关闭；保留兼容清理调用。
    layerManagement.cancelSaveTimer();

    // 清理 updateLayerFromInputs 的 requestAnimationFrame
    if (updateLayerFromInputs.rafId) {
        cancelAnimationFrame(updateLayerFromInputs.rafId);
        updateLayerFromInputs.rafId = null;
    }
}
