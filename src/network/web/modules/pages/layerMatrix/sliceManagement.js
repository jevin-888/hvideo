// 切片管理模块
import { apiPost, apiPut, apiAction } from '../../core/api.js';
import { addToCommandLog } from '../../core/commandLog.js';
import { clearContainer } from '../../utils/domHelpers.js';
import { handleApiOperation } from '../../utils/apiHelpers.js';
import {
    collectSliceKeys,
    getSliceData,
    hydrateSliceFields,
    isSliceVisible,
    normalizeSliceVisualFields
} from './sliceModel.js';
// 注意：refreshLayerInfo 需要从外部传入，避免循环依赖

function isCurrentSliceForm(layerId, sliceKey) {
    const form = document.getElementById('layer-properties-form');
    return !!form &&
        Number(form.dataset.layerId) === Number(layerId) &&
        form.dataset.sliceKey === String(sliceKey);
}

function applySliceRectFromDom(sliceConfig) {
    if (!document.getElementById('slice-x')) return false;

    const getValue = (id, defaultVal = 0) => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        const value = parseFloat(el.value);
        return Number.isNaN(value) ? defaultVal : value;
    };

    const getIntValue = (id, defaultVal = 0) => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        const value = parseInt(el.value, 10);
        return Number.isNaN(value) ? defaultVal : value;
    };

    const x = getIntValue('slice-x', 0);
    const y = getIntValue('slice-y', 0);
    const width = getIntValue('slice-width', 0);
    const height = getIntValue('slice-height', 0);
    sliceConfig.coordinate = `${x} ${y} ${width} ${height}`;
    sliceConfig.range = sliceConfig.coordinate;
    sliceConfig.position = `${x} ${y}`;
    sliceConfig.size = `${width} ${height}`;
    sliceConfig.x = x;
    sliceConfig.y = y;
    sliceConfig.width = width;
    sliceConfig.height = height;

    const rotationInput = document.getElementById('slice-rotation');
    if (rotationInput) {
        const rot = Math.min(360, Math.max(0, Math.round(Number(getValue('slice-rotation', 0)))));
        sliceConfig.rotate = rot;
        sliceConfig.rotation = rot;
    }

    const alphaInput = document.getElementById('slice-alpha');
    if (alphaInput) {
        const alphaInputVal = getValue('slice-alpha', 255);
        sliceConfig.alpha = alphaInputVal / 255;
        sliceConfig.transparency = Math.round(alphaInputVal);
    }

    const priorityInput = document.getElementById('slice-priority');
    if (priorityInput) {
        sliceConfig.priority = getIntValue('slice-priority', sliceConfig.priority ?? 0);
    }

    return true;
}

