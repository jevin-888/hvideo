import { projectionCorrectionApi } from './projectionCorrection/api.js?v=1.1';

let initialized = false;
let activeRegionId = 1;
let currentConfig = null;
let regionLayout = { rows: 1, cols: 1, regions: [] };
let canvasDrag = null;
let visualDrawPending = false;
let visualResizeObserver = null;
let caveViewMode = 'front';
let caveDrag = null;
let caveDrawPending = false;
let caveResizeObserver = null;

const $ = (selector) => document.querySelector(selector);

function numberValue(id, fallback = 0) {
    const value = Number($(`#${id}`)?.value);
    return Number.isFinite(value) ? value : fallback;
}

function boolValue(id) {
    return !!$(`#${id}`)?.checked;
}

function setValue(id, value) {
    const element = $(`#${id}`);
    if (!element) return;
    element.value = value ?? '';
}

function setChecked(id, checked) {
    const element = $(`#${id}`);
    if (!element) return;
    element.checked = !!checked;
}

function setStatus(message, type = 'info') {
    const status = $('#projectionCorrectionStatus');
    if (!status) return;
    status.textContent = message || '';
    status.dataset.type = type;
}

function clampNumber(value, min, max) {
    const number = Number(value);
    if (!Number.isFinite(number)) return min;
    return Math.min(max, Math.max(min, number));
}

function roundedValue(value, digits = 4) {
    const fixed = Number(value).toFixed(digits);
    return fixed.replace(/\.?0+$/, '');
}

function ensurePageShell() {
    let page = $('#projection-correction-page');
    if (page) return page;

    const contentArea = document.querySelector('.content-area .page-content');
    if (!contentArea) return null;

    page = document.createElement('div');
    page.id = 'projection-correction-page';
    page.className = 'page projection-correction-page';
    page.innerHTML = `
        <div class="projection-correction-shell">
            <div class="projection-correction-header">
                <div>
                    <h2>CAVE / 矩阵校正</h2>
                </div>
                <div class="projection-correction-actions">
                    <select id="projectionCorrectionRegion" class="form-control"></select>
                    <button id="projectionCorrectionRefresh" class="btn secondary" type="button">刷新</button>
                    <button id="projectionCorrectionSaveFile" class="btn secondary" type="button">保存配置文件</button>
                </div>
            </div>
            <div class="projection-correction-grid">
                <div class="projection-correction-control-stack">
                <section class="projection-correction-panel projection-correction-panel--visual">
                    <div class="projection-correction-panel__title">图形校正</div>
                    <div id="projectionCorrectionRegionMap" class="projection-correction-region-map"></div>
                    <div class="projection-correction-canvas-frame">
                        <canvas id="projectionCorrectionCanvas"></canvas>
                    </div>
                    <div class="projection-correction-visual-actions">
                        <button id="projectionCorrectionResetMatrix" class="btn secondary" type="button">重置矩阵</button>
                    </div>
                </section>
                <section class="projection-correction-panel">
                    <div class="projection-correction-panel__title">矩阵校正</div>
                    <label class="projection-correction-toggle"><input id="pcMatrixEnabled" type="checkbox"><span>启用矩阵校正</span></label>
                    <div class="projection-correction-fields">
                        <label><span>Offset X</span><input id="pcOffsetX" class="form-control" type="number" step="0.001"></label>
                        <label><span>Offset Y</span><input id="pcOffsetY" class="form-control" type="number" step="0.001"></label>
                        <label><span>Scale X</span><input id="pcScaleX" class="form-control" type="number" step="0.001"></label>
                        <label><span>Scale Y</span><input id="pcScaleY" class="form-control" type="number" step="0.001"></label>
                        <label><span>Rotate Rad</span><input id="pcRotateRad" class="form-control" type="number" step="0.001"></label>
                        <label><span>Keystone X</span><input id="pcKeystoneX" class="form-control" type="number" step="0.001"></label>
                        <label><span>Keystone Y</span><input id="pcKeystoneY" class="form-control" type="number" step="0.001"></label>
                    </div>
                </section>
                </div>
                <section class="projection-correction-panel">
                    <div class="projection-correction-panel__title">CAVE 投影</div>
                    <label class="projection-correction-toggle"><input id="pcCaveEnabled" type="checkbox"><span>启用 CAVE 投影</span></label>
                    <div class="projection-correction-cave-space">
                        <div class="projection-correction-view-tabs" id="projectionCorrectionCaveViews">
                            <button class="btn secondary active" type="button" data-cave-view="front">Front</button>
                            <button class="btn secondary" type="button" data-cave-view="top">Top</button>
                            <button class="btn secondary" type="button" data-cave-view="side">Side</button>
                            <button class="btn secondary" type="button" data-cave-view="iso">Iso</button>
                        </div>
                        <div class="projection-correction-cave-frame">
                            <canvas id="projectionCorrectionCaveCanvas"></canvas>
                        </div>
                        <div class="projection-correction-visual-actions">
                            <button id="projectionCorrectionResetCaveWall" class="btn secondary" type="button">重置墙面</button>
                        </div>
                    </div>
                    <div class="projection-correction-fields">
                        <label><span>Wall Type</span><select id="pcWallType" class="form-control">
                            <option value="0">Front</option>
                            <option value="1">Left</option>
                            <option value="2">Right</option>
                            <option value="3">Floor</option>
                            <option value="4">Ceiling</option>
                        </select></label>
                        <label><span>Eye Distance</span><input id="pcEyeDistance" class="form-control" type="number" step="0.001"></label>
                        <label><span>Near Plane</span><input id="pcNearPlane" class="form-control" type="number" step="0.001"></label>
                        <label><span>Far Plane</span><input id="pcFarPlane" class="form-control" type="number" step="0.1"></label>
                        <label><span>LL X</span><input id="pcLlx" class="form-control" type="number" step="0.001"></label>
                        <label><span>LL Y</span><input id="pcLly" class="form-control" type="number" step="0.001"></label>
                        <label><span>LL Z</span><input id="pcLlz" class="form-control" type="number" step="0.001"></label>
                        <label><span>UL X</span><input id="pcUlx" class="form-control" type="number" step="0.001"></label>
                        <label><span>UL Y</span><input id="pcUly" class="form-control" type="number" step="0.001"></label>
                        <label><span>UL Z</span><input id="pcUlz" class="form-control" type="number" step="0.001"></label>
                        <label><span>LR X</span><input id="pcLrx" class="form-control" type="number" step="0.001"></label>
                        <label><span>LR Y</span><input id="pcLry" class="form-control" type="number" step="0.001"></label>
                        <label><span>LR Z</span><input id="pcLrz" class="form-control" type="number" step="0.001"></label>
                    </div>
                </section>
            </div>
            <div class="projection-correction-footer">
                <button id="projectionCorrectionApply" class="btn primary" type="button">应用到当前区域</button>
                <button id="projectionCorrectionReload" class="btn secondary" type="button">重新读取</button>
                <span id="projectionCorrectionStatus" class="projection-correction-status"></span>
            </div>
            <div id="projectionCorrectionPath" class="projection-correction-path"></div>
        </div>
    `;
    contentArea.appendChild(page);
    return page;
}

