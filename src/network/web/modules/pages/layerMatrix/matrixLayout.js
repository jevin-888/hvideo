export const LAYER_MATRIX_DRAG_TYPE = 'application/x-hsvj-layer';

function toPositiveInteger(value, fallback = 1) {
    const normalized = Math.trunc(Number(value));
    return normalized > 0 ? normalized : fallback;
}

function getPartitionIndex(point, origin, length, count) {
    const relativePoint = Math.max(0, Math.min(length, Number(point) - origin));
    for (let index = 0; index < count - 1; index++) {
        const boundary = Math.round(((index + 1) * length) / count);
        if (relativePoint < boundary) return index;
    }
    return count - 1;
}

export function getMatrixLayout(canvasConfig) {
    const width = Number(canvasConfig?.width) || 0;
    const height = Number(canvasConfig?.height) || 0;
    if (width <= 0 || height <= 0) return null;

    return {
        width,
        height,
        cols: toPositiveInteger(canvasConfig?.cols),
        rows: toPositiveInteger(canvasConfig?.rows)
    };
}

export function getMatrixRegionBounds(canvasConfig, row, col) {
    const layout = getMatrixLayout(canvasConfig);
    if (!layout) return null;

    const normalizedRow = Math.max(0, Math.min(layout.rows - 1, Math.trunc(Number(row) || 0)));
    const normalizedCol = Math.max(0, Math.min(layout.cols - 1, Math.trunc(Number(col) || 0)));
    const left = Math.round((normalizedCol * layout.width) / layout.cols);
    const right = Math.round(((normalizedCol + 1) * layout.width) / layout.cols);
    const top = Math.round((normalizedRow * layout.height) / layout.rows);
    const bottom = Math.round(((normalizedRow + 1) * layout.height) / layout.rows);

    return {
        row: normalizedRow,
        col: normalizedCol,
        x: left,
        y: top,
        width: Math.max(1, right - left),
        height: Math.max(1, bottom - top)
    };
}

export function getMatrixRegionAtPoint(x, y, canvasConfig) {
    const layout = getMatrixLayout(canvasConfig);
    if (!layout) return null;

    const pointX = Number(x);
    const pointY = Number(y);
    if (!Number.isFinite(pointX) || !Number.isFinite(pointY)) return null;
    if (pointX < 0 || pointY < 0 || pointX > layout.width || pointY > layout.height) return null;

    const col = getPartitionIndex(pointX, 0, layout.width, layout.cols);
    const row = getPartitionIndex(pointY, 0, layout.height, layout.rows);
    return getMatrixRegionBounds(canvasConfig, row, col);
}

export function getMatrixSubdivisionKey(baseRow, baseCol) {
    return `${Math.trunc(Number(baseRow) || 0)}:${Math.trunc(Number(baseCol) || 0)}`;
}

function getValidSubdivision(canvasConfig, subdivision) {
    const layout = getMatrixLayout(canvasConfig);
    if (!layout || !subdivision) return null;

    const baseRow = Math.trunc(Number(subdivision.baseRow));
    const baseCol = Math.trunc(Number(subdivision.baseCol));
    const rows = Math.trunc(Number(subdivision.rows));
    const cols = Math.trunc(Number(subdivision.cols));
    if (
        !Number.isInteger(baseRow) || !Number.isInteger(baseCol) ||
        baseRow < 0 || baseRow >= layout.rows ||
        baseCol < 0 || baseCol >= layout.cols ||
        rows < 2 || cols < 2
    ) {
        return null;
    }

    return { baseRow, baseCol, rows, cols };
}

export function getMatrixSubdivisionBounds(canvasConfig, subdivision, subRow, subCol) {
    const normalized = getValidSubdivision(canvasConfig, subdivision);
    if (!normalized) return null;

    const baseBounds = getMatrixRegionBounds(
        canvasConfig,
        normalized.baseRow,
        normalized.baseCol
    );
    if (!baseBounds) return null;

    const normalizedSubRow = Math.max(
        0,
        Math.min(normalized.rows - 1, Math.trunc(Number(subRow) || 0))
    );
    const normalizedSubCol = Math.max(
        0,
        Math.min(normalized.cols - 1, Math.trunc(Number(subCol) || 0))
    );
    const left = baseBounds.x + Math.round((normalizedSubCol * baseBounds.width) / normalized.cols);
    const right = baseBounds.x + Math.round(((normalizedSubCol + 1) * baseBounds.width) / normalized.cols);
    const top = baseBounds.y + Math.round((normalizedSubRow * baseBounds.height) / normalized.rows);
    const bottom = baseBounds.y + Math.round(((normalizedSubRow + 1) * baseBounds.height) / normalized.rows);

    return {
        row: normalized.baseRow,
        col: normalized.baseCol,
        baseRow: normalized.baseRow,
        baseCol: normalized.baseCol,
        subRow: normalizedSubRow,
        subCol: normalizedSubCol,
        subdivisionRows: normalized.rows,
        subdivisionCols: normalized.cols,
        x: left,
        y: top,
        width: Math.max(1, right - left),
        height: Math.max(1, bottom - top)
    };
}

export function getMatrixDropRegionAtPoint(x, y, canvasConfig, subdivisions = []) {
    const baseRegion = getMatrixRegionAtPoint(x, y, canvasConfig);
    if (!baseRegion) return null;

    const subdivision = subdivisions.find(candidate => {
        const normalized = getValidSubdivision(canvasConfig, candidate);
        return normalized && normalized.baseRow === baseRegion.row && normalized.baseCol === baseRegion.col;
    });
    const normalized = getValidSubdivision(canvasConfig, subdivision);
    if (!normalized) return baseRegion;

    const subCol = getPartitionIndex(
        Number(x),
        baseRegion.x,
        baseRegion.width,
        normalized.cols
    );
    const subRow = getPartitionIndex(
        Number(y),
        baseRegion.y,
        baseRegion.height,
        normalized.rows
    );
    return getMatrixSubdivisionBounds(canvasConfig, normalized, subRow, subCol);
}

