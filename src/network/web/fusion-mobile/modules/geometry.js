import { clamp, createGeometryPointGrid, createPointGrid } from '../../modules/pages/fusion/utils/grid.js';
import {
    flattenPoints,
    normalizeFlatPoints,
    normalizeIndex
} from '../../modules/pages/fusion/utils/points.js';

export { clamp, createGeometryPointGrid, createPointGrid, flattenPoints, normalizeFlatPoints, normalizeIndex };

const MASK_MAX_GRID = 128;

export function hydrateGeometry(regionId, payload = {}) {
    const rows = clamp(Number(payload.rows) || 2, 2, 33);
    const cols = clamp(Number(payload.cols) || 2, 2, 33);
    return {
        regionId,
        rows,
        cols,
        interpolationMode: Number(payload.interpolation_mode) || 0,
        showGrid: !!payload.show_grid,
        selected: {
            row: normalizeIndex(payload.selected_row, rows - 1, 0),
            col: normalizeIndex(payload.selected_col, cols - 1, 0)
        },
        points: normalizeFlatPoints(payload.corners || payload.points, rows, cols, {
            exactLength: false,
            fallback: 'grid',
            geometry: true
        })
    };
}

export function hydrateMask(payload = {}) {
    const remote = payload.mask || {};
    const rows = clamp(Number(remote.rows) || 2, 2, MASK_MAX_GRID);
    const cols = clamp(Number(remote.cols) || 2, 2, MASK_MAX_GRID);
    const selected = clampSelectionToPerimeter({
        rows,
        cols,
        selected: {
            row: normalizeIndex(payload.selected_row, rows - 1, 0),
            col: normalizeIndex(payload.selected_col, cols - 1, 0)
        }
    });
    return {
        rows,
        cols,
        enabled: !!remote.enabled,
        showGrid: !!payload.show_guide,
        interpolationMode: Number(remote.interpolation_mode) || 0,
        selected,
        points: normalizeFlatPoints(remote.vertices, rows, cols, {
            exactLength: false,
            fallback: 'grid',
            clampPoints: false
        })
    };
}

export function maskPayload(mask) {
    return {
        show_guide: !!mask.showGrid,
        selected_row: mask.selected?.row ?? 0,
        selected_col: mask.selected?.col ?? 0,
        mask: {
            enabled: !!mask.enabled,
            rows: mask.rows,
            cols: mask.cols,
            interpolation_mode: mask.interpolationMode || 0,
            vertices: flattenPoints(mask.points, { clampPoints: false })
        }
    };
}

export function moveSelection(target, dRow, dCol, perimeterOnly = false) {
    if (!target) return;
    let row = normalizeIndex((target.selected?.row ?? 0) + dRow, target.rows - 1, 0);
    let col = normalizeIndex((target.selected?.col ?? 0) + dCol, target.cols - 1, 0);
    if (perimeterOnly && row > 0 && col > 0 && row < target.rows - 1 && col < target.cols - 1) {
        const distances = [
            { side: 'top', value: row },
            { side: 'bottom', value: target.rows - 1 - row },
            { side: 'left', value: col },
            { side: 'right', value: target.cols - 1 - col }
        ].sort((a, b) => a.value - b.value);
        if (distances[0].side === 'top') row = 0;
        if (distances[0].side === 'bottom') row = target.rows - 1;
        if (distances[0].side === 'left') col = 0;
        if (distances[0].side === 'right') col = target.cols - 1;
    }
    target.selected = { row, col };
}

export function clampSelectionToPerimeter(target) {
    if (!target) return { row: 0, col: 0 };
    let row = normalizeIndex(target.selected?.row, target.rows - 1, 0);
    let col = normalizeIndex(target.selected?.col, target.cols - 1, 0);
    if (row > 0 && col > 0 && row < target.rows - 1 && col < target.cols - 1) {
        const distances = [
            { row: 0, col, value: row },
            { row: target.rows - 1, col, value: target.rows - 1 - row },
            { row, col: 0, value: col },
            { row, col: target.cols - 1, value: target.cols - 1 - col }
        ].sort((a, b) => a.value - b.value);
        row = distances[0].row;
        col = distances[0].col;
    }
    target.selected = { row, col };
    return target.selected;
}

export function updateSelectedPoint(target, u, v) {
    if (!target || !target.selected) return;
    const index = target.selected.row * target.cols + target.selected.col;
    if (!target.points[index]) return;
    target.points[index] = {
        u: clamp(u, 0, 1),
        v: clamp(v, 0, 1)
    };
}

export function moveLocal(target, op, du, dv, options = {}) {
    if (!target || !Array.isArray(target.points)) return;
    const clampPoints = options.clampPoints !== false;
    const selectedRow = normalizeIndex(target.selected?.row, target.rows - 1, 0);
    const selectedCol = normalizeIndex(target.selected?.col, target.cols - 1, 0);
    const movePoint = (row, col) => {
        const index = row * target.cols + col;
        const point = target.points[index];
        if (!point) return;
        const u = point.u + du;
        const v = point.v + dv;
        point.u = clampPoints ? clamp(u, 0, 1) : u;
        point.v = clampPoints ? clamp(v, 0, 1) : v;
    };

    if (op === 'point') movePoint(selectedRow, selectedCol);
    if (op === 'row') {
        for (let col = 0; col < target.cols; col += 1) movePoint(selectedRow, col);
    }
    if (op === 'col') {
        for (let row = 0; row < target.rows; row += 1) movePoint(row, selectedCol);
    }
    if (op === 'all') {
        for (let row = 0; row < target.rows; row += 1) {
            for (let col = 0; col < target.cols; col += 1) movePoint(row, col);
        }
    }
}
