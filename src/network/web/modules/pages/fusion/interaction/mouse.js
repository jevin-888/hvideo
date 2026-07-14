import { patchInteraction, setActiveRegion } from '../actions.js?v=2.95';
import { getActiveGeometry, getActiveMask } from '../selectors.js?v=2.95';
import { FOCUS_MODES } from '../types.js';
import {
    drawGridScene,
    getCanvasRegionPreviewLayouts,
    hitTestTransformedMaskPoint,
    outputScreenToRegionLocal,
    outputScreenToGeometryPoint,
    regionPointToOutputScreen,
    screenToTransformedMaskCanvasPoint
} from '../canvas/renderer.js?v=2.101';
import { GRID_VISUAL_METRICS } from '../canvas/visualStyle.js?v=2.81';
import {
    applyGeometryMoveLocal,
    applyManagerMovePointLocal,
    applyManagerMoveLineLocal,
    getManagerCornerName,
    getManagerCornerSelection,
    setGeometrySelection,
    setManagerCornerSelection,
    setManagerSideSelection,
    snapManagerCornerSelection
} from '../geometry/actions.js?v=2.95';
import { applyMaskMoveLocal, setMaskSelection } from '../mask/actions.js?v=2.95';
import { syncGeometryMoveOp, syncManagerPointMoveOp, syncManagerLineMoveOp } from '../sync/syncGeometry.js?v=2.95';
import { syncMaskMoveOp } from '../sync/syncMask.js?v=2.95';
import { pushGeometryUndo, pushMaskUndo } from '../undo.js';
import { setActiveBlendCorner, setActiveBlendSide } from '../blend/actions.js?v=2.95';

let dragRenderPending = false;
let dragRenderCanvas = null;

function scheduleDragRender(canvas) {
    dragRenderCanvas = canvas || dragRenderCanvas;
    if (dragRenderPending) return;
    dragRenderPending = true;
    requestAnimationFrame(() => {
        dragRenderPending = false;
        if (dragRenderCanvas) drawGridScene(dragRenderCanvas);
    });
}

function canEditTarget(state, targetState) {
    if (!targetState) return false;
    if (state.page.activeTab === 'mask') return !!targetState.showGrid;
    if (state.page.activeTab === 'geometry') return !!targetState.showGrid;
    if (state.page.activeTab === 'blend') return !!state.blend.managerMode;
    return false;
}

function distanceToPoint(screen, mx, my) {
    if (!screen) return Number.POSITIVE_INFINITY;
    return Math.hypot(screen.x - mx, screen.y - my);
}

function hitTestLayoutPoints(points, layout, mx, my, threshold = GRID_VISUAL_METRICS.pointHitRadius) {
    if (!Array.isArray(points)) return -1;
    for (let i = 0; i < points.length; i += 1) {
        if (distanceToPoint(regionPointToOutputScreen(points[i], layout), mx, my) <= threshold) return i;
    }
    return -1;
}

function hitTestGeometryCornerPoint(region, layout, mx, my) {
    if (!region || !Array.isArray(region.points)) return -1;
    const corners = [
        0,
        region.cols - 1,
        (region.rows - 1) * region.cols,
        region.rows * region.cols - 1
    ];
    for (const index of corners) {
        if (index < 0 || index >= region.points.length) continue;
        if (distanceToPoint(regionPointToOutputScreen(region.points[index], layout), mx, my) <= GRID_VISUAL_METRICS.pointHitRadius) return index;
    }
    return -1;
}

function managerDirectionFromDrag(region, index, du, dv) {
    if (!region || index < 0) return -1;
    const row = Math.floor(index / region.cols);
    const col = index % region.cols;
    if (Math.abs(du) >= Math.abs(dv)) {
        if (col === 0) return 2;
        if (col === region.cols - 1) return 3;
    } else {
        if (row === 0) return 0;
        if (row === region.rows - 1) return 1;
    }
    if (col === 0) return 2;
    if (col === region.cols - 1) return 3;
    if (row === 0) return 0;
    if (row === region.rows - 1) return 1;
    return -1;
}

