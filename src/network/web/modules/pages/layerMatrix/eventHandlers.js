// 事件处理模块
import { parseSliceCoordinate, parsePosition, parseSize } from './utils.js';
import { collectSliceKeys, getSliceData, hydrateSliceFields, isSliceVisible, normalizeSliceVisualFields } from './sliceModel.js';
import {
    buildReferenceBounds,
    createLayerItem,
    createSliceItem,
    getItemBounds,
    getItemKey,
    selectionHasLayer,
    setItemBounds,
    translateBounds
} from './geometry.js';
import { getSnapThreshold, snapBounds } from './snapEngine.js';
// 注意：isPointInLayer 需要从外部传入，避免循环依赖

function getSliceRotation(sliceData, layer) {
    const normalized = normalizeSliceVisualFields({ ...(sliceData || {}) });
    return Number(normalized.rotation ?? normalized.rotate ?? layer?.rotation ?? 0);
}

function isAdditiveSelectionEvent(e) {
    return !!(e && (e.ctrlKey || e.metaKey || e.shiftKey));
}

function createInteractionSession(type, items, layers, canvasConfig, primaryItem = null) {
    const cleanItems = (items || []).filter(Boolean);
    const initialBoundsByKey = {};
    cleanItems.forEach(item => {
        const bounds = getItemBounds(item, layers, canvasConfig);
        if (bounds) initialBoundsByKey[getItemKey(item)] = bounds;
    });
    const primary = primaryItem || cleanItems[0] || null;
    return {
        type,
        items: cleanItems.filter(item => initialBoundsByKey[getItemKey(item)]),
        primaryItem: primary,
        initialBoundsByKey
    };
}

function getPrimaryStartBounds(session) {
    if (!session?.primaryItem) return null;
    return session.initialBoundsByKey[getItemKey(session.primaryItem)] || null;
}

function updateSlicePositionInputs(bounds) {
    const xInput = document.getElementById('slice-x');
    const yInput = document.getElementById('slice-y');
    if (xInput) xInput.value = bounds.x;
    if (yInput) yInput.value = bounds.y;
}

/**
 * 检测点击了哪个控制点（支持图层和切片）
 */
export function getClickedHandle(
    mouseX, mouseY, selectedLayer, selectedSlice, layers, canvasConfig,
    systemToCanvas, getHandlePositions, validateConfig, canvas, dpr
) {
    if (!selectedLayer) return null;

    let x = 0, y = 0, width = 0, height = 0;
    let rotation = 0;

    if (selectedSlice) {
        const layer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
        const sliceData = getSliceData(layer, selectedSlice.sliceKey);
        if (!sliceData) return null;

        const fallbackSize = (!validateConfig(canvasConfig)) ? null : {
            width: canvasConfig.width,
            height: canvasConfig.height
        };
        const coord = parseSliceCoordinate(sliceData, fallbackSize);
        if (!coord) return null;

        x = coord.x;
        y = coord.y;
        width = coord.width;
        height = coord.height;
        rotation = getSliceRotation(sliceData, layer);
    } else {
        const layer = layers.find(l => Number(l.id) === Number(selectedLayer));
        if (!layer) return null;

        if (!layer.position || typeof layer.position === 'string') {
            layer.position = parsePosition(layer.position);
        }
        if (!layer.size || typeof layer.size === 'string') {
            layer.size = parseSize(layer.size);
            if (!layer.size.width || !layer.size.height) {
                if (!validateConfig(canvasConfig)) return null;
                layer.size = { width: canvasConfig.width, height: canvasConfig.height };
            }
        }

        x = layer.position?.x ?? 0;
        y = layer.position?.y ?? 0;
        width = layer.size?.width;
        height = layer.size?.height;
        if (!width || width <= 0) {
            if (!validateConfig(canvasConfig)) return null;
            width = canvasConfig.width;
        }
        if (!height || height <= 0) {
            if (!validateConfig(canvasConfig)) return null;
            height = canvasConfig.height;
        }
        rotation = Number(layer.rotation || 0);
    }

    const handles = getHandlePositions(x, y, width, height, rotation);

    for (const key in handles) {
        const handle = handles[key];
        const canvasPos = systemToCanvas(handle.x, handle.y, canvas, dpr, canvasConfig);
        
        const dx = mouseX - canvasPos.x;
        const dy = mouseY - canvasPos.y;
        
        // 判定范围：12 像素
        if (Math.sqrt(dx * dx + dy * dy) <= 12) {
            return key; // 返回 'nw', 'n' 等名称
        }
    }

    return null;
}

/**
 * 检测点击了哪个切片
 */