function normalizeMatrix(matrix = {}) {
    return {
        enabled: !!matrix.enabled,
        offset_x: Number(matrix.offset_x ?? matrix.offsetX ?? 0),
        offset_y: Number(matrix.offset_y ?? matrix.offsetY ?? 0),
        scale_x: Number(matrix.scale_x ?? matrix.scaleX ?? 1),
        scale_y: Number(matrix.scale_y ?? matrix.scaleY ?? 1),
        rotate_rad: Number(matrix.rotate_rad ?? matrix.rotateRad ?? 0),
        keystone_x: Number(matrix.keystone_x ?? matrix.keystoneX ?? 0),
        keystone_y: Number(matrix.keystone_y ?? matrix.keystoneY ?? 0)
    };
}

function normalizeCave(cave = {}) {
    return {
        enabled: !!cave.enabled,
        wall_type: Number(cave.wall_type ?? cave.wallType ?? 0),
        eye_distance: Number(cave.eye_distance ?? cave.eyeDistance ?? 0.065),
        near_plane: Number(cave.near_plane ?? cave.nearPlane ?? 0.1),
        far_plane: Number(cave.far_plane ?? cave.farPlane ?? 100),
        llx: Number(cave.llx ?? 0),
        lly: Number(cave.lly ?? 0),
        llz: Number(cave.llz ?? 0),
        ulx: Number(cave.ulx ?? 0),
        uly: Number(cave.uly ?? 0),
        ulz: Number(cave.ulz ?? 0),
        lrx: Number(cave.lrx ?? 0),
        lry: Number(cave.lry ?? 0),
        lrz: Number(cave.lrz ?? 0)
    };
}

function fillForm(config = {}) {
    const matrix = normalizeMatrix(config.matrix_correction || config.matrix || {});
    const cave = normalizeCave(config.cave || config.cave_wall || {});
    setChecked('pcMatrixEnabled', matrix.enabled);
    setValue('pcOffsetX', matrix.offset_x);
    setValue('pcOffsetY', matrix.offset_y);
    setValue('pcScaleX', matrix.scale_x);
    setValue('pcScaleY', matrix.scale_y);
    setValue('pcRotateRad', matrix.rotate_rad);
    setValue('pcKeystoneX', matrix.keystone_x);
    setValue('pcKeystoneY', matrix.keystone_y);

    setChecked('pcCaveEnabled', cave.enabled);
    setValue('pcWallType', cave.wall_type);
    setValue('pcEyeDistance', cave.eye_distance);
    setValue('pcNearPlane', cave.near_plane);
    setValue('pcFarPlane', cave.far_plane);
    setValue('pcLlx', cave.llx);
    setValue('pcLly', cave.lly);
    setValue('pcLlz', cave.llz);
    setValue('pcUlx', cave.ulx);
    setValue('pcUly', cave.uly);
    setValue('pcUlz', cave.ulz);
    setValue('pcLrx', cave.lrx);
    setValue('pcLry', cave.lry);
    setValue('pcLrz', cave.lrz);
    drawCorrectionCanvas();
    drawCaveSpaceCanvas();
}

function readForm() {
    return {
        matrix_correction: {
            enabled: boolValue('pcMatrixEnabled'),
            offset_x: numberValue('pcOffsetX', 0),
            offset_y: numberValue('pcOffsetY', 0),
            scale_x: numberValue('pcScaleX', 1),
            scale_y: numberValue('pcScaleY', 1),
            rotate_rad: numberValue('pcRotateRad', 0),
            keystone_x: numberValue('pcKeystoneX', 0),
            keystone_y: numberValue('pcKeystoneY', 0)
        },
        cave: {
            enabled: boolValue('pcCaveEnabled'),
            wall_type: Math.round(numberValue('pcWallType', 0)),
            eye_distance: numberValue('pcEyeDistance', 0.065),
            near_plane: numberValue('pcNearPlane', 0.1),
            far_plane: numberValue('pcFarPlane', 100),
            llx: numberValue('pcLlx', 0),
            lly: numberValue('pcLly', 0),
            llz: numberValue('pcLlz', 0),
            ulx: numberValue('pcUlx', 0),
            uly: numberValue('pcUly', 0),
            ulz: numberValue('pcUlz', 0),
            lrx: numberValue('pcLrx', 0),
            lry: numberValue('pcLry', 0),
            lrz: numberValue('pcLrz', 0)
        }
    };
}