function distanceToSegment(px, py, ax, ay, bx, by) {
    const dx = bx - ax;
    const dy = by - ay;
    const len2 = dx * dx + dy * dy;
    if (len2 <= 0) return Math.sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));
    const t = Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / len2));
    const x = ax + t * dx;
    const y = ay + t * dy;
    return Math.sqrt((px - x) * (px - x) + (py - y) * (py - y));
}

function hitTestGeometryBoundaryLine(region, layout, mx, my) {
    if (!region || !Array.isArray(region.points)) return null;
    const sides = [
        { side: 'top', direction: 0, row: 0, col: Math.floor(region.cols / 2) },
        { side: 'bottom', direction: 1, row: region.rows - 1, col: Math.floor(region.cols / 2) },
        { side: 'left', direction: 2, row: Math.floor(region.rows / 2), col: 0 },
        { side: 'right', direction: 3, row: Math.floor(region.rows / 2), col: region.cols - 1 }
    ];
    let best = null;
    for (const side of sides) {
        const count = side.side === 'top' || side.side === 'bottom' ? region.cols : region.rows;
        for (let i = 0; i < count - 1; i += 1) {
            const aIndex = side.side === 'top' ? i
                : side.side === 'bottom' ? (region.rows - 1) * region.cols + i
                : side.side === 'left' ? i * region.cols
                : i * region.cols + region.cols - 1;
            const bIndex = side.side === 'top' ? i + 1
                : side.side === 'bottom' ? (region.rows - 1) * region.cols + i + 1
                : side.side === 'left' ? (i + 1) * region.cols
                : (i + 1) * region.cols + region.cols - 1;
            const a = region.points[aIndex];
            const b = region.points[bIndex];
            if (!a || !b) continue;
            const as = regionPointToOutputScreen(a, layout);
            const bs = regionPointToOutputScreen(b, layout);
            if (!as || !bs) continue;
            const distance = distanceToSegment(mx, my, as.x, as.y, bs.x, bs.y);
            if (distance <= GRID_VISUAL_METRICS.lineHitRadius && (!best || distance < best.distance)) best = { ...side, distance };
        }
    }
    return best;
}

function hitTestRegionPreview(canvas, state, mx, my) {
    const layouts = getCanvasRegionPreviewLayouts(canvas, state);
    for (const layout of layouts) {
        const { rect, cell } = layout;
        const inside = mx >= rect.x && mx <= rect.x + rect.width && my >= rect.y && my <= rect.y + rect.height;
        if (inside) return layout;
        const insideCell = mx >= cell.x && mx <= cell.x + cell.width && my >= cell.y && my <= cell.y + cell.height;
        if (insideCell) return layout;
    }
    return null;
}

function getRegionPreviewLayoutById(canvas, state, regionId) {
    return getCanvasRegionPreviewLayouts(canvas, state)
        .find((layout) => String(layout.regionId) === String(regionId)) || null;
}

