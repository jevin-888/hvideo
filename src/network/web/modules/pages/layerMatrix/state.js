// 图层矩阵状态管理模块
// 管理所有全局状态变量

// 图层选择状态
export let selectedLayer = null;
export let selectedSlice = null; // 格式: {layerId: 1, sliceKey: 'slice1'}
export let selectedItems = []; // 格式: [{type:'layer', layerId:1}]

// 画布相关
export let canvas = null;
export let ctx = null;
export let dpr = 1; // 设备像素比，用于高清屏幕适配

// 图层数据
export let layers = [];
export let layersLoadStatus = 'pending'; // 'pending', '成功', '错误', 'empty'
export let refreshingPromise = null; // 用于处理并发刷新的Promise

// 拖拽状态
export let isDragging = false;
export let isResizing = false;
export let resizeHandle = null;
export let dragStartX = 0;
export let dragStartY = 0;
export let dragLayerStartX = 0;
export let dragLayerStartY = 0;
export let dragLayerStartW = 0;
export let dragLayerStartH = 0;
export let interactionSession = null;
export let snapGuides = [];

// 统一配置对象（所有配置必须从后端config.json加载）
export let canvasConfig = {
    width: 0,              // 幕布分辨率宽度
    height: 0,             // 幕布分辨率高度
    cols: 0,               // 幕布布局列数
    rows: 0,               // 幕布布局行数
    regionWidth: 0,        // 单个区域宽度（仅用于日志）
    regionHeight: 0,       // 单个区域高度（仅用于日志）
    loaded: false          // 配置加载状态
};

// 所有可用的系统图层定义（仅由 GET /layers/authorized 填充，后端已按 license.enabled_layers 过滤）
export let ALL_AVAILABLE_LAYERS = [];

// 状态设置函数
export function setCanvasState(newCanvas, newCtx, newDpr) {
    if (newCanvas !== undefined) canvas = newCanvas;
    if (newCtx !== undefined) ctx = newCtx;
    if (newDpr !== undefined) dpr = newDpr;
}

export function setSelectedLayer(newSelectedLayer) {
    selectedLayer = newSelectedLayer;
}

export function setSelectedSlice(newSelectedSlice) {
    selectedSlice = newSelectedSlice;
}

export function setSelectedItems(newSelectedItems) {
    selectedItems = Array.isArray(newSelectedItems) ? newSelectedItems : [];
}

export function setLayers(newLayers) {
    layers = newLayers;
}

export function setLayersLoadStatus(newStatus) {
    layersLoadStatus = newStatus;
}

export function setRefreshingPromise(newPromise) {
    refreshingPromise = newPromise;
}

export function setDragState(updates) {
    if (updates.isDragging !== undefined) isDragging = updates.isDragging;
    if (updates.isResizing !== undefined) isResizing = updates.isResizing;
    if (updates.resizeHandle !== undefined) resizeHandle = updates.resizeHandle;
    if (updates.dragStartX !== undefined) dragStartX = updates.dragStartX;
    if (updates.dragStartY !== undefined) dragStartY = updates.dragStartY;
    if (updates.dragLayerStartX !== undefined) dragLayerStartX = updates.dragLayerStartX;
    if (updates.dragLayerStartY !== undefined) dragLayerStartY = updates.dragLayerStartY;
    if (updates.dragLayerStartW !== undefined) dragLayerStartW = updates.dragLayerStartW;
    if (updates.dragLayerStartH !== undefined) dragLayerStartH = updates.dragLayerStartH;
    if (updates.interactionSession !== undefined) interactionSession = updates.interactionSession;
    if (updates.snapGuides !== undefined) snapGuides = Array.isArray(updates.snapGuides) ? updates.snapGuides : [];
}
