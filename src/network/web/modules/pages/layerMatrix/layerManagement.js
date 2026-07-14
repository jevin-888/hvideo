// 图层管理模块
import { apiGet, apiPost, apiPut, apiDelete, sendLayerCommand, apiAction } from '../../core/api.js';
import { addToCommandLog } from '../../core/commandLog.js';
import { parsePosition, parseSize } from './utils.js';
import { handleApiOperation } from '../../utils/apiHelpers.js';
import { readLayerCommonInputs } from './panels/InputReader.js';
import { buildConfigLayerPatch, buildRuntimeLayerPatch, configLayerToUiLayer, normalizeLayerFieldAliases } from './layerFieldModel.js';

export function cancelSaveTimer() {
    // 自动保存已关闭；保留导出供页面清理流程调用。
}

/**
 * 创建图层
 */
export async function createLayer(
    layerId, layerType, updateLayerMatrix, layers, refreshLayerInfo,
    renderLayerMatrix = null, drawCanvas = null, updateLayerSelection = null
) {
    try {
        addToCommandLog('新建图层', 'info', `正在创建图层 ${layerId}，类型: ${layerType}`);

        return await handleApiOperation(
            apiAction('layers', 'create_layer', { layerId, layer_type: layerType }),
            '新建图层',
            `成功创建图层 ${layerId}`,
            '创建图层失败',
            async (result) => {
                if (result && result.layerId !== undefined) {
                    if (refreshLayerInfo && layers) {
                        const refreshed = await refreshLayerInfo(layerId, layers);
                        if (!refreshed && result.layer_info) {
                            const index = layers.findIndex(l => Number(l.id) === Number(layerId));
                            if (index !== -1) {
                                layers[index] = { ...layers[index], ...result.layer_info };
                            } else {
                                layers.push(result.layer_info);
                            }
                        }
                    } else if (layers && result.layer_info) {
                        const index = layers.findIndex(l => Number(l.id) === Number(layerId));
                        if (index !== -1) {
                            layers[index] = { ...layers[index], ...result.layer_info };
                        } else {
                            layers.push(result.layer_info);
                        }
                    } else if (updateLayerMatrix) {
                        await updateLayerMatrix();
                        return;
                    }
                    if (renderLayerMatrix) {
                        renderLayerMatrix();
                    }
                    if (updateLayerSelection) {
                        updateLayerSelection();
                    }
                    if (drawCanvas) {
                        drawCanvas();
                    }
                }
            }
        );
    } catch (error) {
        addToCommandLog('新建图层', 'error', `创建图层失败: ${error.message}`);
        return false;
    }
}

/**
 * 删除图层
 */
export async function deleteLayer(layerId, layers, selectedLayer, updateLayerMatrix, setSelectedLayer = null, drawCanvas = null) {
    try {
        const existingLayer = layers.find(l => Number(l.id) === Number(layerId));
        if (!existingLayer) {
            addToCommandLog('图层操作', 'warning', `图层 ${layerId} 不在列表中，无需删除`);
            return;
        }

        await handleApiOperation(
            apiDelete(`/layers/${layerId}`),
            '图层操作',
            `已从当前场景移除图层 ${layerId}，运行层已隐藏`,
            '删除图层失败: 服务器无响应',
            async (result) => {
                // 如果删除的是当前选中的图层，清除选中状态
                if (Number(selectedLayer) === Number(layerId)) {
                    if (setSelectedLayer) {
                        setSelectedLayer(null);
                    }
                }
                // 刷新图层列表
                await updateLayerMatrix();
                // 更新画布显示
                if (drawCanvas) {
                    drawCanvas();
                }
            }
        );
    } catch (error) {
        addToCommandLog('图层操作', 'error', `删除图层失败: ${error.message}`);
    }
}

/**
 * 选择图层
 */
export function selectLayer(
    layerId, layers,
    moveLayerToTop, updateSelectionUI, updateSliceInfo, showLayerProperties, drawCanvas
) {
    moveLayerToTop(layerId, layers);

    updateSelectionUI();
    updateSliceInfo(layerId);
    showLayerProperties(layerId);
    drawCanvas();

    addToCommandLog('选择图层', 'info', `选择图层: ${layerId}`);
}

function isRoamLayer(layerId) {
    return (layerId >= 1 && layerId <= 4) || layerId === 10 || layerId === 11 || layerId === 60;
}

