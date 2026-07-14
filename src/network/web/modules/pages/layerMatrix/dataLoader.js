// 数据加载模块
import { apiGet } from '../../core/api.js';
import { addToCommandLog } from '../../core/commandLog.js';
import { isConfigValid } from './utils.js';
import { normalizeLayerFieldAliases } from './layerFieldModel.js';

function hydrateLayerSlices(layer) {
    if (!layer || typeof layer !== 'object' || !layer.slices || typeof layer.slices !== 'object') return layer;
    Object.entries(layer.slices).forEach(([key, value]) => {
        if (key && key.startsWith('slice') && /^\d+$/.test(key.substring(5)) && value !== null && value !== undefined) {
            layer[key] = value;
        }
    });
    return layer;
}

function normalizeLayerId(layer) {
    const id = Number(layer && layer.id);
    return Number.isInteger(id) ? id : null;
}

function normalizeLayerIdentity(layer) {
    const id = normalizeLayerId(layer);
    return id === null ? normalizeLayerFieldAliases(layer) : normalizeLayerFieldAliases({ ...layer, id });
}

function normalizeLayerListPayload(payload) {
    return Array.isArray(payload) ? payload : null;
}

function describeLayerPayloadError(payload) {
    if (payload === null) return '返回 null';
    if (payload === undefined) return '返回 undefined';
    if (Array.isArray(payload)) return '';
    if (payload && typeof payload === 'object') {
        const keys = Object.keys(payload).slice(0, 8);
        return keys.length > 0 ? `对象字段: ${keys.join(', ')}` : '返回空对象';
    }
    return `返回类型: ${typeof payload}`;
}

function normalizeUniqueLayers(layersData) {
    const seenIds = new Set();
    const duplicateIds = new Set();
    const normalizedLayers = [];

    layersData
        .map(hydrateLayerSlices)
        .sort((a, b) => (a.id || 0) - (b.id || 0))
        .forEach(layer => {
            const layerId = normalizeLayerId(layer);
            if (layerId === null) {
                normalizedLayers.push(layer);
                return;
            }

            if (seenIds.has(layerId)) {
                duplicateIds.add(layerId);
                return;
            }

            seenIds.add(layerId);
            normalizedLayers.push(normalizeLayerFieldAliases({ ...layer, id: layerId }));
        });

    if (duplicateIds.size > 0) {
        addToCommandLog(
            '加载图层',
            'warning',
            `检测到重复图层ID ${Array.from(duplicateIds).join(', ')}，已跳过重复项`
        );
    }

    return normalizedLayers;
}

/**
 * 加载所有系统支持的图层（从授权图层池获取）
 */
export async function loadAllAvailableLayers(ALL_AVAILABLE_LAYERS) {
    try {
        const response = await apiGet('/layers/authorized');
        const layerList = normalizeLayerListPayload(response);

        if (layerList) {
            const normalizedLayers = layerList
                .map(normalizeLayerIdentity)
                .sort((a, b) => (a.id || 0) - (b.id || 0));

            ALL_AVAILABLE_LAYERS.length = 0;
            ALL_AVAILABLE_LAYERS.push(...normalizedLayers);
            // 后端已按 license.enabled_layers 返回授权图层池，不再追加未授权的 10/11

            if (normalizedLayers.length > 0) {
                addToCommandLog('加载图层', 'success', `成功加载 ${normalizedLayers.length} 个图层定义`);
            } else {
                addToCommandLog('加载图层', 'warning', '后台返回的图层定义列表为空');
            }
        } else {
            const detail = describeLayerPayloadError(response);
            addToCommandLog('加载图层', 'error', `获取图层定义失败：响应数据格式错误${detail ? `（${detail}）` : ''}`);
        }
    } catch (error) {
        addToCommandLog('加载图层', 'error', `无法获取图层定义: ${error.message || '未知错误'}`);
    }
}

/**
 * 内部更新函数，用于处理异步逻辑
 */
export async function performLayerMatrixUpdate(
    layers, setLayersLoadStatus, ALL_AVAILABLE_LAYERS, canvasConfig,
    loadSystemConfig, loadAllAvailableLayers, renderLayerMatrix,
    updateCanvasSizeByLayout, drawCanvas, updateLayerSelection,
    isDragging = false, isResizing = false
) {
    if (isDragging || isResizing) return;
    const isFirstLoad = ALL_AVAILABLE_LAYERS.length === 0;
    setLayersLoadStatus('pending');

    if (isFirstLoad) {
        renderLayerMatrix();
        updateLayerSelection();
    }

    try {
        if (!canvasConfig.loaded) {
            await loadSystemConfig();
        }

        await loadAllAvailableLayers(ALL_AVAILABLE_LAYERS);

        const layersData = await apiGet('/layers');
        const layerList = normalizeLayerListPayload(layersData);

        if (layersData === null || layersData === undefined) {
            setLayersLoadStatus('error');
            layers.length = 0;
            addToCommandLog('加载图层', 'error', '获取图层数据失败：服务器未返回数据');
        } else if (layerList) {
            layers.length = 0;
            layers.push(...normalizeUniqueLayers(layerList));
            setLayersLoadStatus(layers.length > 0 ? 'success' : 'empty');
            if (layers.length > 0) {
                addToCommandLog('加载图层', 'success', `成功加载 ${layers.length} 个图层`);
            } else {
                addToCommandLog('加载图层', 'warning', '图层列表为空');
            }
        } else {
            setLayersLoadStatus('error');
            layers.length = 0;
            const detail = describeLayerPayloadError(layersData);
            addToCommandLog('加载图层', 'error', `图层数据格式错误：期望数组，实际类型为 ${typeof layersData}${detail ? `（${detail}）` : ''}`);
        }

        renderLayerMatrix();
        if (isConfigValid(canvasConfig)) {
            await updateCanvasSizeByLayout();
        }
        drawCanvas();
        updateLayerSelection();
    } catch (error) {
        setLayersLoadStatus('error');
        layers.length = 0;
        addToCommandLog('加载图层', 'error', `加载图层失败: ${error.message}`);
        renderLayerMatrix();
        updateLayerSelection();
    }
}

/**
 * 更新图层矩阵（公共接口）
 * 上层（layerMatrix.js）负责维护 refreshingPromise，故此处不再接收。
 */
export async function updateLayerMatrix(
    layers, setLayersLoadStatus, ALL_AVAILABLE_LAYERS, canvasConfig,
    loadSystemConfig, loadAllAvailableLayers, renderLayerMatrix,
    updateCanvasSizeByLayout, drawCanvas, updateLayerSelection,
    isDragging = false, isResizing = false
) {
    if (isDragging || isResizing) return;

    return performLayerMatrixUpdate(
        layers, setLayersLoadStatus, ALL_AVAILABLE_LAYERS, canvasConfig,
        loadSystemConfig, loadAllAvailableLayers, renderLayerMatrix,
        updateCanvasSizeByLayout, drawCanvas, updateLayerSelection,
        isDragging, isResizing
    );
}

