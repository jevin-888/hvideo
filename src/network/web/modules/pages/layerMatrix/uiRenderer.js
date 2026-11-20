// UI渲染模块
import { apiPost, apiPut, apiAction } from '../../core/api.js';
import { clearContainer } from '../../utils/domHelpers.js';
import { escapeHtml, showConfirm } from '../../components/toast.js';
import { addToCommandLog } from '/shared/commandLog.js';
import { getLayerDisplayName, getLayerTemplateDisplayName } from './utils.js';
import { handleApiOperation } from '../../utils/apiHelpers.js';
import { generateTextLayer40Html, generateTextLayer41Html, generateNormalTextHtml, setupTextLayerEvents } from './panels/TextPanel.js';
import { buildCommonPositionSection, createRotationForRoamRow } from './panels/PositionPanel.js';
import { LAYER_MATRIX_DRAG_TYPE } from './matrixLayout.js';
import { generateShapeTypeOptions, updateShapeParamUI, generateShapeParamHTML } from './panels/ShapePanel.js';
import { createNumInput, createNumControl, setupNumberInputButtons } from './panels/CommonControls.js';
import { generateVideoHtml } from './panels/VideoPanel.js';
import { generateCaptureHtml, setupCaptureEdidEvents, saveCaptureResolutionConfig } from './panels/CapturePanel.js';
import { generateImageHtml } from './panels/ImagePanel.js';
import { contentCropBlockHtml } from './panels/SharedBlocks.js';
import { generateEffectHtml } from './panels/EffectPanel.js';
import { getSliceData, isSliceVisible } from './sliceModel.js';
import { normalizeQrAddressValue } from '../../../shared/qrAddressOptions.js?v=1';
// 图层类型展示标签（与顶部卡片、左侧已应用列表共用）
const TYPE_NAME_MAP = {
    'video': '视频', 'image': '图片', 'text': '文本', 'qrcode': '二维码',
    'QRCODE': '二维码', 'effect': '特效'
};
const LAYER_ID_TO_TYPE_LABEL = {
    10: '采集', 11: '采集', 21: '歌词', 30: '弹幕', 31: '投屏',
    40: '欢迎词', 41: '提示消息', 70: 'logo', 71: '二维码'
};
const DEFAULT_ROAM_CONFIG = { enabled: true, mode: 0, speed: 100, loop: true };
const LAYER41_VOD_PLAYLISTS = [
    { id: 'v1:selected', name: '网络VOD已选列表' },
    { id: 'local-vod:selected', name: '本地VOD已选列表' }
];

function buildLayer41PlaylistOptions(playlists, selectedPlaylistId) {
    const selectedId = String(selectedPlaylistId || '');
    const buildOption = ({ id, name }, preserveName = false) => {
        const value = String(id ?? '');
        const selected = value === selectedId ? ' selected' : '';
        return `<option${preserveName ? ' translate="no"' : ''} value="${escapeHtml(value)}"${selected}>${escapeHtml(name || value)}</option>`;
    };
    const vodOptions = LAYER41_VOD_PLAYLISTS.map(item => buildOption(item)).join('');
    const regularOptions = (Array.isArray(playlists) ? playlists : []).map(item => buildOption(item, true)).join('');
    return `<option value=""${selectedId ? '' : ' selected'}>未关联</option>`
        + `<optgroup label="VOD已选列表">${vodOptions}</optgroup>`
        + (regularOptions ? `<optgroup label="普通播放列表">${regularOptions}</optgroup>` : '');
}
let propertyRenderRevision = 0;


function normalizeRoamConfigValue(value, fallback = DEFAULT_ROAM_CONFIG) {
    let parsed = value;
    if (typeof parsed === 'string') {
        try {
            parsed = parsed.trim() ? JSON.parse(parsed) : null;
        } catch (error) {
            parsed = null;
        }
    }
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        parsed = {};
    }
    return { ...fallback, ...parsed };
}

function normalizeLayerId(layerId) {
    const value = Number(layerId);
    return Number.isInteger(value) ? value : null;
}

function normalizeLayerIdentity(layer) {
    const id = normalizeLayerId(layer && layer.id);
    return id === null ? layer : { ...layer, id };
}

function applyParamTabs(formContainer) {
    const content = formContainer.querySelector('.property-content');
    if (!content) return;
    const isCommonId = (id) => {
        if (!id) return false;
        if (/^(layer|slice|qr)-(alpha|priority|x|y|width|height|rotation)$/.test(id)) return true;
        if (id === 'video-volume' || id === 'slice-volume') return true;
        if (/text-content$/.test(id)) return true;
        if (/text-font-size$/.test(id)) return true;
        if (id === 'text-font-file' || id === 'slice-text-font-file') return true;
        if (id === 'qr-content' || id === 'qr-text') return true;
        return false;
    };
    content.querySelectorAll('.setting-item').forEach(item => {
        const ctrl = item.querySelector('input[id], select[id], textarea[id]');
        item.dataset.paramTab = (ctrl && isCommonId(ctrl.id)) ? 'common' : 'advanced';
    });
    content.querySelectorAll('.property-separator').forEach(sep => {
        sep.dataset.paramTab = 'advanced';
    });
    formContainer.querySelectorAll('.property-tab').forEach(btn => {
        btn.addEventListener('click', () => {
            const tab = btn.dataset.paramTab;
            formContainer.querySelectorAll('.property-tab').forEach(b => b.classList.toggle('active', b === btn));
            content.classList.toggle('tab-common', tab === 'common');
            content.classList.toggle('tab-advanced', tab === 'advanced');
        });
    });
}

function getLayerTypeLabel(layer) {
    if (!layer) return '未知';
    const id = layer.id;
    return LAYER_ID_TO_TYPE_LABEL[id] ?? (TYPE_NAME_MAP[layer.type] || layer.type || '未知');
}

function generateMirrorHtml(layer = {}) {
    const tvVerticalCropPx = Math.max(
        0,
        Math.min(4000, Math.round(Number(layer.tv_vertical_crop_px ?? layer.tvVerticalCropPx ?? 0) || 0))
    );
    return `
        <div class="setting-section mirror-setting-section" data-param-tab="advanced">
            <div class="mirror-operations">
                <div class="mirror-action-grid">
                    <div class="mirror-method-title">苹果投屏</div>
                    <button type="button" id="mirror-toggle-btn" class="btn small primary mirror-toggle-btn" aria-pressed="false">启动</button>
                    <button type="button" id="mirror-refresh-status-btn" class="btn small">刷新</button>
                    <button type="button" id="mirror-reset-pin-btn" class="btn small mirror-side-control">重置PIN</button>

                    <div class="mirror-method-title">安卓/Windows</div>
                    <button type="button" id="mirror-android-toggle-btn" class="btn small primary mirror-toggle-btn" aria-pressed="false">启动</button>
                    <button type="button" id="mirror-android-refresh-status-btn" class="btn small">刷新</button>
                    <div class="mirror-autostart-control mirror-ready-hint-control mirror-side-control" title="未投屏时是否在画面上显示提示消息">
                        <span class="mirror-autostart-label">提示消息</span>
                        <label class="preview-switch margin-none">
                            <input type="checkbox" id="mirror-ready-hint-visible" checked>
                            <span class="switch-slider"></span>
                        </label>
                    </div>

                    <div class="mirror-method-title">USB线镜像</div>
                    <button type="button" id="mirror-usb-toggle-btn" class="btn small primary mirror-toggle-btn" aria-pressed="false">启动</button>
                    <button type="button" id="mirror-usb-refresh-status-btn" class="btn small">刷新</button>
                    <select id="mirror-autostart-mode" class="form-control mirror-autostart-select mirror-side-control" title="选择应用启动后自动开启的投屏服务">
                        <option value="none">不自动启动</option>
                        <option value="usb">USB</option>
                        <option value="android">安卓</option>
                        <option value="apple">苹果</option>
                    </select>

                    <div class="mirror-method-title">电视投屏场景</div>
                    <div class="mirror-app-scene-grid">
                        <div class="mirror-autostart-control" title="检测电视投屏启动时切到所选场景，离开后回默认配置">
                            <span class="mirror-autostart-label">自动切换</span>
                            <label class="preview-switch margin-none">
                                <input type="checkbox" id="mirror-usb-app-scene-enabled">
                                <span class="switch-slider"></span>
                            </label>
                        </div>
                        <select id="mirror-usb-app-scene-name" class="form-control mirror-app-scene-select">
                            <option value="">选择场景</option>
                        </select>
                        <button type="button" id="mirror-usb-save-app-scene-btn" class="btn small primary">保存</button>
                    </div>
                    <div class="mirror-number-control mirror-side-control" title="上下各裁剪指定像素后拉伸铺满">
                        <span class="mirror-autostart-label">TV画面</span>
                        ${createNumControl('mirror-tv-vertical-crop-px', tvVerticalCropPx, 0, 4000, 1, { wrapperClass: 'mirror-number-wrapper', inputClass: 'mirror-number-input' })}
                    </div>
                </div>
            </div>
            <div class="mirror-status-panel">
                <div class="mirror-status-row">
                    <span class="mirror-status-label">苹果</span>
                    <span id="mirror-status-text" class="mirror-status-value">正在读取...</span>
                </div>
                <div class="mirror-status-row">
                    <span class="mirror-status-label">安卓</span>
                    <span id="mirror-android-status-text" class="mirror-status-value">正在读取...</span>
                </div>
                <div class="mirror-status-row">
                    <span class="mirror-status-label">USB</span>
                    <span id="mirror-usb-status-text" class="mirror-status-value">正在读取...</span>
                </div>
            </div>
        </div>
    `;
}

function formatMirrorIp(value) {
    const n = Number(value) || 0;
    if (!n) return '';
    return `${n & 255}.${(n >> 8) & 255}.${(n >> 16) & 255}.${(n >> 24) & 255}`;
}

function setMirrorStatusText(el, text, detail = '') {
    if (!el) return;
    el.textContent = text;
    el.title = detail || text;
}

function setMirrorToggleButton(btn, running) {
    if (!btn) return;
    btn.dataset.running = running ? '1' : '0';
    btn.textContent = running ? '停止' : '启动';
    btn.classList.toggle('danger', running);
    btn.classList.toggle('primary', !running);
    btn.setAttribute('aria-pressed', running ? 'true' : 'false');
}

function normalizeMirrorSceneName(sceneName) {
    if (!sceneName || typeof sceneName !== 'string') return '';
    return sceneName.endsWith('.json') ? sceneName.slice(0, -5) : sceneName;
}

async function populateMirrorAppSceneSelect(select, apiGet, selectedScene = '') {
    if (!select || typeof apiGet !== 'function') return;
    const selected = normalizeMirrorSceneName(selectedScene);
    const render = (scenes = []) => {
        const seen = new Set(['默认配置']);
        let html = `<option value="">选择场景</option>`;
        scenes.forEach(scene => {
            const name = normalizeMirrorSceneName(String(scene || ''));
            if (!name || name === '默认配置' || name === 'default') return;
            if (seen.has(name)) return;
            seen.add(name);
            html += `<option translate="no" value="${escapeHtml(name)}" ${name === selected ? 'selected' : ''}>${escapeHtml(name)}</option>`;
        });
        select.innerHTML = html;
        if (selected && !seen.has(selected)) {
            select.insertAdjacentHTML('beforeend',
                `<option translate="no" value="${escapeHtml(selected)}" selected>${escapeHtml(selected)}</option>`);
        }
    };
    render([]);
    try {
        const response = await apiGet('/scenes');
        const scenes = Array.isArray(response) ? response : [];
        render(scenes);
    } catch (error) {
        console.warn('populateMirrorAppSceneSelect failed:', error);
    }
}

/**
 * 渲染图层矩阵
 */
