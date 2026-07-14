import { createNumInput } from './CommonControls.js';
import { escapeHtml } from '../../../components/toast.js';

export function generateTextLayer40Html(prefix, config) {
    const idPrefix = prefix ? prefix + '-' : '';
    const { text, fontFile, textColorCss, textColor, bgColorCss, bgColor, isBgTransparent, fontSize, displayMode, scrollSpeed, alphaVal } = config;
    const fontLabel = fontFile || '默认系统字体';

    return `
        <div class="setting-section">
            <div class="setting-item" style="grid-column: 1 / -1;">
                 <label for="${idPrefix}text-content">显示的文字:</label>
                 <textarea id="${idPrefix}text-content" class="form-control" rows="3" placeholder="输入文本内容">${escapeHtml(text)}</textarea>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                 <label for="${idPrefix}text-font-file">字体:</label>
                 <select id="${idPrefix}text-font-file" class="form-control" aria-label="选择文字字体" title="${escapeHtml(fontLabel)}">
                     <option value="">${escapeHtml(fontLabel)}</option>
                 </select>
            </div>
            <div class="setting-item" style="grid-column: span 1;">
                <label>字体颜色:</label>
                <div class="color-input-container" style="display: flex; gap: 8px; align-items: center;">
                     <div id="${idPrefix}text-color-swatch" class="color-swatch-btn" style="background-color: ${textColorCss}; width: 32px; height: 32px; border-radius: 4px; border: 1px solid #444; cursor: pointer;"></div>
                     <input type="color" id="${idPrefix}text-color-picker" value="${textColorCss.startsWith('#') ? textColorCss : '#FFFFFF'}" style="opacity: 0; position: absolute; pointer-events: none; width: 0; height: 0;">
                     <input type="text" id="${idPrefix}text-color-hex" class="form-control" value="${textColorCss}" style="width: 80px; text-transform: uppercase;" placeholder="#RRGGBB">
                     <input type="hidden" id="${idPrefix}text-color" value="${textColor}">
                </div>
            </div>

            <div class="setting-item" style="grid-column: span 1;">
                 <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 4px;">
                    <label style="margin-bottom: 0;">背景颜色:</label>
                     <div style="display: flex; align-items: center; gap: 4px;">
                         <input type="checkbox" id="${idPrefix}text-bg-transparent" ${isBgTransparent ? 'checked' : ''}>
                         <label for="${idPrefix}text-bg-transparent" style="font-size: 12px; margin-bottom: 0; cursor: pointer;">透明</label>
                    </div>
                </div>
                <div class="color-input-container" style="display: flex; gap: 8px; align-items: center;">
                     <div id="${idPrefix}text-bg-color-swatch" class="color-swatch-btn" style="background-color: ${bgColorCss}; width: 32px; height: 32px; border-radius: 4px; border: 1px solid #444; cursor: pointer;"></div>
                     <input type="color" id="${idPrefix}text-bg-color-picker" value="${bgColorCss.startsWith('#') ? bgColorCss : '#000000'}" style="opacity: 0; position: absolute; pointer-events: none; width: 0; height: 0;" ${isBgTransparent ? 'disabled' : ''}>
                     <input type="text" id="${idPrefix}text-bg-color-hex" class="form-control" value="${bgColorCss}" style="width: 80px; text-transform: uppercase;" placeholder="#RRGGBB" ${isBgTransparent ? 'disabled' : ''}>
                     <input type="hidden" id="${idPrefix}text-bg-color" value="${bgColor}">
                </div>
            </div>

            ${createNumInput(idPrefix + 'text-font-size', '文字大小', fontSize, 1.0, null, 1.0, 1)}

            <div class="setting-item" style="grid-column: span 1;">
                 <label for="${idPrefix}text-display-mode">显示方式:</label>
                 <select id="${idPrefix}text-display-mode" class="form-control">
                     <option value="1" ${displayMode === 1 ? 'selected' : ''}>居中</option>
                     <option value="2" ${displayMode !== 1 ? 'selected' : ''}>滚动</option>
                 </select>
            </div>
            
            ${createNumInput(idPrefix + 'text-scroll-speed', '滚动速度', scrollSpeed, 0, 500, 10, 1)}
            ${createNumInput(prefix === 'slice' ? 'slice-alpha' : 'layer-alpha', '透明度', alphaVal, 0, 255, 1, 1)}
        </div>
    `;
}