export function getClickedSlice(mouseX, mouseY, selectedLayer, layers, canvasConfig, canvasToSystem, parseSliceCoordinate, validateConfig, canvas, dpr) {
    // 按优先级反向遍历图层
    const sortedLayers = selectedLayer !== null && selectedLayer !== undefined
        ? layers.filter(layer => Number(layer.id) === Number(selectedLayer))
        : [...layers].sort((a, b) => b.priority - a.priority);
    const systemPos = canvasToSystem(mouseX, mouseY, canvas, dpr, canvasConfig);

    for (const layer of sortedLayers) {
        // 确保图层尺寸已解析，因为切片坐标计算可能依赖图层尺寸
        if (!layer.size || typeof layer.size === 'string') {
            layer.size = parseSize(layer.size);
        }

        hydrateSliceFields(layer);
        const sliceEntries = collectSliceKeys(layer).map(key => [key, layer[key]]).sort((a, b) => {
            const pA = a[1]?.priority || 0;
            const pB = b[1]?.priority || 0;
            return pB - pA;
        });

        for (const [sliceKey, sliceData] of sliceEntries) {
            if (!isSliceVisible(sliceData)) continue;

            const fallbackSize = (!validateConfig(canvasConfig) || !layer.size) ? null : {
                width: layer.size.width || canvasConfig.width,
                height: layer.size.height || canvasConfig.height
            };
            const coord = parseSliceCoordinate(sliceData, fallbackSize);
            if (!coord) continue;

            const { x, y, width, height } = coord;
            if (width === 0 || height === 0) continue;

            if (systemPos.x >= x && systemPos.x <= x + width &&
                systemPos.y >= y && systemPos.y <= y + height) {
                return { layerId: layer.id, sliceKey: sliceKey };
            }
        }
    }

    return null;
}

/**
 * 检测点击了哪个图层
 */
export function getClickedLayer(
    mouseX, mouseY, selectedLayer, layers, canvasConfig, systemToCanvas, canvas, dpr, isPointInLayer, options = {}
) {
    const ensureParsed = (layer) => {
        if (!layer.position || typeof layer.position === 'string') {
            layer.position = parsePosition(layer.position);
        }
        if (!layer.size || typeof layer.size === 'string') {
            layer.size = parseSize(layer.size);
        }
    };

    // 若点击点落在当前已选中图层的范围内，优先保持选中图层不变，
    // 避免侧栏选了图层2、但因图层1优先级更高覆盖在上方，导致点击画布被切换并拖到图层1。
    // 注意：点不在当前图层内时要继续按层级命中其他图层，跨图层选择依赖这个路径。
    if (!options.ignoreSelectedPreference && selectedLayer !== null && selectedLayer !== undefined) {
        const curLayer = layers.find(l => Number(l.id) === Number(selectedLayer));
        if (curLayer && curLayer.visible !== false) {
            ensureParsed(curLayer);
            if (isPointInLayer(curLayer, mouseX, mouseY, canvasConfig, systemToCanvas, canvas, dpr)) {
                return curLayer.id;
            }
        }
    }

    // 否则按优先级降序遍历，返回最上层命中的图层
    const sortedLayers = [...layers].sort((a, b) => b.priority - a.priority);
    for (const layer of sortedLayers) {
        if (layer.visible === false) continue;
        ensureParsed(layer);
        if (isPointInLayer(layer, mouseX, mouseY, canvasConfig, systemToCanvas, canvas, dpr)) {
            return layer.id;
        }
    }

    return null;
}

/**
 * 画布鼠标按下事件
 */