export function renderLayerMatrix(
    layers, layersLoadStatus, selectedLayer, ALL_AVAILABLE_LAYERS,
    selectLayer, addLayerToMatrix
) {
    const layerMatrix = document.getElementById('layer-matrix');
    if (!layerMatrix) return;

    clearContainer(layerMatrix);

    if (layersLoadStatus === 'pending') {
        const emptyMessage = document.createElement('div');
        emptyMessage.className = 'layer-empty-message';
        emptyMessage.textContent = '正在加载图层...';
        layerMatrix.appendChild(emptyMessage);
        return;
    }

    if (layersLoadStatus === 'error') {
        const emptyMessage = document.createElement('div');
        emptyMessage.className = 'layer-empty-message';
        emptyMessage.textContent = '获取图层失败';
        emptyMessage.title = 'API请求失败或数据解析错误，请检查网络连接和服务器状态';
        layerMatrix.appendChild(emptyMessage);
        return;
    }

    const normalizedLayers = layers.map(normalizeLayerIdentity);
    const normalizedAvailableLayers = ALL_AVAILABLE_LAYERS
        .map(normalizeLayerIdentity)
        .filter(layer => layer && normalizeLayerId(layer.id) !== null);
    const selectedLayerId = normalizeLayerId(selectedLayer);
    const createdLayerIds = new Set(normalizedLayers.map(layer => normalizeLayerId(layer && layer.id)).filter(id => id !== null));
    const allLayersToShow = new Map();

    normalizedAvailableLayers.forEach(layerTemplate => {
        allLayersToShow.set(layerTemplate.id, layerTemplate);
    });

    normalizedLayers.forEach(layer => {
        if (!allLayersToShow.has(layer.id)) {
            allLayersToShow.set(layer.id, {
                id: layer.id,
                type: layer.type || 'unknown',
                name: layer.name || `图层${layer.id}`
            });
        }
    });

    const sortedLayers = Array.from(allLayersToShow.values()).sort((a, b) => (a.id || 0) - (b.id || 0));

    sortedLayers.forEach(layerTemplate => {
        const isCreated = createdLayerIds.has(layerTemplate.id);
        const existingLayer = normalizedLayers.find(l => l.id === layerTemplate.id);

        const layerItem = document.createElement('div');
        layerItem.className = 'layer-item';

        if (isCreated) {
            layerItem.classList.add('created');
        } else {
            layerItem.classList.add('available');
        }

        if (selectedLayerId === layerTemplate.id) {
            layerItem.classList.add('selected');
        }
        layerItem.dataset.layerId = layerTemplate.id;
        layerItem.dataset.layerType = layerTemplate.type;
        const displayName = getLayerTemplateDisplayName(layerTemplate);
        layerItem.draggable = true;
        layerItem.title = isCreated ? '拖动到画布分区重新放置' : '拖动到画布分区创建图层';
        layerItem.addEventListener('dragstart', (event) => {
            if (!event.dataTransfer) return;
            const payload = {
                layerId: layerTemplate.id,
                layerType: layerTemplate.type
            };
            event.dataTransfer.effectAllowed = isCreated ? 'move' : 'copy';
            event.dataTransfer.setData(LAYER_MATRIX_DRAG_TYPE, JSON.stringify(payload));
            event.dataTransfer.setData('text/plain', displayName);
            layerItem.classList.add('dragging');
        });
        layerItem.addEventListener('dragend', () => {
            layerItem.classList.remove('dragging');
        });

        const typeName = getLayerTypeLabel(layerTemplate);
        const visibilitySvg = '<svg class="layer-status-svg icon-visible" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg><svg class="layer-status-svg icon-hidden hidden" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/></svg>';
        const addSvg = '<svg class="layer-status-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>';
        let iconBlock = '';

        if (isCreated) {
            const visibilityClass = existingLayer && existingLayer.visible !== false ? 'visible' : 'hidden';
            const statusTitle = existingLayer && existingLayer.visible !== false ? '已创建 - 可见' : '已创建 - 隐藏';
            iconBlock = `<span class="layer-visibility ${visibilityClass}" title="${statusTitle}">${visibilitySvg}</span>`;
        } else {
            iconBlock = `<span class="layer-add-btn" title="点击添加到图层列表">${addSvg}</span>`;
        }

        const escapedLayerName = escapeHtml(displayName);
        const idActiveClass = isCreated ? 'active' : '';
        layerItem.innerHTML = `
            <div class="layer-item-header">
                ${iconBlock}
                <span class="layer-name">${escapedLayerName}</span>
            </div>
            <div class="layer-status">
                <span class="layer-type">${typeName}</span>
                <span class="layer-id ${idActiveClass}">ID: ${layerTemplate.id}</span>
            </div>
        `;

        if (isCreated) {
            layerItem.addEventListener('click', () => {
                if (layerItem.parentElement.dataset.dragging === 'true') return;
                selectLayer(layerTemplate.id);
            });
        } else {
            const addBtn = layerItem.querySelector('.layer-add-btn');
            if (addBtn) {
                addBtn.addEventListener('click', async (e) => {
                    e.stopPropagation();
                    await addLayerToMatrix(layerTemplate.id, layerTemplate.type);
                });
            }
        }

        layerMatrix.appendChild(layerItem);
    });

    initializeLayerMatrixDragScroll(layerMatrix);
}

function initializeLayerMatrixDragScroll(container) {
    if (!container || container.dataset.dragInitialized === 'true') return;

    let isDown = false;
    let startX;
    let scrollLeft;

    container.addEventListener('mousedown', (e) => {
        if (e.target.closest('.layer-item[draggable="true"]') || e.target.classList.contains('layer-add-btn')) return;
        isDown = true;
        container.dataset.dragging = 'false';
        startX = e.pageX - container.offsetLeft;
        scrollLeft = container.scrollLeft;
        container.style.cursor = 'grabbing';
    });

    container.addEventListener('mouseleave', () => {
        isDown = false;
        container.style.cursor = '';
    });

    container.addEventListener('mouseup', () => {
        isDown = false;
        container.style.cursor = '';
        setTimeout(() => {
            container.dataset.dragging = 'false';
        }, 50);
    });

    container.addEventListener('mousemove', (e) => {
        if (!isDown) return;
        e.preventDefault();
        const x = e.pageX - container.offsetLeft;
        const walk = (x - startX) * 1.5;
        if (Math.abs(walk) > 5) {
            container.dataset.dragging = 'true';
        }
        container.scrollLeft = scrollLeft - walk;
    });

    container.dataset.dragInitialized = 'true';
}

/**
 * 更新左侧图层选择区域
 */
export function updateLayerSelection(
    layers, layersLoadStatus, selectedLayer, ALL_AVAILABLE_LAYERS,
    selectLayer, deleteLayer
) {
    const layerSelection = document.querySelector('.layer-selection');
    if (!layerSelection) return;

    clearContainer(layerSelection);

    if (layersLoadStatus === 'pending') {
        const loadingItem = document.createElement('div');
        loadingItem.className = 'selection-item loading';
        loadingItem.style.justifyContent = 'center';
        loadingItem.textContent = '加载中...';
        layerSelection.appendChild(loadingItem);
        return;
    }

    if (layers && layers.length > 0) {
        // 按层 ID 排序，确保显示顺序一致（不受 moveLayerToTop 影响）
        const sortedLayers = [...layers].sort((a, b) => a.id - b.id);
        sortedLayers.forEach(layer => {
            const selectionItem = document.createElement('div');
            selectionItem.className = 'selection-item';
            selectionItem.dataset.layerId = layer.id;
            selectionItem.dataset.layerType = layer.type;

            if (selectedLayer === layer.id) {
                selectionItem.classList.add('selected');
            }

            const layerName = document.createElement('span');
            layerName.className = 'selection-item-name';
            const displayName = getLayerDisplayName(layer, ALL_AVAILABLE_LAYERS);
            layerName.textContent = displayName;

            const deleteBtn = document.createElement('button');
            deleteBtn.className = 'selection-item-delete';
            deleteBtn.innerHTML = '×';
            deleteBtn.title = '删除图层';
            deleteBtn.setAttribute('aria-label', '删除图层');

            deleteBtn.addEventListener('click', async (e) => {
                e.preventDefault();
                e.stopPropagation();
                await deleteLayer(layer.id, layers, selectedLayer);
            });

            selectionItem.appendChild(layerName);
            selectionItem.appendChild(deleteBtn);

            selectionItem.addEventListener('click', (e) => {
                if (e.target.closest('.selection-item-delete')) {
                    return;
                }
                // 列表项均来自 layers（已创建），直接选中即可。
                selectLayer(layer.id);
            });

            layerSelection.appendChild(selectionItem);
        });
    }
}

/**
 * 统一更新所有选中状态的UI
 */
export function updateSelectionUI(selectedLayer, selectedSlice) {
    document.querySelectorAll('.layer-item').forEach(item => {
        // 使用 loose equality to match string IDs in dataset with number IDs in 状态
        item.classList.toggle('selected',
            selectedLayer !== null && item.dataset.layerId == selectedLayer);
    });

    document.querySelectorAll('.selection-item[data-layer-id]').forEach(item => {
        const isLayerSelected = selectedLayer !== null &&
            item.dataset.layerId == selectedLayer &&
            selectedSlice === null;
        item.classList.toggle('selected', isLayerSelected);
    });

    document.querySelectorAll('.selection-item[data-slice-key]').forEach(item => {
        const isSliceSelected = selectedSlice !== null &&
            item.dataset.sliceKey === selectedSlice.sliceKey;
        item.classList.toggle('selected', isSliceSelected);
    });
}

/**
 * 渲染二维码控制面板HTML
 */
export function renderQRCodeDashboard(layer, idPrefix, coords, parseColor, rgbaStringToCssColor) {
    const qrBgColor = parseColor(layer.qr_bg_color, "0 0.2 1 1"); // 默认 #0033FF
    const qrBgColorCss = rgbaStringToCssColor(qrBgColor);
    const qrSize = layer.qr_size || 256;
    const defaultQrContent = (() => {
        if (typeof window === 'undefined' || !window.location) return 'http://127.0.0.1:8081/';
        const host = window.location.hostname || '127.0.0.1';
        return 'http://' + host + ':8081/';
    })();
    const configuredQrContent = normalizeQrAddressValue(layer.qr_content);

    return `
        <div class="setting-section">
            <div class="qr-params-container" style="grid-column: 1 / -1;">
                <div class="qr-params-layout">
                    <div class="qr-params-row1">
                        <div class="qr-params-left">
                            <table class="qr-params-table">
                                <tbody>
                                    <tr>
                                        <td class="qr-params-label">底部文字</td>
                                        <td class="qr-params-value">
                                            <input type="text" id="qr-text" class="form-control" value="${layer.qr_text || '微信扫码关注'}" placeholder="">
                                        </td>
                                        <td class="qr-params-label">融合调试</td>
                                        <td class="qr-params-value">
                                            <label class="preview-switch" title="启动时显示融合调试二维码，同时受启动 IP 提示开关控制">
                                                <input type="checkbox" id="fusion-debug-enabled" checked>
                                                <span class="switch-slider"></span>
                                            </label>
                                        </td>
                                    </tr>
                                    <tr>
                                        <td class="qr-params-label">边框颜色</td>
                                        <td class="qr-params-value" colspan="3">
                                            <div class="color-input-container" style="display: flex; gap: 8px; align-items: center;">
                                                 <div id="qr-bg-color-swatch" class="color-swatch-btn" style="background-color: ${qrBgColorCss}; width: 32px; height: 32px; border-radius: 4px; border: 1px solid #444; cursor: pointer;"></div>
                                                 <input type="color" id="qr-bg-color-picker" value="${qrBgColorCss.startsWith('#') ? qrBgColorCss : '#0033FF'}" style="opacity: 0; position: absolute; pointer-events: none; width: 0; height: 0;">
                                                 <input type="text" id="qr-bg-color-hex" class="form-control" value="${qrBgColorCss}" style="width: 80px; text-transform: uppercase;" placeholder="#RRGGBB">
                                                 <input type="hidden" id="qr-bg-color" value="${qrBgColor}">
                                                 <input type="text" id="qr-bg-color-rgba" class="form-control" value="${qrBgColor}" style="display: none;">
                                            </div>
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                        <div class="qr-params-center">
                            <table class="qr-params-table">
                                <tbody>
                                    <tr>
                                        <td class="qr-params-label">尺寸</td>
                                        <td class="qr-params-value">
                                                ${createNumControl('qr-size', qrSize, 64, 2048)}
                                        </td>
                                        <td class="qr-params-label"> </td>
                                        <td class="qr-params-value"></td>
                                    </tr>
                                    <tr>
                                        <td class="qr-params-label">地址内容</td>
                                        <td class="qr-params-value qr-params-value-address" colspan="2">
                                            <div class="qr-address-picker" data-qr-address-picker>
                                                <input type="text" id="qr-content" class="form-control" aria-label="二维码地址" value="${escapeHtml(configuredQrContent || defaultQrContent)}" data-initial-value="${escapeHtml(configuredQrContent)}" autocomplete="off">
                                                <button type="button" class="qr-address-toggle" data-qr-address-toggle aria-label="选择二维码地址" aria-expanded="false">▾</button>
                                                <div class="qr-address-menu" data-qr-address-menu role="listbox" hidden></div>
                                            </div>
                                        </td>
                                        <td class="qr-params-value">
                                            <button id="generate-qrcode-btn" class="btn primary gen-btn" style="width: 100%;">生成二维码</button>
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                        <div class="qr-params-right">
                            <div class="qr-preview-container">
                                <div class="qr-preview-card" style="background: ${qrBgColorCss};">
                                    <div id="qr-preview" class="qr-preview">
                                        <div class="qr-placeholder" style="color: #666;">生成中...</div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                    <div class="qr-params-bottom">
                        <div class="bottom-param-item">
                            <label>优先级:</label>
                            ${createNumControl(`${idPrefix}-priority`, coords.priority, 0, 100)}
                        </div>
                        <div class="bottom-param-item">
                            <label>X:</label>
                            ${createNumControl(`${idPrefix}-x`, coords.x, -4000, 4000)}
                        </div>
                        <div class="bottom-param-item">
                            <label>Y:</label>
                            ${createNumControl(`${idPrefix}-y`, coords.y, -4000, 4000)}
                        </div>
                        <div class="bottom-param-item">
                            <label>宽度:</label>
                            ${createNumControl(`${idPrefix}-width`, coords.width, 0, 4000)}
                        </div>
                        <div class="bottom-param-item">
                            <label>高度:</label>
                            ${createNumControl(`${idPrefix}-height`, coords.height, 0, 4000)}
                        </div>
                    </div>
                </div>
            </div>
        </div>`;
}

/**
 * 共享函数：为一组输入框 ID 注册实时预览事件（input + change + keydown）
 * applyFn: 立即同步到服务器的函数；previewFn: 立即更新本地预览的函数（可选）
 */
function registerLivePreviewHandlers(ids, applyFn, previewFn) {
    ids.forEach(id => {
        const input = document.getElementById(id);
        if (!input) return;
        let timer = null;
        // 移除可能存在的旧事件监听器（通过克隆节点并重新绑定）
        const newInput = input.cloneNode(true);
        input.parentNode.replaceChild(newInput, input);
        newInput.addEventListener('input', () => {
            if (previewFn) previewFn();
            clearTimeout(timer);
            timer = setTimeout(() => { applyFn(); timer = null; }, 300);
        });
        newInput.addEventListener('change', () => {
            clearTimeout(timer);
            timer = null;
            applyFn();
        });
        newInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                clearTimeout(timer);
                timer = null;
                if (previewFn) previewFn();
                applyFn();
            }
        });
    });
}

/**
 * 显示图层属性
 * 注意：这是一个非常长的函数，包含大量HTML生成逻辑
 * 由于代码量太大，这里只提供函数签名和核心逻辑框架
 * 完整的HTML生成代码需要从原文件中提取
 */
