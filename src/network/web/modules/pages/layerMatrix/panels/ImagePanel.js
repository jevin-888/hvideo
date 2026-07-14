// 图像图层（33 / 60 / 70）参数 HTML
// 注意：layerId === 60 的形状/黑色透明在漫游配置区域展示，layerId === 71 是二维码（不走此 panel）
import { shapeBlockHtml } from './SharedBlocks.js';

export function generateImageHtml(layer, layerId) {
    if (layerId === 71) return '';

    // Layer 60：本 panel 不输出任何内容（形状/黑色透明 由漫游区单独管理）
    if (layerId === 60) {
        return `<div class="setting-section"></div>`;
    }

    // Layer 70 (Logo)：常驻图层，仅显示动画开关
    if (layerId === 70) {
        return `
            <div class="setting-section">
                <div class="setting-item" style="grid-column: span 2;">
                    <label for="image-animated">显示模式:</label>
                    <select id="image-animated" class="form-control">
                        <option value="static" ${!layer.animated ? 'selected' : ''}>静态</option>
                        <option value="animated" ${layer.animated ? 'selected' : ''}>动态</option>
                    </select>
                </div>
                ${shapeBlockHtml(layer, { includeInvert: false, blackLabel: '黑色变透明' })}
            </div>
        `;
    }

    // 普通图像图层（如 Layer33）：完整参数 + 形状/黑色透明
    const fadeIn = layer.fade_in_time !== undefined ? layer.fade_in_time : 0.5;
    const fadeOut = layer.fade_out_time !== undefined ? layer.fade_out_time : 0.5;
    const dispDur = layer.display_duration !== undefined ? layer.display_duration : 3.0;

    return `
        <div class="setting-section">
            <div class="setting-item" style="grid-column: span 1;">
                <label for="image-filter-mode">过滤模式:</label>
                <select id="image-filter-mode" class="form-control">
                    <option value="0" ${layer.filter_mode === 0 ? 'selected' : ''}>线性过滤</option>
                    <option value="1" ${layer.filter_mode === 1 ? 'selected' : ''}>最近邻过滤</option>
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="image-fade-in-time">淡入时间(秒):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="image-fade-in-time">-</button>
                    <input type="number" id="image-fade-in-time" min="0.0" step="0.1" value="${fadeIn}" data-default="${fadeIn}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="image-fade-in-time">+</button>
                    <button type="button" class="number-btn number-reset" data-target="image-fade-in-time" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="image-fade-out-time">淡出时间(秒):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="image-fade-out-time">-</button>
                    <input type="number" id="image-fade-out-time" min="0.0" step="0.1" value="${fadeOut}" data-default="${fadeOut}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="image-fade-out-time">+</button>
                    <button type="button" class="number-btn number-reset" data-target="image-fade-out-time" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="image-display-duration">显示时长(秒):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="image-display-duration">-</button>
                    <input type="number" id="image-display-duration" min="0.0" step="0.1" value="${dispDur}" data-default="${dispDur}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="image-display-duration">+</button>
                    <button type="button" class="number-btn number-reset" data-target="image-display-duration" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="image-animated">显示模式:</label>
                <select id="image-animated" class="form-control">
                    <option value="static" ${!layer.animated ? 'selected' : ''}>静态</option>
                    <option value="animated" ${layer.animated ? 'selected' : ''}>动态</option>
                </select>
            </div>
            ${shapeBlockHtml(layer, { includeInvert: false, blackLabel: '黑色变透明' })}
        </div>
    `;
}
