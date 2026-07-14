import { ensureBlendRegion, getState, setActiveRegion, setFocusMode } from '../actions.js?v=2.95';
import { drawBlendScene, drawGridScene, getBlendCurveAnchorFromDisplayValue, getBlendCurveHandleLayout } from '../canvas/renderer.js?v=2.101';
import { onCanvasMouseDown, onCanvasMouseMove, onCanvasMouseUp } from '../interaction/mouse.js?v=2.100';
import { handleKeyboard } from '../interaction/keyboard.js?v=2.100';
import { renderFusionUi } from './panel.js?v=2.100';
import { patchBlendSide, pushBlendSideUndo, setActiveBlendSide, setBlendAutoEdges, setBlendMasterEnabled, setBlendSideEnabled } from '../blend/actions.js?v=2.95';
import { hydrateBlendRegionsFromResponse, loadBlendRegion, syncBlendAutoEdges, syncBlendRegion } from '../sync/syncBlend.js?v=2.95';
import { patchColor, resetColor } from '../color/actions.js?v=2.95';
import { flushPendingGeometryOps, persistGeometryRegion, syncGeometryRegion, syncActiveGeometrySelection } from '../sync/syncGeometry.js?v=2.95';
import { persistMask, syncMask } from '../sync/syncMask.js?v=2.95';
import { syncColorRegion } from '../sync/syncColor.js?v=2.95';
import { setBlendCurveParams, setBlendMaster, setMergeGapBrightness } from '../api.js?v=2.95';
import { showNotification } from '../../../components/toast.js';
import { FOCUS_MODES, TABS } from '../types.js';
import { switchFusionTab } from '../modeSwitch.js?v=2.100';
import { clamp } from '../utils/grid.js';


let gridDrawPending = false;
let blendCurveDrag = null;
let blendCurveSyncTimer = null;
let blendCurveSyncChain = Promise.resolve();
let fullMaskSyncTimer = null;

function scheduleGridDraw(canvas) {
    if (gridDrawPending) return;
    gridDrawPending = true;
    window.requestAnimationFrame(() => {
        gridDrawPending = false;
        drawGridScene(canvas);
    });
}

/**
 * 遮罩的全量状态同步（用于切换模式、重置等非高频拖动场景）
 */
export function scheduleFullMaskSync() {
    if (fullMaskSyncTimer) clearTimeout(fullMaskSyncTimer);
    fullMaskSyncTimer = setTimeout(() => {
        fullMaskSyncTimer = null;
        void syncMask();
    }, 80);
}

function ensureBlendCanvas() {
    let canvas = document.getElementById('fusionBlendingCurve');
    if (canvas) return canvas;

    const groups = Array.from(document.querySelectorAll('#fusion-tab-geometry-fusion .control-group'));
    const targetGroup = groups.find((group) => {
        const title = group.querySelector('.control-group__title');
        return title && title.textContent.includes('融合参数');
    });
    if (!targetGroup) return null;

    const wrap = document.createElement('div');
    wrap.className = 'field-group mt-10';
    wrap.innerHTML = '<canvas id="fusionBlendingCurve" style="width:100%;height:220px;border-radius:12px;display:block;background:#0b1220"></canvas>';
    targetGroup.appendChild(wrap);
    return wrap.querySelector('#fusionBlendingCurve');
}

function bindCanvas(canvas) {
    if (!canvas || canvas.dataset.fusionBound) return;
    canvas.dataset.fusionBound = 'true';
    canvas.addEventListener('mousedown', async (event) => {
        const result = onCanvasMouseDown(getState(), canvas, event);
        scheduleGridDraw(canvas);
        if (result.geometrySelectionChanged) {
            void syncActiveGeometrySelection(getState().page.activeRegionId);
        }
    });
    canvas.addEventListener('mousemove', (event) => {
        const state = getState();
        onCanvasMouseMove(state, canvas, event);
    });
    window.addEventListener('mouseup', async () => {
        const state = getState();
        const wasDragging = state.interaction.dragging.active;
        const dragTarget = state.interaction.dragging.target;
        const dragRegionId = state.interaction.dragging.regionId;
        onCanvasMouseUp();
        
        if (wasDragging && dragTarget === 'geometry-point') {
            await syncActiveGeometrySelection(dragRegionId);
            void flushPendingGeometryOps().then(() => syncGeometryRegion(dragRegionId)).then(() => persistGeometryRegion(dragRegionId)).catch((error) => {
                console.warn('fusion geometry final sync failed:', error);
            });
        }
        if (wasDragging && (dragTarget === 'manager-point' || dragTarget === 'manager-line')) {
            void flushPendingGeometryOps().then(() => syncGeometryRegion(dragRegionId)).then(() => persistGeometryRegion(dragRegionId)).catch((error) => {
                console.warn('fusion manager final sync failed:', error);
            });
        }
        if (wasDragging && dragTarget === 'mask-point') {
            void syncMask().then(() => persistMask()).catch((error) => {
                console.warn('fusion mask final sync failed:', error);
            });
        }
    });
}