export function generateTextLayer41Html(prefix, config) {
    const idPrefix = prefix ? prefix + '-' : '';
    const { alphaVal, displayAlign, playlistOptions, fontSize, showCount, displayDuration, startHintTime, endHintTime, showList } = config;
    const showListVal = showList !== undefined ? !!showList : true;

    return `
        <div class="setting-section">
            ${createNumInput(prefix === 'slice' ? 'slice-alpha' : 'layer-alpha', '透明度', alphaVal, 0, 255, 1, 1)}
            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}display-align">显示位置:</label>
                <select id="${idPrefix}display-align" class="form-control">
                    <option value="0" ${displayAlign === 0 ? 'selected' : ''}>靠左居中</option>
                    <option value="1" ${displayAlign === 1 ? 'selected' : ''}>居中</option>
                    <option value="2" ${displayAlign === 2 ? 'selected' : ''}>靠右居中</option>
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}show-list">显示列表:</label>
                <select id="${idPrefix}show-list" class="form-control">
                    <option value="1" ${showListVal ? 'selected' : ''}>显示列表</option>
                    <option value="0" ${!showListVal ? 'selected' : ''}>隐藏列表</option>
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}playlist-id">关联播放列表:</label>
                <select id="${idPrefix}playlist-id" class="form-control">
                    ${playlistOptions}
                </select>
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}text-font-size">字体大小:</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="${idPrefix}text-font-size">-</button>
                    <input type="number" id="${idPrefix}text-font-size" min="1" step="1" value="${fontSize}" data-default="${fontSize}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="${idPrefix}text-font-size">+</button>
                    <button type="button" class="number-btn number-reset" data-target="${idPrefix}text-font-size" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}show-count">显示数量:</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="${idPrefix}show-count">-</button>
                    <input type="number" id="${idPrefix}show-count" min="1" max="20" step="1" value="${showCount}" data-default="${showCount}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="${idPrefix}show-count">+</button>
                    <button type="button" class="number-btn number-reset" data-target="${idPrefix}show-count" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}display-duration">显示时长 (秒):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="${idPrefix}display-duration">-</button>
                    <input type="number" id="${idPrefix}display-duration" min="0" step="0.5" value="${displayDuration}" data-default="${displayDuration}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="${idPrefix}display-duration">+</button>
                    <button type="button" class="number-btn number-reset" data-target="${idPrefix}display-duration" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 3;">
                <label for="${idPrefix}start-hint-time">起始提示时间 (秒):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="${idPrefix}start-hint-time">-</button>
                    <input type="number" id="${idPrefix}start-hint-time" min="0" step="1" value="${startHintTime}" data-default="${startHintTime}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="${idPrefix}start-hint-time">+</button>
                    <button type="button" class="number-btn number-reset" data-target="${idPrefix}start-hint-time" title="恢复默认">↻</button>
                </div>
            </div>
            <div class="setting-item" style="grid-column: span 3;">
                <label for="${idPrefix}end-hint-time">结束提示时间 (秒):</label>
                <div class="number-input-wrapper horizontal layer-param-input">
                    <button type="button" class="number-btn minus" data-target="${idPrefix}end-hint-time">-</button>
                    <input type="number" id="${idPrefix}end-hint-time" min="0" step="1" value="${endHintTime}" data-default="${endHintTime}" class="form-control number-input">
                    <button type="button" class="number-btn plus" data-target="${idPrefix}end-hint-time">+</button>
                    <button type="button" class="number-btn number-reset" data-target="${idPrefix}end-hint-time" title="恢复默认">↻</button>
                </div>
            </div>
        </div>
    `;
}

