import { ensureGlobalMask } from '../actions.js?v=2.95';
import { clamp, createPointGrid } from '../utils/grid.js';
import { normalizeFlatPoints, normalizeIndex } from '../utils/points.js';

const MASK_MAX_GRID = 128;

function toIndex(mask, row, col) {
    return row * mask.cols + col;
}

export function isMaskPerimeterPoint(mask, row, col) {
    if (!mask) return false;
    return row === 0 || col === 0 || row === mask.rows - 1 || col === mask.cols - 1;
}

export function initializeMask(rows = 2, cols = 2) {
    const mask = ensureGlobalMask();
    mask.rows = rows;
    mask.cols = cols;
    mask.points = createPointGrid(rows, cols);
    mask.selected = { row: 0, col: 0, axis: 'col' };
    mask.enabled = false;
    mask.showGrid = false;
    return mask;
}

export function hydrateMask(response = {}) {
    const mask = ensureGlobalMask();
    const remoteMask = response.mask;
    if (!remoteMask) return mask;

    const rows = clamp(Number(remoteMask.rows) || mask.rows || 2, 2, MASK_MAX_GRID);
    const cols = clamp(Number(remoteMask.cols) || mask.cols || 2, 2, MASK_MAX_GRID);
    mask.rows = rows;
    mask.cols = cols;
    mask.points = normalizeFlatPoints(remoteMask.vertices, rows, cols, {
        fallback: 'grid',
        clampPoints: false
    });
    mask.selected = {
        row: normalizeIndex(response.selected_row, rows - 1),
        col: normalizeIndex(response.selected_col, cols - 1),
        axis: mask.selected?.axis || 'col'
    };
    mask.selected = {
        ...clampMaskSelectionToPerimeter(mask, mask.selected.row, mask.selected.col, mask.selected.axis),
        axis: mask.selected.axis
    };
    mask.showGrid = typeof response.show_guide === 'boolean' ? response.show_guide : mask.showGrid;
    mask.enabled = !!remoteMask.enabled;
    mask.interpolationMode = Number(remoteMask.interpolation_mode) || 0;
    return mask;
}

export function setMaskEnabled(enabled) {
    const mask = ensureGlobalMask();
    mask.enabled = enabled;
    return mask;
}

export function setMaskGridVisible(showGrid) {
    const mask = ensureGlobalMask();
    mask.showGrid = showGrid;
    if (showGrid && (mask.selected.row < 0 || mask.selected.col < 0)) {
        mask.selected = { row: 0, col: 0, axis: mask.selected?.axis || 'col' };
    }
    return mask;
}

export function setMaskSelection(row, col, axis = null) {
    const mask = ensureGlobalMask();
    const next = clampMaskSelectionToPerimeter(
        mask,
        clamp(row, 0, mask.rows - 1),
        clamp(col, 0, mask.cols - 1),
        axis || mask.selected?.axis || 'col'
    );
    mask.selected = {
        row: next.row,
        col: next.col,
        axis: axis || mask.selected?.axis || 'col'
    };
    return mask;
}

export function moveMaskSelection(dRow, dCol) {
    const mask = ensureGlobalMask();
    const axis = dCol !== 0 ? 'col' : dRow !== 0 ? 'row' : mask.selected.axis;
    return setMaskSelection(mask.selected.row + dRow, mask.selected.col + dCol, axis);
}

function clampMaskSelectionToPerimeter(mask, row, col, axis = 'col') {
    const lastRow = Math.max(0, mask.rows - 1);
    const lastCol = Math.max(0, mask.cols - 1);
    if (row === 0 || row === lastRow || col === 0 || col === lastCol) {
        return { row, col };
    }
    const distances = [
        { row: 0, col, distance: row },
        { row: lastRow, col, distance: lastRow - row },
        { row, col: 0, distance: col },
        { row, col: lastCol, distance: lastCol - col }
    ];
    const preferHorizontal = axis === 'col';
    distances.sort((a, b) => {
        if (a.distance !== b.distance) return a.distance - b.distance;
        const aHorizontal = a.row === row;
        const bHorizontal = b.row === row;
        if (aHorizontal !== bHorizontal) return aHorizontal === preferHorizontal ? -1 : 1;
        return 0;
    });
    return distances[0];
}

function moveMaskPoint(mask, row, col, du, dv) {
    const point = mask.points[toIndex(mask, row, col)];
    if (!point) return;
    point.u += du;
    point.v += dv;
}

export function applyMaskMoveLocal(op, du, dv) {
    const mask = ensureGlobalMask();
    if ((op === 'point' || op === 'row' || op === 'col') && (mask.selected.row < 0 || mask.selected.col < 0)) return mask;
    const next = clampMaskSelectionToPerimeter(mask, mask.selected.row, mask.selected.col, mask.selected.axis);
    mask.selected = { row: next.row, col: next.col, axis: mask.selected.axis || 'col' };
    if (op === 'row') {
        const row = clamp(mask.selected.row, 0, mask.rows - 1);
        for (let col = 0; col < mask.cols; col += 1) moveMaskPoint(mask, row, col, du, dv);
    } else if (op === 'col') {
        const col = clamp(mask.selected.col, 0, mask.cols - 1);
        for (let row = 0; row < mask.rows; row += 1) moveMaskPoint(mask, row, col, du, dv);
    } else if (op === 'all') {
        for (let row = 0; row < mask.rows; row += 1) {
            for (let col = 0; col < mask.cols; col += 1) moveMaskPoint(mask, row, col, du, dv);
        }
    } else {
        moveMaskPoint(mask, mask.selected.row, mask.selected.col, du, dv);
    }
    return mask;
}

export function setMaskInterpolationMode(mode) {
    const mask = ensureGlobalMask();
    mask.interpolationMode = mode;
    return mask;
}