function readMatrixForm() {
    return readForm().matrix_correction;
}

function writeMatrixForm(matrix = {}) {
    setChecked('pcMatrixEnabled', matrix.enabled !== false);
    setValue('pcOffsetX', roundedValue(matrix.offset_x ?? 0));
    setValue('pcOffsetY', roundedValue(matrix.offset_y ?? 0));
    setValue('pcScaleX', roundedValue(matrix.scale_x ?? 1));
    setValue('pcScaleY', roundedValue(matrix.scale_y ?? 1));
    setValue('pcRotateRad', roundedValue(matrix.rotate_rad ?? 0));
    setValue('pcKeystoneX', roundedValue(matrix.keystone_x ?? 0));
    setValue('pcKeystoneY', roundedValue(matrix.keystone_y ?? 0));
    drawCorrectionCanvas();
}

function resetMatrixForm() {
    writeMatrixForm({
        enabled: false,
        offset_x: 0,
        offset_y: 0,
        scale_x: 1,
        scale_y: 1,
        rotate_rad: 0,
        keystone_x: 0,
        keystone_y: 0
    });
}

function applyMatrixProjection(point, matrix) {
    let x = point.x;
    let y = point.y;
    const wk = Math.max(0.08, 1 + matrix.keystone_x * y + matrix.keystone_y * x);
    x /= wk;
    y /= wk;
    x *= matrix.scale_x;
    y *= matrix.scale_y;
    const c = Math.cos(matrix.rotate_rad);
    const s = Math.sin(matrix.rotate_rad);
    return {
        x: x * c - y * s + matrix.offset_x,
        y: x * s + y * c + matrix.offset_y
    };
}

function getCanvasMetrics(canvas) {
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(1, rect.width);
    const height = Math.max(1, rect.height);
    const pad = Math.min(42, Math.max(24, Math.min(width, height) * 0.1));
    return {
        width,
        height,
        pad,
        plotX: pad,
        plotY: pad,
        plotW: Math.max(1, width - pad * 2),
        plotH: Math.max(1, height - pad * 2)
    };
}

function ndcToCanvas(point, metrics) {
    return {
        x: metrics.plotX + (point.x * 0.5 + 0.5) * metrics.plotW,
        y: metrics.plotY + (1 - (point.y * 0.5 + 0.5)) * metrics.plotH
    };
}

function distance(a, b) {
    return Math.hypot(a.x - b.x, a.y - b.y);
}

function pointInPolygon(point, polygon) {
    let inside = false;
    for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
        const pi = polygon[i];
        const pj = polygon[j];
        const intersect = ((pi.y > point.y) !== (pj.y > point.y)) &&
            (point.x < ((pj.x - pi.x) * (point.y - pi.y)) / ((pj.y - pi.y) || 1e-6) + pi.x);
        if (intersect) inside = !inside;
    }
    return inside;
}

function getVisualGeometry() {
    const canvas = $('#projectionCorrectionCanvas');
    if (!canvas) return null;
    const metrics = getCanvasMetrics(canvas);
    const matrix = normalizeMatrix(readMatrixForm());
    const base = {
        tl: { x: -0.58, y: 0.38 },
        tr: { x: 0.58, y: 0.38 },
        br: { x: 0.58, y: -0.38 },
        bl: { x: -0.58, y: -0.38 }
    };
    const corners = Object.fromEntries(Object.entries(base).map(([key, point]) => [
        key,
        ndcToCanvas(applyMatrixProjection(point, matrix), metrics)
    ]));
    const baseCorners = Object.fromEntries(Object.entries(base).map(([key, point]) => [
        key,
        ndcToCanvas(point, metrics)
    ]));
    const midpoint = (a, b) => ({ x: (a.x + b.x) * 0.5, y: (a.y + b.y) * 0.5 });
    const center = {
        x: (corners.tl.x + corners.tr.x + corners.br.x + corners.bl.x) * 0.25,
        y: (corners.tl.y + corners.tr.y + corners.br.y + corners.bl.y) * 0.25
    };
    return {
        metrics,
        matrix,
        corners,
        baseCorners,
        polygon: [corners.tl, corners.tr, corners.br, corners.bl],
        handles: {
            center,
            'corner-tl': corners.tl,
            'corner-tr': corners.tr,
            'corner-br': corners.br,
            'corner-bl': corners.bl,
            'edge-top': midpoint(corners.tl, corners.tr),
            'edge-right': midpoint(corners.tr, corners.br),
            'edge-bottom': midpoint(corners.bl, corners.br),
            'edge-left': midpoint(corners.tl, corners.bl)
        }
    };
}