export function generateNormalTextHtml(prefix, config) {
    const idPrefix = prefix ? prefix + '-' : '';
    const { text, fontFile, fontSize, textColorCss, textColor, bgColorCss, bgColor, isBgTransparent, alignment } = config;

    return `
        <div class="setting-section">
            <div class="setting-item" style="grid-column: 1 / -1;">
                <label for="${idPrefix}text-content">输入的文字:</label>
                <textarea id="${idPrefix}text-content" class="form-control" rows="3" placeholder="输入文本内容">${escapeHtml(text)}</textarea>
            </div>
            <div class="setting-item" style="grid-column: span 4;">
                <label for="${idPrefix}text-font-file">字体:</label>
                <input type="text" id="${idPrefix}text-font-file" class="form-control" value="${escapeHtml(fontFile)}">
            </div>
            <div class="setting-item" style="grid-column: span 2;">
                <label>文本颜色:</label>
                <div class="color-input-container" style="display: flex; gap: 8px; align-items: center;">
                     <div id="${idPrefix}text-color-swatch" class="color-swatch-btn" style="background-color: ${textColorCss}; width: 32px; height: 32px; border-radius: 4px; border: 1px solid #444; cursor: pointer;"></div>
                     <input type="color" id="${idPrefix}text-color-picker" value="${textColorCss.startsWith('#') ? textColorCss : '#FFFFFF'}" style="opacity: 0; position: absolute; pointer-events: none; width: 0; height: 0;">
                     <input type="text" id="${idPrefix}text-color-hex" class="form-control" value="${textColorCss}" style="width: 80px; text-transform: uppercase;" placeholder="#RRGGBB">
                     <input type="hidden" id="${idPrefix}text-color" value="${textColor}">
                </div>
            </div>

            <div class="setting-item" style="grid-column: span 2;">
                 <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 4px;">
                    <label style="margin-bottom: 0;">背景颜色:</label>
                     <div style="display: flex; align-items: center; gap: 4px;">
                         <input type="checkbox" id="${idPrefix}text-bg-transparent" ${isBgTransparent ? 'checked' : ''}>
                         <label for="${idPrefix}text-bg-transparent" style="font-size: 11px; margin-bottom: 0; cursor: pointer;">透明</label>
                    </div>
                </div>
                <div class="color-input-container" style="display: flex; gap: 8px; align-items: center;">
                     <div id="${idPrefix}text-bg-color-swatch" class="color-swatch-btn" style="background-color: ${bgColorCss}; width: 32px; height: 32px; border-radius: 4px; border: 1px solid #444; cursor: pointer;"></div>
                     <input type="color" id="${idPrefix}text-bg-color-picker" value="${bgColorCss.startsWith('#') ? bgColorCss : '#000000'}" style="opacity: 0; position: absolute; pointer-events: none; width: 0; height: 0;" ${isBgTransparent ? 'disabled' : ''}>
                     <input type="text" id="${idPrefix}text-bg-color-hex" class="form-control" value="${bgColorCss}" style="width: 80px; text-transform: uppercase;" placeholder="#RRGGBB" ${isBgTransparent ? 'disabled' : ''}>
                     <input type="hidden" id="${idPrefix}text-bg-color" value="${bgColor}">
                </div>
            </div>

            <div class="setting-item" style="grid-column: span 2;">
                ${createNumInput(idPrefix + 'text-font-size', '字体大小', fontSize, 1.0, null, 1.0, 2)}
            </div>

            <div class="setting-item" style="grid-column: span 2;">
                <label for="${idPrefix}text-alignment">对齐方式:</label>
                <select id="${idPrefix}text-alignment" class="form-control">
                    <option value="0" ${alignment === 0 ? 'selected' : ''}>左对齐</option>
                    <option value="1" ${alignment === 1 ? 'selected' : ''}>居中对齐</option>
                    <option value="2" ${alignment === 2 ? 'selected' : ''}>右对齐</option>
                </select>
            </div>
        </div>
    `;
}