export async function showLayerProperties(
    layerId, layers, ALL_AVAILABLE_LAYERS,
    getLayerTypeFlags, parsePosition, parseSize, getLayerDisplayName,
    parseColor, rgbaStringToCssColor, cssColorToRgbaString,
    renderQRCodeDashboard, setupQRCodeDashboardListeners,
    setupQRCodeColorInputs, setupLayer40ColorInputs,
    applyLayerProperties, loadImage, updateLayerFromInputs,
    drawCanvas, toggleLayerVisibility, deleteLayer, addToCommandLog, apiGet, apiPut,
    drawQRCodeCard, updateQRCodePreview, generateQRCode,
    fetchAndMergeLayerInfo,
    isDragging = false, isResizing = false
) {
    if (isDragging || isResizing) return;

    const formContainer = document.getElementById('layer-properties-form');
    if (!formContainer) return;

    const renderRevision = ++propertyRenderRevision;
    const isCurrentRender = () =>
        renderRevision === propertyRenderRevision && formContainer.isConnected;

    formContainer.style.display = 'block';
    delete formContainer.dataset.layerId;
    delete formContainer.dataset.sliceKey;

    let layer = layers.find(l => l.id === layerId);

    const needsFetch =
        layerId === 40 || layerId === 41 ||
        layerId === 10 || layerId === 11 ||
        layerId === 21 || layerId === 31 || !layer;

    if (needsFetch && typeof fetchAndMergeLayerInfo === 'function') {
        try {
            const fetchedLayer = await fetchAndMergeLayerInfo(layerId, layers);
            if (fetchedLayer) {
                layer = fetchedLayer;
            }
        } catch (error) {
            console.error(`[LayerMatrix] 获取图层 ${layerId} 信息失败:`, error);
            // 获取失败，使用本地缓存
        }
    }

    // 如果之前没有获取到漫游配置（且是需要的图层）
    if (!isCurrentRender()) return;

    if (layer && !layer.roam_config && ((layerId >= 1 && layerId <= 4) || layerId === 10 || layerId === 11 || layerId === 60)) {
        try {
            const roamConfig = await apiGet(`/layers/${layerId}/roam`);
            if (roamConfig && roamConfig.ok !== false) {
                layer.roam_config = roamConfig;
            }
        } catch (error) {
            console.error(`[LayerMatrix] 获取图层 ${layerId} 漫游配置失败:`, error);
            layer.roam_config = DEFAULT_ROAM_CONFIG;
        }
    }

    if (!isCurrentRender()) return;

    if (!layer) {
        formContainer.innerHTML = '<div class="no-layer-selected">未找到图层信息</div>';
        return;
    }

    const { isVideoLayer, isCaptureLayer, isQRCodeLayer, isImageLayer, isTextLayer, isLyricLayer, isLayer40, isLayer41, isEffectLayer } = getLayerTypeFlags(layer, layerId);

    const position = parsePosition(layer.position);
    const size = parseSize(layer.size);

    const layerName = getLayerDisplayName(layer, ALL_AVAILABLE_LAYERS);
    const escapedLayerName = escapeHtml(layerName);

    // 构建属性表单
    let specialFields = '';

    // 采集图层参数 (Layer10, Layer11)
    if (isCaptureLayer) {
        specialFields += generateCaptureHtml(layer);
    }

    // 视频图层参数 (Layer1-Layer4)
    if (isVideoLayer && !isCaptureLayer && !isQRCodeLayer) {
        specialFields += generateVideoHtml(layer);
    }

    if (layerId === 31 || String(layer.type || '').toUpperCase() === 'MIRROR') {
        specialFields += generateMirrorHtml(layer);
    }

    // 图像图层参数 (Layer33, Layer60, Layer70)
    if (isImageLayer && !isQRCodeLayer && layerId !== 71) {
        specialFields += generateImageHtml(layer, layerId);
    }

    // 二维码图层参数
    if (isQRCodeLayer) {
        specialFields += renderQRCodeDashboard(layer, 'layer', {
            x: position.x || 0,
            y: position.y || 0,
            width: size.width || 0,
            height: size.height || 0,
            priority: layer.priority || layerId
        });
    }

    // 文本图层参数 (Layer21, Layer30, Layer40, Layer41)
    if (isTextLayer) {
        if (isLayer40) {
            // 图层40：只显示必要的参数（文字、文字颜色、文字大小、显示方式、滚动速度、背景颜色）
            const textColor = parseColor(layer.text_color, "1.0 1.0 1.0 1.0");
            const bgColor = parseColor(layer.bg_color, "0.0 0.0 0.0 0.0");
            const bgColorParts = bgColor.split(' ');
            const bgAlpha = bgColorParts.length >= 4 ? parseFloat(bgColorParts[3]) : 0;
            const isBgTransparent = bgAlpha < 0.01;
            const displayMode = layer.alignment !== undefined ? layer.alignment : 1;
            const scrollSpeed = layer.scroll_speed !== undefined ? layer.scroll_speed : 200;
            const textColorCss = rgbaStringToCssColor(textColor);
            const bgColorCss = rgbaStringToCssColor(bgColor);

            specialFields += generateTextLayer40Html('', {
                text: layer.text || '欢迎使用本系统',
                fontFile: layer.font_file || '',
                textColorCss,
                textColor,
                bgColorCss,
                bgColor,
                isBgTransparent,
                fontSize: layer.font_size !== undefined ? layer.font_size : 48.0,
                displayMode,
                scrollSpeed,
                alphaVal: Math.round((layer.alpha !== undefined ? layer.alpha : 1.0) * 255)
            });
        } else if (isLayer41) {
            // 图层41：播放列表提示图层
            // 参数：显示位置、关联播放列表、字体大小、显示数量、显示时长、起始提示时间、结束提示时间
            const fontSize = layer.font_size !== undefined ? layer.font_size : 36.0;
            const showCount = layer.show_count !== undefined ? layer.show_count : 3;
            const displayDuration = layer.display_duration !== undefined ? layer.display_duration : 5.0;
            const startHintTime = layer.start_hint_time !== undefined ? layer.start_hint_time : 10.0;
            const endHintTime = layer.end_hint_time !== undefined ? layer.end_hint_time : 10.0;
            const boundPlaylistId = layer.playlistId || '';
            const displayAlign = layer.display_align !== undefined ? layer.display_align : 0; // 0=靠左, 1=居中, 2=靠右（图层41默认靠左）
            const showList = layer.show_list !== undefined ? layer.show_list : true;

            // 异步获取播放列表以填充下拉框
            let playlists = [];
            try {
                const response = await apiGet('/playlists');
                if (Array.isArray(response)) playlists = response;
            } catch (e) {
                console.error('Failed to fetch playlists for Layer 41', e);
            }
            const playlistOptions = buildLayer41PlaylistOptions(playlists, boundPlaylistId);
            if (!isCurrentRender()) return;

            specialFields += generateTextLayer41Html('layer', {
                alphaVal: Math.round((layer.alpha !== undefined ? layer.alpha : 1.0) * 255),
                displayAlign,
                playlistOptions,
                fontSize,
                showCount,
                displayDuration,
                startHintTime,
                endHintTime,
                showList
            });
        } else if (!isLyricLayer) {
            // 其他文本图层（图层30）：显示完整参数
            const textColor = parseColor(layer.text_color, "1.0 1.0 1.0 1.0");
            const bgColor = parseColor(layer.bg_color, "0.0 0.0 0.0 0.0");
            const isBgTransparent = (bgColor === "0.0 0.0 0.0 0.0");
            const textColorCss = rgbaStringToCssColor(textColor);
            const bgColorCss = rgbaStringToCssColor(bgColor);

            specialFields += generateNormalTextHtml('', {
                text: layer.text || '',
                fontFile: layer.font_file || '',
                fontSize: layer.font_size !== undefined ? layer.font_size : 48.0,
                textColorCss,
                textColor,
                bgColorCss,
                bgColor,
                isBgTransparent,
                alignment: layer.alignment !== undefined ? layer.alignment : 1
            });
        }
    }

    // 特效图层参数
    if (isEffectLayer) {
        specialFields += generateEffectHtml(layer);
    }
    specialFields += contentCropBlockHtml(layer);
    // Capture/Video/Image 的专用面板自行决定需要显示的高级参数，
    // Layer 60 形状仍由漫游区处理；不再追加一套重复控件。

    // 图层21：歌词参数（特殊处理，使用独立的歌词控制接口，与文本参数分开显示）
    if (isLyricLayer) {
        // 获取歌词状态（异步加载）
        let lyricStatus = {
            visible: true,
            loaded: false,
            margin: { left: 0, right: 0, top: 0, bottom: 0 },
            style: {
                primary_color: '0x00FFFFFF',    // 未唱颜色（白色）
                secondary_color: '0x00FF2100',  // 已唱颜色（蓝色）
                outline_color: '0x00000000',    // 描边颜色（黑色）
                outline: 2.0,
                shadow: 0.0,
                alignment: 5
            }
        };

        // 只在图层已创建时才调用歌词状态API（通过检查 layer.visible 等属性判断）
        const isLayerCreated = layer && (layer.visible !== undefined || layer.position !== undefined);
        if (isLayerCreated) {
            // 异步获取歌词状态
            getLyricStatus(apiPost, addToCommandLog).then(status => {
                if (status) {
                    lyricStatus = status;
                    // 更新参数面板中的值
                    updateLyricParamsUI(lyricStatus);
                }
            }).catch(() => {
                // 获取歌词状态失败，静默处理
            });
        }

        // 获取歌词自动加载状态（从系统配置）
        let lyricAutoLoadEnabled = true; // 默认启用
        apiGet('/config.json').then(configData => {
            if (configData && configData.lyric_enabled !== undefined) {
                lyricAutoLoadEnabled = configData.lyric_enabled;
                const autoLoadSwitch = document.getElementById('lyric-auto-load');
                if (autoLoadSwitch) {
                    autoLoadSwitch.checked = lyricAutoLoadEnabled;
                }
            }
        }).catch(() => { });

        specialFields += `
            <div class="setting-section">
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="lyric-bind-layer">关联视频层:</label>
                    <select id="lyric-bind-layer" class="form-control">
                        <option value="1" ${layer.bind_layerId === 1 ? 'selected' : ''}>图层 1</option>
                        <option value="2" ${layer.bind_layerId === 2 ? 'selected' : ''}>图层 2</option>
                        <option value="3" ${layer.bind_layerId === 3 ? 'selected' : ''}>图层 3</option>
                        <option value="4" ${layer.bind_layerId === 4 ? 'selected' : ''}>图层 4</option>
                    </select>
                </div>
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="lyric-visible">可见性:</label>
                    <select id="lyric-visible" class="form-control">
                        <option value="true" ${lyricStatus.visible ? 'selected' : ''}>显示</option>
                        <option value="false" ${!lyricStatus.visible ? 'selected' : ''}>隐藏</option>
                    </select>
                </div>
                <div class="setting-item" style="grid-column: span 1;">
                    <label>歌词自动加载:</label>
                    <label class="preview-switch">
                        <input type="checkbox" id="lyric-auto-load" checked>
                        <span class="switch-slider"></span>
                    </label>
                </div>
            </div>
        `;
    }

    if (!isCurrentRender()) return;

    formContainer.innerHTML = `
        <div class="property-group">
            <div class="property-header">
                <h4>${escapedLayerName}</h4>
                <div class="property-tabs" role="tablist">
                    <button type="button" class="property-tab active" data-param-tab="common">常用</button>
                    <button type="button" class="property-tab" data-param-tab="advanced">高级</button>
                </div>
                <div class="property-actions">
                    <button class="btn small toggle-visibility">${layer.visible !== false ? '隐藏' : '显示'}</button>
                    <button class="btn small danger delete-layer">删除</button>
                </div>
            </div>

            <div class="property-content layer-properties-content tab-common ${((isVideoLayer && !isCaptureLayer && !isQRCodeLayer) || isCaptureLayer) ? 'property-content--compact-advanced' : ''}">
                ${specialFields}
                ${!isQRCodeLayer ? `
                <div class="property-separator" style="grid-column: 1 / -1; height: 1px; background: rgba(255,255,255,0.1); margin: 5px 0;"></div>
                <div class="setting-section ${((isVideoLayer && !isCaptureLayer && !isQRCodeLayer) || isCaptureLayer) ? 'compact-advanced-params compact-advanced-params--common' : ''}">
                    ${buildCommonPositionSection('layer', {
        priority: layer.priority || layerId,
        x: position.x || 0,
        y: position.y || 0,
        width: size.width || 0,
        height: size.height || 0,
        rotation: layer.rotation !== undefined ? layer.rotation : 0.0,
        // 说明：确保 alpha 按 0-1.0 范围传递
        alpha: layerId === 70 ? (layer.alpha_255 !== undefined ? layer.alpha_255 / 255 : 1.0) : (layer.alpha !== undefined ? layer.alpha : 1.0)
    }, layerId, { alphaInFirstRow: (isVideoLayer && !isCaptureLayer) || isLayer40 || isLayer41, rotationInRoamRow: (layerId >= 1 && layerId <= 4) || layerId === 10 || layerId === 11 || layerId === 60 })}
                </div>

                <!-- 漫游配置区域（仅视频图层1-4、采集图层10/11、图片图层60显示） -->
                ${((layerId >= 1 && layerId <= 4) || layerId === 10 || layerId === 11 || layerId === 60) ? `
                <div class="property-separator" style="grid-column: 1 / -1; height: 1px; background: rgba(255,255,255,0.1); margin: 10px 0;"></div>
                <div class="setting-section" id="layer-roam-config-section">
                    <!-- 旋转角度与漫游模式同一行（非文本、非二维码的图层） -->
                    ${(!isQRCodeLayer && !isTextLayer) ? createRotationForRoamRow('layer', layer.rotation !== undefined ? layer.rotation : 0.0) : ''}
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="layer-roam-mode">漫游模式:</label>
                        <select id="layer-roam-mode" class="form-control layer-param-control">
                            <option value="0" ${(!layer.roam_config || layer.roam_config.mode === 0) ? 'selected' : ''}>关闭</option>
                            <option value="1" ${layer.roam_config?.mode === 1 ? 'selected' : ''}>左右移动</option>
                            <option value="2" ${layer.roam_config?.mode === 2 ? 'selected' : ''}>上下移动</option>
                            <option value="3" ${layer.roam_config?.mode === 3 ? 'selected' : ''}>圆形路径</option>
                        </select>
                    </div>
                    ${layerId === 60 ? `
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="layer-shape-type">图层形状:</label>
                        <select id="layer-shape-type" class="form-control layer-param-control">
                            ${generateShapeTypeOptions(layer.shapeType ?? layer.shape_type ?? 0)}
                        </select>
                    </div>
                    ` : ''}
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="layer-roam-speed">移动速度(像素/秒):</label>
                        ${createNumControl('layer-roam-speed', layer.roam_config?.speed || 100, 1, 1000)}
                    </div>
                    <!-- 左右移动参数 -->
                    <div id="roam-horizontal-params" class="setting-item" style="grid-column: span 1; display: ${layer.roam_config?.mode === 1 ? 'block' : 'none'};">
                        <label for="layer-roam-range-x">移动范围(像素):</label>
                        ${createNumControl('layer-roam-range-x', layer.roam_config?.rangeX || 500, 0, 4000, 10)}
                    </div>
                    <!-- 上下移动参数 -->
                    <div id="roam-vertical-params" class="setting-item" style="grid-column: span 1; display: ${layer.roam_config?.mode === 2 ? 'block' : 'none'};">
                        <label for="layer-roam-range-y">移动范围(像素):</label>
                        ${createNumControl('layer-roam-range-y', layer.roam_config?.rangeY || 500, 0, 4000, 10)}
                    </div>
                    <!-- 圆形路径参数 -->
                    <div id="roam-circular-params" class="setting-item" style="grid-column: span 1; display: ${layer.roam_config?.mode === 3 ? 'block' : 'none'};">
                        <label for="layer-roam-radius">圆形半径(像素):</label>
                        ${createNumControl('layer-roam-radius', layer.roam_config?.radius || 200, 0, 2000, 10)}
                    </div>
                    <div class="setting-item" style="grid-column: span 1;">
                        <label>循环播放:</label>
                        <label class="preview-switch">
                            <input type="checkbox" id="roam-loop" ${layer.roam_config?.loop !== false ? 'checked' : ''}>
                            <span class="switch-slider"></span>
                        </label>
                    </div>
                     ${layerId === 60 ? `
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="layer-black-to-transparent">黑色变透明:</label>
                        <select id="layer-black-to-transparent" class="form-control">
                            <option value="false" ${!layer.black_to_transparent ? 'selected' : ''}>关闭</option>
                            <option value="true" ${layer.black_to_transparent ? 'selected' : ''}>开启</option>
                        </select>
                    </div>
                    ` : ''}
                </div>
                ` : ''}
                ` : ''}
            </div>
        </div>
    `;
    formContainer.dataset.layerId = String(layerId);

    if (isCaptureLayer) setupCaptureEdidEvents(formContainer, isCurrentRender, async capture => {
        const config = await saveCaptureResolutionConfig(layerId, capture);
        if (!config) return false;
        layer.capture_width = config.captureWidth;
        layer.capture_height = config.captureHeight;
        layer.capture_resolution_locked = config.captureResolutionLocked;
        return true;
    });

    applyParamTabs(formContainer);

    // 数字输入框加减按钮
    setupNumberInputButtons(formContainer, (id, val, action) => {
        if (id === 'layer-alpha' || id === 'layer-alpha-slider') {
            const alphaVal = document.getElementById('layer-alpha-value');
            if (alphaVal) alphaVal.textContent = Math.round(val * 100) + '%';
        }
        if (id === 'video-volume') {
            const hintType = action === 'plus' ? 'up' : (action === 'minus' ? 'down' : 'auto');
            apiPost('/video/volume', {
                layerId: Number(layerId),
                volume: Math.max(0, Math.min(100, Number(val))) / 100,
                hint_type: hintType
            }).catch(error => {
                console.error('图层音量提示同步失败:', error);
            });
        }
    }, { excludeSelector: '[id$="-roam-config-section"]' });

    // 透明度滑块监听已移除，转为普通数字输入框监听

    // 图层属性输入框变化时，实时更新幕布显示并同步到服务器
    registerLivePreviewHandlers(
        ['layer-x', 'layer-y', 'layer-width', 'layer-height', 'layer-priority', 'layer-rotation', 'layer-alpha', 'layer-crop-left', 'layer-crop-top', 'layer-crop-right', 'layer-crop-bottom', 'qr-x', 'qr-y', 'qr-width', 'qr-height', 'qr-priority'],
        () => applyLayerProperties(layerId),
        () => updateLayerFromInputs(layerId)
    );

    // 切换可见性
    formContainer.querySelector('.toggle-visibility')?.addEventListener('click', () => toggleLayerVisibility(layerId));

    // 删除图层
    formContainer.querySelectorAll('.delete-layer').forEach(btn => {
        btn.addEventListener('click', () => deleteLayer(layerId));
    });

    // 图层21：歌词参数事件监听
    if (isLyricLayer) {
        setupLyricParamsListeners(layerId, apiPost, addToCommandLog);
    }

    if (layerId === 31 || String(layer.type || '').toUpperCase() === 'MIRROR') {
        const readyHintSwitch = document.getElementById('mirror-ready-hint-visible');
        if (readyHintSwitch) {
            readyHintSwitch.checked = layer.mirror_ready_hint_visible !== false
                && layer.mirrorReadyHintVisible !== false;
            readyHintSwitch.addEventListener('change', () => {
                updateLayerFromInputs(layerId);
                applyLayerProperties(layerId);
            });
        }

        const tvCropInput = document.getElementById('mirror-tv-vertical-crop-px');
        if (tvCropInput) {
            const cropValue = Math.max(
                0,
                Math.min(4000, Math.round(Number(layer.tv_vertical_crop_px ?? layer.tvVerticalCropPx ?? tvCropInput.value) || 0))
            );
            tvCropInput.value = String(cropValue);
            let tvCropTimer = null;
            tvCropInput.addEventListener('change', () => {
                clearTimeout(tvCropTimer);
                updateLayerFromInputs(layerId);
                applyLayerProperties(layerId);
            });
            tvCropInput.addEventListener('input', () => {
                updateLayerFromInputs(layerId);
                clearTimeout(tvCropTimer);
                tvCropTimer = setTimeout(() => {
                    applyLayerProperties(layerId);
                    tvCropTimer = null;
                }, 300);
            });
        }

        const statusEl = document.getElementById('mirror-status-text');
        const toggleBtn = document.getElementById('mirror-toggle-btn');
        const updateMirrorStatus = async (action = 'status', writeLog = true) => {
            const result = await apiPost(`/mirror/actions/${action}`, { layerId });
            if (!result) {
                setMirrorStatusText(statusEl, '状态读取失败');
                return;
            }
            const ip = formatMirrorIp(result.connectedIp);
            const summary = `${result.started ? '接收中' : '未启动'} · ${result.connected ? '已连接' : '等待连接'} · PIN ${result.pinCode || '----'}`;
            const detail = `${summary}${ip ? ` · IP ${ip}` : ''}`;
            setMirrorStatusText(statusEl, summary, detail);
            setMirrorToggleButton(toggleBtn, !!result.started);
            if (writeLog) {
                addToCommandLog('投屏', result.started ? 'success' : 'info', `投屏服务状态：${result.started ? '已启动' : '未启动'}，${result.connected ? '已连接' : '未连接'}`);
            }
        };
        toggleBtn?.addEventListener('click', () => updateMirrorStatus(toggleBtn.dataset.running === '1' ? 'stop' : 'start'));
        document.getElementById('mirror-reset-pin-btn')?.addEventListener('click', () => updateMirrorStatus('reset_pin'));
        document.getElementById('mirror-refresh-status-btn')?.addEventListener('click', () => updateMirrorStatus('status'));
        updateMirrorStatus('status');

        const androidStatusEl = document.getElementById('mirror-android-status-text');
        const androidToggleBtn = document.getElementById('mirror-android-toggle-btn');
        const updateAndroidMirrorStatus = async (action = 'status', writeLog = true) => {
            const result = await apiPost(`/mirror/actions/android_${action}`, { layerId });
            if (!result) {
                setMirrorStatusText(androidStatusEl, '安卓状态读取失败');
                return;
            }
            const summary = `${result.serviceStarted ? '服务启动' : '服务未启'} · ${result.p2pEnabled ? 'P2P可用' : 'P2P未启'} · ${result.connected ? '已连接' : '等待连接'}`;
            const detail = `${summary} · ${result.wfdEnabled ? 'WFD已启' : 'WFD未启'}${result.message ? ` · ${result.message}` : ''}`;
            setMirrorStatusText(androidStatusEl, summary, detail);
            setMirrorToggleButton(androidToggleBtn, !!result.serviceStarted);
            if (writeLog) {
                addToCommandLog('安卓投屏', result.serviceStarted ? 'success' : 'info', result.message || 'Miracast状态已更新');
            }
        };
        androidToggleBtn?.addEventListener('click', () => updateAndroidMirrorStatus(androidToggleBtn.dataset.running === '1' ? 'stop' : 'start'));
        document.getElementById('mirror-android-refresh-status-btn')?.addEventListener('click', () => updateAndroidMirrorStatus('status'));
        updateAndroidMirrorStatus('status');

        const usbStatusEl = document.getElementById('mirror-usb-status-text');
        const usbToggleBtn = document.getElementById('mirror-usb-toggle-btn');
        const mirrorAutostartSelect = document.getElementById('mirror-autostart-mode');
        const usbAppSceneEnabled = document.getElementById('mirror-usb-app-scene-enabled');
        const usbAppSceneSelect = document.getElementById('mirror-usb-app-scene-name');
        const usbSaveAppSceneBtn = document.getElementById('mirror-usb-save-app-scene-btn');
        let usbAppSceneEditRevision = 0;
        let usbAppSceneFormDirty = false;
        let usbAppSceneSaving = false;
        const markUsbAppSceneFormDirty = () => {
            usbAppSceneEditRevision += 1;
            usbAppSceneFormDirty = true;
        };
        const syncUsbAppSceneControls = (result, requestRevision, force = false) => {
            if (!result || (!force && (usbAppSceneFormDirty
                || usbAppSceneSaving
                || requestRevision !== usbAppSceneEditRevision))) {
                return;
            }
            if (usbAppSceneEnabled) {
                usbAppSceneEnabled.checked = !!result.appSceneDetectEnabled;
            }
            if (usbAppSceneSelect && result.appSceneName) {
                usbAppSceneSelect.value = result.appSceneName;
            }
        };
        if (mirrorAutostartSelect) {
            mirrorAutostartSelect.value = 'none';
        }
        const updateMirrorAutostartStatus = async (mode = null) => {
            const isUpdate = mode !== null;
            const payload = isUpdate ? { mode } : {};
            let result = null;
            try {
                const endpoint = isUpdate
                    ? '/mirror/actions/autostart_set'
                    : '/mirror/actions/autostart_status';
                result = await apiPost(endpoint, payload);
            } catch (error) {
                console.warn('updateMirrorAutostartStatus failed:', error);
            }
            if (!result) {
                addToCommandLog('投屏自启动', 'error', '配置读取或保存失败');
                return null;
            }
            if (mirrorAutostartSelect
                && (!isUpdate || document.activeElement !== mirrorAutostartSelect)) {
                mirrorAutostartSelect.value = result.mode || 'none';
            }
            if (isUpdate) {
                const modeLabels = { none: '不自动启动', usb: 'USB', android: '安卓', apple: '苹果' };
                addToCommandLog('投屏自启动', 'success',
                    '启动时开启：' + (modeLabels[result.mode] || result.mode));
            }
            return result;
        };
        const updateUsbMirrorStatus = async (action = 'status', writeLog = true) => {
            const requestRevision = usbAppSceneEditRevision;
            let result = null;
            try {
                result = await apiPost(`/mirror/actions/usb_${action}`, { layerId });
            } catch (error) {
                console.warn('updateUsbMirrorStatus failed:', error);
            }
            if (!result) {
                setMirrorStatusText(usbStatusEl, 'USB状态读取失败');
                return;
            }
            const size = result.width && result.height ? ` · ${result.width}x${result.height}` : '';
            const auth = result.awaitingAuthorization ? ' · 等待授权' : '';
            const visibleAppPackage = result.foregroundLaunchPackage || result.suggestedAppPackage || result.foregroundPackage || '';
            const currentApp = visibleAppPackage ? ` · ${visibleAppPackage}` : '';
            const appScene = result.appSceneDetectEnabled
                ? ` · TV场景${result.appSceneMatched ? '命中' : '待命'}`
                : '';
            const summary = `${result.started ? '镜像中' : '未启动'} · ${result.connected ? 'ADB已连' : 'ADB未连'}${appScene}${size}${auth}${currentApp}`;
            const detail = `${summary}${result.deviceName ? ` · ${result.deviceName}` : ''}${result.message ? ` · ${result.message}` : ''}${result.foregroundRawFocus ? ` · ${result.foregroundRawFocus}` : ''}${result.runningPackages ? ` · running=${result.runningPackages}` : ''}`;
            setMirrorStatusText(usbStatusEl, summary, detail);
            setMirrorToggleButton(usbToggleBtn, !!result.started);
            syncUsbAppSceneControls(result, requestRevision);
            if (writeLog) {
                addToCommandLog('USB投屏', result.started ? 'success' : 'info', result.message || 'USB镜像状态已更新');
            }
            return result;
        };
        const saveUsbAppSceneConfig = async () => {
            const payload = {
                layerId,
                enabled: !!usbAppSceneEnabled?.checked,
                sceneName: usbAppSceneSelect?.value || ''
            };
            if (payload.enabled && !payload.sceneName) {
                addToCommandLog('USB TV场景', 'error', '请先选择电视投屏对应场景');
                return;
            }

            usbAppSceneSaving = true;
            if (usbAppSceneEnabled) usbAppSceneEnabled.disabled = true;
            if (usbAppSceneSelect) usbAppSceneSelect.disabled = true;
            if (usbSaveAppSceneBtn) usbSaveAppSceneBtn.disabled = true;
            try {
                const result = await apiPost('/mirror/actions/usb_app_scene_config', payload);
                if (!result) {
                    addToCommandLog('USB TV场景', 'error', '保存失败');
                    return;
                }
                usbAppSceneEditRevision += 1;
                usbAppSceneFormDirty = false;
                syncUsbAppSceneControls(result, usbAppSceneEditRevision, true);
                addToCommandLog('USB TV场景', 'success',
                    result.appSceneDetectEnabled
                        ? `已保存：电视投屏检测 -> ${result.appSceneName || payload.sceneName}`
                        : '已关闭自动场景切换');
            } finally {
                usbAppSceneSaving = false;
                if (usbAppSceneEnabled) usbAppSceneEnabled.disabled = false;
                if (usbAppSceneSelect) usbAppSceneSelect.disabled = false;
                if (usbSaveAppSceneBtn) usbSaveAppSceneBtn.disabled = false;
            }
        };
        usbToggleBtn?.addEventListener('click', () => updateUsbMirrorStatus(usbToggleBtn.dataset.running === '1' ? 'stop' : 'start'));
        usbAppSceneEnabled?.addEventListener('change', markUsbAppSceneFormDirty);
        usbAppSceneSelect?.addEventListener('change', markUsbAppSceneFormDirty);
        mirrorAutostartSelect?.addEventListener('change', async () => {
            const selectedMode = mirrorAutostartSelect.value;
            mirrorAutostartSelect.disabled = true;
            const result = await updateMirrorAutostartStatus(selectedMode);
            mirrorAutostartSelect.disabled = false;
            if (!result) {
                await updateMirrorAutostartStatus();
            }
        });
        usbSaveAppSceneBtn?.addEventListener('click', saveUsbAppSceneConfig);
        document.getElementById('mirror-usb-refresh-status-btn')?.addEventListener('click', () => updateUsbMirrorStatus('status'));
        updateMirrorAutostartStatus();
        updateUsbMirrorStatus('status').then(result => {
            if (result?.appSceneName) {
                populateMirrorAppSceneSelect(usbAppSceneSelect, apiGet, result.appSceneName);
            } else {
                populateMirrorAppSceneSelect(usbAppSceneSelect, apiGet);
            }
        });

        let mirrorStatusPollInFlight = false;
        const pollMirrorStatuses = async () => {
            if (!statusEl?.isConnected && !androidStatusEl?.isConnected && !usbStatusEl?.isConnected) {
                return;
            }
            if (!mirrorStatusPollInFlight) {
                mirrorStatusPollInFlight = true;
                try {
                    await Promise.all([
                        updateMirrorStatus('status', false),
                        updateAndroidMirrorStatus('status', false),
                        updateUsbMirrorStatus('status', false)
                    ]);
                } catch (error) {
                    console.warn('pollMirrorStatuses failed:', error);
                } finally {
                    mirrorStatusPollInFlight = false;
                }
            }
            if (statusEl?.isConnected || androidStatusEl?.isConnected || usbStatusEl?.isConnected) {
                setTimeout(pollMirrorStatuses, 1000);
            }
        };
        setTimeout(pollMirrorStatuses, 1000);
    }

    // 视频图层及采集图层参数事件监听
    if (isVideoLayer || isCaptureLayer) {
        ['video-priority', 'video-volume', 'video-gaussian-blur', 'video-fit-mode'].forEach(id => {
            const input = document.getElementById(id);
            if (input) {
                let timer = null;
                input.addEventListener('change', () => {
                    clearTimeout(timer);
                    // 立即更新本地预览
                    updateLayerFromInputs(layerId);
                    // 立即同步到服务器
                    applyLayerProperties(layerId);
                });
                input.addEventListener('input', () => {
                    // 立即更新本地预览
                    updateLayerFromInputs(layerId);
                    // 清除之前的定时器
                    clearTimeout(timer);
                    // 延迟同步到服务器（防抖，300ms后同步）
                    timer = setTimeout(() => {
                        applyLayerProperties(layerId);
                        timer = null;
                    }, 300);
                });
            }
        });

        const layerInvertSelect = document.getElementById('layer-invert');
        if (layerInvertSelect) {
            layerInvertSelect.addEventListener('change', () => {
                updateLayerFromInputs(layerId);
                setTimeout(() => { applyLayerProperties(layerId); }, 100);
            });
        }

        const blackToTransparentSelect = document.getElementById('layer-black-to-transparent');
        if (blackToTransparentSelect) {
            blackToTransparentSelect.addEventListener('change', () => {
                updateLayerFromInputs(layerId);
                setTimeout(() => { applyLayerProperties(layerId); }, 100);
            });
        }
    }

    // 采集图层参数事件监听
    if (isCaptureLayer) {

        ['capture-type', 'capture-index'].forEach(id => {
            const input = document.getElementById(id);
            if (input) {
                input.addEventListener('change', () => {
                    updateLayerFromInputs(layerId);
                    applyLayerProperties(layerId);
                });
            }
        });
    }

    // 图像图层参数事件监听（图层不管文件操作，无加载图像）
    if (isImageLayer && !isQRCodeLayer) {
        ['image-filter-mode', 'image-fade-in-time', 'image-fade-out-time', 'image-display-duration'].forEach(id => {
            const input = document.getElementById(id);
            if (input) {
                let timer = null;
                input.addEventListener('change', () => {
                    clearTimeout(timer);
                    // 立即更新本地预览
                    updateLayerFromInputs(layerId);
                    // 立即同步到服务器
                    applyLayerProperties(layerId);
                });
                input.addEventListener('input', () => {
                    // 立即更新本地预览
                    updateLayerFromInputs(layerId);
                    // 清除之前的定时器
                    clearTimeout(timer);
                    // 延迟同步到服务器（防抖，300ms后同步）
                    timer = setTimeout(() => {
                        applyLayerProperties(layerId);
                        timer = null;
                    }, 300);
                });
            }
        });

        const imageAnimatedSelect = document.getElementById('image-animated');
        if (imageAnimatedSelect) {
            imageAnimatedSelect.addEventListener('change', () => {
                applyLayerProperties(layerId);
            });
        }

        const imageScaleModeSelect = document.getElementById('image-scale-mode');
        if (imageScaleModeSelect) {
            imageScaleModeSelect.addEventListener('change', () => {
                applyLayerProperties(layerId);
            });
        }
    }

    // 图层形状事件监听
    const shapeTypeSelect = document.getElementById('layer-shape-type');
    if (shapeTypeSelect) {
        shapeTypeSelect.addEventListener('change', function () {
            const paramItem = document.getElementById('layer-shape-param-item');
            if (paramItem) {
                updateShapeParamUI(parseInt(this.value) || 0, paramItem, 'layer', 'block');
            }
            // 立即更新本地预览
            updateLayerFromInputs(layerId);
            // 延迟同步到服务器
            setTimeout(() => {
                applyLayerProperties(layerId);
            }, 100);
        });
    }

    const shapeParamInput = document.getElementById('layer-shape-param');
    if (shapeParamInput) {
        let timer = null;
        shapeParamInput.addEventListener('input', () => {
            // 立即更新本地预览
            updateLayerFromInputs(layerId);
            // 清除之前的定时器
            clearTimeout(timer);
            // 延迟同步到服务器
            timer = setTimeout(() => {
                applyLayerProperties(layerId);
                timer = null;
            }, 300);
        });
        shapeParamInput.addEventListener('change', () => {
            // 立即更新本地预览
            updateLayerFromInputs(layerId);
            // 清除定时器，立即同步
            clearTimeout(timer);
            timer = null;
            applyLayerProperties(layerId);
        });
    }

    // 二维码图层参数事件监听
    if (isQRCodeLayer) {
        setupQRCodeDashboardListeners(layerId, 'layer');
    }

    // 特效图层参数事件监听
    if (isEffectLayer) {
        ['effect-id', 'effect-params'].forEach(id => {
            const input = document.getElementById(id);
            if (input) {
                let timer = null;
                input.addEventListener('change', () => {
                    clearTimeout(timer);
                    // 立即更新本地预览
                    updateLayerFromInputs(layerId);
                    // 立即同步到服务器
                    applyLayerProperties(layerId);
                });
                input.addEventListener('input', () => {
                    // 立即更新本地预览
                    updateLayerFromInputs(layerId);
                    // 清除之前的定时器
                    clearTimeout(timer);
                    // 延迟同步到服务器（防抖，500ms后同步）
                    timer = setTimeout(() => {
                        applyLayerProperties(layerId);
                        timer = null;
                    }, 500);
                });
            }
        });
    }

    // 漫游配置事件监听
    // 只有视频图层(1-4)、采集图层(10/11)、图片墙(60)才设置漫游配置监听器
    if ((layerId >= 1 && layerId <= 4) || layerId === 10 || layerId === 11 || layerId === 60) {
        setupRoamConfigListeners(layerId);
    }

    // 文本图层参数事件监听
    if (isTextLayer) {
        if (isLyricLayer) {
            const bindLayerSelect = document.getElementById('lyric-bind-layer');
            if (bindLayerSelect) {
                bindLayerSelect.addEventListener('change', () => {
                    applyLayerProperties(layerId);
                });
            }
        } else if (isLayer40) {
            setupTextLayerEvents(layerId, '', null, () => updateLayerFromInputs(layerId), applyLayerProperties, cssColorToRgbaString);
            loadFontList(layerId, layer.font_file || '', apiGet, applyLayerProperties);

            ['text-content', 'text-font-file', 'text-font-size', 'text-display-mode', 'text-scroll-speed'].forEach(id => {
                const input = document.getElementById(id);
                if (input) {
                    let timer = null;
                    input.addEventListener('change', () => {
                        clearTimeout(timer);
                        updateLayerFromInputs(layerId);
                        applyLayerProperties(layerId);
                    });
                    input.addEventListener('input', () => {
                        updateLayerFromInputs(layerId);
                        clearTimeout(timer);
                        timer = setTimeout(() => {
                            applyLayerProperties(layerId);
                            timer = null;
                        }, 300);
                    });
                }
            });
        } else if (isLayer41) {
            const updateLayer41LocalState = () => {
                if (!layer) return;
                const getValue = (elementId, defaultVal = 0) => {
                    const el = document.getElementById(elementId);
                    return el ? (isNaN(parseFloat(el.value)) ? defaultVal : parseFloat(el.value)) : defaultVal;
                };
                const getIntValue = (elementId, defaultVal = 0) => {
                    const el = document.getElementById(elementId);
                    return el ? (isNaN(parseInt(el.value)) ? defaultVal : parseInt(el.value)) : defaultVal;
                };
                const getStringValue = (elementId, defaultVal = '') => {
                    const el = document.getElementById(elementId);
                    return el ? el.value.trim() || defaultVal : defaultVal;
                };

                layer.display_align = getIntValue('layer-display-align', layer.display_align !== undefined ? layer.display_align : 0);
                layer.playlistId = getStringValue('layer-playlist-id', layer.playlistId || '');
                layer.font_size = getValue('layer-text-font-size', layer.font_size || 36.0);
                layer.show_count = Math.round(getValue('layer-show-count', layer.show_count || 3));
                layer.display_duration = getValue('layer-display-duration', layer.display_duration || 5.0);
                layer.start_hint_time = getValue('layer-start-hint-time', layer.start_hint_time || 10.0);
                layer.end_hint_time = getValue('layer-end-hint-time', layer.end_hint_time || 10.0);
                layer.show_list = getIntValue('layer-show-list', 1) !== 0;
                drawCanvas();
            };

            ['layer-display-align', 'layer-show-list', 'layer-playlist-id', 'layer-text-font-size', 'layer-show-count', 'layer-display-duration', 'layer-start-hint-time', 'layer-end-hint-time'].forEach(id => {
                const input = document.getElementById(id);
                if (input) {
                    let timer = null;
                    input.addEventListener('change', () => {
                        clearTimeout(timer);
                        updateLayer41LocalState();
                        applyLayerProperties(layerId);
                    });
                    input.addEventListener('input', () => {
                        updateLayer41LocalState();
                        clearTimeout(timer);
                        timer = setTimeout(() => {
                            applyLayerProperties(layerId);
                            timer = null;
                        }, 300);
                    });
                }
            });
        } else {
            // 其他文本图层（图层30）：监听逻辑
            setupTextLayerEvents(layerId, '', null, () => updateLayerFromInputs(layerId), applyLayerProperties, cssColorToRgbaString);

            ['text-content', 'text-font-file', 'text-font-size', 'text-alignment'].forEach(id => {
                const input = document.getElementById(id);
                if (input) {
                    let timer = null;
                    input.addEventListener('change', () => {
                        clearTimeout(timer);
                        updateLayerFromInputs(layerId);
                        applyLayerProperties(layerId);
                    });
                    input.addEventListener('input', () => {
                        updateLayerFromInputs(layerId);
                        clearTimeout(timer);
                        timer = setTimeout(() => {
                            applyLayerProperties(layerId);
                            timer = null;
                        }, 300);
                    });
                }
            });
        }
    }

    // 说明：监听器和其他逻辑...
}