function drawCorrectionCanvas() {
    if (visualDrawPending) return;
    visualDrawPending = true;
    requestAnimationFrame(() => {
        visualDrawPending = false;
        const canvas = $('#projectionCorrectionCanvas');
        if (!canvas) return;
        const rect = canvas.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        const width = Math.max(1, Math.round(rect.width * dpr));
        const height = Math.max(1, Math.round(rect.height * dpr));
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
        }
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, rect.width, rect.height);
        const geometry = getVisualGeometry();
        if (!geometry) return;
        const { metrics, corners, baseCorners, polygon, handles, matrix } = geometry;

        ctx.fillStyle = 'rgba(5, 10, 24, 0.82)';
        ctx.fillRect(0, 0, metrics.width, metrics.height);

        ctx.strokeStyle = 'rgba(148, 163, 184, 0.12)';
        ctx.lineWidth = 1;
        for (let i = 0; i <= 8; i += 1) {
            const x = metrics.plotX + (metrics.plotW * i) / 8;
            const y = metrics.plotY + (metrics.plotH * i) / 8;
            ctx.beginPath();
            ctx.moveTo(x, metrics.plotY);
            ctx.lineTo(x, metrics.plotY + metrics.plotH);
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(metrics.plotX, y);
            ctx.lineTo(metrics.plotX + metrics.plotW, y);
            ctx.stroke();
        }

        ctx.setLineDash([6, 5]);
        ctx.strokeStyle = 'rgba(148, 163, 184, 0.45)';
        ctx.lineWidth = 1.25;
        ctx.beginPath();
        ctx.moveTo(baseCorners.tl.x, baseCorners.tl.y);
        ctx.lineTo(baseCorners.tr.x, baseCorners.tr.y);
        ctx.lineTo(baseCorners.br.x, baseCorners.br.y);
        ctx.lineTo(baseCorners.bl.x, baseCorners.bl.y);
        ctx.closePath();
        ctx.stroke();
        ctx.setLineDash([]);

        ctx.fillStyle = matrix.enabled ? 'rgba(102, 126, 234, 0.24)' : 'rgba(148, 163, 184, 0.16)';
        ctx.strokeStyle = matrix.enabled ? '#8da2ff' : 'rgba(203, 213, 225, 0.72)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(polygon[0].x, polygon[0].y);
        polygon.slice(1).forEach(point => ctx.lineTo(point.x, point.y));
        ctx.closePath();
        ctx.fill();
        ctx.stroke();

        ctx.strokeStyle = 'rgba(255, 255, 255, 0.2)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo((corners.tl.x + corners.bl.x) * 0.5, (corners.tl.y + corners.bl.y) * 0.5);
        ctx.lineTo((corners.tr.x + corners.br.x) * 0.5, (corners.tr.y + corners.br.y) * 0.5);
        ctx.moveTo((corners.tl.x + corners.tr.x) * 0.5, (corners.tl.y + corners.tr.y) * 0.5);
        ctx.lineTo((corners.bl.x + corners.br.x) * 0.5, (corners.bl.y + corners.br.y) * 0.5);
        ctx.stroke();

        Object.entries(handles).forEach(([key, point]) => {
            if (key === 'center') {
                ctx.fillStyle = '#38ef7d';
                ctx.beginPath();
                ctx.arc(point.x, point.y, 6, 0, Math.PI * 2);
                ctx.fill();
                return;
            }
            ctx.fillStyle = key.startsWith('corner') ? '#fbbf24' : '#93c5fd';
            ctx.strokeStyle = 'rgba(15, 23, 42, 0.9)';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.rect(point.x - 5, point.y - 5, 10, 10);
            ctx.fill();
            ctx.stroke();
        });

        ctx.fillStyle = 'rgba(226, 232, 240, 0.78)';
        ctx.font = '12px sans-serif';
        ctx.fillText(`区域 ${activeRegionId}`, 14, 22);
    });
}

function getCanvasPointer(event) {
    const canvas = $('#projectionCorrectionCanvas');
    if (!canvas) return null;
    const rect = canvas.getBoundingClientRect();
    return {
        x: event.clientX - rect.left,
        y: event.clientY - rect.top
    };
}

function getCanvasHit(point) {
    const geometry = getVisualGeometry();
    if (!geometry || !point) return null;
    const hitRadius = 14;
    const handleOrder = [
        'center',
        'corner-tl',
        'corner-tr',
        'corner-br',
        'corner-bl',
        'edge-top',
        'edge-right',
        'edge-bottom',
        'edge-left'
    ];
    for (const key of handleOrder) {
        if (distance(point, geometry.handles[key]) <= hitRadius) {
            return { handle: key, geometry };
        }
    }
    if (pointInPolygon(point, geometry.polygon)) {
        return { handle: 'body', geometry };
    }
    return null;
}

function cursorForHandle(handle) {
    if (!handle) return 'default';
    if (handle === 'body' || handle === 'center') return 'move';
    if (handle === 'edge-left' || handle === 'edge-right') return 'ew-resize';
    if (handle === 'edge-top' || handle === 'edge-bottom') return 'ns-resize';
    return 'crosshair';
}

function updateCanvasCursor(event) {
    const canvas = $('#projectionCorrectionCanvas');
    if (!canvas || canvasDrag) return;
    const hit = getCanvasHit(getCanvasPointer(event));
    canvas.style.cursor = cursorForHandle(hit?.handle);
}

function updateMatrixFromCanvasDrag(point) {
    if (!canvasDrag || !point) return;
    const { metrics } = canvasDrag.geometry;
    const dx = point.x - canvasDrag.startPoint.x;
    const dy = point.y - canvasDrag.startPoint.y;
    const dxNdc = dx / Math.max(1, metrics.plotW * 0.5);
    const dyNdc = -dy / Math.max(1, metrics.plotH * 0.5);
    const start = canvasDrag.startMatrix;
    const matrix = { ...start, enabled: true };

    if (canvasDrag.handle === 'body' || canvasDrag.handle === 'center') {
        matrix.offset_x = clampNumber(start.offset_x + dxNdc, -2, 2);
        matrix.offset_y = clampNumber(start.offset_y + dyNdc, -2, 2);
    } else if (canvasDrag.handle === 'edge-left' || canvasDrag.handle === 'edge-right') {
        const direction = canvasDrag.handle === 'edge-right' ? 1 : -1;
        matrix.scale_x = clampNumber(start.scale_x + direction * dxNdc, 0.1, 5);
    } else if (canvasDrag.handle === 'edge-top' || canvasDrag.handle === 'edge-bottom') {
        const direction = canvasDrag.handle === 'edge-top' ? 1 : -1;
        matrix.scale_y = clampNumber(start.scale_y + direction * dyNdc, 0.1, 5);
    } else if (canvasDrag.handle.startsWith('corner')) {
        const xSign = canvasDrag.handle.endsWith('tr') || canvasDrag.handle.endsWith('br') ? 1 : -1;
        const ySign = canvasDrag.handle.endsWith('tl') || canvasDrag.handle.endsWith('tr') ? 1 : -1;
        matrix.keystone_y = clampNumber(start.keystone_y - dxNdc * xSign * 0.8, -0.95, 0.95);
        matrix.keystone_x = clampNumber(start.keystone_x - dyNdc * ySign * 0.8, -0.95, 0.95);
    }

    writeMatrixForm(matrix);
    setStatus(`区域 ${activeRegionId} 已调整`, 'info');
}