function isEditableTarget(target) {
    if (!(target instanceof Element)) return false;
    return !!target.closest('input, textarea, select, [contenteditable="true"]');
}

function focusFusionPage() {
    const page = document.getElementById('fusion-page');
    if (!page || !page.classList.contains('active')) return;
    try {
        page.focus({ preventScroll: true });
    } catch (_) {
        page.focus();
    }
}

function bindKeyboardFocus() {
    const page = document.getElementById('fusion-page');
    if (!page || page.dataset.keyboardFocusBound) return;
    page.dataset.keyboardFocusBound = 'true';
    page.addEventListener('pointerdown', (event) => {
        if (isEditableTarget(event.target)) return;
        window.requestAnimationFrame(focusFusionPage);
    }, true);
    page.addEventListener('mousedown', (event) => {
        if (isEditableTarget(event.target)) return;
        window.requestAnimationFrame(focusFusionPage);
    }, true);
    focusFusionPage();
}

function bindLongPressMenuGuard() {
    const page = document.getElementById('fusion-page');
    if (!page || page.dataset.longPressGuardBound) return;
    page.dataset.longPressGuardBound = 'true';

    ['contextmenu', 'selectstart', 'dragstart'].forEach((eventName) => {
        page.addEventListener(eventName, (event) => {
            if (isEditableTarget(event.target)) return;
            event.preventDefault();
        }, true);
    });
}

function bindBlendCanvas(canvas) {
    if (!canvas || canvas.dataset.bound) return;
    canvas.dataset.bound = 'true';
    canvas.addEventListener('pointerdown', (event) => {
        const state = getState();
        const regionId = getBlendCurveBaseRegionId(state);
        const side = state.blend.activeSide;
        const sideState = state.blend.byRegionId[regionId]?.[side] || state.blend.byRegionId[state.page.activeRegionId]?.[side];
        if (!sideState) return;

        const point = getCanvasPointerPoint(canvas, event);
        const layout = getBlendCurveHandleLayout(canvas, sideState);
        const insidePlot = point.x >= layout.rect.x
            && point.x <= layout.rect.x + layout.rect.width
            && point.y >= layout.rect.y
            && point.y <= layout.rect.y + layout.rect.height;
        const nearest = layout.handles.reduce((best, handle) => {
            const dx = handle.screen.x - point.x;
            const dy = handle.screen.y - point.y;
            const distance = Math.hypot(dx, dy);
            return distance < best.distance ? { handle, distance } : best;
        }, { handle: null, distance: Number.POSITIVE_INFINITY });

        if (!insidePlot || nearest.distance > 24) return;
        const handleId = nearest.handle.id;
        const ids = getBlendCurveTargetIds(state);
        ids.forEach((id) => pushBlendSideUndo(id));
        blendCurveDrag = {
            handleId,
            pointerId: event.pointerId,
            side,
            regionId,
            ids
        };
        canvas.classList.add('is-dragging');
        if (canvas.setPointerCapture) canvas.setPointerCapture(event.pointerId);
        event.preventDefault();
        applyBlendCurveDrag(canvas, event, false);
    });

    canvas.addEventListener('pointermove', (event) => {
        if (!blendCurveDrag || blendCurveDrag.pointerId !== event.pointerId) return;
        event.preventDefault();
        applyBlendCurveDrag(canvas, event, false);
    });

    const finishDrag = (event) => {
        if (!blendCurveDrag || blendCurveDrag.pointerId !== event.pointerId) return;
        event.preventDefault();
        applyBlendCurveDrag(canvas, event, true);
        if (canvas.releasePointerCapture && (!canvas.hasPointerCapture || canvas.hasPointerCapture(event.pointerId))) {
            canvas.releasePointerCapture(event.pointerId);
        }
        canvas.classList.remove('is-dragging');
        blendCurveDrag = null;
    };
    canvas.addEventListener('pointerup', finishDrag);
    canvas.addEventListener('pointercancel', finishDrag);
    window.addEventListener('pointerup', finishDrag);
    window.addEventListener('pointercancel', finishDrag);
}