// ==================== 歌词相关辅助函数 (模块级别) ====================

// 加载字体列表并填充下拉选择框
async function loadFontList(layerId, currentFontFile, apiGet, applyLayerProperties, selectId = 'text-font-file', applyArgs = []) {
    const fontSelect = document.getElementById(selectId);
    if (!fontSelect) return;

    fontSelect.setAttribute('aria-busy', 'true');
    fontSelect.title = '正在加载字体列表…';

    try {
        const fonts = await apiGet('/materials?type=font');

        let fontList = [];
        if (Array.isArray(fonts) && fonts.length > 0) {
            const seen = new Set();
            fontList = fonts
                .filter(f => {
                    const path = f.path || f.name || '';
                    const ext = path.toLowerCase();
                    return ext.endsWith('.ttf') || ext.endsWith('.otf') || ext.endsWith('.ttc');
                })
                .map(f => {
                    const path = f.path || f.name || '';
                    const fileName = f.name || path.split(/[/\\]/).pop() || '';
                    return { name: fileName, path: path };
                })
                .filter(f => {
                    const key = (f.name || '').toLowerCase();
                    if (!key || seen.has(key)) return false;
                    seen.add(key);
                    return true;
                });
        }

        while (fontSelect.children.length > 0) {
            fontSelect.removeChild(fontSelect.lastChild);
        }

        const defaultOpt = document.createElement('option');
        defaultOpt.value = '';
        defaultOpt.textContent = '默认系统字体';
        defaultOpt.title = '默认系统字体';
        defaultOpt.selected = !currentFontFile;
        fontSelect.appendChild(defaultOpt);

        const currentBasename = currentFontFile ? currentFontFile.split(/[/\\]/).pop() : '';
        const isCurrentFont = (f) =>
            currentFontFile && (currentFontFile === f.name || currentFontFile.endsWith(f.name) ||
                (currentBasename && currentBasename.toLowerCase() === (f.name || '').toLowerCase()));

        const addedValues = new Set();

        if (currentFontFile) {
            const currentDisplay = currentFontFile.length > 14
                ? currentFontFile.substring(0, 11) + '...'
                : currentFontFile;
            const currentOpt = document.createElement('option');
            currentOpt.value = currentFontFile;
            currentOpt.textContent = currentDisplay;
            currentOpt.title = currentFontFile;
            currentOpt.selected = true;
            fontSelect.appendChild(currentOpt);
            addedValues.add(currentFontFile.toLowerCase());
            addedValues.add(currentBasename.toLowerCase());
        }

        fontList.forEach(font => {
            if (isCurrentFont(font)) return;
            const val = font.name || '';
            if (addedValues.has(val.toLowerCase())) return;
            addedValues.add(val.toLowerCase());
            const option = document.createElement('option');
            option.value = font.name;
            let displayName = font.name;
            if (displayName.length > 14) {
                displayName = displayName.substring(0, 11) + '...';
            }
            option.setAttribute('translate', 'no');
            option.textContent = displayName;
            option.title = font.name;
            fontSelect.appendChild(option);
        });

        fontSelect.removeAttribute('aria-busy');
        fontSelect.title = currentFontFile ? currentFontFile : '默认系统字体';

        const newFontSelect = fontSelect.cloneNode(true);
        fontSelect.parentNode.replaceChild(newFontSelect, fontSelect);
        newFontSelect.addEventListener('change', () => {
            applyLayerProperties(layerId, ...applyArgs);
        });
    } catch (error) {
        fontSelect.removeAttribute('aria-busy');
        fontSelect.title = '字体列表加载失败';
        while (fontSelect.children.length > 1) {
            fontSelect.removeChild(fontSelect.lastChild);
        }
        const failOpt = document.createElement('option');
        failOpt.value = '';
        failOpt.textContent = '字体列表加载失败';
        failOpt.disabled = true;
        fontSelect.appendChild(failOpt);
    }
}