export function onCanvasMouseDown(
    e, canvas, dpr, cssToCanvasCoord, getClickedHandle, getClickedSlice, getClickedLayer,
    resizeHandle, dragStartX, dragStartY,
    dragLayerStartX, dragLayerStartY, dragLayerStartW, dragLayerStartH,
    selectedLayer, selectedSlice, layers, canvasConfig,
    validateConfig, parseSliceCoordinate, canvasToSystem,
    selectSlice, selectLayer,
    selectedItems = [], toggleLayerSelectionForMultiSelect = null
) {
    e.preventDefault();
    e.stopPropagation();

    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    const canvasCoord = cssToCanvasCoord(cssX, cssY);
    const mouseX = canvasCoord.x;
    const mouseY = canvasCoord.y;
    const additiveSelection = isAdditiveSelectionEvent(e);
    let isDragging = false;
    let isResizing = false;

    // 首先检查是否点击了控制点
    const handle = getClickedHandle(mouseX, mouseY);
    if (handle) {
        isResizing = true;
        resizeHandle = handle;
        dragStartX = mouseX;
        dragStartY = mouseY;

        if (selectedSlice) {
            const layer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
            const sliceData = getSliceData(layer, selectedSlice.sliceKey);
            if (sliceData) {
                const fallbackSize = (!validateConfig(canvasConfig)) ? null : {
                    width: canvasConfig.width,
                    height: canvasConfig.height
                };
                const coord = parseSliceCoordinate(sliceData, fallbackSize);
                if (coord) {
                    dragLayerStartX = coord.x;
                    dragLayerStartY = coord.y;
                    dragLayerStartW = coord.width || (validateConfig(canvasConfig) ? canvasConfig.width : 1920);
                    dragLayerStartH = coord.height || (validateConfig(canvasConfig) ? canvasConfig.height : 1080);
                } else {
                    dragLayerStartX = 0;
                    dragLayerStartY = 0;
                    dragLayerStartW = validateConfig(canvasConfig) ? canvasConfig.width : 1920;
                    dragLayerStartH = validateConfig(canvasConfig) ? canvasConfig.height : 1080;
                }
            }
        } else {
            const layer = layers.find(l => Number(l.id) === Number(selectedLayer));
            if (layer) {
                // [Fix] 确保位置和尺寸已解析，避免字符串导致初始值为 0
                if (!layer.position || typeof layer.position === 'string') {
                    layer.position = parsePosition(layer.position);
                }
                if (!layer.size || typeof layer.size === 'string') {
                    layer.size = parseSize(layer.size);
                }

                dragLayerStartX = layer.position?.x ?? 0;
                dragLayerStartY = layer.position?.y ?? 0;
                dragLayerStartW = layer.size?.width;
                dragLayerStartH = layer.size?.height;
                if (!dragLayerStartW || dragLayerStartW === 0) {
                    dragLayerStartW = validateConfig(canvasConfig) ? canvasConfig.width : 1920;
                }
                if (!dragLayerStartH || dragLayerStartH === 0) {
                    dragLayerStartH = validateConfig(canvasConfig) ? canvasConfig.height : 1080;
                }
            }
        }
        return {
            isResizing, resizeHandle, dragStartX, dragStartY,
            dragLayerStartX, dragLayerStartY, dragLayerStartW, dragLayerStartH,
            snapGuides: []
        };
    }

    // 如果选中了切片，且点击位置在选中的切片上，直接拖拽切片
    if (selectedSlice) {
        const layer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
        const sliceData = getSliceData(layer, selectedSlice.sliceKey);
        if (isSliceVisible(sliceData)) {
            const systemPos = canvasToSystem(mouseX, mouseY, canvas, dpr, canvasConfig);
            const fallbackSize = (!validateConfig(canvasConfig) || !layer.size) ? null : {
                width: layer.size.width || canvasConfig.width,
                height: layer.size.height || canvasConfig.height
            };
            const coord = parseSliceCoordinate(sliceData, fallbackSize);
            if (coord) {
                const { x, y, width, height } = coord;
                if (systemPos.x >= x && systemPos.x <= x + width &&
                    systemPos.y >= y && systemPos.y <= y + height) {
                    isDragging = true;
                    dragStartX = mouseX;
                    dragStartY = mouseY;
                    dragLayerStartX = x;
                    dragLayerStartY = y;
                    const item = createSliceItem(selectedSlice.layerId, selectedSlice.sliceKey);
                    const interactionSession = createInteractionSession('move', [item], layers, canvasConfig, item);
                    return { isDragging, dragStartX, dragStartY, dragLayerStartX, dragLayerStartY, interactionSession, snapGuides: [] };
                }
            }
        }
    }

    // 检测点击了什么，点击什么就选中什么
    const clickedSlice = getClickedSlice(mouseX, mouseY);
    if (clickedSlice && !additiveSelection) {
        selectSlice(clickedSlice.layerId, clickedSlice.sliceKey);
        isDragging = true;
        dragStartX = mouseX;
        dragStartY = mouseY;

        const layer = layers.find(l => Number(l.id) === Number(clickedSlice.layerId));
        const sliceData = getSliceData(layer, clickedSlice.sliceKey);
        if (sliceData) {
            const coord = parseSliceCoordinate(sliceData);
            dragLayerStartX = coord?.x ?? 0;
            dragLayerStartY = coord?.y ?? 0;
        }
        const item = createSliceItem(clickedSlice.layerId, clickedSlice.sliceKey);
        const interactionSession = createInteractionSession('move', [item], layers, canvasConfig, item);
        return { isDragging, dragStartX, dragStartY, dragLayerStartX, dragLayerStartY, interactionSession, snapGuides: [] };
    }

    // 检测图层
    const clickedLayerId = getClickedLayer(mouseX, mouseY, additiveSelection ? { ignoreSelectedPreference: true } : {});
    if (clickedLayerId !== null) {
        const layer = layers.find(l => Number(l.id) === Number(clickedLayerId));
        
        // 确保位置已解析
        if (layer && (typeof layer.position === 'string' || !layer.position)) {
            layer.position = parsePosition(layer.position);
        }

        const startX = layer?.position?.x ?? 0;
        const startY = layer?.position?.y ?? 0;

        if (additiveSelection && toggleLayerSelectionForMultiSelect) {
            toggleLayerSelectionForMultiSelect(clickedLayerId);
            return { snapGuides: [] };
        }

        const clickedItem = createLayerItem(clickedLayerId);
        const selectionItems = (selectedSlice == null && selectionHasLayer(selectedItems, clickedLayerId))
            ? selectedItems.filter(item => item?.type === 'layer')
            : [clickedItem];
        const primaryItem = selectionItems.find(item => Number(item.layerId) === Number(clickedLayerId)) || clickedItem;
        const interactionSession = createInteractionSession('move', selectionItems, layers, canvasConfig, primaryItem);

        const keepMultiSelection = selectionItems.length > 1;
        if (!keepMultiSelection) {
            selectLayer(clickedLayerId);
        }

        return { 
            isDragging: true, 
            dragStartX: mouseX, 
            dragStartY: mouseY, 
            dragLayerStartX: startX, 
            dragLayerStartY: startY,
            selectedLayer: clickedLayerId,
            selectedSlice: null,
            selectedItems: selectionItems,
            interactionSession,
            snapGuides: []
        };
    }

    return { snapGuides: [] };
}

