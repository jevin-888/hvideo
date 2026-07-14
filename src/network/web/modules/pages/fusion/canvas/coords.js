import { clamp } from '../utils/grid.js';

export function calculateAspectRatioRect(width, height, aspectRatio = 16 / 9) {
    let rectWidth;
    let rectHeight;

    if (width / height > aspectRatio) {
        rectHeight = height;
        rectWidth = rectHeight * aspectRatio;
    } else {
        rectWidth = width;
        rectHeight = rectWidth / aspectRatio;
    }

    return {
        width: rectWidth,
        height: rectHeight,
        x: (width - rectWidth) / 2,
        y: (height - rectHeight) / 2
    };
}

export function projectionToScreen(u, v, rect) {
    return {
        x: rect.x + u * rect.width,
        y: rect.y + v * rect.height
    };
}

export function getRegionLayoutRect(bounds, rows, cols, index) {
    // 矩阵布局统一按行×列(rows×cols)：12×1 表示 12 行 1 列。
    // 线性区域序号先按行推进，再按列推进。
    const safeRows = Math.max(1, rows || 1);
    const safeCols = Math.max(1, cols || 1);
    const cellWidth = bounds.width / safeCols;
    const cellHeight = bounds.height / safeRows;
    const row = Math.floor(index / safeCols);
    const col = index % safeCols;
    return {
        x: bounds.x + col * cellWidth,
        y: bounds.y + row * cellHeight,
        width: cellWidth,
        height: cellHeight,
        row,
        col
    };
}

export function getRegionPreviewRect(bounds, rows, cols, index, region, aspectRatio = 16 / 9, pad = 14) {
    const base = getRegionLayoutRect(bounds, rows, cols, index);
    const defaultRect = calculateAspectRatioRect(Math.max(1, base.width - pad * 2), Math.max(1, base.height - pad * 2), aspectRatio);
    defaultRect.x += base.x + pad;
    defaultRect.y += base.y + pad;
    return { cell: base, rect: defaultRect };
}

function makePaddedAspectRect(area, aspectRatio, padding, labelHeight) {
    const available = {
        x: area.x + padding,
        y: area.y + labelHeight,
        width: Math.max(1, area.width - padding * 2),
        height: Math.max(1, area.height - labelHeight - padding)
    };
    const rect = calculateAspectRatioRect(available.width, available.height, aspectRatio);
    rect.x += available.x;
    rect.y += available.y;
    return rect;
}

function resolveAspectRatio(aspectRatio, index) {
    const value = typeof aspectRatio === 'function'
        ? aspectRatio(index)
        : Array.isArray(aspectRatio)
            ? aspectRatio[index]
            : aspectRatio;
    const number = Number(value);
    return Number.isFinite(number) && number > 0 ? number : 16 / 9;
}

export function getRegionPreviewLayouts(bounds, rows, cols, regionIds = [], activeRegionId = null, aspectRatio = 16 / 9) {
    const ids = Array.isArray(regionIds) ? regionIds : [];
    const count = ids.length;
    if (count <= 0) return [];
    if (count === 1) {
        const layout = getRegionPreviewRect(bounds, rows, cols, 0, null, resolveAspectRatio(aspectRatio, 0), 18);
        return [{ ...layout, index: 0, role: 'focus' }];
    }

    const activeIndex = Math.max(0, ids.findIndex((id) => String(id) === String(activeRegionId)));
    const gap = clamp(Math.min(bounds.width, bounds.height) * 0.022, 12, 24);
    const thumbCount = count - 1;
    const minThumbWidth = 92;
    const thumbCols = Math.max(1, Math.min(thumbCount, Math.floor((bounds.width + gap) / (minThumbWidth + gap))));
    const thumbRows = Math.ceil(thumbCount / thumbCols);
    const rawThumbHeight = clamp(bounds.height * 0.16, 72, 132);
    const maxThumbAreaHeight = Math.max(72, bounds.height * 0.34);
    const thumbCellHeight = clamp(
        (maxThumbAreaHeight - gap * Math.max(0, thumbRows - 1)) / thumbRows,
        58,
        rawThumbHeight
    );
    const thumbAreaHeight = thumbRows * thumbCellHeight + gap * Math.max(0, thumbRows - 1);
    const focusHeight = bounds.height - thumbAreaHeight - gap;

    if (focusHeight < 120) {
        return ids.map((_, index) => {
            const layout = getRegionPreviewRect(bounds, rows, cols, index, null, resolveAspectRatio(aspectRatio, index));
            return { ...layout, index, role: index === activeIndex ? 'focus' : 'grid' };
        });
    }

    const layouts = new Array(count);
    const focusCell = {
        x: bounds.x,
        y: bounds.y,
        width: bounds.width,
        height: focusHeight
    };
    layouts[activeIndex] = {
        index: activeIndex,
        role: 'focus',
        cell: focusCell,
        rect: makePaddedAspectRect(focusCell, resolveAspectRatio(aspectRatio, activeIndex), clamp(bounds.width * 0.014, 18, 34), 42)
    };

    const thumbStartY = bounds.y + focusHeight + gap;
    const thumbCellWidth = (bounds.width - gap * Math.max(0, thumbCols - 1)) / thumbCols;
    let thumbOrder = 0;
    ids.forEach((_, index) => {
        if (index === activeIndex) return;
        const row = Math.floor(thumbOrder / thumbCols);
        const col = thumbOrder % thumbCols;
        const cell = {
            x: bounds.x + col * (thumbCellWidth + gap),
            y: thumbStartY + row * (thumbCellHeight + gap),
            width: thumbCellWidth,
            height: thumbCellHeight
        };
        layouts[index] = {
            index,
            role: 'thumb',
            cell,
            rect: makePaddedAspectRect(cell, resolveAspectRatio(aspectRatio, index), 8, 24)
        };
        thumbOrder += 1;
    });

    return layouts;
}