function clearSliceAliases(layer) {
    if (!layer || typeof layer !== 'object') return;
    Object.keys(layer).forEach(key => {
        if (key === 'slices' || (key.startsWith('slice') && /^\d+$/.test(key.substring(5)))) {
            delete layer[key];
        }
    });
}

function upsertLayer(layers, layerId, newData) {
    const index = layers.findIndex(l => Number(l.id) === Number(layerId));
    if (index !== -1) {
        const oldLayer = layers[index];
        clearSliceAliases(oldLayer);
        layers[index] = normalizeLayerFieldAliases({ ...oldLayer, ...newData });
        return layers[index];
    }
    const layer = normalizeLayerFieldAliases(newData);
    layers.push(layer);
    return layer;
}

export async function fetchAndMergeLayerInfo(layerId, layers, options = {}) {
    const existingLayer = layers.find(l => Number(l.id) === Number(layerId));
    const needsRoam = options.fetchRoam !== false &&
        isRoamLayer(Number(layerId)) &&
        (options.forceRoam || !existingLayer || !existingLayer.roam_config);

    const fetchPromises = [
        apiGet(`/runtime/layers/${layerId}`),
        apiGet(`/config/layers/${layerId}`)
    ];
    if (needsRoam) {
        fetchPromises.push(apiGet(`/layers/${layerId}/roam`));
    }

    const results = await Promise.allSettled(fetchPromises);
    const runtimeInfo = results[0].status === 'fulfilled' ? results[0].value : null;
    const configInfo = results[1].status === 'fulfilled' ? results[1].value : null;
    const roamConfig = results[2] && results[2].status === 'fulfilled' ? results[2].value : null;

    if (!runtimeInfo || runtimeInfo.ok === false) {
        const index = layers.findIndex(l => Number(l.id) === Number(layerId));
        if (index !== -1) layers.splice(index, 1);
        return null;
    }

    const configUiLayer = configLayerToUiLayer(configInfo);
    const merged = normalizeLayerFieldAliases({ ...runtimeInfo, ...configUiLayer });
    merged.id = Number(layerId);
    if (runtimeInfo.position !== undefined) merged.position = runtimeInfo.position;
    if (runtimeInfo.size !== undefined) merged.size = runtimeInfo.size;
    if (existingLayer && existingLayer.visible !== undefined) {
        merged.visible = existingLayer.visible;
    }
    if (roamConfig && roamConfig.ok !== false) {
        merged.roam_config = roamConfig;
    }

    if (Number(layerId) === 21 && (!merged.font_file || merged.font_file === '')) {
        const fromList = layers.find(l => Number(l.id) === Number(layerId));
        if (fromList && fromList.font_file) {
            merged.font_file = fromList.font_file;
        } else {
            try {
                const cfg = await apiGet('/config.json');
                if (cfg && typeof cfg === 'object' && cfg.layer21 && cfg.layer21.font_file) {
                    merged.font_file = cfg.layer21.font_file;
                }
            } catch (_) { /* 忽略 */ }
        }
    }

    return upsertLayer(layers, layerId, merged);
}

/**
 * 切换图层可见性
 */
export async function toggleLayerVisibility(
    layerId, layers, drawCanvas, showLayerProperties
) {
    const layer = layers.find(l => Number(l.id) === Number(layerId));
    if (!layer) return;

    const newVisible = layer.visible === false ? true : false;

    await handleApiOperation(
        (async () => {
            const runtimeResult = await apiPut(`/runtime/layers/${layerId}`, { visible: newVisible });
            if (!runtimeResult) return null;
            const configResult = await apiPut(`/config/layers/${layerId}`, { visible: newVisible });
            if (!configResult) return null;
            return { runtime: runtimeResult, config: configResult };
        })(),
        '图层操作',
        `图层 ${layerId} ${newVisible ? '已显示' : '已隐藏'}`,
        '切换可见性失败: 服务器未确认更改',
        (result) => {
            layer.visible = newVisible;
            // 立即更新可见性按钮文字
            const visibilityBtn = document.querySelector('.toggle-visibility');
            if (visibilityBtn) {
                visibilityBtn.textContent = newVisible ? '隐藏' : '显示';
            }
            // 更新图层列表中该图层的状态图标，而不是重新渲染整个列表
            const layerItem = document.querySelector(`.layer-item[data-layer-id="${layerId}"]`);
            if (layerItem) {
                const statusIcon = layerItem.querySelector('.layer-visibility');
                if (statusIcon) {
                    statusIcon.className = `layer-visibility ${newVisible ? 'visible' : 'hidden'}`;
                    statusIcon.title = newVisible ? '已创建 - 可见' : '已创建 - 隐藏';
                }
            }
            drawCanvas();
            showLayerProperties(layerId);
        }
    );
}

