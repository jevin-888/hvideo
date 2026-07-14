export function clamp(value, min, max) {
    const number = Number(value);
    if (!Number.isFinite(number)) return min;
    return Math.max(min, Math.min(max, number));
}

export function clamp01(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return 0;
    return Math.max(0, Math.min(1, number));
}

export function createPointGrid(rows, cols) {
    const points = [];
    for (let r = 0; r < rows; r += 1) {
        for (let c = 0; c < cols; c += 1) {
            points.push({
                u: cols > 1 ? c / (cols - 1) : 0.5,
                v: rows > 1 ? r / (rows - 1) : 0.5
            });
        }
    }
    return points;
}

export function createGeometryPointGrid(rows, cols) {
    const points = [];
    for (let r = 0; r < rows; r += 1) {
        for (let c = 0; c < cols; c += 1) {
            points.push({
                u: cols > 1 ? -1 + (2 * c) / (cols - 1) : 0,
                v: rows > 1 ? 1 - (2 * r) / (rows - 1) : 0
            });
        }
    }
    return points;
}
