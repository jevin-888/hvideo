// 画布绘制模块
import { isConfigValid } from './utils.js';
import { parseSliceCoordinate } from './utils.js';
import {
    buildSliceLayerForDraw,
    collectSliceKeys,
    hydrateSliceFields,
    isSliceVisible
} from './sliceModel.js';
import { getItemBounds, getItemKey } from './geometry.js';

// 存储清理函数
let canvasCleanupFunctions = [];

/**
 * 清理画布相关资源
 */
export function cleanupCanvas() {
    canvasCleanupFunctions.forEach(cleanup => {
        try {
            cleanup();
        } catch (e) {
            // 清理画布资源时出错，忽略
        }
    });
    canvasCleanupFunctions = [];
}

/**
 * 初始化画布
 */
export function initializeCanvas(
    canvas, ctx, dpr,
    isDragging, isResizing,
    onCanvasMouseDown, onCanvasMouseMove, onCanvasMouseUp, onCanvasDoubleClick,
    updateCursor, cssToCanvasCoord, updateCanvasSizeByLayout, drawCanvas,
    canvasConfig,
    onCanvasContextMenu  // 可选：右键菜单（快速对齐）。未传时与旧行为一致：仅阻止系统菜单。
) {
    // 先清理之前的资源
    cleanupCanvas();

    canvas = document.getElementById('layer-canvas');
    if (canvas) {
        ctx = canvas.getContext('2d');

        dpr = window.devicePixelRatio || 1;

        canvas.draggable = false;
        canvas.style.userSelect = 'none';
        canvas.style.webkitUserSelect = 'none';

        const onDragStart = (e) => {
            e.preventDefault();
            return false;
        };
        canvas.addEventListener('dragstart', onDragStart);
        canvasCleanupFunctions.push(() => {
            canvas.removeEventListener('dragstart', onDragStart);
        });

        const onContextMenu = (e) => {
            e.preventDefault();
            if (typeof onCanvasContextMenu === 'function') {
                try { onCanvasContextMenu(e); } catch (_) { /* 不阻塞画布 */ }
            }
            return false;
        };
        canvas.addEventListener('contextmenu', onContextMenu);
        canvasCleanupFunctions.push(() => {
            canvas.removeEventListener('contextmenu', onContextMenu);
        });

        let capturedPointerId = null;

        function onPointerDown(e) {
            const result = onCanvasMouseDown(e);
            if (result && (result.isDragging || result.isResizing)) {
                canvas.setPointerCapture(e.pointerId);
                capturedPointerId = e.pointerId;
            }
        }

        function onPointerMove(e) {
            onCanvasMouseMove(e);

            // [Fix] 不再因为鼠标越界 50px 就主动释放 Pointer Capture：
            // 1) 这里的 isDragging/isResizing 是 init 时被闭包捕获的初值(false)，根本不更新；
            // 2) 拖动中释放捕获会立刻触发 pointerleave → onCanvasMouseUp → 拖动被强行结束，
            //    表现就是日志里 PUT burst 只持续 ~250ms 后断开，大屏每次只跟随几下就停。
            // Pointer Capture 本身就保证捕获期间事件继续路由到 canvas，无需手动救援。

            // 仅在未捕获 pointer（非拖动）时刷新光标
            if (capturedPointerId === null && canvas) {
                const rect = canvas.getBoundingClientRect();
                const cssX = e.clientX - rect.left;
                const cssY = e.clientY - rect.top;
                if (cssX >= 0 && cssX <= rect.width && cssY >= 0 && cssY <= rect.height) {
                    const canvasCoord = cssToCanvasCoord(cssX, cssY);
                    updateCursor(canvasCoord.x, canvasCoord.y);
                }
            }
        }

        function onPointerEnter(e) {
            // [Fix] 用 capturedPointerId 而不是失效的闭包变量判定是否处于拖动
            if (canvas && capturedPointerId === null) {
                const rect = canvas.getBoundingClientRect();
                const cssX = e.clientX - rect.left;
                const cssY = e.clientY - rect.top;
                if (cssX >= 0 && cssX <= rect.width && cssY >= 0 && cssY <= rect.height) {
                    const canvasCoord = cssToCanvasCoord(cssX, cssY);
                    updateCursor(canvasCoord.x, canvasCoord.y);
                }
            }
        }

        function onPointerLeave(e) {
            // [Fix] 拖动中（已捕获 pointer）不要做任何事 —— Pointer Capture 保证事件继续路由到
            // canvas，pointerleave 即使触发也只是浏览器层面的"鼠标位置不在 canvas 矩形内"，
            // 不代表用户松开了鼠标。原代码无条件释放捕获并触发 mouseUp，导致鼠标稍一越界拖动就断。
            if (capturedPointerId !== null) {
                return;
            }

            if (canvas) {
                canvas.style.setProperty('cursor', 'default', 'important');
                const container = canvas.parentElement;
                if (container && container.classList.contains('layer-canvas-container')) {
                    container.style.setProperty('cursor', 'default', 'important');
                }
            }
        }

        function finishPointerInteraction(e, options = {}) {
            const hadCapturedPointer = capturedPointerId !== null;
            const pointerId = capturedPointerId;

            if (pointerId !== null && e && typeof e.pointerId === 'number' && e.pointerId !== pointerId) {
                return;
            }

            capturedPointerId = null;
            if (pointerId !== null && canvas.hasPointerCapture(pointerId)) {
                canvas.releasePointerCapture(pointerId);
            }

            if (hadCapturedPointer || options.force) {
                onCanvasMouseUp(e);
            }
        }

        function onPointerUp(e) {
            finishPointerInteraction(e, { force: true });
        }

        function onPointerCancel(e) {
            finishPointerInteraction(e, { force: true });
        }

        function onLostPointerCapture(e) {
            finishPointerInteraction(e);
        }

        function onWindowPointerUp(e) {
            finishPointerInteraction(e, { force: true });
        }

        function onWindowBlur(e) {
            finishPointerInteraction(e, { force: true });
        }

        function onVisibilityChange(e) {
            if (document.hidden) {
                finishPointerInteraction(e, { force: true });
            }
        }

        function resetPointerCaptureOnly() {
            capturedPointerId = null;
        }

        if (window.PointerEvent) {
            canvas.addEventListener('pointerdown', onPointerDown);
            canvas.addEventListener('pointermove', onPointerMove);
            canvas.addEventListener('pointerenter', onPointerEnter);
            canvas.addEventListener('pointerleave', onPointerLeave);
            canvas.addEventListener('pointerup', onPointerUp);
            canvas.addEventListener('pointercancel', onPointerCancel);
            canvas.addEventListener('lostpointercapture', onLostPointerCapture);
            window.addEventListener('pointerup', onWindowPointerUp);
            window.addEventListener('pointercancel', onPointerCancel);
            window.addEventListener('blur', onWindowBlur);
            document.addEventListener('visibilitychange', onVisibilityChange);
            canvasCleanupFunctions.push(() => {
                finishPointerInteraction(new Event('cleanup'));
                canvas.removeEventListener('pointerdown', onPointerDown);
                canvas.removeEventListener('pointermove', onPointerMove);
                canvas.removeEventListener('pointerenter', onPointerEnter);
                canvas.removeEventListener('pointerleave', onPointerLeave);
                canvas.removeEventListener('pointerup', onPointerUp);
                canvas.removeEventListener('pointercancel', onPointerCancel);
                canvas.removeEventListener('lostpointercapture', onLostPointerCapture);
                window.removeEventListener('pointerup', onWindowPointerUp);
                window.removeEventListener('pointercancel', onPointerCancel);
                window.removeEventListener('blur', onWindowBlur);
                document.removeEventListener('visibilitychange', onVisibilityChange);
                resetPointerCaptureOnly();
            });
        } else {
            canvas.addEventListener('mousedown', onCanvasMouseDown);
            canvas.addEventListener('mousemove', onCanvasMouseMove);
            canvas.addEventListener('mouseup', onCanvasMouseUp);
            canvas.addEventListener('mouseleave', onCanvasMouseUp);
            document.addEventListener('mouseup', onCanvasMouseUp);
            document.addEventListener('mousemove', onCanvasMouseMove);
            canvasCleanupFunctions.push(() => {
                canvas.removeEventListener('mousedown', onCanvasMouseDown);
                canvas.removeEventListener('mousemove', onCanvasMouseMove);
                canvas.removeEventListener('mouseup', onCanvasMouseUp);
                canvas.removeEventListener('mouseleave', onCanvasMouseUp);
                document.removeEventListener('mouseup', onCanvasMouseUp);
                document.removeEventListener('mousemove', onCanvasMouseMove);
            });
        }

        // 双击事件：将图层填满整个幕布区域
        if (onCanvasDoubleClick) {
            canvas.addEventListener('dblclick', onCanvasDoubleClick);
            canvasCleanupFunctions.push(() => {
                canvas.removeEventListener('dblclick', onCanvasDoubleClick);
            });
        }

        const resizeObserver = new ResizeObserver(entries => {
            for (let entry of entries) {
                if (entry.target === canvas.parentElement) {
                    const width = entry.contentRect.width;
                    const height = entry.contentRect.height;

                    if (width > 0 && height > 0) {
                        if (isConfigValid(canvasConfig)) {
                            updateCanvasSizeByLayout().then(() => drawCanvas());
                        }
                    }
                }
            }
        });

        if (canvas.parentElement) {
            resizeObserver.observe(canvas.parentElement);
            canvasCleanupFunctions.push(() => {
                resizeObserver.disconnect();
            });
        }

        drawCanvas();

        return { canvas, ctx, dpr };
    }
    return { canvas, ctx, dpr };
}