function getBlendCurveBaseRegionId(state) {
    return state.page.activeRegionId;
}

function getBlendCurveTargetIds(state) {
    const ids = state.blend.managerMode ? (state.layout.regionIds || [state.page.activeRegionId]) : [state.page.activeRegionId];
    return ids.filter((id) => Number(id) > 0);
}

function getCanvasPointerPoint(canvas, event) {
    const rect = canvas.getBoundingClientRect();
    const scaleX = canvas.width / Math.max(1, rect.width);
    const scaleY = canvas.height / Math.max(1, rect.height);
    return {
        x: (event.clientX - rect.left) * scaleX,
        y: (event.clientY - rect.top) * scaleY
    };
}

function pointToBlendCurveValues(canvas, point) {
    const layout = getBlendCurveHandleLayout(canvas, {});
    return {
        x: clamp((point.x - layout.rect.x) / layout.rect.width, 0, 1),
        y: clamp(1 - ((point.y - layout.rect.y) / layout.rect.height), 0, 1)
    };
}

function buildBlendCurvePatch(state, side, handleId, values) {
    const regionId = getBlendCurveBaseRegionId(state);
    const sideState = state.blend.byRegionId[regionId]?.[side]
        || state.blend.byRegionId[state.page.activeRegionId]?.[side]
        || {};
    const stripStart = clamp(sideState.stripStart ?? 0, 0, 255);
    const stripEnd = clamp(sideState.stripEnd ?? 255, 0, 255);
    const patch = {};
    if (handleId === 'stripStart') {
        patch.stripStart = Math.round(clamp(values.x * 255, 0, Math.max(0, stripEnd - 1)));
    } else if (handleId === 'stripEnd') {
        patch.stripEnd = Math.round(clamp(values.x * 255, Math.min(255, stripStart + 1), 255));
    } else {
        patch.anchor = Number(getBlendCurveAnchorFromDisplayValue(sideState, values.y).toFixed(3));
    }
    return patch;
}

function syncBlendCurveDrag(ids, side, patch, reload) {
    if (!ids.length || !Object.keys(patch).length) return;
    blendCurveSyncChain = blendCurveSyncChain.catch(() => {}).then(() => Promise.all(ids.map(async (regionId) => {
        await setBlendCurveParams(regionId, side, patch);
        if (reload) await loadBlendRegion(regionId);
    })));
    void blendCurveSyncChain.then(() => renderFusionUi()).catch((error) => {
        console.warn('fusion blend curve sync failed:', error);
    });
}

function scheduleBlendCurveSync(ids, side, patch, immediate) {
    if (blendCurveSyncTimer) {
        clearTimeout(blendCurveSyncTimer);
        blendCurveSyncTimer = null;
    }
    if (immediate) {
        syncBlendCurveDrag(ids, side, patch, true);
        return;
    }
    blendCurveSyncTimer = setTimeout(() => {
        blendCurveSyncTimer = null;
        syncBlendCurveDrag(ids, side, patch, false);
    }, 80);
}

function applyBlendCurveDrag(canvas, event, immediate) {
    if (!blendCurveDrag) return;
    const point = getCanvasPointerPoint(canvas, event);
    const values = pointToBlendCurveValues(canvas, point);
    const state = getState();
    const patch = buildBlendCurvePatch(state, blendCurveDrag.side, blendCurveDrag.handleId, values);
    if (!Object.keys(patch).length) return;
    blendCurveDrag.ids.forEach((regionId) => {
        patchBlendSide(regionId, blendCurveDrag.side, patch, { undo: false });
    });
    renderFusionUi();
    scheduleBlendCurveSync(blendCurveDrag.ids, blendCurveDrag.side, patch, immediate);
}