export function onCanvasMouseDown(state, canvas, event) {
    const rect = canvas.getBoundingClientRect();
    const mx = event.clientX - rect.left;
    const my = event.clientY - rect.top;

    if (state.page.focusMode === FOCUS_MODES.LAYOUT && state.layout.regionIds.length > 0) {
        if (state.page.activeTab === 'mask') {
            const active = getActiveMask();
            if (!canEditTarget(state, active)) return { geometrySelectionChanged: false, maskSelectionChanged: false };
            const hit = hitTestTransformedMaskPoint(canvas, state, active, mx, my);
            const pointIndex = hit?.pointIndex ?? -1;
            if (pointIndex >= 0) {
                const row = Math.floor(pointIndex / active.cols);
                const col = pointIndex % active.cols;
                setMaskSelection(row, col);
                pushMaskUndo(active);
                patchInteraction({
                    active: true,
                    pointIndex,
                    regionId: hit.regionId,
                    target: 'mask-point',
                    lastU: active.points[pointIndex].u,
                    lastV: active.points[pointIndex].v
                });
                return { geometrySelectionChanged: false, maskSelectionChanged: true };
            }
            return { geometrySelectionChanged: false, maskSelectionChanged: false };
        }

        // 先检测是否点击了某个区域
        const previewLayout = hitTestRegionPreview(canvas, state, mx, my);
        
        if (previewLayout) {
            const { regionId, region: active, rect: layoutRect } = previewLayout;
            const wasActiveRegion = String(regionId) === String(state.page.activeRegionId);
            // 检测是否点击了热点
            if (!layoutRect) return { geometrySelectionChanged: false, maskSelectionChanged: false };
            if (!wasActiveRegion) {
                setActiveRegion(regionId);
                if (state.page.activeTab === 'blend') {
                    const geometry = snapManagerCornerSelection(regionId);
                    setActiveBlendCorner(getManagerCornerName(geometry));
                }
                scheduleDragRender(canvas);
                return { geometrySelectionChanged: state.page.activeTab === 'geometry' || state.page.activeTab === 'blend', maskSelectionChanged: false };
            }
            if (!canEditTarget(state, active)) return { geometrySelectionChanged: false, maskSelectionChanged: false };
            const index = state.page.activeTab === 'blend'
                ? hitTestGeometryCornerPoint(active, previewLayout, mx, my)
                : hitTestLayoutPoints(active.points, previewLayout, mx, my);
            if (index >= 0) {
                // 切换到该区域并选中热点
                setActiveRegion(regionId);
                const row = Math.floor(index / active.cols);
                const col = index % active.cols;
                let dragPointIndex = index;
                if (state.page.activeTab === 'blend') {
                    setManagerCornerSelection(regionId, row, col);
                    const geometry = state.geometry.byRegionId[regionId];
                    const cornerRow = geometry?.selected?.row ?? row;
                    const cornerCol = geometry?.selected?.col ?? col;
                    dragPointIndex = cornerRow * active.cols + cornerCol;
                    const corner = `${cornerRow === 0 ? 'top' : 'bottom'}-${cornerCol === 0 ? 'left' : 'right'}`;
                    setActiveBlendCorner(corner);
                    setActiveBlendSide(cornerCol === 0 ? 'left' : cornerCol === active.cols - 1 ? 'right' : cornerRow === 0 ? 'top' : 'bottom');
                    pushGeometryUndo(regionId, getActiveGeometry());
                } else {
                    setGeometrySelection(regionId, row, col);
                    pushGeometryUndo(regionId, getActiveGeometry());
                }
                const dragging = { active: true, pointIndex: dragPointIndex, regionId, target: state.page.activeTab === 'blend' ? 'manager-point' : 'geometry-point' };
                if (state.page.activeTab === 'geometry' || state.page.activeTab === 'blend') {
                    dragging.lastU = active.points[dragPointIndex].u;
                    dragging.lastV = active.points[dragPointIndex].v;
                }
                patchInteraction(dragging);
                return {
                    geometrySelectionChanged: state.page.activeTab === 'geometry' || state.page.activeTab === 'blend',
                    maskSelectionChanged: state.page.activeTab === 'mask'
                };
            } else if (state.page.activeTab === 'blend') {
                const line = hitTestGeometryBoundaryLine(active, previewLayout, mx, my);
                if (line) {
                    setActiveRegion(regionId);
                    setActiveBlendSide(line.side);
                    setManagerSideSelection(regionId, line.side);
                    const geometry = state.geometry.byRegionId[regionId];
                    setActiveBlendCorner(getManagerCornerName(geometry));
                    pushGeometryUndo(regionId, getActiveGeometry());
                    const corner = getManagerCornerSelection(geometry);
                    const pointIndex = corner.row * active.cols + corner.col;
                    const geometryPoint = outputScreenToGeometryPoint(previewLayout, mx, my);
                    patchInteraction({
                        active: true,
                        pointIndex,
                        regionId,
                        target: 'manager-line',
                        direction: line.direction,
                        side: line.side,
                        lastU: geometryPoint?.u ?? 0,
                        lastV: geometryPoint?.v ?? 0
                    });
                    return { geometrySelectionChanged: true, maskSelectionChanged: false };
                }
                // 只切换区域，不选中热点
                setActiveRegion(regionId);
            } else {
                // 只切换区域，不选中热点
                setActiveRegion(regionId);
            }
        }
        return { geometrySelectionChanged: false, maskSelectionChanged: false };
    }

    return { geometrySelectionChanged: false, maskSelectionChanged: false };
}

