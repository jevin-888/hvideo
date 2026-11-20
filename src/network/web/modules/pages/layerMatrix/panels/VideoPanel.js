// 视频图层（1-4）参数 HTML
import { createNumInput } from './CommonControls.js';
import { shapeBlockHtml, fitModeBlockHtml } from './SharedBlocks.js';

export function generateVideoHtml(layer) {
    const volumePercent = layer.volume !== undefined ? Math.round(layer.volume * 100) : 100;
    const alphaPct = Math.round((layer.alpha !== undefined ? layer.alpha : 1.0) * 255);
    const blur = layer.gaussian_blur !== undefined ? Math.round(layer.gaussian_blur) : 0;

    return `
        <div class="setting-section compact-advanced-params compact-advanced-params--video">
            ${createNumInput('layer-alpha', '透明度', alphaPct, 0, 255, 1, 1)}
            ${createNumInput('video-volume', '音量(%)', volumePercent, '0', '100', '5', 1)}
            ${createNumInput('video-gaussian-blur', '高斯模糊', blur, '0', '10', '1', 1)}
            ${fitModeBlockHtml(layer)}
            ${shapeBlockHtml(layer, { includeInvert: true })}
        </div>
    `;
}