/**
 * 画布鼠标移动事件
 */
export function onCanvasMouseMove(
    e, canvas, dpr, cssToCanvasCoord, updateCursor, isDragging, isResizing,
    selectedLayer, selectedSlice, layers, canvasConfig,
    canvasToSystem, parseSliceCoordinate, validateConfig, parsePosition, parseSize,
    drawCanvas, updatePropertyInputs, resizeHandle,
    dragStartX, dragStartY, dragLayerStartX, dragLayerStartY, dragLayerStartW, dragLayerStartH,
    getClickedHandle, getClickedLayer,
    syncLayerRectRealTime,
    interactionSession = null, setInteractionState = null
) {
    if (!isDragging && !isResizing) {
        if (canvas) {
            const rect = canvas.getBoundingClientRect();
            const cssX = e.clientX - rect.left;
            const cssY = e.clientY - rect.top;
            if (cssX >= 0 && cssX <= rect.width && cssY >= 0 && cssY <= rect.height) {
                const canvasCoord = cssToCanvasCoord(cssX, cssY);
                updateCursor(
                    canvasCoord.x, canvasCoord.y, canvas, selectedLayer, layers, canvasConfig,
                    getClickedHandle, getClickedLayer, selectedSlice, dpr
                );
            } else {
                canvas.style.setProperty('cursor', 'default', 'important');
            }
        }
        return;
    }

    if (!canvas) return;

    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    const canvasCoord = cssToCanvasCoord(cssX, cssY);
    const mouseX = canvasCoord.x;
    const mouseY = canvasCoord.y;

    const deltaX = mouseX - dragStartX;
    const deltaY = mouseY - dragStartY;
    const systemDelta = canvasToSystem(deltaX, deltaY, canvas, dpr, canvasConfig);

    // 基础辅助函数：将全局位移映射到局部坐标系（用于缩放）
    const getLocalDelta = (rotationDeg) => {
        const rad = rotationDeg * Math.PI / 180;
        const cos = Math.cos(rad);
        const sin = Math.sin(rad);
        // 全局 -> 局部 (旋转逆变换)
        return {
            x: systemDelta.x * cos + systemDelta.y * sin,
            y: -systemDelta.x * sin + systemDelta.y * cos
        };
    };

    if (isDragging) {
        if (interactionSession?.type === 'move' && interactionSession.items?.length > 0) {
            const primaryStart = getPrimaryStartBounds(interactionSession);
            if (!primaryStart) return;

            const rawPrimaryBounds = translateBounds(primaryStart, systemDelta.x, systemDelta.y);
            const referenceBounds = buildReferenceBounds(layers, interactionSession.items, canvasConfig);
            const snap = snapBounds(rawPrimaryBounds, referenceBounds, canvasConfig, {
                threshold: getSnapThreshold(canvas, dpr, canvasConfig, 8),
                disabled: e.altKey
            });
            const effectiveDelta = {
                x: systemDelta.x + snap.delta.x,
                y: systemDelta.y + snap.delta.y
            };

            interactionSession.items.forEach(item => {
                const startBounds = interactionSession.initialBoundsByKey[getItemKey(item)];
                if (!startBounds) return;
                const nextBounds = translateBounds(startBounds, effectiveDelta.x, effectiveDelta.y);
                setItemBounds(item, layers, nextBounds, canvasConfig);
            });

            if (typeof setInteractionState === 'function') {
                setInteractionState({ snapGuides: snap.guides });
            }

            const primaryItem = interactionSession.primaryItem;
            const primaryBounds = primaryItem ? getItemBounds(primaryItem, layers, canvasConfig) : null;
            if (primaryBounds && primaryItem?.type === 'slice') {
                updateSlicePositionInputs(primaryBounds);
            } else if (primaryBounds && primaryItem?.type === 'layer') {
                const primaryLayer = layers.find(l => Number(l.id) === Number(primaryItem.layerId));
                if (primaryLayer) updatePropertyInputs(primaryLayer);
            }

            drawCanvas();

            if (syncLayerRectRealTime && interactionSession.items.length === 1 && primaryItem?.type === 'layer' && primaryBounds) {
                syncLayerRectRealTime(primaryItem.layerId, primaryBounds.x, primaryBounds.y, primaryBounds.width, primaryBounds.height);
            }
            return;
        }

        if (selectedSlice) {
            const layer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
            const sliceData = getSliceData(layer, selectedSlice.sliceKey);
            if (sliceData) {
                const newX = Math.round(dragLayerStartX + systemDelta.x);
                const newY = Math.round(dragLayerStartY + systemDelta.y);
                const coord = parseSliceCoordinate(sliceData);
                const width = coord?.width || 100;
                const height = coord?.height || 100;

                sliceData.position = `${newX} ${newY}`;
                sliceData.coordinate = `${newX} ${newY} ${width} ${height}`;
                sliceData.range = sliceData.coordinate;
                sliceData.x = newX;
                sliceData.y = newY;
                sliceData.width = width;
                sliceData.height = height;

                drawCanvas();
                const xInput = document.getElementById('slice-x');
                const yInput = document.getElementById('slice-y');
                if (xInput) xInput.value = newX;
                if (yInput) yInput.value = newY;
            }
        } else {
            const layer = layers.find(l => Number(l.id) === Number(selectedLayer));
            if (layer) {
                if (!layer.position || typeof layer.position === 'string') {
                    layer.position = parsePosition(layer.position);
                }
                const newX = Math.round(dragLayerStartX + systemDelta.x);
                const newY = Math.round(dragLayerStartY + systemDelta.y);
                layer.position.x = newX;
                layer.position.y = newY;
                drawCanvas();
                updatePropertyInputs(layer);
                
                // 实时同步到大屏 (50ms 节流已在 service 层实现)
                if (syncLayerRectRealTime) {
                    const w = layer.size?.width || canvasConfig?.width || 1920;
                    const h = layer.size?.height || canvasConfig?.height || 1080;
                    syncLayerRectRealTime(layer.id, newX, newY, w, h);
                }
            }
        }
    } else if (isResizing) {
        let target;
        let rotationDeg = 0;
        let parentLayer = null;
        if (selectedSlice) {
            parentLayer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
            target = getSliceData(parentLayer, selectedSlice.sliceKey);
            rotationDeg = getSliceRotation(target, parentLayer);
        } else {
            target = layers.find(l => Number(l.id) === Number(selectedLayer));
            rotationDeg = Number(target?.rotation || 0);
        }

        if (target) {
            const minSize = 20;
            const localDelta = getLocalDelta(rotationDeg);
            const rad = rotationDeg * Math.PI / 180;

            let newWidth = dragLayerStartW;
            let newHeight = dragLayerStartH;
            let localShiftX = 0;
            let localShiftY = 0;

            switch (resizeHandle) {
                case 'se':
                    newWidth = Math.max(minSize, dragLayerStartW + localDelta.x);
                    newHeight = Math.max(minSize, dragLayerStartH + localDelta.y);
                    localShiftX = (newWidth - dragLayerStartW) / 2;
                    localShiftY = (newHeight - dragLayerStartH) / 2;
                    break;
                case 'e':
                    newWidth = Math.max(minSize, dragLayerStartW + localDelta.x);
                    localShiftX = (newWidth - dragLayerStartW) / 2;
                    break;
                case 's':
                    newHeight = Math.max(minSize, dragLayerStartH + localDelta.y);
                    localShiftY = (newHeight - dragLayerStartH) / 2;
                    break;
                case 'nw':
                    newWidth = Math.max(minSize, dragLayerStartW - localDelta.x);
                    newHeight = Math.max(minSize, dragLayerStartH - localDelta.y);
                    localShiftX = -(newWidth - dragLayerStartW) / 2;
                    localShiftY = -(newHeight - dragLayerStartH) / 2;
                    break;
                case 'n':
                    newHeight = Math.max(minSize, dragLayerStartH - localDelta.y);
                    localShiftY = -(newHeight - dragLayerStartH) / 2;
                    break;
                case 'w':
                    newWidth = Math.max(minSize, dragLayerStartW - localDelta.x);
                    localShiftX = -(newWidth - dragLayerStartW) / 2;
                    break;
                case 'ne':
                    newWidth = Math.max(minSize, dragLayerStartW + localDelta.x);
                    newHeight = Math.max(minSize, dragLayerStartH - localDelta.y);
                    localShiftX = (newWidth - dragLayerStartW) / 2;
                    localShiftY = -(newHeight - dragLayerStartH) / 2;
                    break;
                case 'sw':
                    newWidth = Math.max(minSize, dragLayerStartW - localDelta.x);
                    newHeight = Math.max(minSize, dragLayerStartH + localDelta.y);
                    localShiftX = -(newWidth - dragLayerStartW) / 2;
                    localShiftY = (newHeight - dragLayerStartH) / 2;
                    break;
            }

            const cos = Math.cos(rad);
            const sin = Math.sin(rad);
            const globalShiftX = localShiftX * cos - localShiftY * sin;
            const globalShiftY = localShiftX * sin + localShiftY * cos;

            const oldCenterX = dragLayerStartX + dragLayerStartW / 2;
            const oldCenterY = dragLayerStartY + dragLayerStartH / 2;
            const newCenterX = oldCenterX + globalShiftX;
            const newCenterY = oldCenterY + globalShiftY;

            const newX = Math.round(newCenterX - newWidth / 2);
            const newY = Math.round(newCenterY - newHeight / 2);
            const intW = Math.round(newWidth);
            const intH = Math.round(newHeight);

            if (selectedSlice) {
                target.position = `${newX} ${newY}`;
                target.size = `${intW} ${intH}`;
                target.coordinate = `${newX} ${newY} ${intW} ${intH}`;
                target.range = target.coordinate;
                target.x = newX;
                target.y = newY;
                target.width = intW;
                target.height = intH;

                const xInput = document.getElementById('slice-x');
                const yInput = document.getElementById('slice-y');
                const wInput = document.getElementById('slice-width');
                const hInput = document.getElementById('slice-height');
                if (xInput) xInput.value = newX;
                if (yInput) yInput.value = newY;
                if (wInput) wInput.value = intW;
                if (hInput) hInput.value = intH;
            } else {
                if (!target.position || typeof target.position === 'string') {
                    target.position = parsePosition(target.position);
                }
                if (!target.size || typeof target.size === 'string') {
                    target.size = parseSize(target.size);
                }
                target.position.x = newX;
                target.position.y = newY;
                target.size.width = intW;
                target.size.height = intH;
                updatePropertyInputs(target);
                
                // 实时同步到大屏
                if (syncLayerRectRealTime) {
                    const finalW = intW || canvasConfig?.width || 1920;
                    const finalH = intH || canvasConfig?.height || 1080;
                    syncLayerRectRealTime(target.id, newX, newY, finalW, finalH);
                }
            }
            drawCanvas();
        }
    }
}