export function onCanvasMouseMove(state, canvas, event) {
    if (!state.interaction.dragging.active) return;
    const dragTargetState = state.interaction.dragging.target === 'mask-point'
        ? getActiveMask()
        : state.geometry.byRegionId[state.interaction.dragging.regionId];
    if (!canEditTarget(state, dragTargetState)) {
        patchInteraction({ active: false, pointIndex: -1, regionId: null, target: null });
        return;
    }
    const rect = canvas.getBoundingClientRect();
    const mx = event.clientX - rect.left;
    const my = event.clientY - rect.top;
    
    let drawLayout;
    let maskCanvasPoint = null;
    if (state.page.focusMode === FOCUS_MODES.LAYOUT && state.layout.regionIds.length > 0) {
        if (state.interaction.dragging.target === 'mask-point') {
            maskCanvasPoint = screenToTransformedMaskCanvasPoint(canvas, state, state.interaction.dragging.regionId, mx, my);
            if (!maskCanvasPoint) return;
        } else {
            const layout = getRegionPreviewLayoutById(canvas, state, state.interaction.dragging.regionId);
            if (!layout?.rect) return;
            drawLayout = layout;
        }
    } else {
        return;
    }
    
    const localPoint = drawLayout && !maskCanvasPoint ? outputScreenToGeometryPoint(drawLayout, mx, my) : null;
    const u = maskCanvasPoint ? maskCanvasPoint.u : (localPoint?.u ?? 0);
    const v = maskCanvasPoint ? maskCanvasPoint.v : (localPoint?.v ?? 0);
    if (state.page.activeTab === 'mask') {
        const lastU = state.interaction.dragging.lastU ?? u;
        const lastV = state.interaction.dragging.lastV ?? v;
        const du = u - lastU;
        const dv = v - lastV;
        if (du !== 0 || dv !== 0) {
            applyMaskMoveLocal('point', du, dv);
            patchInteraction({ lastU: u, lastV: v });
            scheduleDragRender(canvas);
            void syncMaskMoveOp('point', du, dv);
        }
    } else if (state.interaction.dragging.target === 'manager-point') {
        const lastU = state.interaction.dragging.lastU ?? u;
        const lastV = state.interaction.dragging.lastV ?? v;
        const du = u - lastU;
        const dv = v - lastV;
        snapManagerCornerSelection(state.interaction.dragging.regionId);
        setActiveBlendCorner(getManagerCornerName(dragTargetState));
        const direction = managerDirectionFromDrag(dragTargetState, state.interaction.dragging.pointIndex, du, dv);
        if (direction >= 0 && (du !== 0 || dv !== 0)) {
            patchInteraction({ lastU: u, lastV: v });
            const corner = getManagerCornerName(dragTargetState);
            applyManagerMovePointLocal(state.interaction.dragging.regionId, direction, du, dv, corner);
            scheduleDragRender(canvas);
            void syncManagerPointMoveOp(state.interaction.dragging.regionId, direction, du, dv, corner);
        }
    } else if (state.interaction.dragging.target === 'manager-line') {
        const lastU = state.interaction.dragging.lastU ?? u;
        const lastV = state.interaction.dragging.lastV ?? v;
        const du = u - lastU;
        const dv = v - lastV;
        const direction = state.interaction.dragging.direction ?? -1;
        snapManagerCornerSelection(state.interaction.dragging.regionId);
        setActiveBlendCorner(getManagerCornerName(dragTargetState));
        if (direction >= 0 && (du !== 0 || dv !== 0)) {
            patchInteraction({ lastU: u, lastV: v });
            applyManagerMoveLineLocal(state.interaction.dragging.regionId, direction, du, dv);
            scheduleDragRender(canvas);
            void syncManagerLineMoveOp(state.interaction.dragging.regionId, direction, du, dv);
        }
    } else {
        const lastU = state.interaction.dragging.lastU ?? u;
        const lastV = state.interaction.dragging.lastV ?? v;
        const du = u - lastU;
        const dv = v - lastV;
        if (du !== 0 || dv !== 0) {
            patchInteraction({ lastU: u, lastV: v });
            applyGeometryMoveLocal(state.interaction.dragging.regionId, 'point', du, dv);
            scheduleDragRender(canvas);
            void syncGeometryMoveOp(state.interaction.dragging.regionId, 'point', du, dv, false);
        }
    }
}

export function onCanvasMouseUp() {
    patchInteraction({ active: false, pointIndex: -1, regionId: null, target: null });
}