/**
 * 根据幕布分辨率调整画布大小比例
 */
export function updateCanvasSizeByLayout(canvas, ctx, dpr, canvasConfig) {
    if (!canvas || !isConfigValid(canvasConfig)) {
        return Promise.resolve();
    }

    return new Promise((resolve) => {
        requestAnimationFrame(() => {
            requestAnimationFrame(() => {
                const container = canvas.parentElement;
                if (!container) {
                    resolve();
                    return;
                }

                const containerRect = container.getBoundingClientRect();
                if (containerRect.width <= 0 || containerRect.height <= 0) {
                    resolve();
                    return;
                }

                const containerWidth = containerRect.width - 20;
                const reservedHeightForCanvasMath = 0;
                const containerHeight = Math.max(200, containerRect.height - 20 - reservedHeightForCanvasMath);

                if (containerWidth <= 0 || containerHeight <= 0) {
                    resolve();
                    return;
                }

                if (!canvasConfig.width || !canvasConfig.height || canvasConfig.height <= 0) {
                    resolve();
                    return;
                }

                const canvasAspectRatio = canvasConfig.width / canvasConfig.height;
                let canvasWidth, canvasHeight;
                if (containerWidth / containerHeight > canvasAspectRatio) {
                    canvasHeight = containerHeight;
                    canvasWidth = canvasHeight * canvasAspectRatio;
                } else {
                    canvasWidth = containerWidth;
                    canvasHeight = canvasWidth / canvasAspectRatio;
                }

                const minSize = 200;
                if (canvasWidth < minSize) {
                    canvasWidth = minSize;
                    canvasHeight = canvasWidth / canvasAspectRatio;
                }
                if (canvasHeight < minSize) {
                    canvasHeight = minSize;
                    canvasWidth = canvasHeight * canvasAspectRatio;
                }

                dpr = window.devicePixelRatio || 1;

                const styleWidth = Math.round(canvasWidth);
                const styleHeight = Math.round(canvasHeight);
                const backingWidth = Math.round(canvasWidth * dpr);
                const backingHeight = Math.round(canvasHeight * dpr);

                if (canvas.style.width !== styleWidth + 'px') {
                    canvas.style.width = styleWidth + 'px';
                }
                if (canvas.style.height !== styleHeight + 'px') {
                    canvas.style.height = styleHeight + 'px';
                }

                if (canvas.width !== backingWidth) {
                    canvas.width = backingWidth;
                }
                if (canvas.height !== backingHeight) {
                    canvas.height = backingHeight;
                }

                ctx = canvas.getContext('2d');

                resolve();
            });
        });
    });
}