/**
 * 画布鼠标释放事件
 */
export async function onCanvasMouseUp(
    e, isDragging, isResizing, selectedLayer, selectedSlice, layers,
    applySliceProperties, syncLayerToServer, selectedItems = []
) {
    if (!isDragging && !isResizing) {
        return;
    }

    try {
        if (selectedLayer) {
            const layerItems = (selectedItems || []).filter(item => item?.type === 'layer');
            if (!selectedSlice && layerItems.length > 1) {
                for (const item of layerItems) {
                    const layer = layers.find(l => Number(l.id) === Number(item.layerId));
                    if (layer) await syncLayerToServer(layer);
                }
            } else if (selectedSlice) {
                const layer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
                const sliceData = getSliceData(layer, selectedSlice.sliceKey);
                if (sliceData) {
                    await applySliceProperties(selectedSlice.layerId, selectedSlice.sliceKey, layers);
                }
            } else {
                const layer = layers.find(l => Number(l.id) === Number(selectedLayer));
                if (layer) {
                    await syncLayerToServer(layer);
                }
            }
        }
    } catch (error) {
        console.error('Failed to sync layer properties on mouseup:', error);
    }

    return {
        isDragging: false,
        isResizing: false,
        resizeHandle: null
    };
}

/**
 * 更新光标样式
 */