// 发送歌词控制命令
async function sendLyricCommand(action, params = {}, apiPost) {
    return await apiAction('lyrics', action, params);
}

// 获取歌词状态
async function getLyricStatus(apiPost, addToCommandLog) {
    try {
        const result = await sendLyricCommand('getStatus', {}, apiPost);
        if (result) {
            const data = result && typeof result === 'object' ? result : {};
            let styleInfo = {
                font_size: 0,
                primary_color: '0x00FFFFFF',
                secondary_color: '0x00FF2100',
                outline_color: '0x00000000',
                outline: 2.0, shadow: 0.0, alignment: 5
            };

            // 只有当歌词已加载时才尝试获取样式，避免500错误
            const isLoaded = data.loaded === true;
            if (isLoaded) {
                try {
                    const styleResult = await sendLyricCommand('get_style', { style_name: '歌词' }, apiPost);
                    if (styleResult && typeof styleResult === 'object') {
                        const styleData = styleResult;
                        styleInfo = {
                            font_size: styleData.font_size || 0,
                            primary_color: formatColorHex(styleData.primary_color),
                            secondary_color: formatColorHex(styleData.secondary_color),
                            outline_color: formatColorHex(styleData.outline_color),
                            outline: styleData.outline || 2.0,
                            shadow: styleData.shadow || 0.0,
                            alignment: styleData.alignment || 5
                        };
                    }
                } catch (err) { /* 静默处理样式获取失败 */ }
            }

            return {
                visible: data.visible !== false,
                loaded: isLoaded,
                margin: data.margin || { left: 0, right: 0, top: 0, bottom: 0 },
                style: styleInfo
            };
        }
    } catch (error) { /* 静默处理 */ }
    return null;
}