/**
 * 刷新图层信息
 */
export async function refreshLayerInfo(layerId, layers) {
    try {
        return !!(await fetchAndMergeLayerInfo(layerId, layers));
    } catch (error) {
        console.error(`[LayerMatrix] 刷新图层 ${layerId} 信息失败:`, error);
        // 发生错误时，从数组中移除该图层
        const index = layers.findIndex(l => Number(l.id) === Number(layerId));
        if (index !== -1) {
            layers.splice(index, 1);
        }
        return false;
    }
}

/**
 * 同步图层到服务器
 */
export async function syncLayerToServer(layer) {
    clearPendingRealTimeSync(layer.id);

    // 验证并格式化 position
    let position = null;
    if (layer.position && typeof layer.position === 'object') {
        const x = typeof layer.position.x === 'number' ? layer.position.x : 0;
        const y = typeof layer.position.y === 'number' ? layer.position.y : 0;
        position = { x, y };
    } else if (layer.position) {
        // 如果是字符串格式，尝试解析
        const parsed = parsePosition(layer.position);
        if (parsed) {
            position = parsed;
        }
    }

    // 验证并格式化 size
    let size = null;
    if (layer.size && typeof layer.size === 'object') {
        const width = typeof layer.size.width === 'number' ? layer.size.width : 1920;
        const height = typeof layer.size.height === 'number' ? layer.size.height : 1080;
        size = { width, height };
    } else if (layer.size) {
        // 如果是字符串格式，尝试解析
        const parsed = parseSize(layer.size);
        if (parsed) {
            size = parsed;
        }
    }

    // 只发送有效的属性
    const properties = {};
    if (position) {
        properties.position = { x: position.x, y: position.y };
    }
    if (size) {
        properties.size = { width: size.width, height: size.height };
    }
    properties._rectSyncSeq = ++rectSyncSeq;

    if (Object.keys(properties).length === 0) return;

    await handleApiOperation(
        (async () => {
            const configResult = await apiPut(`/config/layers/${layer.id}`, buildConfigLayerPatch(properties, layer.id));
            if (!configResult) return null;
            const runtimeResult = await apiPut(`/runtime/layers/${layer.id}`, properties);
            if (!runtimeResult) return null;
            if (properties.position && properties.size) {
                lastSentRect = {
                    x: Math.round(properties.position.x),
                    y: Math.round(properties.position.y),
                    width: Math.round(properties.size.width),
                    height: Math.round(properties.size.height)
                };
                setTimeout(() => {
                    apiPut(`/runtime/layers/${layer.id}`, properties).catch(() => {});
                }, SYNC_INTERVAL + 30);
                setTimeout(() => {
                    apiPut(`/runtime/layers/${layer.id}`, properties).catch(() => {});
                }, SYNC_INTERVAL * 3);
                realTimeSyncPaused = false;
            }
            return { runtime: runtimeResult, config: configResult };
        })(),
        '同步图层',
        `图层 ${layer.id} 已同步`,
        `图层 ${layer.id} 同步失败`
    );
}

/**
 * 将图层移到数组末尾，确保绘制时在最上层
 */
export function moveLayerToTop(layerId, layers) {
    const layerIndex = layers.findIndex(l => Number(l.id) === Number(layerId));
    if (layerIndex !== -1) {
        const layer = layers.splice(layerIndex, 1)[0];
        layers.push(layer);
    }
}

/**
 * 检测图层是否在点击范围内
 */