function bindVisualCanvas() {
    const canvas = $('#projectionCorrectionCanvas');
    if (!canvas) return;
    canvas.addEventListener('pointerdown', (event) => {
        const hit = getCanvasHit(getCanvasPointer(event));
        if (!hit) return;
        event.preventDefault();
        canvas.setPointerCapture?.(event.pointerId);
        canvasDrag = {
            pointerId: event.pointerId,
            handle: hit.handle,
            geometry: hit.geometry,
            startPoint: getCanvasPointer(event),
            startMatrix: normalizeMatrix(readMatrixForm())
        };
    });
    canvas.addEventListener('pointermove', (event) => {
        if (!canvasDrag) {
            updateCanvasCursor(event);
            return;
        }
        event.preventDefault();
        updateMatrixFromCanvasDrag(getCanvasPointer(event));
    });
    const finishDrag = (event) => {
        if (!canvasDrag) return;
        canvas.releasePointerCapture?.(canvasDrag.pointerId);
        canvasDrag = null;
        saveCorrection().catch(error => setStatus(error.message || '保存失败', 'error'));
        updateCanvasCursor(event);
    };
    canvas.addEventListener('pointerup', finishDrag);
    canvas.addEventListener('pointercancel', finishDrag);

    if (!visualResizeObserver && typeof ResizeObserver !== 'undefined') {
        visualResizeObserver = new ResizeObserver(() => drawCorrectionCanvas());
        visualResizeObserver.observe(canvas);
    }
}

const CAVE_POINT_FIELDS = {
    ll: { x: 'pcLlx', y: 'pcLly', z: 'pcLlz', label: 'LL' },
    ul: { x: 'pcUlx', y: 'pcUly', z: 'pcUlz', label: 'UL' },
    lr: { x: 'pcLrx', y: 'pcLry', z: 'pcLrz', label: 'LR' }
};

const CAVE_VIEW_AXES = {
    front: { h: 'x', v: 'y' },
    top: { h: 'x', v: 'z' },
    side: { h: 'z', v: 'y' }
};

function readCaveForm() {
    return readForm().cave;
}

function writeCaveForm(cave = {}) {
    setChecked('pcCaveEnabled', cave.enabled !== false);
    setValue('pcWallType', Math.round(cave.wall_type ?? numberValue('pcWallType', 0)));
    setValue('pcEyeDistance', roundedValue(cave.eye_distance ?? numberValue('pcEyeDistance', 0.065)));
    setValue('pcNearPlane', roundedValue(cave.near_plane ?? numberValue('pcNearPlane', 0.1)));
    setValue('pcFarPlane', roundedValue(cave.far_plane ?? numberValue('pcFarPlane', 100)));
    Object.entries(CAVE_POINT_FIELDS).forEach(([key, fields]) => {
        ['x', 'y', 'z'].forEach((axis) => {
            const value = cave[`${key}${axis}`];
            if (value !== undefined) setValue(fields[axis], roundedValue(value));
        });
    });
    drawCaveSpaceCanvas();
}

function getCavePoints(cave = readCaveForm()) {
    const ll = { x: cave.llx, y: cave.lly, z: cave.llz };
    const ul = { x: cave.ulx, y: cave.uly, z: cave.ulz };
    const lr = { x: cave.lrx, y: cave.lry, z: cave.lrz };
    const ur = {
        x: ul.x + lr.x - ll.x,
        y: ul.y + lr.y - ll.y,
        z: ul.z + lr.z - ll.z
    };
    return { ll, ul, lr, ur };
}

function getCaveCanvasMetrics(canvas) {
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(1, rect.width);
    const height = Math.max(1, rect.height);
    const pad = Math.min(44, Math.max(26, Math.min(width, height) * 0.11));
    return {
        width,
        height,
        plotX: pad,
        plotY: pad,
        plotW: Math.max(1, width - pad * 2),
        plotH: Math.max(1, height - pad * 2)
    };
}

function caveAxisRange(points, hAxis, vAxis) {
    const values = Object.values(points);
    let minH = Math.min(-1, ...values.map(point => point[hAxis]));
    let maxH = Math.max(1, ...values.map(point => point[hAxis]));
    let minV = Math.min(-1, ...values.map(point => point[vAxis]));
    let maxV = Math.max(1, ...values.map(point => point[vAxis]));
    const spanH = Math.max(0.5, maxH - minH);
    const spanV = Math.max(0.5, maxV - minV);
    minH -= spanH * 0.18;
    maxH += spanH * 0.18;
    minV -= spanV * 0.18;
    maxV += spanV * 0.18;
    return { minH, maxH, minV, maxV };
}

function cavePointToCanvas(point, geometry) {
    const { metrics, hAxis, vAxis, range } = geometry;
    return {
        x: metrics.plotX + ((point[hAxis] - range.minH) / Math.max(1e-6, range.maxH - range.minH)) * metrics.plotW,
        y: metrics.plotY + (1 - ((point[vAxis] - range.minV) / Math.max(1e-6, range.maxV - range.minV))) * metrics.plotH
    };
}