export function setupTextLayerEvents(layerId, prefix, sliceKey, triggerUpdate, applyProperties, cssColorToRgbaString) {
    const idPrefix = prefix ? prefix + '-' : '';
    const setupColorSync = (pickerId, hexInputId, hiddenInputId, swatchId) => {
        const picker = document.getElementById(pickerId);
        const hexInput = document.getElementById(hexInputId);
        const hiddenInput = document.getElementById(hiddenInputId);
        const swatch = document.getElementById(swatchId);

        if (!picker || !hexInput || !hiddenInput || !swatch) return;

        // 打开 picker on swatch click
        swatch.addEventListener('click', () => picker.click());

        // Picker change -> 更新 Hex, Hidden, Swatch
        picker.addEventListener('input', () => {
            const hex = picker.value;
            hexInput.value = hex.toUpperCase();
            swatch.style.backgroundColor = hex;
            hiddenInput.value = cssColorToRgbaString(hex);
            triggerUpdate();
        });

        picker.addEventListener('change', () => {
            if (prefix === 'slice') applyProperties(layerId, sliceKey);
            else applyProperties(layerId);
        });

        // Hex Input change -> 更新 Picker, Hidden, Swatch
        hexInput.addEventListener('change', () => {
            let hex = hexInput.value.trim();
            if (!hex.startsWith('#')) hex = '#' + hex;
            // 说明：校验十六进制颜色
            if (/^#[0-9A-Fa-f]{6}$/.test(hex)) {
                picker.value = hex;
                swatch.style.backgroundColor = hex;
                hiddenInput.value = cssColorToRgbaString(hex);
                triggerUpdate();
                if (prefix === 'slice') applyProperties(layerId, sliceKey);
                else applyProperties(layerId);
            }
        });
    };

    setupColorSync(`${idPrefix}text-color-picker`, `${idPrefix}text-color-hex`, `${idPrefix}text-color`, `${idPrefix}text-color-swatch`);
    setupColorSync(`${idPrefix}text-bg-color-picker`, `${idPrefix}text-bg-color-hex`, `${idPrefix}text-bg-color`, `${idPrefix}text-bg-color-swatch`);

    // 说明：透明复选框绑定
    const bgTransCheckbox = document.getElementById(`${idPrefix}text-bg-transparent`);
    if (bgTransCheckbox) {
        bgTransCheckbox.addEventListener('change', () => {
            const isTrans = bgTransCheckbox.checked;
            const hexInput = document.getElementById(`${idPrefix}text-bg-color-hex`);
            const picker = document.getElementById(`${idPrefix}text-bg-color-picker`);
            const hiddenInput = document.getElementById(`${idPrefix}text-bg-color`);
            const swatch = document.getElementById(`${idPrefix}text-bg-color-swatch`);

            if (hexInput) hexInput.disabled = isTrans;
            if (picker) picker.disabled = isTrans;
            if (isTrans) {
                hiddenInput.value = "0.0 0.0 0.0 0.0";
                swatch.style.background = 'linear-gradient(45deg, #ccc 25%, transparent 25%), linear-gradient(-45deg, #ccc 25%, transparent 25%), linear-gradient(45deg, transparent 75%, #ccc 75%), linear-gradient(-45deg, transparent 75%, #ccc 75%)';
                swatch.style.backgroundSize = '8px 8px';
                swatch.style.backgroundColor = '#fff';
            } else {
                // 说明：从十六进制输入恢复
                const hex = (hexInput && hexInput.value) || '#000000';
                hiddenInput.value = cssColorToRgbaString(hex);
                swatch.style.background = '';
                swatch.style.backgroundColor = hex;
            }
            triggerUpdate();
            if (prefix === 'slice') applyProperties(layerId, sliceKey);
            else applyProperties(layerId);
        });
    }
}
