// 视频图层（1-4）参数 HTML
import { createNumInput } from './CommonControls.js';
import { shapeBlockHtml } from './SharedBlocks.js';

export function generateVideoHtml(layer) {
    const volumePercent = layer.volume !== undefined ? Math.round(layer.volume * 100) : 100;
    const alphaPct = Math.round((layer.alpha !== undefined ? layer.alpha : 1.0) * 255);
    const blur = layer.gaussian_blur !== undefined ? Math.round(layer.gaussian_blur) : 0;

    return `
        <div class="setting-section compact-advanced-params compact-advanced-params--video">
            ${createNumInput('layer-alpha', '透明度', alphaPct, 0, 255, 1, 1)}
            <div class="setting-item" style="grid-column: span 1;">
                <label for="video-volume">音量(%):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="video-volume">-</button>
                    <input type="number" id="video-volume" min="0" max="100" step="5" value="${volumePercent}" data-default="${volumePercent}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="video-volume">+</button>
                    <button type="button" class="number-btn number-reset" data-target="video-volume" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="video-gaussian-blur">高斯模糊:</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="video-gaussian-blur">-</button>
                    <input type="number" id="video-gaussian-blur" min="0" max="10" step="1" value="${blur}" data-default="${blur}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="video-gaussian-blur">+</button>
                    <button type="button" class="number-btn number-reset" data-target="video-gaussian-blur" title="恢复默认">↻</button>
                </div>
            </div>
            ${shapeBlockHtml(layer, { includeInvert: true })}
        </div>
    `;
}