function isoProjectPoint(point) {
    return {
        h: (point.x - point.z) * 0.72,
        v: point.y - (point.x + point.z) * 0.32
    };
}

function getCaveSpaceGeometry() {
    const canvas = $('#projectionCorrectionCaveCanvas');
    if (!canvas) return null;
    const metrics = getCaveCanvasMetrics(canvas);
    const points = getCavePoints();
    if (caveViewMode === 'iso') {
        const projected = Object.fromEntries(Object.entries(points).map(([key, point]) => [
            key,
            isoProjectPoint(point)
        ]));
        const range = caveAxisRange(projected, 'h', 'v');
        const pointToCanvas = (point) => ({
            x: metrics.plotX + ((point.h - range.minH) / Math.max(1e-6, range.maxH - range.minH)) * metrics.plotW,
            y: metrics.plotY + (1 - ((point.v - range.minV) / Math.max(1e-6, range.maxV - range.minV))) * metrics.plotH
        });
        return {
            metrics,
            points,
            hAxis: 'h',
            vAxis: 'v',
            range,
            canvasPoints: Object.fromEntries(Object.entries(projected).map(([key, point]) => [key, pointToCanvas(point)])),
            editable: false
        };
    }
    const axes = CAVE_VIEW_AXES[caveViewMode] || CAVE_VIEW_AXES.front;
    const range = caveAxisRange(points, axes.h, axes.v);
    const baseGeometry = { metrics, hAxis: axes.h, vAxis: axes.v, range };
    return {
        ...baseGeometry,
        points,
        canvasPoints: Object.fromEntries(Object.entries(points).map(([key, point]) => [key, cavePointToCanvas(point, baseGeometry)])),
        editable: true
    };
}

function drawCaveGrid(ctx, geometry) {
    const { metrics } = geometry;
    ctx.strokeStyle = 'rgba(148, 163, 184, 0.12)';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 8; i += 1) {
        const x = metrics.plotX + (metrics.plotW * i) / 8;
        const y = metrics.plotY + (metrics.plotH * i) / 8;
        ctx.beginPath();
        ctx.moveTo(x, metrics.plotY);
        ctx.lineTo(x, metrics.plotY + metrics.plotH);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(metrics.plotX, y);
        ctx.lineTo(metrics.plotX + metrics.plotW, y);
        ctx.stroke();
    }
}

function drawCaveSpaceCanvas() {
    if (caveDrawPending) return;
    caveDrawPending = true;
    requestAnimationFrame(() => {
        caveDrawPending = false;
        const canvas = $('#projectionCorrectionCaveCanvas');
        if (!canvas) return;
        const rect = canvas.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        const width = Math.max(1, Math.round(rect.width * dpr));
        const height = Math.max(1, Math.round(rect.height * dpr));
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
        }
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, rect.width, rect.height);
        const geometry = getCaveSpaceGeometry();
        if (!geometry) return;
        const { metrics, canvasPoints } = geometry;

        ctx.fillStyle = 'rgba(5, 10, 24, 0.82)';
        ctx.fillRect(0, 0, metrics.width, metrics.height);
        drawCaveGrid(ctx, geometry);

        const wall = [canvasPoints.ll, canvasPoints.lr, canvasPoints.ur, canvasPoints.ul];
        ctx.fillStyle = boolValue('pcCaveEnabled') ? 'rgba(56, 239, 125, 0.16)' : 'rgba(148, 163, 184, 0.14)';
        ctx.strokeStyle = boolValue('pcCaveEnabled') ? '#38ef7d' : 'rgba(203, 213, 225, 0.68)';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(wall[0].x, wall[0].y);
        wall.slice(1).forEach(point => ctx.lineTo(point.x, point.y));
        ctx.closePath();
        ctx.fill();
        ctx.stroke();

        ctx.strokeStyle = 'rgba(255, 255, 255, 0.22)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(canvasPoints.ll.x, canvasPoints.ll.y);
        ctx.lineTo(canvasPoints.ur.x, canvasPoints.ur.y);
        ctx.moveTo(canvasPoints.ul.x, canvasPoints.ul.y);
        ctx.lineTo(canvasPoints.lr.x, canvasPoints.lr.y);
        ctx.stroke();

        const handleStyle = {
            ll: '#fbbf24',
            ul: '#93c5fd',
            lr: '#f472b6'
        };
        Object.entries(handleStyle).forEach(([key, color]) => {
            const point = canvasPoints[key];
            ctx.fillStyle = color;
            ctx.strokeStyle = 'rgba(15, 23, 42, 0.9)';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.arc(point.x, point.y, 7, 0, Math.PI * 2);
            ctx.fill();
            ctx.stroke();
            ctx.fillStyle = 'rgba(226, 232, 240, 0.92)';
            ctx.font = '12px sans-serif';
            ctx.fillText(CAVE_POINT_FIELDS[key].label, point.x + 9, point.y - 8);
        });

        ctx.fillStyle = 'rgba(148, 163, 184, 0.8)';
        ctx.font = '12px sans-serif';
        const axisText = caveViewMode === 'iso'
            ? 'ISO'
            : `${geometry.hAxis.toUpperCase()} / ${geometry.vAxis.toUpperCase()}`;
        ctx.fillText(axisText, 14, 22);
    });
}

function getCavePointer(event) {
    const canvas = $('#projectionCorrectionCaveCanvas');
    if (!canvas) return null;
    const rect = canvas.getBoundingClientRect();
    return {
        x: event.clientX - rect.left,
        y: event.clientY - rect.top
    };
}