export function updateCursor(
    mouseX, mouseY, canvas, selectedLayer, layers, canvasConfig, getClickedHandle, getClickedLayer, selectedSlice, dpr
) {
    if (!canvas) return;

    let newCursor = 'default';

    if (!selectedLayer) {
        const clickedLayer = getClickedLayer(mouseX, mouseY);
        newCursor = clickedLayer !== null ? 'move' : 'default';
    } else {
        const handle = getClickedHandle(mouseX, mouseY);
        if (handle) {
            const cursorByIndex = [
                'n-resize', 'ne-resize', 'e-resize', 'se-resize',
                's-resize', 'sw-resize', 'w-resize', 'nw-resize'
            ];
            const handleAngle = {
                'n': 0, 'ne': 45, 'e': 90, 'se': 135,
                's': 180, 'sw': 225, 'w': 270, 'nw': 315
            };
            let rotation = 0;
            if (selectedSlice) {
                const layer = layers.find(l => Number(l.id) === Number(selectedSlice.layerId));
                const sliceData = getSliceData(layer, selectedSlice.sliceKey);
                rotation = Number(sliceData?.rotation || sliceData?.rotate || layer?.rotation || 0);
            } else {
                const layer = layers.find(l => Number(l.id) === Number(selectedLayer));
                rotation = Number(layer?.rotation || 0);
            }
            const angle = ((handleAngle[handle] || 0) + rotation + 360) % 360;
            const cursorIndex = Math.round(angle / 45) % 8;
            newCursor = cursorByIndex[cursorIndex] || 'default';
        } else {
            const clickedLayer = getClickedLayer(mouseX, mouseY);
            newCursor = clickedLayer !== null ? 'move' : 'default';
        }
    }

    canvas.style.setProperty('cursor', newCursor, 'important');

    const container = canvas.parentElement;
    if (container && container.classList.contains('layer-canvas-container')) {
        container.style.setProperty('cursor', newCursor, 'important');
    }
}