/**
 * 获取画布缩放比例
 */
export function getCanvasScale(canvas, dpr, canvasConfig) {
    if (!canvas) {
        return { x: 1, y: 1 };
    }
    const logicalWidth = canvas.width / dpr;
    const logicalHeight = canvas.height / dpr;

    if (!isConfigValid(canvasConfig)) {
        return { x: 1, y: 1 };
    }

    return {
        x: logicalWidth / canvasConfig.width,
        y: logicalHeight / canvasConfig.height
    };
}

/**
 * 系统坐标转画布坐标
 */
export function systemToCanvas(x, y, canvas, dpr, canvasConfig) {
    const scale = getCanvasScale(canvas, dpr, canvasConfig);
    return {
        x: x * scale.x,
        y: y * scale.y
    };
}

/**
 * 画布坐标转系统坐标
 */
export function canvasToSystem(x, y, canvas, dpr, canvasConfig) {
    const scale = getCanvasScale(canvas, dpr, canvasConfig);
    return {
        x: Math.round(x / scale.x),
        y: Math.round(y / scale.y)
    };
}

/**
 * CSS坐标转画布内部坐标
 */
export function cssToCanvasCoord(cssX, cssY) {
    return {
        x: cssX,
        y: cssY
    };
}

/**
 * 根据图层ID生成唯一颜色
 */