function getCaveHit(point) {
    const geometry = getCaveSpaceGeometry();
    if (!geometry || !point || !geometry.editable) return null;
    const handleOrder = ['ll', 'ul', 'lr'];
    for (const key of handleOrder) {
        if (distance(point, geometry.canvasPoints[key]) <= 16) {
            return { key, geometry };
        }
    }
    return null;
}

function canvasToCaveAxes(point, geometry) {
    const { metrics, range } = geometry;
    const h = range.minH + ((point.x - metrics.plotX) / Math.max(1, metrics.plotW)) * (range.maxH - range.minH);
    const v = range.maxV - ((point.y - metrics.plotY) / Math.max(1, metrics.plotH)) * (range.maxV - range.minV);
    return {
        h: clampNumber(h, -10, 10),
        v: clampNumber(v, -10, 10)
    };
}

function writeCavePointFromDrag(point) {
    if (!caveDrag || !point) return;
    const values = canvasToCaveAxes(point, caveDrag.geometry);
    const fields = CAVE_POINT_FIELDS[caveDrag.key];
    if (!fields) return;
    setChecked('pcCaveEnabled', true);
    setValue(fields[caveDrag.geometry.hAxis], roundedValue(values.h));
    setValue(fields[caveDrag.geometry.vAxis], roundedValue(values.v));
    drawCaveSpaceCanvas();
    setStatus(`区域 ${activeRegionId} CAVE 空间已调整`, 'info');
}

function updateCaveCursor(event) {
    const canvas = $('#projectionCorrectionCaveCanvas');
    if (!canvas || caveDrag) return;
    const hit = getCaveHit(getCavePointer(event));
    canvas.style.cursor = hit ? 'move' : (caveViewMode === 'iso' ? 'default' : 'crosshair');
}

function bindCaveSpaceCanvas() {
    const canvas = $('#projectionCorrectionCaveCanvas');
    if (!canvas) return;
    canvas.addEventListener('pointerdown', (event) => {
        const hit = getCaveHit(getCavePointer(event));
        if (!hit) return;
        event.preventDefault();
        canvas.setPointerCapture?.(event.pointerId);
        caveDrag = {
            pointerId: event.pointerId,
            key: hit.key,
            geometry: hit.geometry
        };
        writeCavePointFromDrag(getCavePointer(event));
    });
    canvas.addEventListener('pointermove', (event) => {
        if (!caveDrag) {
            updateCaveCursor(event);
            return;
        }
        event.preventDefault();
        writeCavePointFromDrag(getCavePointer(event));
    });
    const finishDrag = (event) => {
        if (!caveDrag) return;
        canvas.releasePointerCapture?.(caveDrag.pointerId);
        caveDrag = null;
        saveCorrection().catch(error => setStatus(error.message || '保存失败', 'error'));
        updateCaveCursor(event);
    };
    canvas.addEventListener('pointerup', finishDrag);
    canvas.addEventListener('pointercancel', finishDrag);

    if (!caveResizeObserver && typeof ResizeObserver !== 'undefined') {
        caveResizeObserver = new ResizeObserver(() => drawCaveSpaceCanvas());
        caveResizeObserver.observe(canvas);
    }
}

function caveWallTemplate(wallType) {
    switch (Number(wallType)) {
        case 1:
            return { llx: -1, lly: -1, llz: 1, ulx: -1, uly: 1, ulz: 1, lrx: -1, lry: -1, lrz: -1 };
        case 2:
            return { llx: 1, lly: -1, llz: -1, ulx: 1, uly: 1, ulz: -1, lrx: 1, lry: -1, lrz: 1 };
        case 3:
            return { llx: -1, lly: -1, llz: 1, ulx: -1, uly: -1, ulz: -1, lrx: 1, lry: -1, lrz: 1 };
        case 4:
            return { llx: -1, lly: 1, llz: -1, ulx: -1, uly: 1, ulz: 1, lrx: 1, lry: 1, lrz: -1 };
        case 0:
        default:
            return { llx: -1, lly: -1, llz: 0, ulx: -1, uly: 1, ulz: 0, lrx: 1, lry: -1, lrz: 0 };
    }
}

function resetCaveWallForm() {
    const cave = readCaveForm();
    writeCaveForm({
        ...cave,
        enabled: true,
        ...caveWallTemplate(cave.wall_type)
    });
}

function renderRegionMap() {
    const map = $('#projectionCorrectionRegionMap');
    if (!map) return;
    const rows = Math.max(1, regionLayout.rows || 1);
    const cols = Math.max(1, regionLayout.cols || 1);
    map.style.gridTemplateColumns = `repeat(${cols}, minmax(42px, 1fr))`;
    map.innerHTML = '';
    const byCell = new Map();
    regionLayout.regions.forEach((region, index) => {
        const outputRow = Number(region.output_row ?? Math.floor(Number(region.output_index ?? index) / cols));
        const outputCol = Number(region.output_col ?? (Number(region.output_index ?? index) % cols));
        byCell.set(`${outputRow}:${outputCol}`, region);
    });
    for (let row = 0; row < rows; row += 1) {
        for (let col = 0; col < cols; col += 1) {
            const region = byCell.get(`${row}:${col}`);
            const cell = document.createElement('button');
            cell.type = 'button';
            cell.className = 'projection-correction-region-cell';
            if (!region) {
                cell.disabled = true;
                map.appendChild(cell);
                continue;
            }
            const id = Number(region.id || row * cols + col + 1);
            cell.textContent = String(id);
            cell.dataset.regionId = String(id);
            cell.classList.toggle('active', id === Number(activeRegionId));
            cell.addEventListener('click', async () => {
                activeRegionId = id;
                const select = $('#projectionCorrectionRegion');
                if (select) select.value = String(id);
                renderRegionMap();
                await loadCorrection().catch(error => setStatus(error.message || '读取失败', 'error'));
            });
            map.appendChild(cell);
        }
    }
}