export function isPointInLayer(layer, mouseX, mouseY, canvasConfig, systemToCanvas, canvas, dpr) {
    if (!layer || layer.visible === false) return false;

    const x = layer.position?.x ?? 0;
    const y = layer.position?.y ?? 0;
    let width = layer.size?.width;
    let height = layer.size?.height;

    if (!width || width <= 0) {
        if (!canvasConfig.loaded) return false;
        width = canvasConfig.width;
    }
    if (!height || height <= 0) {
        if (!canvasConfig.loaded) return false;
        height = canvasConfig.height;
    }

    const canvasPos = systemToCanvas(x, y, canvas, dpr, canvasConfig);
    const canvasSize = systemToCanvas(width, height, canvas, dpr, canvasConfig);
    const centerX = canvasPos.x + canvasSize.x / 2;
    const centerY = canvasPos.y + canvasSize.y / 2;
    const rotationRad = Number(layer.rotation || 0) * Math.PI / 180;

    let hitX = mouseX;
    let hitY = mouseY;
    if (rotationRad !== 0) {
        const dx = mouseX - centerX;
        const dy = mouseY - centerY;
        const cos = Math.cos(rotationRad);
        const sin = Math.sin(rotationRad);
        hitX = centerX + dx * cos + dy * sin;
        hitY = centerY - dx * sin + dy * cos;
    }

    return hitX >= canvasPos.x &&
        hitX <= canvasPos.x + canvasSize.x &&
        hitY >= canvasPos.y &&
        hitY <= canvasPos.y + canvasSize.y;
}

/**
 * 应用图层属性
 */