function bindBrightnessDragControl(input, label, updateBrightness) {
    const host = input.closest('.brightness-drag-field') || input;
    if (!host || host.dataset.brightnessDragBound) return;
    host.dataset.brightnessDragBound = 'true';

    let pressTimer = null;
    let dragState = null;
    let pressPoint = null;
    const colorByLabel = {
        R: '239, 68, 68',
        G: '34, 197, 94',
        B: '59, 130, 246'
    };

    const clearPressTimer = () => {
        if (pressTimer) {
            clearTimeout(pressTimer);
            pressTimer = null;
        }
        pressPoint = null;
    };

    const renderInlineDrag = (value) => {
        const ratio = Math.max(0, Math.min(1, value / 255));
        host.style.setProperty('--brightness-color', colorByLabel[label] || '59, 130, 246');
        host.style.setProperty('--brightness-level', `${ratio * 100}%`);
    };
    host._renderBrightnessLevel = renderInlineDrag;

    const valueFromPointer = (event) => {
        const rect = host.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left) / Math.max(1, rect.width)));
        return Math.round(ratio * 255);
    };

    const startDrag = (event) => {
        const value = Math.max(0, Math.min(255, parseInt(input.value || 128, 10)));
        input.blur();
        dragState = { pointerId: event.pointerId };
        renderInlineDrag(value);
        host.classList.add('is-brightness-dragging');
        if (host.setPointerCapture) host.setPointerCapture(event.pointerId);
        event.preventDefault();
        updateDrag(event, false);
    };

    const updateDrag = (event, final = false) => {
        if (!dragState || dragState.pointerId !== event.pointerId) return;
        const value = valueFromPointer(event);
        renderInlineDrag(value);
        updateBrightness(value, { reload: final });
        event.preventDefault();
    };

    const finishDrag = (event) => {
        clearPressTimer();
        if (!dragState || dragState.pointerId !== event.pointerId) return;
        updateDrag(event, true);
        host.classList.remove('is-brightness-dragging');
        if (host.releasePointerCapture && (!host.hasPointerCapture || host.hasPointerCapture(event.pointerId))) {
            host.releasePointerCapture(event.pointerId);
        }
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
    renderInlineDrag(Math.max(0, Math.min(255, parseInt(input.value || 128, 10))));
}

function bindTabButtons() {
    document.querySelectorAll('[data-fusion-tab]').forEach((button) => {
        if (button.dataset.fusionTabBound) return;
        button.dataset.fusionTabBound = 'true';
        button.addEventListener('click', async () => {
            const target = button.getAttribute('data-fusion-tab');
            if (target === 'fusion-tab-geometry-fusion') {
                if (getState().page.activeTab === TABS.COLOR) await switchFusionTab(TABS.GEOMETRY);
            }
            if (target === 'fusion-tab-color') {
                await switchFusionTab(TABS.COLOR);
            }
            renderFusionUi();
        });
    });
}

function bindHeaderControls() {
    const overviewButton = document.getElementById('fusionProjectorOverviewButton');
    if (overviewButton && !overviewButton.dataset.bound) {
        overviewButton.dataset.bound = 'true';
        overviewButton.addEventListener('click', () => {
            setFocusMode(FOCUS_MODES.LAYOUT);
            renderFusionUi();
        });
    }

    const presetButton = document.getElementById('fusionPresetButton');
    const presetMenu = document.getElementById('fusionPresetMenu');
    const menuContainer = presetMenu ? presetMenu.querySelector('.fusion-preset__items-container') : null;

    if (presetButton && presetMenu && menuContainer) {
        const renderRegionItems = () => {
            const state = getState();
            menuContainer.innerHTML = state.layout.regionIds.map((regionId) => `
                <button type="button" class="btn secondary fusion-region-item${regionId === state.page.activeRegionId ? ' active' : ''}" data-region-id="${regionId}">
                    投影 ${regionId}${regionId === state.page.activeRegionId ? ' · 当前' : ''}
                </button>
            `).join('');

            menuContainer.querySelectorAll('[data-region-id]').forEach((button) => {
                button.addEventListener('click', () => {
                    setActiveRegion(Number(button.getAttribute('data-region-id')));
                    renderFusionUi();
                    if (presetMenu) presetMenu.setAttribute('aria-hidden', 'true');
                });
            });
        };

        if (presetButton && !presetButton.dataset.bound) {
            presetButton.dataset.bound = 'true';
            presetButton.addEventListener('click', () => {
                renderRegionItems();
                const hidden = presetMenu.getAttribute('aria-hidden') !== 'false';
                presetMenu.setAttribute('aria-hidden', hidden ? 'false' : 'true');
            });
        }

        if (presetMenu && !presetMenu.dataset.bound) {
            presetMenu.dataset.bound = 'true';
            document.addEventListener('click', (event) => {
                if (!presetMenu.contains(event.target) && !presetButton.contains(event.target)) {
                    presetMenu.setAttribute('aria-hidden', 'true');
                }
            });
        }
    }
}

