import { getRegionLayoutRect, projectionToScreen } from './coords.js';

export function hitTestPoint(points, rect, mx, my, threshold = 18) {
    for (let i = 0; i < points.length; i += 1) {
        const screen = projectionToScreen(points[i].u, points[i].v, rect);
        const dx = screen.x - mx;
        const dy = screen.y - my;
        if (Math.sqrt(dx * dx + dy * dy) <= threshold) {
            return i;
        }
    }
    return -1;
}

export function hitTestRegionCell(bounds, rows, cols, mx, my, regionCount) {
    for (let i = 0; i < regionCount; i += 1) {
        const cell = getRegionLayoutRect(bounds, rows, cols, i);
        const inside = mx >= cell.x && mx <= cell.x + cell.width && my >= cell.y && my <= cell.y + cell.height;
        if (inside) return i;
    }
    return -1;
}