/**
 * 画布右键事件 — 在选中图层上弹出"快速选择"对齐菜单。
 * 命中规则与单击相同（getClickedLayer），未命中任何图层则不弹出。
 * 实际菜单 UI 与位置计算在 panels/QuickAlignMenu.js。
 */
export function onCanvasContextMenu(
    e, canvas, dpr, cssToCanvasCoord, getClickedLayer, getClickedSlice,
    selectedLayer, layers, canvasConfig, validateConfig, parsePosition, parseSize,
    selectLayer, selectSlice, drawCanvas, updatePropertyInputs, syncLayerToServer, applySliceProperties,
    showQuickAlignMenu
) {
    e.preventDefault();
    if (!canvas || !validateConfig(canvasConfig)) return;

    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    const canvasCoord = cssToCanvasCoord(cssX, cssY);

    const clickedSlice = getClickedSlice ? getClickedSlice(canvasCoord.x, canvasCoord.y) : null;
    if (clickedSlice) {
        selectSlice(clickedSlice.layerId, clickedSlice.sliceKey);
        const layer = layers.find(l => Number(l.id) === Number(clickedSlice.layerId));
        const sliceData = getSliceData(layer, clickedSlice.sliceKey);
        if (!layer || !sliceData) return;
        const coord = parseSliceCoordinate(sliceData, {
            width: layer.size?.width || canvasConfig.width,
            height: layer.size?.height || canvasConfig.height
        });
        if (!coord) return;
        const menuTarget = {
            id: layer.id,
            position: { x: coord.x, y: coord.y },
            size: { width: coord.width, height: coord.height }
        };
        showQuickAlignMenu({
            clientX: e.clientX,
            clientY: e.clientY,
            layer: menuTarget,
            canvasConfig,
            onApply: async (result) => {
                const pos = result && result.position ? result.position : result;
                const size = result && result.size ? result.size : null;
                const newX = pos && Number.isFinite(pos.x) ? pos.x : coord.x;
                const newY = pos && Number.isFinite(pos.y) ? pos.y : coord.y;
                const newW = size && Number.isFinite(size.width) ? size.width : coord.width;
                const newH = size && Number.isFinite(size.height) ? size.height : coord.height;
                sliceData.position = `${newX} ${newY}`;
                sliceData.size = `${newW} ${newH}`;
                sliceData.coordinate = `${newX} ${newY} ${newW} ${newH}`;
                sliceData.range = sliceData.coordinate;
                sliceData.x = newX;
                sliceData.y = newY;
                sliceData.width = newW;
                sliceData.height = newH;
                drawCanvas();
                if (applySliceProperties) await applySliceProperties(clickedSlice.layerId, clickedSlice.sliceKey, layers, drawCanvas);
                await selectSlice(clickedSlice.layerId, clickedSlice.sliceKey);
            }
        });
        return;
    }

    const clickedLayerId = getClickedLayer(canvasCoord.x, canvasCoord.y);
    if (clickedLayerId === null) return;

    if (clickedLayerId !== selectedLayer) {
        selectLayer(clickedLayerId);
    }

    const layer = layers.find(l => Number(l.id) === Number(clickedLayerId));
    if (!layer) return;
    if (!layer.position || typeof layer.position === 'string') {
        layer.position = parsePosition(layer.position);
    }
    if (!layer.size || typeof layer.size === 'string') {
        layer.size = parseSize(layer.size);
    }

    showQuickAlignMenu({
        clientX: e.clientX,
        clientY: e.clientY,
        layer,
        canvasConfig,
        onApply: (result /*说明：, item */) => {
            // 兼容旧签名：早期 computeQuickAlign 直接返回 {x,y}
            const pos  = result && result.position ? result.position : result;
            const size = result && result.size     ? result.size     : null;
            if (pos && Number.isFinite(pos.x)) layer.position.x = pos.x;
            if (pos && Number.isFinite(pos.y)) layer.position.y = pos.y;
            if (size && Number.isFinite(size.width))  layer.size.width  = size.width;
            if (size && Number.isFinite(size.height)) layer.size.height = size.height;
            drawCanvas();
            updatePropertyInputs(layer);
            syncLayerToServer(layer);
        }
    });
}

