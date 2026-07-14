// 跨多个图层 panel 复用的 HTML 片段：
//   shapeBlockHtml — 图层形状 + 形状参数 + 黑色透明 (+ 可选 反转)
//   fitModeBlockHtml — 填充模式 (铺满显示 / 保持视频比例显示)
// 这些片段使用与原内联代码相同的 id (layer-shape-type / video-fit-mode 等)，
// 以保证既有事件绑定无须改动。
import { generateShapeTypeOptions, generateShapeParamHTML } from './ShapePanel.js';
import { createNumInput } from './CommonControls.js';

export function invertSelectHtml(invert) {
    const v = invert | 0;
    return `
        <div class="setting-item" style="grid-column: span 1;">
            <label>图像反转:</label>
            <select id="layer-invert" class="layer-select">
                <option value="0" ${v === 0 ? 'selected' : ''}>无反转</option>
                <option value="1" ${v === 1 ? 'selected' : ''}>水平反转</option>
                <option value="2" ${v === 2 ? 'selected' : ''}>垂直反转</option>
                <option value="3" ${v === 3 ? 'selected' : ''}>水平+垂直</option>
            </select>
        </div>`;
}

export function blackTransparentSelectHtml(blackToTransparent, label = '黑色透明') {
    const v = !!blackToTransparent;
    return `
        <div class="setting-item" style="grid-column: span 1;">
            <label for="layer-black-to-transparent">${label}:</label>
            <select id="layer-black-to-transparent" class="form-control">
                <option value="false" ${!v ? 'selected' : ''}>关闭</option>
                <option value="true" ${v ? 'selected' : ''}>开启</option>
            </select>
        </div>`;
}

/**
 * 形状块：形状下拉 + 形状参数 + 黑色透明 [+ 可选反转]
 * 示例/字段：@param {Object} layer
 * 示例/字段：@param {Object} opts
 *   includeInvert  是否额外输出图像反转下拉（仅采集 / 视频图层为 true）
 *   blackLabel     黑色透明 label，默认 "黑色透明"
 */
export function shapeBlockHtml(layer, opts = {}) {
    const shapeType = Number(layer.shapeType ?? layer.shape_type ?? 0);
    const shapeParam = Number(layer.shapeParam ?? layer.shape_param ?? 0.0);
    const blackToTransparent = layer.black_to_transparent ?? layer.blackToTransparent ?? false;
    return `
        <div class="setting-item" style="grid-column: span 1;">
            <label for="layer-shape-type">图层形状:</label>
            <select id="layer-shape-type" class="form-control">
                ${generateShapeTypeOptions(shapeType)}
            </select>
        </div>
        ${generateShapeParamHTML(shapeType, shapeParam, 'layer', 'block')}
        ${opts.includeInvert ? invertSelectHtml(layer.invert) : ''}
        ${blackTransparentSelectHtml(blackToTransparent, opts.blackLabel)}
    `;
}

/**
 * 填充模式块：铺满显示 / 保持视频比例显示
 */
export function fitModeBlockHtml(layer) {
    const fitMode = Number(layer.fit_mode ?? layer.fitMode ?? 0) > 0 ? 1 : 0;
    return `
        <div class="setting-item" style="grid-column: span 1;">
            <label for="video-fit-mode">填充模式:</label>
            <select id="video-fit-mode" class="form-control">
                <option value="0" ${fitMode === 0 ? 'selected' : ''}>铺满显示</option>
                <option value="1" ${fitMode === 1 ? 'selected' : ''}>保持视频比例显示</option>
            </select>
        </div>`;
}