function setConfigPath(config = {}) {
    const path = config.projection_correction_path || config.config_path || '';
    const element = $('#projectionCorrectionPath');
    if (!element) return;
    element.textContent = path ? `配置文件：${path}` : '';
}

async function loadRegions() {
    const regionSelect = $('#projectionCorrectionRegion');
    const layout = await projectionCorrectionApi.loadRegionConfig();
    const regions = Array.isArray(layout?.regions) ? layout.regions : [];
    regionLayout = {
        rows: Number(layout?.layout_out_rows ?? layout?.grid_out_rows ?? 1) || 1,
        cols: Number(layout?.layout_out_cols ?? layout?.grid_out_cols ?? regions.length ?? 1) || 1,
        regions
    };
    regionSelect.innerHTML = '';
    regions.forEach((region, index) => {
        const id = Number(region.id || index + 1);
        const option = document.createElement('option');
        option.value = String(id);
        option.textContent = `区域 ${id}`;
        regionSelect.appendChild(option);
    });
    if (!regions.length) {
        const option = document.createElement('option');
        option.value = '1';
        option.textContent = '区域 1';
        regionSelect.appendChild(option);
    }
    if (![...regionSelect.options].some(option => Number(option.value) === Number(activeRegionId))) {
        activeRegionId = Number(regionSelect.options[0]?.value || 1);
    }
    regionSelect.value = String(activeRegionId);
    renderRegionMap();
}

async function loadCorrection() {
    setStatus('正在读取...', 'info');
    currentConfig = await projectionCorrectionApi.load(activeRegionId);
    fillForm(currentConfig || {});
    setConfigPath(currentConfig || {});
    renderRegionMap();
    setStatus(`区域 ${activeRegionId} 已读取`, 'ok');
}

async function saveCorrection() {
    setStatus('正在应用...', 'info');
    currentConfig = await projectionCorrectionApi.save(activeRegionId, readForm());
    fillForm(currentConfig || {});
    setConfigPath(currentConfig || {});
    setStatus('已应用并保存到独立配置文件', 'ok');
}

async function saveConfigFileOnly() {
    setStatus('正在保存配置文件...', 'info');
    await projectionCorrectionApi.save(activeRegionId, readForm());
    const result = await projectionCorrectionApi.saveConfig(activeRegionId);
    currentConfig = result || currentConfig;
    fillForm(currentConfig || {});
    setConfigPath(currentConfig || {});
    setStatus('projection_correction.json 已保存', 'ok');
}

function bindEvents() {
    $('#projectionCorrectionRegion')?.addEventListener('change', async (event) => {
        activeRegionId = Number(event.target.value || 1);
        await loadCorrection().catch(error => setStatus(error.message || '读取失败', 'error'));
    });
    $('#projectionCorrectionRefresh')?.addEventListener('click', () => refreshProjectionCorrection());
    $('#projectionCorrectionReload')?.addEventListener('click', () => loadCorrection().catch(error => setStatus(error.message || '读取失败', 'error')));
    $('#projectionCorrectionApply')?.addEventListener('click', () => saveCorrection().catch(error => setStatus(error.message || '保存失败', 'error')));
    $('#projectionCorrectionSaveFile')?.addEventListener('click', () => saveConfigFileOnly().catch(error => setStatus(error.message || '保存失败', 'error')));
    $('#projectionCorrectionResetMatrix')?.addEventListener('click', () => {
        resetMatrixForm();
        saveCorrection().catch(error => setStatus(error.message || '保存失败', 'error'));
    });
    $('#projectionCorrectionResetCaveWall')?.addEventListener('click', () => {
        resetCaveWallForm();
        saveCorrection().catch(error => setStatus(error.message || '保存失败', 'error'));
    });
    document.querySelectorAll('#projectionCorrectionCaveViews [data-cave-view]').forEach((button) => {
        button.addEventListener('click', () => {
            caveViewMode = button.dataset.caveView || 'front';
            document.querySelectorAll('#projectionCorrectionCaveViews [data-cave-view]').forEach((item) => {
                item.classList.toggle('active', item === button);
            });
            drawCaveSpaceCanvas();
        });
    });
    [
        'pcMatrixEnabled',
        'pcOffsetX',
        'pcOffsetY',
        'pcScaleX',
        'pcScaleY',
        'pcRotateRad',
        'pcKeystoneX',
        'pcKeystoneY'
    ].forEach((id) => {
        const element = $(`#${id}`);
        element?.addEventListener('input', () => drawCorrectionCanvas());
        element?.addEventListener('change', () => drawCorrectionCanvas());
    });
    [
        'pcCaveEnabled',
        'pcWallType',
        'pcLlx',
        'pcLly',
        'pcLlz',
        'pcUlx',
        'pcUly',
        'pcUlz',
        'pcLrx',
        'pcLry',
        'pcLrz'
    ].forEach((id) => {
        const element = $(`#${id}`);
        element?.addEventListener('input', () => drawCaveSpaceCanvas());
        element?.addEventListener('change', () => drawCaveSpaceCanvas());
    });
    bindVisualCanvas();
    bindCaveSpaceCanvas();
}

export async function initializeProjectionCorrection() {
    const page = ensurePageShell();
    if (!page) return;
    if (!initialized) {
        bindEvents();
        initialized = true;
    }
    await refreshProjectionCorrection();
}

export async function refreshProjectionCorrection() {
    ensurePageShell();
    try {
        await loadRegions();
        await loadCorrection();
    } catch (error) {
        setStatus(error.message || '读取失败', 'error');
    }
}