// 格式化颜色
function formatColorHex(colorValue) {
    if (typeof colorValue === 'string') return colorValue;
    if (typeof colorValue === 'number') return '0x' + colorValue.toString(16).padStart(8, '0').toUpperCase();
    return '0x00FFFFFF';
}

// 更新歌词参数UI
function updateLyricParamsUI(lyricStatus, isSlice = false) {
    if (!lyricStatus) return;
    const selectId = isSlice ? 'slice-lyric-visible' : 'lyric-visible';
    const visibleSelect = document.getElementById(selectId);
    if (visibleSelect) {
        visibleSelect.value = lyricStatus.visible ? 'true' : 'false';
    }
}

// 设置歌词参数事件监听器
function setupLyricParamsListeners(layerId, apiPost, addToCommandLog, isSlice = false) {
    const visibleId = isSlice ? 'slice-lyric-visible' : 'lyric-visible';
    const bindId = isSlice ? 'slice-lyric-bind-layer' : 'lyric-bind-layer';

    document.getElementById(visibleId)?.addEventListener('change', async (e) => {
        const visible = e.target.value === 'true';
        await handleApiOperation(
            sendLyricCommand('set_visible', { visible: visible }, apiPost),
            '歌词控制',
            `歌词${visible ? '显示' : '隐藏'} 成功`,
            null
        );
    });

    document.getElementById(bindId)?.addEventListener('change', async (e) => {
        const bindLayerIdValue = parseInt(e.target.value);
        await handleApiOperation(
            apiPut(`/layers/${layerId}`, { bind_layerId: bindLayerIdValue }),
            '歌词控制',
            `已将歌词关联到图层 ${bindLayerIdValue}`,
            null
        );
    });

    // 歌词自动加载开关（仅在非切片模式下）
    if (!isSlice) {
        document.getElementById('lyric-auto-load')?.addEventListener('change', async (e) => {
            const enabled = e.target.checked;
            await handleApiOperation(
                apiPut('/system/config', { lyric_enabled: enabled }),
                '歌词控制',
                `歌词自动加载已${enabled ? '启用' : '禁用'}`,
                null
            );
        });
    }
}

/**
 * 显示切片属性
 * 注意：这也是一个很长的函数，包含大量HTML生成逻辑
 */