/**
 * 画布双击事件 - 将选中图层填充到整个幕布区域
 */
export async function onCanvasDoubleClick(
    e, canvas, dpr, cssToCanvasCoord, getClickedLayer, getClickedSlice,
    selectedLayer, selectedSlice, layers, canvasConfig, validateConfig, parsePosition, parseSize,
    selectLayer, selectSlice, drawCanvas, updatePropertyInputs, syncLayerToServer, applySliceProperties
) {
    if (!canvas || !validateConfig(canvasConfig)) return;

    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    const canvasCoord = cssToCanvasCoord(cssX, cssY);
    const mouseX = canvasCoord.x;
    const mouseY = canvasCoord.y;

    const clickedSlice = getClickedSlice ? getClickedSlice(mouseX, mouseY) : null;
    const targetSlice = clickedSlice || selectedSlice || null;
    if (targetSlice) {
        const layer = layers.find(l => Number(l.id) === Number(targetSlice.layerId));
        const sliceData = getSliceData(layer, targetSlice.sliceKey);
        if (!layer || !sliceData) return;

        sliceData.position = '0 0';
        sliceData.size = `${canvasConfig.width} ${canvasConfig.height}`;
        sliceData.coordinate = `0 0 ${canvasConfig.width} ${canvasConfig.height}`;
        sliceData.range = sliceData.coordinate;
        sliceData.x = 0;
        sliceData.y = 0;
        sliceData.width = canvasConfig.width;
        sliceData.height = canvasConfig.height;

        drawCanvas();
        if (applySliceProperties) await applySliceProperties(targetSlice.layerId, targetSlice.sliceKey, layers, drawCanvas);
        await selectSlice(targetSlice.layerId, targetSlice.sliceKey);
        return { filledSlice: targetSlice };
    }

    const clickedLayerId = getClickedLayer(mouseX, mouseY);
    if (clickedLayerId === null) return;

    if (clickedLayerId !== selectedLayer) {
        selectLayer(clickedLayerId);
    }

    const layer = layers.find(l => Number(l.id) === Number(clickedLayerId));
    if (!layer) return;

    if (!layer.position || typeof layer.position === 'string') {
        layer.position = parsePosition(layer.position);
    }
    if (!layer.size || typeof layer.size === 'string') {
        layer.size = parseSize(layer.size);
    }

    layer.position.x = 0;
    layer.position.y = 0;
    layer.size.width = canvasConfig.width;
    layer.size.height = canvasConfig.height;

    drawCanvas();
    updatePropertyInputs(layer);
    syncLayerToServer(layer);

    return { filledLayerId: clickedLayerId };
}
