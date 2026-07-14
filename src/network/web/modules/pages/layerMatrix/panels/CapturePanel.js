// 采集图层（10/11）参数 HTML
import { shapeBlockHtml, fitModeBlockHtml } from './SharedBlocks.js';

export function generateCaptureHtml(layer, layerId) {
    const captureType = layer.capture_type ?? layer.captureType ?? 'AUTO';
    const captureIndex = layer.capture_index ?? layer.captureIndex ?? 0;
    const captureRotationRaw = Number(layer.capture_rotation ?? layer.captureRotation ?? 0) || 0;
    const captureRotation = [-1, 0, 90, 180, 270].includes(captureRotationRaw) ? captureRotationRaw : 0;
    const invert = Number(layer.invert ?? 0) || 0;
    const transformPreset = captureRotation === 0 && invert === 0
        ? 'normal'
        : (captureRotation === -1 ? 'auto' : 'custom');

    return `
        <div class="setting-section compact-advanced-params compact-advanced-params--capture">
            <div class="setting-item" style="grid-column: span 1;">
                <label for="capture-type">输入选择:</label>
                <select id="capture-type" class="form-control">
                    <option value="AUTO" ${captureType === 'AUTO' ? 'selected' : ''}>自动</option>
                    <option value="HDMI" ${captureType === 'HDMI' ? 'selected' : ''}>HDMI</option>
                    <option value="USB" ${captureType === 'USB' ? 'selected' : ''}>USB</option>
                    <option value="MIPI" ${captureType === 'MIPI' ? 'selected' : ''}>MIPI</option>
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="capture-index">设备索引:</label>
                <input id="capture-index" type="number" class="form-control" min="0" step="1" value="${captureIndex}">
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="capture-transform-preset">方向预设:</label>
                <select id="capture-transform-preset" class="form-control">
                    <option value="normal" ${transformPreset === 'normal' ? 'selected' : ''}>正常</option>
                    <option value="auto" ${transformPreset === 'auto' ? 'selected' : ''}>自动</option>
                    <option value="custom" ${transformPreset === 'custom' ? 'selected' : ''}>自定义</option>
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label for="capture-rotation">输入旋转:</label>
                <select id="capture-rotation" class="form-control">
                    <option value="-1" ${captureRotation === -1 ? 'selected' : ''}>自动</option>
                    <option value="0" ${captureRotation === 0 ? 'selected' : ''}>0</option>
                    <option value="90" ${captureRotation === 90 ? 'selected' : ''}>90</option>
                    <option value="180" ${captureRotation === 180 ? 'selected' : ''}>180</option>
                    <option value="270" ${captureRotation === 270 ? 'selected' : ''}>270</option>
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label>&nbsp;</label>
                <button id="capture-restart-rk628-btn" type="button" class="btn" title="重新初始化RK628/MIPI采集链路">重启采集</button>
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label>采集场景:</label>
                <div style="display: flex; gap: 6px; flex-wrap: wrap;">
                    <button id="capture-tv-mode-btn" type="button" class="btn small primary" title="无黑边：自动旋转补偿，铺满显示，并切换TVMirror场景">无黑边旋转+TV</button>
                    <button id="capture-normal-mode-btn" type="button" class="btn small" title="有黑边：不旋转，保持比例显示，并切换默认配置">有黑边不旋转+普通</button>
                </div>
            </div>
            ${shapeBlockHtml(layer, { includeInvert: true })}
            ${fitModeBlockHtml(layer)}
        </div>
    `;
}