export async function showSliceProperties(
    layerId, sliceKey, layers, getLayerTypeFlags, parseSliceCoordinate,
    parseColor, rgbaStringToCssColor, cssColorToRgbaString,
    renderQRCodeDashboard, setupQRCodeDashboardListeners,
    applySliceProperties, toggleSliceVisibility, deleteSlice,
    updateSliceInfo, drawCanvas, loadImage, addToCommandLog, apiGet, apiPost
) {
    const formContainer = document.getElementById('layer-properties-form');
    if (!formContainer) return;

    const renderRevision = ++propertyRenderRevision;
    const isCurrentRender = () =>
        renderRevision === propertyRenderRevision && formContainer.isConnected;

    formContainer.style.display = 'block';
    delete formContainer.dataset.layerId;
    delete formContainer.dataset.sliceKey;

    const layer = layers.find(l => Number(l.id) === Number(layerId));
    if (!layer) return;

    if (layer.slices && typeof layer.slices === 'object' && layer[sliceKey] === undefined && layer.slices[sliceKey] !== undefined) {
        layer[sliceKey] = layer.slices[sliceKey];
    }
    const sliceData = layer[sliceKey];
    if (!sliceData) return;

    const sliceIndex = sliceKey.replace('slice', '');
    const sliceName = `图层${layerId} -切片${sliceIndex} `;

    // 解析切片参数
    const fallbackSize = { width: layer?.size?.width || 0, height: layer?.size?.height || 0 };
    const coord = parseSliceCoordinate(sliceData, fallbackSize);

    const { isVideoLayer, isCaptureLayer, isQRCodeLayer, isImageLayer, isTextLayer, isLyricLayer, isLayer40, isLayer41 } = getLayerTypeFlags(layer, layerId);

    // 非文本图层切片使用自己的漫游配置，避免写到主图层漫游。
    const showRoamForSlice = (layerId !== 21 && layerId !== 30 && layerId !== 40 && layerId !== 41);
    const sliceRoamConfig = normalizeRoamConfigValue(sliceData.roamConfig ?? sliceData.roam_config);

    // Layer 41 切片需要关联播放列表下拉，与图层一致
    let layer41PlaylistOptions = '';
    if (isLayer41) {
        let playlists = [];
        try {
            const response = await apiGet('/playlists');
            if (Array.isArray(response)) playlists = response;
        } catch (e) {
            console.error('[LayerMatrix] Failed to fetch playlists for Layer 41 slice', e);
        }
        layer41PlaylistOptions = buildLayer41PlaylistOptions(playlists, layer.playlistId || '');
    }

    // 注意：这里需要生成完整的HTML表单
    if (!isCurrentRender()) return;

    if (isQRCodeLayer) {
        formContainer.innerHTML = `
            <div class="property-group">
                <div class="property-header">
                    <h4>${sliceName}</h4>
                </div>
                <div class="property-content">
                    ${renderQRCodeDashboard(layer, 'slice', coord || { x: 0, y: 0, width: 0, height: 0, priority: layerId })}
                    ${contentCropBlockHtml(sliceData, 'slice')}
                </div>
            </div>
        `;
        formContainer.dataset.layerId = String(layerId);
        formContainer.dataset.sliceKey = String(sliceKey);
        setupQRCodeDashboardListeners(layerId, 'slice', sliceKey);
        registerLivePreviewHandlers(
            ['slice-crop-left', 'slice-crop-top', 'slice-crop-right', 'slice-crop-bottom'],
            () => applySliceProperties(layerId, sliceKey)
        );
    } else {
        // 构建切片特有字段 (基于图层类型，与图层属性面板保持一致)
        let sliceSpecialFields = '';

        if (isCaptureLayer) {
            const captureType = sliceData.capture_type ?? sliceData.captureType ?? '';
            const sliceShapeType = Number(sliceData.shapeType ?? sliceData.shape_type ?? 0);
            const sliceShapeParam = Number(sliceData.shapeParam ?? sliceData.shape_param ?? 0.0);
            const sliceInvert = sliceData.invert !== undefined ? sliceData.invert : (sliceData.mirror ? 1 : 0);
            const sliceCaptureBlackToTransparent = sliceData.black_to_transparent !== undefined
                ? sliceData.black_to_transparent
                : (sliceData.blackToTransparent ?? layer.black_to_transparent ?? layer.blackToTransparent ?? false);
            const sliceCaptureGaussianBlur = Math.round(sliceData.gaussian_blur ?? sliceData.gaussianBlur ?? layer.gaussian_blur ?? layer.gaussianBlur ?? 0);
            const sliceFitMode = Number(sliceData.fit_mode !== undefined ? sliceData.fit_mode : (sliceData.fitMode !== undefined ? sliceData.fitMode : (layer.fit_mode ?? 0))) > 0 ? 1 : 0;

            sliceSpecialFields += `
                <div class="setting-section">
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="slice-capture-type">输入选择:</label>
                        <select id="slice-capture-type" class="form-control">
                            <option value="" ${captureType === '' ? 'selected' : ''}>跟随主图层</option>
                            <option value="AUTO" ${captureType === 'AUTO' ? 'selected' : ''}>自动</option>
                            <option value="HDMI" ${captureType === 'HDMI' ? 'selected' : ''}>HDMI</option>
                            <option value="USB" ${captureType === 'USB' ? 'selected' : ''}>USB</option>
                            <option value="MIPI" ${captureType === 'MIPI' ? 'selected' : ''}>MIPI</option>
                        </select>
                    </div>
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="slice-shape-type">图层形状:</label>
                        <select id="slice-shape-type" class="form-control">
                            ${generateShapeTypeOptions(sliceShapeType)}
                        </select>
                    </div>
                    ${generateShapeParamHTML(sliceShapeType, sliceShapeParam, 'slice', 'block')}
                    <div class="setting-item" style="grid-column: span 1;">
                        <label>图像反转:</label>
                        <select id="slice-invert" class="layer-select">
                            <option value="0" ${sliceInvert === 0 ? 'selected' : ''}>无反转</option>
                            <option value="1" ${sliceInvert === 1 ? 'selected' : ''}>水平反转</option>
                            <option value="2" ${sliceInvert === 2 ? 'selected' : ''}>垂直反转</option>
                            <option value="3" ${sliceInvert === 3 ? 'selected' : ''}>水平+垂直</option>
                        </select>
                    </div>
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="slice-black-to-transparent">黑色透明:</label>
                        <select id="slice-black-to-transparent" class="form-control">
                            <option value="false" ${!sliceCaptureBlackToTransparent ? 'selected' : ''}>关闭</option>
                            <option value="true" ${sliceCaptureBlackToTransparent ? 'selected' : ''}>开启</option>
                        </select>
                    </div>
                    ${createNumInput('slice-gaussian-blur', '高斯模糊', sliceCaptureGaussianBlur, 0, 10, 1, 1)}
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="slice-fit-mode">填充模式:</label>
                        <select id="slice-fit-mode" class="form-control">
                            <option value="0" ${sliceFitMode === 0 ? 'selected' : ''}>铺满显示</option>
                            <option value="1" ${sliceFitMode === 1 ? 'selected' : ''}>保持视频比例显示</option>
                        </select>
                    </div>
                </div>
            `;
        }


        if (isImageLayer && !isQRCodeLayer) {
            const filterMode = sliceData.filter_mode !== undefined ? sliceData.filter_mode : (layer.filter_mode !== undefined ? layer.filter_mode : 0);
            const animated = sliceData.animated !== undefined ? sliceData.animated : (layer.animated !== undefined ? layer.animated : false);

            if (layerId === 70) {
                // Logo 图层切片只显示动画开关
                sliceSpecialFields += `
                    <div class="setting-section">
                        <div class="setting-item" style="grid-column: span 2;">
                            <label for="slice-animated">显示模式:</label>
                            <select id="slice-animated" class="form-control">
                                <option value="static" ${!animated ? 'selected' : ''}>静态</option>
                                <option value="animated" ${animated ? 'selected' : ''}>动态</option>
                            </select>
                        </div>
                    </div>
                `;
            } else {
                // 与图层图像参数区域共享布局：同顺序 过滤模式→淡入→淡出→显示时长→显示模式，span 一致
                const sliceFadeIn = sliceData.fade_in_time !== undefined ? sliceData.fade_in_time : 0.5;
                const sliceFadeOut = sliceData.fade_out_time !== undefined ? sliceData.fade_out_time : 0.5;
                const sliceDisplayDur = sliceData.display_duration !== undefined ? sliceData.display_duration : 3.0;
                sliceSpecialFields += `
                    <div class="setting-section">
                        <div class="setting-item" style="grid-column: span 2;">
                            <label for="slice-filter-mode">过滤模式:</label>
                            <select id="slice-filter-mode" class="form-control">
                                <option value="0" ${filterMode === 0 ? 'selected' : ''}>线性过滤</option>
                                <option value="1" ${filterMode === 1 ? 'selected' : ''}>最近邻过滤</option>
                            </select>
                        </div>
                        ${createNumInput('slice-fade-in-time', '淡入时间(秒)', sliceFadeIn, 0, null, 0.1, 1)}
                        ${createNumInput('slice-fade-out-time', '淡出时间(秒)', sliceFadeOut, 0, null, 0.1, 1)}
                        ${createNumInput('slice-display-duration', '显示时长(秒)', sliceDisplayDur, 0, null, 0.1, 1)}
                        <div class="setting-item" style="grid-column: span 2;">
                            <label for="slice-animated">显示模式:</label>
                            <select id="slice-animated" class="form-control">
                                <option value="static" ${!animated ? 'selected' : ''}>静态</option>
                                <option value="animated" ${animated ? 'selected' : ''}>动态</option>
                            </select>
                        </div>
                    </div>
                `;
            }
        }


        if (isTextLayer && !isQRCodeLayer && !isLyricLayer) {
            const isLayer40 = layerId === 40;
            const textColor = sliceData.text_color || layer.text_color || "1.0 1.0 1.0 1.0";
            const bgColor = sliceData.bg_color || layer.bg_color || "0.0 0.0 0.0 0.0";
            const textColorCss = rgbaStringToCssColor(textColor);
            const bgColorCss = rgbaStringToCssColor(bgColor);
            const scrollSpeed = sliceData.scroll_speed !== undefined ? sliceData.scroll_speed : (layer.scroll_speed !== undefined ? layer.scroll_speed : 200);
            const displayMode = sliceData.alignment !== undefined ? sliceData.alignment : (layer.alignment !== undefined ? layer.alignment : 1);
            const fontSize = sliceData.font_size !== undefined ? sliceData.font_size : (layer.font_size !== undefined ? layer.font_size : 48.0);
            const fontFile = sliceData.font_file || layer.font_file || '';

            if (isLayer40) {
                // 跑马灯切片参数（与图层40属性面板布局一致，使用 createNumInput）
                const isBgTransparent = (bgColor === "0.0 0.0 0.0 0.0");
                const alphaVal = Math.round((sliceData.alpha !== undefined ? sliceData.alpha : 1.0) * 255);

                sliceSpecialFields += generateTextLayer40Html('slice', { text: sliceData.text || layer.text || '欢迎使用本系统', fontFile, textColorCss, textColor, bgColorCss, bgColor, isBgTransparent, fontSize, displayMode, scrollSpeed, alphaVal });
            } else if (isLayer41) {
                // Layer 41 切片参数：与图层41一致（显示位置、关联播放列表、字体大小、显示数量、显示时长、起始/结束提示时间）
                const sliceDisplayAlign = sliceData.display_align !== undefined ? sliceData.display_align : (layer.display_align !== undefined ? layer.display_align : 0);
                const sliceFontSize = sliceData.font_size !== undefined ? sliceData.font_size : (layer.font_size !== undefined ? layer.font_size : 36.0);
                const sliceShowCount = sliceData.show_count !== undefined ? sliceData.show_count : (layer.show_count !== undefined ? layer.show_count : 3);
                const sliceDisplayDuration = sliceData.display_duration !== undefined ? sliceData.display_duration : (layer.display_duration !== undefined ? layer.display_duration : 5.0);
                const sliceStartHintTime = sliceData.start_hint_time !== undefined ? sliceData.start_hint_time : (layer.start_hint_time !== undefined ? layer.start_hint_time : 10.0);
                const sliceEndHintTime = sliceData.end_hint_time !== undefined ? sliceData.end_hint_time : (layer.end_hint_time !== undefined ? layer.end_hint_time : 10.0);
                const alphaVal = Math.round((sliceData.alpha !== undefined ? sliceData.alpha : 1.0) * 255);
                const showList = layer.show_list !== undefined ? layer.show_list : true;

                sliceSpecialFields += generateTextLayer41Html('slice', { alphaVal, displayAlign: sliceDisplayAlign, playlistOptions: layer41PlaylistOptions, fontSize: sliceFontSize, showCount: sliceShowCount, displayDuration: sliceDisplayDuration, startHintTime: sliceStartHintTime, endHintTime: sliceEndHintTime, showList });
            } else {
                // 普通文本切片参数 (30)，与图层30属性面板布局一致
                const isBgTransparent = (bgColor === "0.0 0.0 0.0 0.0");

                sliceSpecialFields += generateNormalTextHtml('slice', { text: sliceData.text || layer.text || '', fontFile: sliceData.font_file || layer.font_file || '', fontSize, textColorCss, textColor, bgColorCss, bgColor, isBgTransparent, alignment: displayMode });
            }
        }

        if (isLyricLayer) {
            // 获取歌词状态并渲染
            let lyricStatus = { visible: true };
            getLyricStatus(apiPost, addToCommandLog).then(status => {
                if (status) {
                    lyricStatus = status;
                    updateLyricParamsUI(lyricStatus, true);
                }
            }).catch(() => { });

            sliceSpecialFields += `
                <div class="setting-section">
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="slice-lyric-bind-layer">关联视频层:</label>
                        <select id="slice-lyric-bind-layer" class="form-control">
                            <option value="1" ${layer.bind_layerId === 1 ? 'selected' : ''}>图层 1</option>
                            <option value="2" ${layer.bind_layerId === 2 ? 'selected' : ''}>图层 2</option>
                            <option value="3" ${layer.bind_layerId === 3 ? 'selected' : ''}>图层 3</option>
                            <option value="4" ${layer.bind_layerId === 4 ? 'selected' : ''}>图层 4</option>
                        </select>
                    </div>
                    <div class="setting-item" style="grid-column: span 1;">
                        <label for="slice-lyric-visible">可见性:</label>
                        <select id="slice-lyric-visible" class="form-control">
                            <option value="true" ${lyricStatus.visible ? 'selected' : ''}>显示</option>
                            <option value="false" ${!lyricStatus.visible ? 'selected' : ''}>隐藏</option>
                        </select>
                    </div>
                </div>
            `;
        }

        const sliceShapeType = Number(sliceData.shapeType ?? sliceData.shape_type ?? 0);
        const sliceShapeParam = Number(sliceData.shapeParam ?? sliceData.shape_param ?? 0.0);
        const sliceVolumePct = Math.round((sliceData.volume !== undefined ? sliceData.volume : (layer.volume !== undefined ? layer.volume : 1.0)) * 100);
        const sliceGaussianBlur = Math.round(sliceData.gaussian_blur !== undefined ? sliceData.gaussian_blur : (sliceData.gaussianBlur !== undefined ? sliceData.gaussianBlur : (layer.gaussian_blur !== undefined ? layer.gaussian_blur : 0)));
        const sliceInvertVal = sliceData.invert !== undefined ? sliceData.invert : (sliceData.mirror ? 1 : 0);
        const sliceFitMode = Number(sliceData.fit_mode !== undefined ? sliceData.fit_mode : (sliceData.fitMode !== undefined ? sliceData.fitMode : (layer.fit_mode ?? 0))) > 0 ? 1 : 0;
        const sliceBlackToTransparent = sliceData.black_to_transparent !== undefined ? sliceData.black_to_transparent : (sliceData.blackToTransparent || false);

        // 仅对视频和特定图像图层输出特殊的额外控制（透明度/音量/高斯模糊/形状等），与主图层属性面板一致
        let sliceVideoControlSection = '';
        if (isVideoLayer && !isCaptureLayer && !isQRCodeLayer) {
            sliceVideoControlSection = `
            <div class="setting-section">
                ${createNumInput('slice-alpha', '透明度', Math.round((sliceData.alpha !== undefined ? sliceData.alpha : (sliceData.transparency !== undefined ? sliceData.transparency / 255 : 1.0)) * 255), 0, 255, 1, 1)}
                ${createNumInput('slice-volume', '音量(%)', sliceVolumePct, 0, 100, 5, 1)}
                ${createNumInput('slice-gaussian-blur', '高斯模糊', sliceGaussianBlur, 0, 10, 1, 1)}
                <div class="setting-item" style="grid-column: span 2;">
                    <label for="slice-shape-type">图层形状:</label>
                    <select id="slice-shape-type" class="form-control">
                        ${generateShapeTypeOptions(sliceShapeType)}
                    </select>
                </div>
                ${generateShapeParamHTML(sliceShapeType, sliceShapeParam, 'slice', 'block')}
                <div class="setting-item" style="grid-column: span 1;">
                    <label>图像反转:</label>
                    <select id="slice-invert" class="layer-select">
                        <option value="0" ${sliceInvertVal === 0 ? 'selected' : ''}>无反转</option>
                        <option value="1" ${sliceInvertVal === 1 ? 'selected' : ''}>水平反转</option>
                        <option value="2" ${sliceInvertVal === 2 ? 'selected' : ''}>垂直反转</option>
                        <option value="3" ${sliceInvertVal === 3 ? 'selected' : ''}>水平+垂直</option>
                    </select>
                </div>
                <div class="setting-item" style="grid-column: span 1;">
                    <label>黑色透明:</label>
                    <select id="slice-black-to-transparent" class="form-control">
                        <option value="false" ${!sliceBlackToTransparent ? 'selected' : ''}>关闭</option>
                        <option value="true" ${sliceBlackToTransparent ? 'selected' : ''}>开启</option>
                    </select>
                </div>
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="slice-fit-mode">填充模式:</label>
                    <select id="slice-fit-mode" class="form-control">
                        <option value="0" ${sliceFitMode === 0 ? 'selected' : ''}>铺满显示</option>
                        <option value="1" ${sliceFitMode === 1 ? 'selected' : ''}>保持视频比例显示</option>
                    </select>
                </div>
            </div>
            `;
        } else if (isImageLayer && !isQRCodeLayer && layerId !== 60) {
            // 图形图层的切片控制块（没有音量和高斯模糊）
            sliceVideoControlSection = `
            <div class="setting-section">
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="slice-shape-type">图层形状:</label>
                    <select id="slice-shape-type" class="form-control">
                        ${generateShapeTypeOptions(sliceShapeType)}
                    </select>
                </div>
                ${generateShapeParamHTML(sliceShapeType, sliceShapeParam, 'slice', 'block')}
                <div class="setting-item" style="grid-column: span 1;">
                    <label>图像反转:</label>
                    <select id="slice-invert" class="layer-select">
                        <option value="0" ${sliceInvertVal === 0 ? 'selected' : ''}>无反转</option>
                        <option value="1" ${sliceInvertVal === 1 ? 'selected' : ''}>水平反转</option>
                        <option value="2" ${sliceInvertVal === 2 ? 'selected' : ''}>垂直反转</option>
                        <option value="3" ${sliceInvertVal === 3 ? 'selected' : ''}>水平+垂直</option>
                    </select>
                </div>
                <div class="setting-item" style="grid-column: span 1;">
                    <label>黑色透明:</label>
                    <select id="slice-black-to-transparent" class="form-control">
                        <option value="false" ${!sliceBlackToTransparent ? 'selected' : ''}>关闭</option>
                        <option value="true" ${sliceBlackToTransparent ? 'selected' : ''}>开启</option>
                    </select>
                </div>
            </div>
            `;
        }
        // 漫游配置区域（非文本图层切片显示，按 layerId 显式排除 21/30/40/41）
        const sliceRoamSection = showRoamForSlice ? `
            <div class="property-separator" style="grid-column: 1 / -1; height: 1px; background: rgba(255,255,255,0.1); margin: 10px 0;"></div>
            <div class="setting-section" id="slice-roam-config-section" style="grid-column: 1 / -1;">
                ${showRoamForSlice ? createRotationForRoamRow('slice', sliceData.rotation ?? sliceData.rotate ?? 0.0) : ''}
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="slice-roam-mode">漫游模式:</label>
                    <select id="slice-roam-mode" class="form-control layer-param-control">
                        <option value="0" ${sliceRoamConfig.mode === 0 ? 'selected' : ''}>关闭</option>
                        <option value="1" ${sliceRoamConfig.mode === 1 ? 'selected' : ''}>左右移动</option>
                        <option value="2" ${sliceRoamConfig.mode === 2 ? 'selected' : ''}>上下移动</option>
                        <option value="3" ${sliceRoamConfig.mode === 3 ? 'selected' : ''}>圆形路径</option>
                    </select>
                </div>
                ${layerId === 60 ? `
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="slice-shape-type">图层形状:</label>
                    <select id="slice-shape-type" class="form-control layer-param-control">
                        ${generateShapeTypeOptions(sliceData.shapeType ?? sliceData.shape_type ?? layer.shapeType ?? layer.shape_type ?? 0)}
                    </select>
                </div>
                ` : ''}
                <div class="setting-item" style="grid-column: span 1;">
                    <label for="slice-roam-speed">移动速度(像素/秒):</label>
                    ${createNumControl('slice-roam-speed', sliceRoamConfig.speed || 100, 1, 1000)}
                </div>
                <div id="slice-roam-horizontal-params" class="setting-item" style="grid-column: span 1; display: ${sliceRoamConfig.mode === 1 ? 'block' : 'none'};">
                    <label for="slice-roam-range-x">移动范围(像素):</label>
                    ${createNumControl('slice-roam-range-x', sliceRoamConfig.rangeX || 500, 0, 4000, 10)}
                </div>
                <div id="slice-roam-vertical-params" class="setting-item" style="grid-column: span 1; display: ${sliceRoamConfig.mode === 2 ? 'block' : 'none'};">
                    <label for="slice-roam-range-y">移动范围(像素):</label>
                    ${createNumControl('slice-roam-range-y', sliceRoamConfig.rangeY || 500, 0, 4000, 10)}
                </div>
                <div id="slice-roam-circular-params" class="setting-item" style="grid-column: span 1; display: ${sliceRoamConfig.mode === 3 ? 'block' : 'none'};">
                    <label for="slice-roam-radius">圆形半径(像素):</label>
                    ${createNumControl('slice-roam-radius', sliceRoamConfig.radius || 200, 0, 2000, 10)}
                </div>
                <div class="setting-item" style="grid-column: span 1;">
                    <label>循环播放:</label>
                    <label class="preview-switch">
                        <input type="checkbox" id="slice-roam-loop" ${sliceRoamConfig.loop !== false ? 'checked' : ''}>
                        <span class="switch-slider"></span>
                    </label>
                </div>
                ${layerId === 60 ? `
                <div class="setting-item" style="grid-column: span 1;">
                    <label>黑色变透明:</label>
                    <label class="preview-switch">
                        <input type="checkbox" id="slice-black-to-transparent" ${(sliceData.black_to_transparent ?? sliceData.blackToTransparent ?? layer.black_to_transparent ?? layer.blackToTransparent) ? 'checked' : ''}>
                        <span class="switch-slider"></span>
                    </label>
                </div>
                ` : ''}
            </div>
        ` : '';
        const useCompactSliceLayout = ((isVideoLayer && !isCaptureLayer && !isQRCodeLayer) || isCaptureLayer);
        const sliceControls = `
            ${sliceSpecialFields}
            ${sliceVideoControlSection}
            ${contentCropBlockHtml(sliceData, 'slice')}

            <div class="property-separator" style="grid-column: 1 / -1; height: 1px; background: rgba(255,255,255,0.1); margin: 5px 0;"></div>

            <div class="setting-section ${useCompactSliceLayout ? 'compact-advanced-params compact-advanced-params--common' : ''}">
                ${buildCommonPositionSection('slice', {
            priority: sliceData.priority ?? layer.priority ?? 0,
            x: coord.x ?? 0,
            y: coord.y ?? 0,
            width: coord.width ?? 0,
            height: coord.height ?? 0,
            rotation: sliceData.rotation ?? sliceData.rotate ?? 0.0,
            alpha: sliceData.alpha !== undefined ? sliceData.alpha : (sliceData.transparency !== undefined ? sliceData.transparency / 255 : 1.0)
        }, layerId, { alphaInFirstRow: !isCaptureLayer, rotationInRoamRow: showRoamForSlice })}
            </div>
            ${sliceRoamSection}
            `;

        formContainer.innerHTML = `
            <div class="property-group">
                <div class="property-header">
                    <h4>${sliceName}</h4>
                    <div class="property-tabs" role="tablist">
                        <button type="button" class="property-tab active" data-param-tab="common">常用</button>
                        <button type="button" class="property-tab" data-param-tab="advanced">高级</button>
                    </div>
                    <div class="property-actions">
                         <button id="slice-toggle-visibility-btn" class="btn small">${isSliceVisible(sliceData) ? '隐藏' : '显示'}</button>
                         <button id="slice-delete-btn" class="btn small danger">删除</button>
                         <button id="slice-save-btn" class="btn small primary">保存</button>
                    </div>
                </div>
                <div class="property-content slice-properties-content tab-common ${useCompactSliceLayout ? 'property-content--compact-advanced' : ''}">
                    ${sliceControls}
                </div>
            </div>
        `;
        formContainer.dataset.layerId = String(layerId);
        formContainer.dataset.sliceKey = String(sliceKey);

        applyParamTabs(formContainer);

        // 绑定数字输入框加减按钮
        setupNumberInputButtons(formContainer, undefined, { excludeSelector: '[id$="-roam-config-section"]' });

        // 漫游配置区域事件（切片漫游保存到当前切片）
        if (showRoamForSlice) {
            setupRoamConfigListeners(layerId, 'slice', { sliceKey, applySliceProperties });
        }

        // 绑定事件
        let sliceUpdateTimer = null;
        const triggerSliceUpdate = () => {
            // 立即更新画布预览（不等待 API 响应）
            const layer = layers.find(l => Number(l.id) === Number(layerId));
            if (layer && layer.slices && typeof layer.slices === 'object' && layer[sliceKey] === undefined && layer.slices[sliceKey] !== undefined) {
                layer[sliceKey] = layer.slices[sliceKey];
            }
            if (layer && layer[sliceKey]) {
                const sliceData = layer[sliceKey];
                // 更新切片的图层形状参数（从 UI 读取）
                const shapeTypeEl = document.getElementById('slice-shape-type');
                const shapeParamEl = document.getElementById('slice-shape-param');
                if (shapeTypeEl) {
                    sliceData.shape_type = parseInt(shapeTypeEl.value) || 0;
                }
                if (shapeParamEl) {
                    sliceData.shape_param = parseFloat(shapeParamEl.value) || 0.0;
                }
                // 立即刷新画布
                drawCanvas();
            }

            clearTimeout(sliceUpdateTimer);
            sliceUpdateTimer = setTimeout(() => {
                applySliceProperties(layerId, sliceKey);
            }, 300);
        };

        // Alpha 滑块显示更新
        // Alpha 滑块显示更新已移除，转为普通数字输入框监听


        if (isTextLayer && !isQRCodeLayer && !isLyricLayer) {
            setupTextLayerEvents(layerId, 'slice', sliceKey, triggerSliceUpdate, applySliceProperties, cssColorToRgbaString);
        }

        // 绑定输入框变化事件 - 实时预览
        registerLivePreviewHandlers(
            [
                'slice-x', 'slice-y', 'slice-width', 'slice-height', 'slice-priority',
                'slice-rotation', 'slice-alpha', 'slice-invert', 'slice-volume',
                'slice-gaussian-blur', 'slice-text-content', 'slice-text-scroll-speed',
                'slice-text-font-size', 'slice-text-display-mode', 'slice-text-alignment',
                'slice-text-font-file', 'slice-fade-in-time', 'slice-fade-out-time', 'slice-display-duration',
                'slice-filter-mode', 'slice-animated',
                'slice-capture-type',
                'slice-shape-type', 'slice-shape-param', 'slice-black-to-transparent',
                'slice-fit-mode',
                'slice-crop-left', 'slice-crop-top', 'slice-crop-right', 'slice-crop-bottom',
                // Layer 41 切片特有字段
                'slice-display-align', 'slice-playlist-id', 'slice-show-count', 'slice-start-hint-time', 'slice-end-hint-time'
            ],
            () => applySliceProperties(layerId, sliceKey)
        );

        // 图层不管文件操作，无切片加载图像

        // 保存按钮
        document.getElementById('slice-save-btn')?.addEventListener('click', async () => {
            clearTimeout(sliceUpdateTimer);
            await applySliceProperties(layerId, sliceKey);
        });

        document.getElementById('slice-toggle-visibility-btn')?.addEventListener('click', async (event) => {
            const button = event.currentTarget;
            button.disabled = true;
            try {
                await toggleSliceVisibility(layerId, sliceKey);
            } finally {
                button.disabled = false;
            }
        });

        // 歌词图层事件监听 (切片版)
        if (isTextLayer && !isQRCodeLayer && !isLyricLayer && !isLayer41) {
            loadFontList(layerId, isLayer40 ? (getSliceData(layer, sliceKey)?.font_file || layer.font_file || '') : (layer.font_file || ''), apiGet, applySliceProperties, 'slice-text-font-file', [sliceKey]);
        }

        if (isLyricLayer) {
            document.getElementById('slice-lyric-visible')?.addEventListener('change', async (e) => {
                const visible = e.target.value === 'true';
                await sendLyricCommand('set_visible', { visible: visible }, apiPost);
                addToCommandLog('歌词控制', 'success', `歌词${visible ? '显示' : '隐藏'} 成功`);
            });

            document.getElementById('slice-lyric-bind-layer')?.addEventListener('change', async (e) => {
                const bindId = parseInt(e.target.value);
                await apiPut(`/layers/${layerId}`, { bind_layerId: bindId });
                addToCommandLog('歌词控制', 'success', `已将歌词关联到图层 ${bindId}`);
            });
        }

        // 删除按钮
        document.getElementById('slice-delete-btn')?.addEventListener('click', async () => {
            const confirmed = await showConfirm(`确定要删除切片 ${sliceIndex} 吗？`, '删除确认');
            if (confirmed) {
                await deleteSlice(layerId, sliceKey);
            }
        });
    }
}