function mergeFormSliceFields(layerId, sliceKey, sliceConfig) {
    if (!isCurrentSliceForm(layerId, sliceKey)) {
        return sliceConfig;
    }

    const getValue = (id, defaultVal = 0) => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        const value = parseFloat(el.value);
        return Number.isNaN(value) ? defaultVal : value;
    };

    const getIntValue = (id, defaultVal = 0) => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        const value = parseInt(el.value, 10);
        return Number.isNaN(value) ? defaultVal : value;
    };

    const getStringValue = (id, defaultVal = '') => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        return el.value.trim() || defaultVal;
    };

    applySliceRectFromDom(sliceConfig);

    const invertEl = document.getElementById('slice-invert');
    if (invertEl) {
        sliceConfig.invert = invertEl.tagName === 'SELECT' ? getIntValue('slice-invert', 0) : (invertEl.checked ? 1 : 0);
        sliceConfig.mirror = (sliceConfig.invert === 1 || sliceConfig.invert === 3);
    }

    if (document.getElementById('slice-volume')) {
        sliceConfig.volume = getValue('slice-volume', 100) / 100;
    }

    if (document.getElementById('slice-gaussian-blur')) {
        const blur = Math.round(getValue('slice-gaussian-blur', 0));
        sliceConfig.gaussian_blur = blur;
        sliceConfig.gaussianBlur = blur;
    }

    if (document.getElementById('slice-fit-mode')) {
        const fitMode = getIntValue('slice-fit-mode', 0) > 0 ? 1 : 0;
        sliceConfig.fit_mode = fitMode;
        sliceConfig.fitMode = fitMode;
    }

    if (document.getElementById('slice-roam-mode')) {
        const loopEl = document.getElementById('slice-roam-loop');
        const roamConfig = {
            enabled: true,
            mode: getIntValue('slice-roam-mode', 0),
            speed: getValue('slice-roam-speed', 100),
            rangeX: getValue('slice-roam-range-x', 500),
            rangeY: getValue('slice-roam-range-y', 500),
            radius: getValue('slice-roam-radius', 200),
            loop: loopEl ? loopEl.checked : true
        };
        sliceConfig.roamConfig = roamConfig;
        sliceConfig.roam_config = roamConfig;
    }

    if (document.getElementById('slice-capture-type')) {
        const captureType = getStringValue('slice-capture-type', '');
        if (captureType) {
            sliceConfig.capture_type = captureType;
            sliceConfig.captureType = captureType;
            sliceConfig.capture_index = 0;
            sliceConfig.captureIndex = 0;
        } else {
            delete sliceConfig.capture_type;
            delete sliceConfig.captureType;
            delete sliceConfig.capture_index;
            delete sliceConfig.captureIndex;
        }
    }

    if (document.getElementById('slice-filter-mode')) {
        sliceConfig.filter_mode = getIntValue('slice-filter-mode');
    }
    if (document.getElementById('slice-fade-in-time')) {
        sliceConfig.fade_in_time = getValue('slice-fade-in-time', 0.5);
    }
    if (document.getElementById('slice-fade-out-time')) {
        sliceConfig.fade_out_time = getValue('slice-fade-out-time', 0.5);
    }
    if (document.getElementById('slice-display-duration')) {
        sliceConfig.display_duration = getValue('slice-display-duration', 3.0);
    }

    const animatedEl = document.getElementById('slice-animated');
    if (animatedEl) {
        sliceConfig.animated = animatedEl.value === 'animated';
    }

    if (document.getElementById('slice-shape-type')) {
        const shapeType = getIntValue('slice-shape-type', 0);
        const shapeParam = getValue('slice-shape-param', 0.0);
        sliceConfig.shape_type = shapeType;
        sliceConfig.shapeType = shapeType;
        sliceConfig.shape_param = shapeParam;
        sliceConfig.shapeParam = shapeParam;
    }

    const blackToTransparentEl = document.getElementById('slice-black-to-transparent');
    if (blackToTransparentEl) {
        if (blackToTransparentEl.tagName === 'SELECT') {
            sliceConfig.black_to_transparent = blackToTransparentEl.value === 'true';
        } else {
            sliceConfig.black_to_transparent = blackToTransparentEl.checked;
        }
        sliceConfig.blackToTransparent = sliceConfig.black_to_transparent;
    }

    if (document.getElementById('slice-text-content')) {
        sliceConfig.text = getStringValue('slice-text-content');
        sliceConfig.text_color = getStringValue('slice-text-color', '1.0 1.0 1.0 1.0');
        sliceConfig.bg_color = getStringValue('slice-text-bg-color', '0.0 0.0 0.0 0.0');

        if (document.getElementById('slice-text-display-mode')) {
            sliceConfig.alignment = getIntValue('slice-text-display-mode', 1);
        } else if (document.getElementById('slice-text-alignment')) {
            sliceConfig.alignment = getIntValue('slice-text-alignment', 1);
        }

        sliceConfig.scroll_speed = getIntValue('slice-text-scroll-speed', 200);

        if (document.getElementById('slice-text-font-size')) {
            sliceConfig.font_size = getValue('slice-text-font-size', 48.0);
        }
        if (document.getElementById('slice-text-font-file')) {
            const fontVal = getStringValue('slice-text-font-file');
            sliceConfig.font_file = fontVal ? (fontVal.includes('/') ? fontVal.replace(/^.*\//, '') : fontVal) : '';
        }
    }

    if (document.getElementById('slice-display-align')) {
        sliceConfig.display_align = getIntValue('slice-display-align', 0);
    }

    const slicePlaylistEl = document.getElementById('slice-playlist-id');
    if (slicePlaylistEl) {
        sliceConfig.playlistId = getStringValue('slice-playlist-id', '');
    }
    if (document.getElementById('slice-show-count')) {
        sliceConfig.show_count = getIntValue('slice-show-count', 3);
    }
    if (document.getElementById('slice-display-duration')) {
        sliceConfig.display_duration = getValue('slice-display-duration', 5.0);
    }
    if (document.getElementById('slice-start-hint-time')) {
        sliceConfig.start_hint_time = getValue('slice-start-hint-time', 10.0);
    }
    if (document.getElementById('slice-end-hint-time')) {
        sliceConfig.end_hint_time = getValue('slice-end-hint-time', 10.0);
    }

    return sliceConfig;
}

export async function createSlice(
    selectedLayer, layers, canvasConfig, refreshLayerInfo, updateSliceInfo, selectSlice, drawCanvas
) {
    if (!selectedLayer) {
        addToCommandLog('切片操作', 'error', '请先选择一个图层');
        return;
    }

    const layer = layers.find(l => Number(l.id) === Number(selectedLayer));
    if (!layer) {
        addToCommandLog('切片操作', 'error', '未找到选中的图层');
        return;
    }

    hydrateSliceFields(layer);

    // 自动计算下一个可用的切片索引：删除中间切片后优先复用空洞。
    const MAX_SLICES = 6;
    const usedSliceIndexes = new Set();
    collectSliceKeys(layer).forEach(key => {
        const index = parseInt(key.substring(5), 10);
        if (Number.isInteger(index) && index > 0) {
            usedSliceIndexes.add(index);
        }
    });

    // 限制最多 6 个切片
    if (usedSliceIndexes.size >= MAX_SLICES) {
        addToCommandLog('切片操作', 'warning', `切片数量已达上限（最多 ${MAX_SLICES} 个）`);
        return;
    }

    let sliceIndex = 1;
    while (usedSliceIndexes.has(sliceIndex) && sliceIndex <= MAX_SLICES) {
        sliceIndex += 1;
    }

    try {
        const posX = 0;
        const posY = 0;
        const width = canvasConfig?.width || layer?.size?.width || 0;
        const height = canvasConfig?.height || layer?.size?.height || 0;

        const sliceConfig = {
            coordinate: `${posX} ${posY} ${width} ${height}`,
            range: `${posX} ${posY} ${width} ${height}`,
            rotate: layer?.rotation || 0,
            transparency: Math.round((layer?.alpha || 1.0) * 255),
            enable: layer?.visible !== false,
            mirror: layer?.mirror || false,
            priority: layer?.priority || 0,
            position: layer?.position ? `${posX} ${posY}` : "0 0",
            size: layer?.size ? `${width} ${height}` : "0 0",
            visible: layer?.visible !== false,
            rotation: layer?.rotation || 0,
            alpha: layer?.alpha || 1.0,
            scale: layer?.scale || 1.0,
            shape_type: layer?.shape_type ?? layer?.shapeType ?? 0,
            shapeType: layer?.shape_type ?? layer?.shapeType ?? 0,
            shape_param: layer?.shape_param ?? layer?.shapeParam ?? 0.0,
            shapeParam: layer?.shape_param ?? layer?.shapeParam ?? 0.0,
            black_to_transparent: layer?.black_to_transparent ?? layer?.blackToTransparent ?? false,
            blackToTransparent: layer?.black_to_transparent ?? layer?.blackToTransparent ?? false,
            invert: layer?.invert ?? 0,
            gaussian_blur: layer?.gaussian_blur ?? layer?.gaussianBlur ?? 0,
            gaussianBlur: layer?.gaussian_blur ?? layer?.gaussianBlur ?? 0,
            fit_mode: Number(layer?.fit_mode ?? layer?.fitMode ?? 0) > 0 ? 1 : 0,
            fitMode: Number(layer?.fit_mode ?? layer?.fitMode ?? 0) > 0 ? 1 : 0
        };
        // 采集切片默认跟随主图层；只有切片面板显式选择输入时才写 captureType。
        normalizeSliceVisualFields(sliceConfig);

        await handleApiOperation(
            apiAction('layers', 'create_slice', {
                    layerId: parseInt(selectedLayer, 10),
                    slice_index: sliceIndex,
                    slice_config: sliceConfig
                }),
            '切片操作',
            `已创建切片 ${sliceIndex}`,
            '创建切片失败',
            async (result) => {
                const data = result;
                const sliceKey = data?.slice_key || `slice${sliceIndex}`;
                const createdSliceConfig = data?.slice_config && typeof data.slice_config === 'object'
                    ? data.slice_config
                    : sliceConfig;
                layer[sliceKey] = { ...createdSliceConfig };
                if (drawCanvas) drawCanvas();
                const refreshed = await refreshLayerInfo(selectedLayer, layers);
                const refreshedLayer = layers.find(l => Number(l.id) === Number(selectedLayer));
                if (refreshed && refreshedLayer && !refreshedLayer[sliceKey]) {
                    refreshedLayer[sliceKey] = { ...createdSliceConfig };
                }
                updateSliceInfo(selectedLayer);
                if (selectSlice) {
                    selectSlice(selectedLayer, sliceKey);
                }
                if (drawCanvas) drawCanvas();
            }
        );
    } catch (error) {
        addToCommandLog('切片操作', 'error', `创建切片失败: ${error.message}`);
    }
}

/**
 * 删除切片
 */
export async function deleteSlice(
    layerId, sliceKey, layers, refreshLayerInfo, updateSliceInfo, showSliceProperties, drawCanvas
) {
    const properties = {};
    properties[sliceKey] = null;

    return handleApiOperation(
        apiPut(`/layers/${layerId}`, properties),
        '切片操作',
        `已删除切片 ${sliceKey}`,
        '删除切片失败: 服务器无响应',
        async (result) => {
            await refreshLayerInfo(layerId, layers);
            updateSliceInfo(layerId);
            const form = document.getElementById('layer-properties-form');
            if (form && Number(form.dataset.layerId) === Number(layerId) && form.dataset.sliceKey === String(sliceKey)) {
                delete form.dataset.layerId;
                delete form.dataset.sliceKey;
                form.innerHTML = '<div class="no-layer-selected">已删除切片</div>';
            }
            drawCanvas();
        }
    );
}

/**
 * 选择切片
 */
export async function selectSlice(
    layerId, sliceKey, layers,
    moveLayerToTop, updateSelectionUI, updateSliceInfo, showSliceProperties, drawCanvas
) {
    moveLayerToTop(layerId, layers);

    updateSelectionUI();
    updateSliceInfo(layerId);
    await showSliceProperties(layerId, sliceKey);
    drawCanvas();

    const sliceIndex = sliceKey.replace('slice', '');
    addToCommandLog('选择切片', 'info', `选择切片: Layer${layerId} 切片${sliceIndex}`);
}

/**
 * 切换切片可见性
 */
export async function toggleSliceVisibility(
    layerId, sliceKey, layers, refreshLayerInfo, updateSliceInfo, showSliceProperties, drawCanvas
) {
    const layer = layers.find(l => Number(l.id) === Number(layerId));
    if (!layer) return;

    const sliceData = getSliceData(layer, sliceKey);
    if (!sliceData) return;

    const currentVisible = isSliceVisible(sliceData);
    const newVisible = !currentVisible;

    const sliceConfig = { ...sliceData };
    sliceConfig.visible = newVisible;
    sliceConfig.enable = newVisible;
    normalizeSliceVisualFields(sliceConfig);

    const properties = {};
    properties[sliceKey] = sliceConfig;

    await handleApiOperation(
        apiPut(`/layers/${layerId}`, properties),
        '切片操作',
        null, // 静默成功
        null, // 错误已由API层处理
        async (result) => {
            await refreshLayerInfo(layerId, layers);
            updateSliceInfo(layerId);
            showSliceProperties(layerId, sliceKey);
            drawCanvas();
        }
    );
}

/**
 * 更新切片信息
 * @param {Function} getLayerDisplayName - 获取图层显示名称，统一 "Layer{id} 名称"
 * @param {Array} ALL_AVAILABLE_LAYERS - 图层模板列表
 */
export function updateSliceInfo(layerId, layers, selectedSlice, onSelectSlice, onDeleteSlice, getLayerDisplayName, ALL_AVAILABLE_LAYERS) {
    const layer = layers.find(l => Number(l.id) === Number(layerId));
    const sliceListEl = document.getElementById('slice-list');

    if (!sliceListEl) return;

    let sliceCount = 0;
    const sliceKeys = [];
    if (layer) {
        hydrateSliceFields(layer);
        sliceKeys.push(...collectSliceKeys(layer));
        sliceCount = sliceKeys.length;
    }

    clearContainer(sliceListEl);

    if (sliceCount > 0) {
        const layerDisplayName = (getLayerDisplayName && layer) ? getLayerDisplayName(layer, ALL_AVAILABLE_LAYERS || []) : `Layer${layerId} 图层`;
        sliceKeys.forEach(key => {
            const sliceData = layer[key];
            const sliceIndex = key.replace('slice', '');
            const sliceName = `${layerDisplayName} 切片${sliceIndex}`;

            const selectionItem = document.createElement('div');
            selectionItem.className = 'selection-item';
            selectionItem.dataset.sliceKey = key;

            if (selectedSlice && selectedSlice.layerId === layerId && selectedSlice.sliceKey === key) {
                selectionItem.classList.add('selected');
            }

            const sliceNameEl = document.createElement('span');
            sliceNameEl.className = 'selection-item-name';
            sliceNameEl.textContent = sliceName;

            const deleteBtn = document.createElement('button');
            deleteBtn.className = 'selection-item-delete';
            deleteBtn.innerHTML = '×';
            deleteBtn.title = '删除切片';
            deleteBtn.setAttribute('aria-label', '删除切片');

            deleteBtn.addEventListener('click', async (e) => {
                e.preventDefault();
                e.stopPropagation();
                if (onDeleteSlice) {
                    await onDeleteSlice(layerId, key);
                }
            });

            selectionItem.appendChild(sliceNameEl);
            selectionItem.appendChild(deleteBtn);

            selectionItem.addEventListener('click', async (e) => {
                if (e.target.closest('.selection-item-delete')) {
                    return;
                }
                if (onSelectSlice) {
                    onSelectSlice(layerId, key);
                }
            });

            sliceListEl.appendChild(selectionItem);
        });
    } else {
        sliceListEl.innerHTML = '<div class="no-slices">暂无切片，点击"创建切片"按钮创建</div>';
    }
}

/**
 * 应用切片属性
 */
export async function applySliceProperties(
    layerId, sliceKey, layers, drawCanvas
) {
    const layer = layers.find(l => Number(l.id) === Number(layerId));
    if (!layer) return;

    const sliceData = getSliceData(layer, sliceKey);
    if (!sliceData) return;

    const properties = {};
    const sliceConfig = mergeFormSliceFields(layerId, sliceKey, { ...sliceData });

    normalizeSliceVisualFields(sliceConfig);
    properties[sliceKey] = sliceConfig;
    // Layer 41 关联播放列表为图层级字段，需一并提交以更新图层
    if (isCurrentSliceForm(layerId, sliceKey) && document.getElementById('slice-playlist-id')) {
        properties.playlistId = document.getElementById('slice-playlist-id').value.trim();
    }

    return handleApiOperation(
        apiPut(`/layers/${layerId}`, properties),
        '应用切片属性',
        `成功更新切片 ${sliceKey}`,
        '更新切片失败: 服务器未确认更改',
        (result) => {
            Object.assign(sliceData, sliceConfig);
            if (properties.playlistId !== undefined) {
                layer.playlistId = properties.playlistId;
            }
            drawCanvas();
        }
    );
}


export async function syncSliceConfig(
    layerId, sliceKey, layers, drawCanvas
) {
    const layer = layers.find(l => Number(l.id) === Number(layerId));
    if (!layer) return false;

    const sliceData = getSliceData(layer, sliceKey);
    if (!sliceData) return false;

    const properties = {};
    properties[sliceKey] = normalizeSliceVisualFields(mergeFormSliceFields(layerId, sliceKey, { ...sliceData }));

    return handleApiOperation(
        apiPut(`/layers/${layerId}`, properties),
        '应用切片属性',
        `成功更新切片 ${sliceKey}`,
        '更新切片失败: 服务器未确认更改',
        () => {
            if (drawCanvas) drawCanvas();
        }
    );
}
