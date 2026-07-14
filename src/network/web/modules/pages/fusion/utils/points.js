import { clamp01, createGeometryPointGrid, createPointGrid } from './grid.js';

export function flattenPoints(points, options = {}) {
    const source = Array.isArray(points) ? points : [];
    return source.flatMap((point) => {
        const u = options.clampPoints ? clamp01(point?.u) : Number(point?.u) || 0;
        const v = options.clampPoints ? clamp01(point?.v) : Number(point?.v) || 0;
        return [u, v];
    });
}

export function normalizeIndex(value, maxIndex) {
    const max = Math.max(0, Number(maxIndex) || 0);
    const number = Number(value);
    if (!Number.isFinite(number)) return 0;
    return Math.max(0, Math.min(max, Math.round(number)));
}

export function normalizeFlatPoints(values, rows, cols, options = {}) {
    const safeRows = Math.max(1, Number(rows) || 1);
    const safeCols = Math.max(1, Number(cols) || 1);
    const total = safeRows * safeCols;
    const list = Array.isArray(values) ? values : [];
    const points = [];
    const fallbackGrid = options.geometry
        ? createGeometryPointGrid(safeRows, safeCols)
        : createPointGrid(safeRows, safeCols);

    for (let index = 0; index < total; index += 1) {
        const base = index * 2;
        const fallbackPoint = fallbackGrid[index];
        let u = Number(list[base]);
        let v = Number(list[base + 1]);

        if (!Number.isFinite(u) || !Number.isFinite(v)) {
            if (options.fallback === 'grid') {
                points.push(fallbackPoint);
                continue;
            }
            return null;
        }

        if (options.clampPoints) {
            u = clamp01(u);
            v = clamp01(v);
        }

        points.push({ u, v });
    }

    return points;
}