function bindBlendControls() {
    const notifyBlendRequired = () => {
        showNotification('请先打开融合带，再操作融合参数', 'warning');
    };
    let masterToggleBusy = false;
    let autoEdgesToggleBusy = false;
    let sideButtonBusy = false;

    const isLocalBlendSideOn = (regionId, side) => {
        const sideState = getState().blend.byRegionId[regionId]?.[side];
        return !!sideState?.enabled;
    };

    const masterToggle = document.getElementById('fusionBlendMasterToggle');
    if (masterToggle && !masterToggle.dataset.bound) {
        masterToggle.dataset.bound = 'true';
        masterToggle.addEventListener('change', async () => {
            if (masterToggleBusy) {
                renderFusionUi();
                return;
            }
            masterToggleBusy = true;
            try {
                const response = await setBlendMaster(!!masterToggle.checked);
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
            } catch (error) {
                console.warn('fusion blend display toggle failed:', error);
                showNotification('融合带显示切换失败', 'error');
            } finally {
                masterToggleBusy = false;
                renderFusionUi();
            }
        });
    }

    const autoEdgesToggle = document.getElementById('fusionBlendAutoEdgesToggle');
    if (autoEdgesToggle && !autoEdgesToggle.dataset.bound) {
        autoEdgesToggle.dataset.bound = 'true';
        autoEdgesToggle.addEventListener('change', async () => {
            if (autoEdgesToggleBusy) {
                renderFusionUi();
                return;
            }
            autoEdgesToggleBusy = true;
            const nextEnabled = !!autoEdgesToggle.checked;
            setBlendAutoEdges(nextEnabled);
            try {
                renderFusionUi();
                const response = await syncBlendAutoEdges(nextEnabled);
                const actualEnabled = typeof response?.blend_auto_edges === 'boolean'
                    ? response.blend_auto_edges
                    : (typeof response?.enabled === 'boolean' ? response.enabled : nextEnabled);
                setBlendAutoEdges(actualEnabled);
                hydrateBlendRegionsFromResponse(response);
                showNotification(actualEnabled ? '融合边已切到自动' : '融合边已切到手动', 'success');
            } catch (error) {
                console.warn('fusion blend edge mode toggle failed:', error);
                showNotification('融合边自动/手动切换失败', 'error');
            } finally {
                autoEdgesToggleBusy = false;
                renderFusionUi();
            }
        });
    }

    document.querySelectorAll('.fusion-blending-side').forEach((button) => {
        if (button.dataset.bound) return;
        button.dataset.bound = 'true';
        button.addEventListener('click', async (event) => {
            event.preventDefault();
            const state = getState();
            if (!state.blend.masterEnabled) {
                notifyBlendRequired();
                return;
            }
            const side = button.getAttribute('data-side');
            const regionId = state.page.activeRegionId;
            if (sideButtonBusy) return;
            sideButtonBusy = true;
            setActiveBlendSide(side);
            if (state.blend.autoEdges) {
                sideButtonBusy = false;
                renderFusionUi();
                showNotification('自动模式按输入相邻关系开启融合边，切手动后可逐边开关', 'info');
                return;
            }
            setBlendSideEnabled(regionId, side, !isLocalBlendSideOn(regionId, side));
            try {
                renderFusionUi();
                await syncBlendRegion(regionId);
                await loadBlendRegion(regionId);
            } catch (error) {
                console.warn('fusion side toggle sync failed:', error);
                showNotification('融合边开关同步失败', 'error');
                await loadBlendRegion(regionId).catch(() => {});
            } finally {
                sideButtonBusy = false;
                renderFusionUi();
            }
        });
    });

    [['fusionGammaInput', 'gamma'], ['fusionSlopeInput', 'slope'], ['fusionGainInput', 'anchor']].forEach(([id, key]) => {
        const input = document.getElementById(id);
        if (!input || input.dataset.bound) return;
        input.dataset.bound = 'true';
        let blendInputThrottle = null;
        input.addEventListener('input', (event) => {
            event.stopPropagation();
            const state = getState();
            const ids = state.blend.managerMode ? (state.layout.regionIds || [state.page.activeRegionId]) : [state.page.activeRegionId];
            const nextValue = parseFloat(input.value) || 0;
            ids.forEach((regionId) => {
                const patch = { [key]: nextValue };
                patchBlendSide(regionId, state.blend.activeSide, patch);
            });
            renderFusionUi();
            if (blendInputThrottle) clearTimeout(blendInputThrottle);
            blendInputThrottle = setTimeout(() => {
                blendInputThrottle = null;
                Promise.all(ids.map(async (regionId) => {
                    await setBlendCurveParams(regionId, state.blend.activeSide, { [key]: nextValue });
                    await loadBlendRegion(regionId);
                })).then(() => renderFusionUi());
            }, 100);
        });
    });

    [
        ['fusionGammaDecrease', 'fusionGammaIncrease', 'fusionGammaReset', 'fusionGammaInput', 'gamma', 1.8],
        ['fusionSlopeDecrease', 'fusionSlopeIncrease', 'fusionSlopeReset', 'fusionSlopeInput', 'slope', 1.0],
        ['fusionGainDecrease', 'fusionGainIncrease', 'fusionGainReset', 'fusionGainInput', 'anchor', 0.5]
    ].forEach(([decreaseId, increaseId, resetId, inputId, key, defaultValue]) => {
        const input = document.getElementById(inputId);
        let blendButtonThrottle = null;
        const bindButton = (buttonId, mode) => {
            const button = document.getElementById(buttonId);
            if (!button || button.dataset.bound) return;
            button.dataset.bound = 'true';
            button.addEventListener('click', (event) => {
                event.preventDefault();
                event.stopPropagation();
                if (blendButtonThrottle) return;
                blendButtonThrottle = setTimeout(() => { blendButtonThrottle = null; }, 150);
                const min = Number(input?.min ?? 0);
                const max = Number(input?.max ?? 1);
                const step = Number(input?.step ?? 0.1);
                const current = Number(input?.value ?? defaultValue);
                let next = defaultValue;
                if (mode === 'decrease') next = current - step;
                if (mode === 'increase') next = current + step;
                next = Math.max(min, Math.min(max, next));
                const decimals = String(step).includes('.') ? String(step).split('.')[1].length : 0;
                next = Number(next.toFixed(decimals));
                if (input) input.value = String(next);
                const state = getState();
                const ids = state.blend.managerMode ? (state.layout.regionIds || [state.page.activeRegionId]) : [state.page.activeRegionId];
                ids.forEach((regionId) => {
                    const patch = { [key]: next };
                    patchBlendSide(regionId, state.blend.activeSide, patch);
                });
                renderFusionUi();
                void Promise.all(ids.map(async (regionId) => {
                    await setBlendCurveParams(regionId, state.blend.activeSide, { [key]: next });
                    await loadBlendRegion(regionId);
                })).then(() => renderFusionUi());
            });
        };
        bindButton(decreaseId, 'decrease');
        bindButton(increaseId, 'increase');
        bindButton(resetId, 'reset');
    });

    const brightConfig = [
        { inputId: 'fusionBrightRInput', decreaseId: 'fusionBrightRDecrease', increaseId: 'fusionBrightRIncrease', resetId: 'fusionBrightRReset', colorId: 0, label: 'R' },
        { inputId: 'fusionBrightGInput', decreaseId: 'fusionBrightGDecrease', increaseId: 'fusionBrightGIncrease', resetId: 'fusionBrightGReset', colorId: 1, label: 'G' },
        { inputId: 'fusionBrightBInput', decreaseId: 'fusionBrightBDecrease', increaseId: 'fusionBrightBIncrease', resetId: 'fusionBrightBReset', colorId: 2, label: 'B' }
    ];
    brightConfig.forEach(({ inputId, decreaseId, increaseId, resetId, colorId, label }) => {
        const input = document.getElementById(inputId);
        if (!input) return;

        let brightInputThrottle = null;
        const getTarget = () => {
            const state = getState();
            const ids = state.blend.managerMode ? (state.layout.regionIds || [state.page.activeRegionId]) : [state.page.activeRegionId];
            return {
                ids: ids.filter((id) => Number(id) > 0),
                side: state.blend.activeSide
            };
        };
        const clampBright = (value) => Math.max(0, Math.min(255, Math.round(Number(value) || 0)));
        const patchBrightnessLocal = (ids, side, value) => {
            ids.forEach((regionId) => {
                const region = ensureBlendRegion(regionId);
                if (!region[side]) return;
                const nextBright = Array.isArray(region[side].bright)
                    ? region[side].bright.slice(0, 3)
                    : [128, 128, 128];
                nextBright[colorId] = value;
                patchBlendSide(regionId, side, { bright: nextBright }, { undo: false });
            });
        };
        const syncBrightness = (ids, side, value, reload) => {
            if (brightInputThrottle) {
                clearTimeout(brightInputThrottle);
                brightInputThrottle = null;
            }
            const run = () => {
                void Promise.all(ids.map(async (regionId) => {
                    await setMergeGapBrightness(regionId, side, colorId, value);
                    if (reload) await loadBlendRegion(regionId);
                })).then(() => renderFusionUi()).catch((error) => {
                    console.warn('fusion brightness sync failed:', error);
                });
            };
            if (reload) run();
            else brightInputThrottle = setTimeout(run, 80);
        };
        const updateBrightness = (value, options = {}) => {
            const next = clampBright(value);
            input.value = String(next);
            input.textContent = String(next);
            const host = input.closest('.brightness-drag-field');
            if (host && typeof host._renderBrightnessLevel === 'function') {
                host._renderBrightnessLevel(next);
            }
            const { ids, side } = getTarget();
            patchBrightnessLocal(ids, side, next);
            renderFusionUi();
            syncBrightness(ids, side, next, !!options.reload);
        };

        let brightButtonThrottle = null;
        [
            { buttonId: decreaseId, delta: -1 },
            { buttonId: increaseId, delta: 1 },
            { buttonId: resetId, reset: true }
        ].forEach(({ buttonId, delta = 0, reset = false }) => {
            const button = document.getElementById(buttonId);
            if (!button || button.dataset.bound) return;
            button.dataset.bound = 'true';
            button.addEventListener('click', (event) => {
                event.preventDefault();
                if (brightButtonThrottle) return;
                brightButtonThrottle = setTimeout(() => { brightButtonThrottle = null; }, 150);
                const current = parseInt(input?.value ?? 128, 10);
                updateBrightness(reset ? 128 : current + delta, { reload: true });
            });
        });

        bindBrightnessDragControl(input, label, updateBrightness);
    });
}

