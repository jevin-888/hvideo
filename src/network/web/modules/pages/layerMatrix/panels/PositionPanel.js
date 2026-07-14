import { createNumInput } from './CommonControls.js';

/**
 * 主图层与切片共用：优先级、X、Y、宽度、高度、旋转角度、透明度（同一布局、同一控件）
 * @param {string} idPrefix - 'layer' 或 'slice'
 * @param {{ priority: number, x: number, y: number, 宽度: number, 高度: number, rotation: number, alpha: number }} data
 * @param {number} layerId - 用于排除图层70不显示透明度
 * @param {{ alphaInFirstRow?: boolean, rotationInRoamRow?: boolean }} options - alphaInFirstRow 为 true 时不输出透明度；rotationInRoamRow 为 true 时旋转角度不在此处输出，由漫游行单独显示（采集图层）
 */
export function buildCommonPositionSection(idPrefix, data, layerId, options = {}) {
    const alphaVal = Math.round((data.alpha !== undefined ? data.alpha : 1.0) * 255);
    const showAlphaHere = !options.alphaInFirstRow;
    const rotationVal = data.rotation !== undefined ? Math.round(Number(data.rotation)) : 0;
    const rotationInput = options.rotationInRoamRow ? '' : createNumInput(idPrefix + '-rotation', '旋转角度', Math.min(360, Math.max(0, rotationVal)), 0, 360, 1, 1);

    return `
                ${showAlphaHere ? createNumInput(idPrefix + '-alpha', '透明度', alphaVal, 0, 255, 1, 1) : ''}
                ${createNumInput(idPrefix + '-priority', '优先级', data.priority ?? 0, 0, 100, 1, 1)}
                ${createNumInput(idPrefix + '-x', 'X', data.x ?? 0, -4000, null, 1, 1)}
                ${createNumInput(idPrefix + '-y', 'Y', data.y ?? 0, -4000, null, 1, 1)}
                ${createNumInput(idPrefix + '-width', '宽度', data.width ?? 0, 0, null, 1, 1)}
                ${createNumInput(idPrefix + '-height', '高度', data.height ?? 0, 0, null, 1, 1)}
                ${rotationInput}
    `;
}

/** 采集图层/切片：在漫游行内输出的旋转角度控件（与漫游模式同一行） */
export function createRotationForRoamRow(idPrefix, rotationValue) {
    const val = rotationValue !== undefined ? Math.round(Number(rotationValue)) : 0;
    return createNumInput(idPrefix + '-rotation', '旋转角度', Math.min(360, Math.max(0, val)), 0, 360, 1, 1);
}