export async function applyLayerProperties(
    layerId, layers, canvasConfig, getLayerTypeFlags,
    drawCanvas, updateLayerMatrix, refreshUI
) {
    const getValue = (id, defaultVal = 0) => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        const value = parseFloat(el.value);
        return isNaN(value) ? defaultVal : value;
    };

    const getIntValue = (id, defaultVal = 0) => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        const value = parseInt(el.value);
        return isNaN(value) ? defaultVal : value;
    };

    const getStringValue = (id, defaultVal = '') => {
        const el = document.getElementById(id);
        if (!el) return defaultVal;
        return el.value.trim() || defaultVal;
    };

    const normalizeCaptureRotation = (value) => {
        const rotation = Number(value) || 0;
        return [-1, 0, 90, 180, 270].includes(rotation) ? rotation : 0;
    };

    const layer = layers.find(l => Number(l.id) === Number(layerId));
    if (!layer) return;

    const { isVideoLayer, isCaptureLayer, isQRCodeLayer, isImageLayer, isTextLayer, isLyricLayer, isLayer40, isLayer41, isEffectLayer } = getLayerTypeFlags(layer, layerId);

    // 通用参数：position/size/priority/rotation/alpha/volume/gaussian_blur/
    // shape_*/black_to_transparent/invert/crop_*/fit_mode/roam_config 由通用读取器统一获取。
    // 二维码图层稍后会覆盖 position/size/priority；rotation 在 QR 上由读取器自动跳过（无 layer-rotation）
    const properties = readLayerCommonInputs(layerId, layer);

    // 同步漫游配置到本地 layer，便于画布预览
    if (properties.roam_config) {
        layer.roam_config = { ...properties.roam_config };
    }

    // 采集图层参数（含分辨率：预设或自定义宽高）
    if (isCaptureLayer) {
        const nextCaptureType = getStringValue('capture-type', 'AUTO');
        const nextCaptureIndex = Math.max(0, getIntValue('capture-index', 0));
        const nextCaptureRotation = normalizeCaptureRotation(getIntValue('capture-rotation', 0));
        const currentCaptureType = layer.capture_type ?? layer.captureType ?? 'AUTO';
        const currentCaptureIndex = Math.max(0, Number(layer.capture_index ?? layer.captureIndex ?? 0) || 0);
        const currentCaptureRotation = normalizeCaptureRotation(layer.capture_rotation ?? layer.captureRotation ?? 0);
        if (nextCaptureType !== currentCaptureType) {
            properties.capture_type = nextCaptureType;
        }
        if (nextCaptureIndex !== currentCaptureIndex) {
            properties.capture_index = nextCaptureIndex;
        }
        if (nextCaptureRotation !== currentCaptureRotation) {
            properties.capture_rotation = nextCaptureRotation;
        }
    }

    // 图层不管文件操作，不采集 image_file

    // 图像图层参数（animated 显示模式）
    if (isImageLayer && !isQRCodeLayer) {
        const animatedEl = document.getElementById('image-animated');
        if (animatedEl) {
            properties.animated = animatedEl.value === 'animated';
        }
        if (document.getElementById('image-filter-mode')) {
            properties.filter_mode = getIntValue('image-filter-mode', 0);
        }
        if (document.getElementById('image-fade-in-time')) {
            properties.fade_in_time = getValue('image-fade-in-time', 0.5);
        }
        if (document.getElementById('image-fade-out-time')) {
            properties.fade_out_time = getValue('image-fade-out-time', 0.5);
        }
        if (document.getElementById('image-display-duration')) {
            properties.display_duration = getValue('image-display-duration', 3.0);
        }
        if (document.getElementById('image-scale-mode')) {
            properties.scale_mode = getIntValue('image-scale-mode', 0);
        }
    }

    // 漫游 / 形状 / 黑色透明 / 反转 已由 readLayerCommonInputs 统一读取

    // 二维码图层参数
    if (isQRCodeLayer) {
        properties.qr_content = getStringValue('qr-content');
        properties.qr_size = getIntValue('qr-size', 256);
        properties.qr_logo_size = getIntValue('qr-logo-size', 0);
        const qrTextValue = getStringValue('qr-text') || getStringValue('qr-text-display');
        properties.qr_text = qrTextValue;
        properties.qr_text_color = getStringValue('qr-text-color', '1.0 1.0 1.0 1.0');
        const bgColorInput = document.getElementById('qr-bg-color-rgba') || document.getElementById('qr-bg-color');
        properties.qr_bg_color = bgColorInput ? bgColorInput.value.trim() : getStringValue('qr-bg-color', '0 0.2 1 1'); // 默认 #0051FF
        properties.qr_fg_color = getStringValue('qr-fg-color', '0.0 0.0 0.0 1.0');
        properties.qr_error_correction = getIntValue('qr-error-correction', 1);

        properties.position = {
            x: getIntValue('qr-x', getIntValue('layer-x', 0)),
            y: getIntValue('qr-y', getIntValue('layer-y', 0))
        };
        properties.size = {
            width: getIntValue('qr-width', getIntValue('layer-width', 0)),
            height: getIntValue('qr-height', getIntValue('layer-height', 0))
        };
        properties.priority = getIntValue('qr-priority', getIntValue('layer-priority', layerId));
    }

    // 文本图层参数
    if (isTextLayer) {
        if (isLyricLayer) {
            const fontVal = getStringValue('text-font-file');
            properties.font_file = fontVal ? (fontVal.includes('/') ? fontVal.replace(/^.*\//, '') : fontVal) : '';
            const newBindLayerId = getIntValue('lyric-bind-layer', 1);

            // 处理歌词可见性设置
            const lyricVisibleSelect = document.getElementById('lyric-visible');
            if (lyricVisibleSelect) {
                properties.subtitle_visible = lyricVisibleSelect.value === 'true';
            }

            if (layer.bind_layerId !== newBindLayerId) {
                try {
                    const bindResult = await apiAction('lyrics', 'bind_layer', {
                        layerId,
                        bind_layerId: newBindLayerId
                    });

                    if (bindResult !== null) {
                        addToCommandLog('歌词设置', 'success', `已关联到视频图层 ${newBindLayerId}`);
                        layer.bind_layerId = newBindLayerId;
                    } else {
                        addToCommandLog('歌词设置', 'error', `关联视频图层失败`);
                    }
                } catch (error) {
                    addToCommandLog('歌词设置', 'error', `关联视频图层出错: ${error.message}`);
                }
            }
        } else if (isLayer40) {
            properties.text = getStringValue('text-content');
            const fontVal = getStringValue('text-font-file');
            properties.font_file = fontVal ? (fontVal.includes('/') ? fontVal.replace(/^.*\//, '') : fontVal) : '';
            properties.font_size = getValue('text-font-size', 48.0);
            properties.text_color = getStringValue('text-color', '1.0 1.0 1.0 1.0');
            properties.bg_color = getStringValue('text-bg-color', '0.0 0.0 0.0 0.0');
            properties.alignment = getIntValue('text-display-mode', 1);
            properties.scroll_speed = getIntValue('text-scroll-speed', 200);
        } else if (isLayer41) {
            properties.display_align = getIntValue('layer-display-align', 0);
            properties.show_list = getIntValue('layer-show-list', 1) !== 0;
            properties.playlistId = getStringValue('layer-playlist-id');
            properties.font_size = getValue('layer-text-font-size', 36.0);
            properties.show_count = getIntValue('layer-show-count', 3);
            properties.display_duration = getValue('layer-display-duration', 5.0);
            properties.start_hint_time = getValue('layer-start-hint-time', 10.0);
            properties.end_hint_time = getValue('layer-end-hint-time', 10.0);
        } else {
            properties.text = getStringValue('text-content');
            const fontVal = getStringValue('text-font-file');
            properties.font_file = fontVal ? (fontVal.includes('/') ? fontVal.replace(/^.*\//, '') : fontVal) : '';
            properties.font_size = getValue('text-font-size', 48.0);
            properties.text_color = getStringValue('text-color', '1.0 1.0 1.0 1.0');
            properties.bg_color = getStringValue('text-bg-color', '0.0 0.0 0.0 0.0');
            properties.alignment = getIntValue('text-alignment', 1);
        }
    }

    // 特效图层参数
    if (isEffectLayer) {
        properties.effect_id = getStringValue('effect-id');
        properties.effect_params = getStringValue('effect-params');
    }

    const runtimePatch = buildRuntimeLayerPatch(properties);
    const configPatch = buildConfigLayerPatch(properties, layerId);

    await handleApiOperation(
        (async () => {
            const runtimeResult = Object.keys(runtimePatch).length > 0
                ? await apiPut(`/runtime/layers/${layerId}`, runtimePatch)
                : { ok: true };
            if (!runtimeResult) return null;

            const configResult = Object.keys(configPatch).length > 0
                ? await apiPut(`/config/layers/${layerId}`, configPatch)
                : { ok: true };
            if (!configResult) return null;

            return { runtime: runtimeResult, config: configResult };
        })(),
        '应用图层属性',
        `成功更新图层 ${layerId}（点击保存后写入文件）`,
        `更新图层 ${layerId} 失败: 服务器未确认更改`,
        async (result) => {
            // 从后端重新获取完整的layer数据，确保数据同步
            await refreshLayerInfo(layerId, layers);
            // 确保数据已完全更新后再刷新UI
            await new Promise(resolve => setTimeout(resolve, 50));
            drawCanvas();
            // 如果提供了刷新UI的回调，调用它以确保UI显示同步
            if (refreshUI && typeof refreshUI === 'function') {
                await refreshUI(layerId);
            }
        }
    );
}

/**
 * 更新属性输入框
 */
export function updatePropertyInputs(layer) {
    const xInput = document.getElementById('layer-x');
    const yInput = document.getElementById('layer-y');
    const widthInput = document.getElementById('layer-width');
    const heightInput = document.getElementById('layer-height');
    const priorityInput = document.getElementById('layer-priority');
    const rotationInput = document.getElementById('layer-rotation');
    const alphaInput = document.getElementById('layer-alpha');

    if (xInput) xInput.value = layer.position?.x ?? 0;
    if (yInput) yInput.value = layer.position?.y ?? 0;
    if (widthInput) widthInput.value = layer.size?.width ?? 0;
    if (heightInput) heightInput.value = layer.size?.height ?? 0;

    if (priorityInput) priorityInput.value = layer.priority ?? 0;
    if (rotationInput) rotationInput.value = layer.rotation !== undefined ? Math.min(360, Math.max(0, Math.round(Number(layer.rotation)))) : 0;
    if (alphaInput) alphaInput.value = Math.round((layer.alpha !== undefined ? layer.alpha : 1.0) * 255);
}

/**
 * 加载图片；imageInputId 为输入框 id，默认 'image-file'，切片可为 'slice-image-file'
 */
export async function loadImage(layerId, imageInputId = 'image-file') {
    const imageFileInput = document.getElementById(imageInputId);
    if (!imageFileInput) return;

    const imageFile = imageFileInput.value.trim();
    if (!imageFile) {
        addToCommandLog('加载图片', 'error', '请输入图像文件');
        return;
    }

    await handleApiOperation(
        sendLayerCommand(layerId, 'loadImage', { image_file: imageFile }),
        '加载图片',
        `图片加载成功: ${imageFile}`,
        '图片加载失败: 未知错误'
    );
}

/**
 * 获取当前选中的图层
 */
export function getSelectedLayer(selectedLayer) {
    return selectedLayer;
}

// 实时同步图层矩形的防抖/节流变量
let realTimeSyncGeneration = 0;
let realTimeSyncTimer = null;
let lastSyncTime = 0;
let lastSentRect = { x: -1, y: -1, width: -1, height: -1 };
let pendingSyncArgs = null;
let isSyncing = false;
let realTimeSyncPaused = false;
let rectSyncSeq = 0;

// GET 风暴已修复，节流降到 50ms (~20Hz)，让大屏渲染端能流畅跟上拖动。
const SYNC_INTERVAL = 50;

function clearPendingRealTimeSync(layerId) {
    realTimeSyncGeneration++;
    realTimeSyncPaused = true;
    if (realTimeSyncTimer) {
        clearTimeout(realTimeSyncTimer);
        realTimeSyncTimer = null;
    }
    if (pendingSyncArgs && Number(pendingSyncArgs.layerId) === Number(layerId)) {
        pendingSyncArgs = null;
    }
}

/**
 * 实时同步图层位置和尺寸到服务器（用于鼠标拖拽过程中）
 */
export function syncLayerRectRealTime(layerId, x, y, width, height) {
    if (realTimeSyncPaused) return;
    const roundedX = Math.round(x);
    const roundedY = Math.round(y);
    const roundedW = Math.round(width);
    const roundedH = Math.round(height);

    // 检查是否有实质性变化
    if (roundedX === lastSentRect.x &&
        roundedY === lastSentRect.y &&
        roundedW === lastSentRect.width &&
        roundedH === lastSentRect.height) {
        return; 
    }

    // 更新待同步参数
    pendingSyncArgs = { layerId, x, y, width, height, generation: realTimeSyncGeneration };

    // 如果当前正在同步中，只需更新参数后返回，当前同步完成后会自动检查 pendingSyncArgs
    if (isSyncing) {
        return;
    }

    const now = Date.now();
    const elapsed = now - lastSyncTime;

    if (elapsed >= SYNC_INTERVAL) {
        // 满足间隔，立即触发同步
        if (realTimeSyncTimer) {
            clearTimeout(realTimeSyncTimer);
            realTimeSyncTimer = null;
        }
        triggerSync();
    } else {
        // 不满足间隔，安排定时器
        if (realTimeSyncTimer) return;

        realTimeSyncTimer = setTimeout(() => {
            realTimeSyncTimer = null;
            triggerSync();
        }, SYNC_INTERVAL - elapsed);
    }
}

/**
 * 内部触发函数，处理异步流程控制
 */
async function triggerSync() {
    if (isSyncing || !pendingSyncArgs) return;

    const args = pendingSyncArgs;
    pendingSyncArgs = null; // 取出后清空

    isSyncing = true;
    try {
        if (args.generation === realTimeSyncGeneration) {
            await performRealTimeSync(args.layerId, args.x, args.y, args.width, args.height, args.generation);
        }
    } catch (e) {
        console.warn('[RealTimeSync] 同步过程出错:', e);
    } finally {
        isSyncing = false;
        
        // 检查在同步期间是否又有新的坐标产生
        if (pendingSyncArgs) {
            const now = Date.now();
            const elapsed = now - lastSyncTime;
            
            if (elapsed >= SYNC_INTERVAL) {
                // 满足间隔，立即触发下一次
                triggerSync();
            } else {
                // 安排下一次
                if (!realTimeSyncTimer) {
                    realTimeSyncTimer = setTimeout(triggerSync, SYNC_INTERVAL - elapsed);
                }
            }
        }
    }
}


async function performRealTimeSync(layerId, x, y, width, height, generation) {
    if (generation !== realTimeSyncGeneration || realTimeSyncPaused) return;
    const roundedX = Math.round(x);
    const roundedY = Math.round(y);
    const roundedW = Math.round(width);
    const roundedH = Math.round(height);

    lastSyncTime = Date.now();
    lastSentRect = { x: roundedX, y: roundedY, width: roundedW, height: roundedH };

    const properties = {
        position: { x: roundedX, y: roundedY },
        size: { width: roundedW, height: roundedH },
        visible: true,
        _rectSyncSeq: ++rectSyncSeq
    };

    try {
        await apiPut(`/runtime/layers/${layerId}`, properties);
    } catch (e) {
        console.warn(`[RealTimeSync] 同步图层 ${layerId} 失败:`, e.message);
    }
}