function bindColorControls() {
    [['fusionColorBrightness', 'brightness'], ['fusionColorContrast', 'contrast'], ['fusionColorSaturation', 'saturation']].forEach(([id, key]) => {
        const input = document.getElementById(id);
        if (!input || input.dataset.bound) return;
        input.dataset.bound = 'true';
        input.addEventListener('change', async () => {
            const state = getState();
            patchColor(state.page.activeRegionId, { [key]: parseFloat(input.value) || 1 });
            renderFusionUi();
            await syncColorRegion(state.page.activeRegionId);
        });
    });

    const resetButton = document.querySelector('#fusion-tab-color .btn.secondary.w-100.mt-10');
    if (resetButton && !resetButton.dataset.bound) {
        resetButton.dataset.bound = 'true';
        resetButton.addEventListener('click', async () => {
            const state = getState();
            resetColor(state.page.activeRegionId);
            renderFusionUi();
            await syncColorRegion(state.page.activeRegionId);
        });
    }
}

export function bindUi() {
    const canvas = document.getElementById('fusionCanvas');
    bindKeyboardFocus();
    bindLongPressMenuGuard();
    bindCanvas(canvas);
    bindTabButtons();
    bindHeaderControls();
    bindBlendControls();
    bindColorControls();

    const blendCanvas = ensureBlendCanvas();
    if (blendCanvas) {
        bindBlendCanvas(blendCanvas);
        drawBlendScene(blendCanvas);
    }

    if (!document.body.dataset.fusionKeyboardBound) {
        document.body.dataset.fusionKeyboardBound = 'true';
        const handleFusionKeydown = (event) => {
            if (event.__fusionKeyboardHandled) return;
            if (handleKeyboard(getState(), event)) {
                event.__fusionKeyboardHandled = true;
                event.stopImmediatePropagation();
            }
        };
        window.addEventListener('keydown', handleFusionKeydown, true);
        document.addEventListener('keydown', handleFusionKeydown, true);
    }
}