/**
 * 设置漫游配置事件监听器
 * @param {number} layerId - 图层ID
 * @param {string} prefix - 'layer' 或 'slice'
 * @param {object} options - 切片保存所需的上下文
 */
function setupRoamConfigListeners(layerId, prefix = 'layer', options = {}) {
    const idPrefix = prefix === 'slice' ? 'slice-' : 'layer-';
    
    const roamModeSelect = document.getElementById(idPrefix + 'roam-mode');
    const roamSpeedInput = document.getElementById(idPrefix + 'roam-speed');
    const roamRangeXInput = document.getElementById(idPrefix + 'roam-range-x');
    const roamRangeYInput = document.getElementById(idPrefix + 'roam-range-y');
    const roamRadiusInput = document.getElementById(idPrefix + 'roam-radius');
    const roamCustomPathTextarea = document.getElementById(idPrefix + 'roam-custom-path');
    const roamLoopCheckbox = document.getElementById(idPrefix + 'roam-loop');
    const roamResetBtn = document.getElementById(idPrefix + 'roam-reset-btn');

    // 漫游模式切换时显示/隐藏对应的参数区域
    if (roamModeSelect) {
        roamModeSelect.addEventListener('change', function () {
            const mode = parseInt(this.value);
            const horizontalParams = document.getElementById(idPrefix + 'roam-horizontal-params');
            const verticalParams = document.getElementById(idPrefix + 'roam-vertical-params');
            const circularParams = document.getElementById(idPrefix + 'roam-circular-params');
            const customParams = document.getElementById(idPrefix + 'roam-custom-params');

            if (horizontalParams) horizontalParams.style.display = mode === 1 ? 'block' : 'none';
            if (verticalParams) verticalParams.style.display = mode === 2 ? 'block' : 'none';
            if (circularParams) circularParams.style.display = mode === 3 ? 'block' : 'none';
            if (customParams) customParams.style.display = mode === 4 ? 'block' : 'none';

            saveRoamConfig(layerId, prefix, mode === 0, options);
        });
    }

    bindLiveUpdate(roamLoopCheckbox);

    // A committed change cancels the input debounce, so one edit saves once.
    function bindLiveUpdate(element, debounce = 0) {
        if (!element) return;
        let timer = null;
        if (debounce > 0) {
            element.addEventListener('input', () => {
                clearTimeout(timer);
                timer = setTimeout(() => saveRoamConfig(layerId, prefix, false, options), debounce);
            });
        }
        element.addEventListener('change', () => {
            clearTimeout(timer);
            saveRoamConfig(layerId, prefix, false, options);
        });
    }

    [roamSpeedInput, roamRangeXInput, roamRangeYInput, roamRadiusInput].forEach(input => bindLiveUpdate(input, 300));
    bindLiveUpdate(roamCustomPathTextarea, 500);

    // 重置计时器按钮
    if (roamResetBtn) {
        roamResetBtn.addEventListener('click', async () => {
            if (prefix === 'slice') {
                await saveRoamConfig(layerId, prefix, true, options);
                return;
            }
            try {
                await apiPost(`/layers/${layerId}/roam/reset`);
                addToCommandLog('漫游控制', 'success', '漫游计时器已重置');
            } catch (error) {
                addToCommandLog('漫游控制', 'error', `重置失败: ${error.message || '未知错误'}`);
            }
        });
    }

    // 数字输入框加减按钮
    setupNumberInputButtons(document.getElementById(prefix === 'slice' ? 'slice-roam-config-section' : 'layer-roam-config-section'));
}

/**
 * 保存漫游配置
 * @param {number} layerId - 图层ID
 * @param {string} prefix - 'layer' 或 'slice'
 * @param {object} options - 切片保存所需的上下文
 */
async function saveRoamConfig(layerId, prefix = 'layer', resetPosition = false, options = {}) {
    const idPrefix = prefix === 'slice' ? 'slice-' : 'layer-';
    
    const roamModeSelect = document.getElementById(idPrefix + 'roam-mode');
    const roamSpeedInput = document.getElementById(idPrefix + 'roam-speed');
    const roamRangeXInput = document.getElementById(idPrefix + 'roam-range-x');
    const roamRangeYInput = document.getElementById(idPrefix + 'roam-range-y');
    const roamRadiusInput = document.getElementById(idPrefix + 'roam-radius');
    const roamCustomPathTextarea = document.getElementById(idPrefix + 'roam-custom-path');
    const roamLoopCheckbox = document.getElementById(idPrefix + 'roam-loop');

    if (!roamModeSelect) return;

    const mode = parseInt(roamModeSelect.value) || 0;

    const config = {
        enabled: true,
        mode,
        speed: roamSpeedInput ? parseFloat(roamSpeedInput.value) || 100 : 100,
        loop: roamLoopCheckbox ? roamLoopCheckbox.checked : true
    };

    if (mode === 1 && roamRangeXInput) {
        config.rangeX = parseInt(roamRangeXInput.value) || 0;
    } else if (mode === 2 && roamRangeYInput) {
        config.rangeY = parseInt(roamRangeYInput.value) || 0;
    } else if (mode === 3 && roamRadiusInput) {
        config.radius = parseFloat(roamRadiusInput.value) || 0;
    } else if (mode === 4 && roamCustomPathTextarea) {
        try {
            const pathText = roamCustomPathTextarea.value.trim();
            if (pathText) {
                config.customPath = JSON.parse(pathText);
            } else {
                config.customPath = [];
            }
        } catch (e) {
            addToCommandLog('漫游配置', 'error', `自定义路径JSON格式错误: ${e.message}`);
            return;
        }
    }

    try {
        if (prefix === 'slice') {
            if (typeof options.applySliceProperties !== 'function' || !options.sliceKey) {
                addToCommandLog('漫游配置', 'error', '切片漫游保存缺少切片上下文');
                return;
            }
            await options.applySliceProperties(layerId, options.sliceKey);
            return;
        }

        await apiPost(`/layers/${layerId}/roam`, config);
        if (resetPosition) {
            await apiPost(`/layers/${layerId}/roam/reset`);
        }
        addToCommandLog('漫游配置', 'success', '漫游配置已保存');
    } catch (error) {
        addToCommandLog('漫游配置', 'error', `保存失败: ${error.message || '未知错误'}`);
    }
}