function getLayerColorById(layerId) {
    const hue = (layerId * 137.508) % 360;
    const saturation = 65 + (layerId % 20);
    const lightness = 50 + (layerId % 15);

    return {
        fill: `hsla(${hue}, ${saturation}%, ${lightness}%, 0.35)`,
        stroke: `hsl(${hue}, ${saturation}%, ${lightness - 10}%)`
    };
}

/**
 * 获取图层颜色
 */
export function getLayerColor(layer, isSelected) {
    const color = getLayerColorById(layer.id);

    if (isSelected) {
        return {
            fill: color.fill.replace('0.35', '0.55'),
            stroke: '#00bfff'
        };
    }
    return color;
}

/**
 * 绘制单个图层（支持图层形状）
 */
export function drawLayerRect(
    layer, isSelected, ctx, canvas, dpr, canvasConfig,
    systemToCanvas, getLayerColor, getLayerDisplayName, ALL_AVAILABLE_LAYERS,
    drawResizeHandles, validateConfig
) {
    if (!ctx) return;

    const x = layer.position?.x ?? 0;
    const y = layer.position?.y ?? 0;
    let width = layer.size?.width;
    let height = layer.size?.height;

    if (!width || width <= 0) {
        if (!validateConfig(canvasConfig)) return;
        width = canvasConfig.width;
    }
    if (!height || height <= 0) {
        if (!validateConfig(canvasConfig)) return;
        height = canvasConfig.height;
    }

    const canvasPos = systemToCanvas(x, y, canvas, dpr, canvasConfig);
    const canvasSize = systemToCanvas(width, height, canvas, dpr, canvasConfig);
    const centerX = canvasPos.x + canvasSize.x / 2;
    const centerY = canvasPos.y + canvasSize.y / 2;
    const halfWidth = canvasSize.x / 2;
    const halfHeight = canvasSize.y / 2;
    const minSize = Math.min(halfWidth, halfHeight);

    const color = getLayerColor(layer, isSelected);
    const rotationDeg = Number(layer.rotation || 0);
    const rotationRad = rotationDeg * Math.PI / 180;

    // 获取形状类型：0=矩形, 1=圆形, 2=三角形, 3=圆角矩形, 4=星形, 5=六边形, 6=菱形, 7=心形, 8=花瓣
    const shapeType = Number(layer.shapeType ?? layer.shape_type ?? 0);
    const shapeParam = Number(layer.shapeParam ?? layer.shape_param ?? 0);

    ctx.save();
    ctx.translate(centerX, centerY);
    if (rotationRad !== 0) {
        ctx.rotate(rotationRad);
    }
    ctx.translate(-centerX, -centerY);

    ctx.fillStyle = color.fill;
    ctx.strokeStyle = color.stroke;
    ctx.lineWidth = isSelected ? 2 : 1;
    ctx.setLineDash([]);

    ctx.beginPath();

    if (shapeType === 1) {
        // 圆形：在矩形区域内绘制内切圆
        const radius = minSize;
        ctx.arc(centerX, centerY, radius, 0, Math.PI * 2);
    } else if (shapeType === 2) {
        // 三角形：等腰三角形，顶点在上方中心
        const topX = centerX;
        const topY = canvasPos.y;
        const bottomLeftX = canvasPos.x;
        const bottomLeftY = canvasPos.y + canvasSize.y;
        const bottomRightX = canvasPos.x + canvasSize.x;
        const bottomRightY = canvasPos.y + canvasSize.y;
        ctx.moveTo(topX, topY);
        ctx.lineTo(bottomRightX, bottomRightY);
        ctx.lineTo(bottomLeftX, bottomLeftY);
        ctx.closePath();
    } else if (shapeType === 3) {
        // 圆角矩形
        const cornerRadius = Math.min(shapeParam * minSize * 2, minSize);
        const rx = canvasPos.x;
        const ry = canvasPos.y;
        const rw = canvasSize.x;
        const rh = canvasSize.y;
        ctx.moveTo(rx + cornerRadius, ry);
        ctx.lineTo(rx + rw - cornerRadius, ry);
        ctx.arcTo(rx + rw, ry, rx + rw, ry + cornerRadius, cornerRadius);
        ctx.lineTo(rx + rw, ry + rh - cornerRadius);
        ctx.arcTo(rx + rw, ry + rh, rx + rw - cornerRadius, ry + rh, cornerRadius);
        ctx.lineTo(rx + cornerRadius, ry + rh);
        ctx.arcTo(rx, ry + rh, rx, ry + rh - cornerRadius, cornerRadius);
        ctx.lineTo(rx, ry + cornerRadius);
        ctx.arcTo(rx, ry, rx + cornerRadius, ry, cornerRadius);
        ctx.closePath();
    } else if (shapeType === 4) {
        // 星形
        const n = Math.max(3, Math.min(8, Math.round(shapeParam) || 5)); // 角的数量，默认5
        const outerRadius = minSize;
        const innerRadius = outerRadius * 0.5;
        const angleStep = (Math.PI * 2) / n;
        for (let i = 0; i <= n * 2; i++) {
            const angle = (i * angleStep) / 2 - Math.PI / 2;
            const radius = i % 2 === 0 ? outerRadius : innerRadius;
            const x = centerX + radius * Math.cos(angle);
            const y = centerY + radius * Math.sin(angle);
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.closePath();
    } else if (shapeType === 5) {
        // 六边形
        const radius = minSize;
        for (let i = 0; i <= 6; i++) {
            const angle = (i * Math.PI * 2) / 6 - Math.PI / 2;
            const x = centerX + radius * Math.cos(angle);
            const y = centerY + radius * Math.sin(angle);
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.closePath();
    } else if (shapeType === 6) {
        // 菱形
        const topX = centerX;
        const topY = canvasPos.y;
        const rightX = canvasPos.x + canvasSize.x;
        const rightY = centerY;
        const bottomX = centerX;
        const bottomY = canvasPos.y + canvasSize.y;
        const leftX = canvasPos.x;
        const leftY = centerY;
        ctx.moveTo(topX, topY);
        ctx.lineTo(rightX, rightY);
        ctx.lineTo(bottomX, bottomY);
        ctx.lineTo(leftX, leftY);
        ctx.closePath();
    } else if (shapeType === 7) {
        // 心形：使用参数方程绘制
        const scale = minSize / 16; // 调整缩放以适应画布
        const steps = 50; // 增加采样点以获得更平滑的曲线
        for (let i = 0; i <= steps; i++) {
            const t = (i / steps) * Math.PI * 2;
            // 心形参数方程：x = 16sin³(t), y = 13cos(t) - 5cos(2t) - 2cos(3t) - cos(4t)
            const x = scale * 16 * Math.pow(Math.sin(t), 3);
            const y = -scale * (13 * Math.cos(t) - 5 * Math.cos(2 * t) - 2 * Math.cos(3 * t) - Math.cos(4 * t));
            if (i === 0) {
                ctx.moveTo(centerX + x, centerY + y);
            } else {
                ctx.lineTo(centerX + x, centerY + y);
            }
        }
        ctx.closePath();
    } else if (shapeType === 8) {
        // 花瓣：使用极坐标方程
        const n = Math.max(3, Math.min(8, Math.round(shapeParam) || 5)); // 花瓣数量，默认5
        const radius = minSize;
        const steps = n * 20; // 每个花瓣20个点
        for (let i = 0; i <= steps; i++) {
            const t = (i / steps) * Math.PI * 2;
            // 花瓣极坐标方程：r = a * (1 + k * cos(n*t))
            const r = radius * (1 + 0.5 * Math.cos(n * t));
            const x = centerX + r * Math.cos(t);
            const y = centerY + r * Math.sin(t);
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        ctx.closePath();
    } else {
        // 默认矩形
        ctx.rect(canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y);
    }

    ctx.fill();
    ctx.stroke();

    let displayName = getLayerDisplayName(layer, ALL_AVAILABLE_LAYERS);
    const textX = canvasPos.x + 6;
    const textY = canvasPos.y + 16;

    ctx.font = 'bold 13px "Microsoft YaHei", "PingFang SC", "Helvetica Neue", Arial, sans-serif';

    ctx.fillStyle = 'rgba(0, 0, 0, 0.6)';
    ctx.fillRect(textX - 2, textY - 12, ctx.measureText(displayName).width + 4, 16);

    ctx.strokeStyle = 'rgba(0, 0, 0, 0.8)';
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.miterLimit = 2;
    ctx.strokeText(displayName, textX, textY);

    ctx.fillStyle = '#ffffff';
    ctx.fillText(displayName, textX, textY);

    if (isSelected) {
        drawResizeHandles(canvasPos.x, canvasPos.y, canvasSize.x, canvasSize.y, ctx);
    }

    ctx.restore();
}

/**
 * 绘制图层的切片
 */
export function drawLayerSlices(
    layer, selectedSlice, ctx, canvas, dpr, canvasConfig,
    systemToCanvas, drawSliceRect, validateConfig,
    drawLayerRect, getLayerColor, getLayerDisplayName, ALL_AVAILABLE_LAYERS, drawResizeHandles
) {
    if (!ctx || !layer) return;

    hydrateSliceFields(layer);
    const sliceKeys = collectSliceKeys(layer);

    sliceKeys.forEach(sliceKey => {
        const sliceData = layer[sliceKey];
        if (isSliceVisible(sliceData)) {
            const isSelected = selectedSlice &&
                selectedSlice.layerId === layer.id &&
                selectedSlice.sliceKey === sliceKey;
            drawSliceRect(
                layer, sliceKey, sliceData, isSelected, ctx, canvas, dpr, canvasConfig,
                systemToCanvas, validateConfig, drawLayerRect, getLayerColor,
                getLayerDisplayName, ALL_AVAILABLE_LAYERS, drawResizeHandles
            );
        }
    });
}

/**
 * 绘制单个切片矩形
 */
export function drawSliceRect(
    layer, sliceKey, sliceData, isSelected, ctx, canvas, dpr, canvasConfig,
    systemToCanvas, validateConfig, drawLayerRect, getLayerColor, getLayerDisplayName,
    ALL_AVAILABLE_LAYERS, drawResizeHandles
) {
    if (!ctx) return;

    const fallbackSize = (!validateConfig(canvasConfig) || !layer.size) ? null : {
        width: (layer.size.width && layer.size.width > 0) ? layer.size.width : canvasConfig.width,
        height: (layer.size.height && layer.size.height > 0) ? layer.size.height : canvasConfig.height
    };

    const coord = parseSliceCoordinate(sliceData, fallbackSize);
    if (!coord) return;

    let { x, y, width, height } = coord;

    if (width === 0 || height === 0) {
        if (!validateConfig(canvasConfig)) return;
        width = (layer.size?.width && layer.size.width > 0) ? layer.size.width : canvasConfig.width;
        height = (layer.size?.height && layer.size.height > 0) ? layer.size.height : canvasConfig.height;
    }

    const canvasPos = systemToCanvas(x, y, canvas, dpr, canvasConfig);
    const canvasSize = systemToCanvas(width, height, canvas, dpr, canvasConfig);

    const sliceIndex = sliceKey.replace('slice', '');
    const sliceName = `${getLayerDisplayName(layer, ALL_AVAILABLE_LAYERS)} 切片${sliceIndex}`;

    const sliceLayerForDraw = buildSliceLayerForDraw(layer, sliceData, { x, y, width, height }, sliceName);

    drawLayerRect(
        sliceLayerForDraw, isSelected, ctx, canvas, dpr, canvasConfig,
        systemToCanvas, getLayerColor, getLayerDisplayName, ALL_AVAILABLE_LAYERS,
        drawResizeHandles, validateConfig
    );
}

/**
 * 绘制调整大小控制点
 */
export function drawResizeHandles(x, y, width, height, ctx) {
    if (!ctx) return;

    const handleRadius = 4;

    ctx.fillStyle = '#667eea';
    ctx.strokeStyle = '#ffffff';
    ctx.lineWidth = 1.5;

    // 因为已经在 ctx.rotate 环境下，这里不需要再传 rotation，传 0 即可
    const handles = getHandlePositions(x, y, width, height, 0);

    Object.values(handles).forEach(handle => {
        ctx.beginPath();
        ctx.arc(handle.x, handle.y, handleRadius, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();
    });
}

export function drawSelectionOverlays(
    ctx, canvas, dpr, canvasConfig, selectedItems, selectedLayer, selectedSlice,
    layers, systemToCanvas, validateConfig
) {
    if (!ctx || !canvas || !validateConfig(canvasConfig)) return;
    const items = Array.isArray(selectedItems) ? selectedItems : [];
    if (items.length <= 1) return;

    const activeKey = selectedSlice
        ? `slice:${Number(selectedSlice.layerId)}:${selectedSlice.sliceKey}`
        : (selectedLayer !== null && selectedLayer !== undefined ? `layer:${Number(selectedLayer)}` : '');

    ctx.save();
    ctx.setLineDash([6, 4]);
    ctx.lineWidth = 1.5;

    items.forEach(item => {
        const key = getItemKey(item);
        if (!key || key === activeKey) return;
        const bounds = getItemBounds(item, layers, canvasConfig);
        if (!bounds) return;
        const pos = systemToCanvas(bounds.x, bounds.y, canvas, dpr, canvasConfig);
        const size = systemToCanvas(bounds.width, bounds.height, canvas, dpr, canvasConfig);
        ctx.strokeStyle = 'rgba(255, 214, 102, 0.95)';
        ctx.fillStyle = 'rgba(255, 214, 102, 0.08)';
        ctx.fillRect(pos.x, pos.y, size.x, size.y);
        ctx.strokeRect(pos.x, pos.y, size.x, size.y);
    });

    ctx.restore();
}

export function drawSnapGuides(ctx, canvas, dpr, canvasConfig, guides, systemToCanvas, validateConfig) {
    if (!ctx || !canvas || !validateConfig(canvasConfig) || !Array.isArray(guides) || guides.length === 0) return;

    ctx.save();
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = '#ffd166';
    ctx.setLineDash([8, 5]);

    guides.forEach(guide => {
        ctx.beginPath();
        if (guide.axis === 'x') {
            const p1 = systemToCanvas(guide.value, guide.from ?? 0, canvas, dpr, canvasConfig);
            const p2 = systemToCanvas(guide.value, guide.to ?? canvasConfig.height, canvas, dpr, canvasConfig);
            ctx.moveTo(Math.round(p1.x) + 0.5, p1.y);
            ctx.lineTo(Math.round(p2.x) + 0.5, p2.y);
        } else {
            const p1 = systemToCanvas(guide.from ?? 0, guide.value, canvas, dpr, canvasConfig);
            const p2 = systemToCanvas(guide.to ?? canvasConfig.width, guide.value, canvas, dpr, canvasConfig);
            ctx.moveTo(p1.x, Math.round(p1.y) + 0.5);
            ctx.lineTo(p2.x, Math.round(p2.y) + 0.5);
        }
        ctx.stroke();
    });

    ctx.setLineDash([]);
    ctx.restore();
}

/**
 * 获取控制点中心位置（支持旋转）
 */
export function getHandlePositions(x, y, width, height, rotation = 0) {
    const centerX = x + width / 2;
    const centerY = y + height / 2;
    const rad = rotation * Math.PI / 180;
    const cos = Math.cos(rad);
    const sin = Math.sin(rad);

    const getRotatedPoint = (px, py) => {
        const dx = px - centerX;
        const dy = py - centerY;
        return {
            x: centerX + (dx * cos - dy * sin),
            y: centerY + (dx * sin + dy * cos)
        };
    };

    return {
        nw: getRotatedPoint(x, y),
        n: getRotatedPoint(x + width / 2, y),
        ne: getRotatedPoint(x + width, y),
        w: getRotatedPoint(x, y + height / 2),
        e: getRotatedPoint(x + width, y + height / 2),
        sw: getRotatedPoint(x, y + height),
        s: getRotatedPoint(x + width / 2, y + height),
        se: getRotatedPoint(x + width, y + height)
    };
}

/**
 * 绘制画布
 */
export function drawCanvas(
    canvas, ctx, dpr, layers, selectedLayer, selectedSlice, canvasConfig,
    drawLayerRect, drawLayerSlices, systemToCanvas, getLayerColor, getLayerDisplayName,
    ALL_AVAILABLE_LAYERS, drawResizeHandles, validateConfig, selectedItems = [], snapGuides = []
) {
    if (!canvas || !ctx) {
        return;
    }

    ctx.save();

    ctx.clearRect(0, 0, canvas.width, canvas.height);

    ctx.setTransform(1, 0, 0, 1, 0, 0);

    ctx.scale(dpr, dpr);

    const logicalWidth = canvas.width / dpr;
    const logicalHeight = canvas.height / dpr;

    ctx.fillStyle = '#1a1a2e';
    ctx.fillRect(0, 0, logicalWidth, logicalHeight);

    ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
    ctx.lineWidth = 1;

    const gridSize = 50;
    for (let x = 0; x <= logicalWidth; x += gridSize) {
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, logicalHeight);
        ctx.stroke();
    }
    for (let y = 0; y <= logicalHeight; y += gridSize) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(logicalWidth, y);
        ctx.stroke();
    }

    const selectedLayerObj = selectedLayer ? layers.find(l => l.id === selectedLayer) : null;
    const otherLayers = layers.filter(layer => layer.id !== selectedLayer);

    otherLayers.forEach(layer => {
        if (layer.visible !== false) {
            drawLayerRect(
                layer, false, ctx, canvas, dpr, canvasConfig,
                systemToCanvas, getLayerColor, getLayerDisplayName, ALL_AVAILABLE_LAYERS,
                drawResizeHandles, validateConfig
            );
        }
    });

    if (selectedLayerObj && selectedLayerObj.visible !== false) {
        const isLayerSelected = !(selectedSlice && selectedSlice.layerId === selectedLayer);
        drawLayerRect(
            selectedLayerObj, isLayerSelected, ctx, canvas, dpr, canvasConfig,
            systemToCanvas, getLayerColor, getLayerDisplayName, ALL_AVAILABLE_LAYERS,
            drawResizeHandles, validateConfig
        );

        drawLayerSlices(
            selectedLayerObj, selectedSlice, ctx, canvas, dpr, canvasConfig,
            systemToCanvas, drawSliceRect, validateConfig,
            drawLayerRect, getLayerColor, getLayerDisplayName, ALL_AVAILABLE_LAYERS, drawResizeHandles
        );
    }

    drawSelectionOverlays(
        ctx, canvas, dpr, canvasConfig, selectedItems, selectedLayer, selectedSlice,
        layers, systemToCanvas, validateConfig
    );
    drawSnapGuides(ctx, canvas, dpr, canvasConfig, snapGuides, systemToCanvas, validateConfig);

    ctx.restore();
}

