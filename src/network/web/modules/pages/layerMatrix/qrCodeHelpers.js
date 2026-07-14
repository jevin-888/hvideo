// 二维码辅助模块
import { apiPut, sendLayerCommand } from '../../core/api.js';
import { addToCommandLog } from '../../core/commandLog.js';
import { rgbaStringToCssColor, cssColorToRgbaString } from './utils.js';
import { showError } from '../../components/toast.js';

/**
 * 通用 RGBA 颜色三联输入控件（picker / hex / hidden rgba）+ 色块预览。
 * 适用于二维码 / Layer40 等多个场景。把分散的初始化、监听绑定、清理统一在一处，避免重复实现。
 *
 * 示例/字段：@param {Object} cfg
 *   pickerId      <input type="color"> 的 id
 *   hexId         可选：<input type="text"> hex 文本框 id（如 #qr-bg-color-hex）
 *   hiddenId      可选：<input type="hidden"> 存储 "R G B A" 字符串的 id（旧字段名）
 *   altHiddenId   可选：第二个 hidden（如 qr-bg-color-rgba 与 qr-bg-color 并存）
 *   swatchId      色块 div 的 id
 *   defaultRgba   初始化时若 hidden 为空则使用的 rgba 字符串
 *   onChange      值变化时回调（参数：rgbaStr, cssColor）
 *   isDisabled    可选：函数返回是否禁用此 picker（如透明开关）
 *   cleanupArray  存放反注册函数的数组（统一清理）
 */
function setupRgbaColorPicker(cfg) {
    const picker = document.getElementById(cfg.pickerId);
    const swatch = document.getElementById(cfg.swatchId);
    const hex = cfg.hexId ? document.getElementById(cfg.hexId) : null;
    const hidden = cfg.hiddenId ? document.getElementById(cfg.hiddenId) : null;
    const altHidden = cfg.altHiddenId ? document.getElementById(cfg.altHiddenId) : null;

    // 至少需要 picker、swatch 与一个 rgba 容器（hidden 或 altHidden）
    if (!picker || !swatch || (!hidden && !altHidden)) return;

    // 初始化：从已有 rgba 推导 hex / 色块
    try {
        const rgbaValue = (altHidden && altHidden.value) || (hidden && hidden.value) || cfg.defaultRgba || '0.0 0.0 0.0 1.0';
        const hexColor = rgbaStringToCssColor(rgbaValue);
        swatch.style.backgroundColor = hexColor;
        picker.value = hexColor;
        if (hex && !hex.value) hex.value = hexColor.toUpperCase();
    } catch (_) { /* 初始化失败保持默认 */ }

    const writeAll = (cssColor) => {
        const rgbaStr = cssColorToRgbaString(cssColor);
        if (hidden) hidden.value = rgbaStr;
        if (altHidden) altHidden.value = rgbaStr;
        if (hex) hex.value = cssColor.toUpperCase();
        swatch.style.backgroundColor = cssColor;
        if (cfg.onChange) cfg.onChange(rgbaStr, cssColor);
    };

    const pickerHandler = () => writeAll(picker.value);
    picker.addEventListener('input', pickerHandler);
    cfg.cleanupArray.push(() => picker.removeEventListener('input', pickerHandler));

    if (hex) {
        const hexHandler = () => {
            let v = hex.value.trim();
            if (!v.startsWith('#')) v = '#' + v;
            if (/^#[0-9A-Fa-f]{6}$/.test(v)) {
                picker.value = v;
                writeAll(v);
            }
        };
        hex.addEventListener('change', hexHandler);
        cfg.cleanupArray.push(() => hex.removeEventListener('change', hexHandler));
    }

    const swatchHandler = () => {
        if (cfg.isDisabled && cfg.isDisabled()) return;
        picker.click();
    };
    swatch.addEventListener('click', swatchHandler);
    cfg.cleanupArray.push(() => swatch.removeEventListener('click', swatchHandler));
}

/**
 * 绘制二维码卡片
 */
export async function drawQRCodeCard(canvas, width, content, qrText, fgColor, bgColor, errorCorrection) {
    const ctx = canvas.getContext('2d');
    if (!ctx) {
        throw new Error('Canvas 2D context 获取失败');
    }

    const drawRoundedRect = (x, y, w, h, radius) => {
        if (typeof ctx.roundRect === 'function') {
            ctx.beginPath();
            ctx.roundRect(x, y, w, h, radius);
            ctx.fill();
            return;
        }

        const r = Math.max(0, Math.min(radius, Math.min(w, h) / 2));
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);
        ctx.quadraticCurveTo(x + w, y, x + w, y + r);
        ctx.lineTo(x + w, y + h - r);
        ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
        ctx.lineTo(x + r, y + h);
        ctx.quadraticCurveTo(x, y + h, x, y + h - r);
        ctx.lineTo(x, y + r);
        ctx.quadraticCurveTo(x, y, x + r, y);
        ctx.closePath();
        ctx.fill();
    };

    const normalizedQrText = (qrText || '').trim() || '微信扫码关注';

    const padding = Math.round(width * 0.04);
    const qrBoxSize = width - (padding * 2);
    const textSpace = Math.round(width * 0.16);
    const bottomPadding = Math.round(width * 0.02);

    const height = qrBoxSize + (padding * 2) + textSpace + bottomPadding;

    canvas.width = width;
    canvas.height = height;

    const toHex = (str) => {
        if (!str || str.startsWith('#')) return str || '#000000';
        const p = str.trim().split(/\s+/).map(parseFloat);
        if (p.length < 3) return '#000000';
        const hex = (c) => Math.round(c * 255).toString(16).padStart(2, '0');
        return `#${hex(p[0])}${hex(p[1])}${hex(p[2])}`;
    };

    const toCssRgba = (str) => {
        if (!str) return 'rgba(139, 164, 217, 1)';
        if (str.startsWith('#')) return str;
        const p = str.trim().split(/\s+/).map(parseFloat);
        if (p.length < 3) return str;
        return `rgba(${Math.round(p[0] * 255)}, ${Math.round(p[1] * 255)}, ${Math.round(p[2] * 255)}, ${p[3] !== undefined ? p[3] : 1})`;
    };

    ctx.fillStyle = toCssRgba(bgColor);
    const r = Math.round(width * 0.06);
    ctx.beginPath();
    drawRoundedRect(0, 0, width, height, r);
    ctx.fill();

    ctx.fillStyle = '#ffffff';
    const boxR = Math.round(width * 0.04);
    ctx.beginPath();
    drawRoundedRect(padding, padding, qrBoxSize, qrBoxSize, boxR);
    ctx.fill();

    const qrInternalPadding = Math.round(qrBoxSize * 0.05);
    const qrDrawSize = qrBoxSize - (qrInternalPadding * 2);

    const qrTemp = document.createElement('canvas');
    await new Promise((resolve, reject) => {
        QRCode.toCanvas(qrTemp, content || '微信扫码关注', {
            width: qrDrawSize,
            margin: 1,
            color: { dark: toHex(fgColor), light: '#ffffff' },
            errorCorrectionLevel: ['L', 'M', 'Q', 'H'][errorCorrection] || 'M'
        }, (err) => err ? reject(err) : resolve());
    });

    ctx.drawImage(qrTemp, padding + qrInternalPadding, padding + qrInternalPadding);

    if (normalizedQrText) {
        ctx.fillStyle = '#ffffff';
        let fontSize = Math.round(width * 0.12);

        const maxChars = 10;
        if (normalizedQrText.length > maxChars) {
            fontSize = Math.round(fontSize * (maxChars / normalizedQrText.length));
            if (fontSize < Math.round(width * 0.06)) fontSize = Math.round(width * 0.06);
        }

        ctx.font = `bold ${fontSize}px "Microsoft YaHei", "PingFang SC", "Noto Sans CJK SC", sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';

        const textAlignTop = padding + qrBoxSize;
        const textAlignBottom = height - (bottomPadding * 0.5);
        const textY = (textAlignTop + textAlignBottom) / 2;

        ctx.fillText(normalizedQrText, width / 2, textY);
    }

    return height;
}

// 存储定时器ID，用于清理
let qrPreviewTimeoutId = null;

/**
 * 清理二维码预览定时器
 */
export function cleanupQRCodePreview() {
    if (qrPreviewTimeoutId) {
        clearTimeout(qrPreviewTimeoutId);
        qrPreviewTimeoutId = null;
    }
}

/**
 * 更新二维码预览
 */
export async function updateQRCodePreview(drawQRCodeCard) {
    const qrPreview = document.getElementById('qr-preview');
    if (!qrPreview) return;

    const qrContentInput = document.getElementById('qr-content');
    const qrTextInput = document.getElementById('qr-text');
    const qrFgColorPicker = document.getElementById('qr-fg-color-picker');
    const qrBgColorPicker = document.getElementById('qr-bg-color-picker');
    const qrFgColorInput = document.getElementById('qr-fg-color');
    const qrBgColorInput = document.getElementById('qr-bg-color-rgba') || document.getElementById('qr-bg-color');
    const qrErrorCorrection = document.getElementById('qr-error-correction');

    const content = qrContentInput ? qrContentInput.value.trim() : '';
    const text = (qrTextInput ? qrTextInput.value.trim() : '') || '微信扫码关注';
    const errorLevel = qrErrorCorrection ? parseInt(qrErrorCorrection.value) : 1;

    let fgColor = qrFgColorPicker ? qrFgColorPicker.value : (qrFgColorInput ? qrFgColorInput.value : '#000000');
    let bgColor = qrBgColorPicker ? qrBgColorPicker.value : (qrBgColorInput ? qrBgColorInput.value : '#8ba4d9');

    if (typeof QRCode === 'undefined') {
        qrPreview.innerHTML = '<div class="qr-placeholder">加载中...</div>';
        cleanupQRCodePreview();
        qrPreviewTimeoutId = setTimeout(() => updateQRCodePreview(drawQRCodeCard), 500);
        return;
    }

    try {
        let canvas = qrPreview.querySelector('canvas.preview-canvas');
        if (!canvas) {
            qrPreview.innerHTML = '';
            canvas = document.createElement('canvas');
            canvas.className = 'preview-canvas';
            canvas.style.width = '100px';
            canvas.style.height = 'auto';
            canvas.style.display = 'block';
            qrPreview.appendChild(canvas);
        }

        await drawQRCodeCard(canvas, 100, content, text, fgColor, bgColor, errorLevel);

        const qrSizeInput = document.getElementById('qr-size');
        const widthInput = document.getElementById('layer-width') || document.getElementById('qr-width');
        const heightInput = document.getElementById('layer-height') || document.getElementById('qr-height');

        if (qrSizeInput && widthInput && heightInput) {
            const currentSize = parseInt(qrSizeInput.value) || 256;
            const tempCanvas = document.createElement('canvas');
            const calculatedHeight = await drawQRCodeCard(tempCanvas, currentSize, content, text, fgColor, bgColor, errorLevel);

            const currentWidth = parseInt(widthInput.value) || 0;
            const currentHeight = parseInt(heightInput.value) || 0;
            if (document.activeElement !== widthInput && document.activeElement !== heightInput) {
                if (currentWidth === 0 || isNaN(currentWidth)) {
                    widthInput.value = currentSize;
                }
                if (currentHeight === 0 || isNaN(currentHeight)) {
                    heightInput.value = calculatedHeight;
                }
            }
        }
    } catch (error) {
        console.error('QR Preview Error:', error);
    }
}

/**
 * 生成二维码
 */
export async function generateQRCode(layerId, applyLayerProperties, drawCanvas = null, refreshLayerInfo = null, showLayerProperties = null, updateLayerMatrix = null, options = {}) {
    addToCommandLog('开始生成', 'info', `点击了生成按钮, 图层ID: ${layerId}`);

    const generateQRBtn = document.getElementById('generate-qrcode-btn');
    const originalBtnText = generateQRBtn ? generateQRBtn.textContent : '生成二维码';

    if (typeof QRCode === 'undefined') {
        const errorMsg = '二维码生成失败: QRCode库未加载。请检查网络连接或刷新页面。';
        addToCommandLog('生成二维码', 'error', errorMsg);
        showError(errorMsg);
        return;
    }

    if (generateQRBtn) {
        generateQRBtn.disabled = true;
        generateQRBtn.textContent = '正在生成...';
    }

    const isSliceMode = options.idPrefix === 'slice';
    try {
        if (isSliceMode && typeof options.applySliceProperties === 'function') {
            await options.applySliceProperties(layerId, options.sliceKey);
        } else {
            await applyLayerProperties(layerId);
        }
        addToCommandLog('生成二维码', 'info', '参数已同步到后端');
    } catch (e) {
        addToCommandLog('生成二维码', 'error', '参数保存失败，无法生成二维码');
        if (generateQRBtn) {
            generateQRBtn.disabled = false;
            generateQRBtn.textContent = originalBtnText;
        }
        return;
    }

    try {
        const qrContentInput = document.getElementById('qr-content');
        const qrTextInput = document.getElementById('qr-text');
        const qrSizeInput = document.getElementById('qr-size');
        const qrErrorCorrectionInput = document.getElementById('qr-error-correction');
        const qrFgColorInput = document.getElementById('qr-fg-color');
        const qrBgColorInput = document.getElementById('qr-bg-color-rgba') || document.getElementById('qr-bg-color');

        if (!qrContentInput || !qrSizeInput) {
            throw new Error('找不到必要的输入字段');
        }

        const content = qrContentInput.value.trim();
        const qrText = (qrTextInput ? qrTextInput.value.trim() : '') || '微信扫码关注';
        const size = parseInt(qrSizeInput.value) || 256;
        const errorCorrection = qrErrorCorrectionInput ? parseInt(qrErrorCorrectionInput.value) || 1 : 1;
        const fgColor = qrFgColorInput ? qrFgColorInput.value.trim() || '0.0 0.0 0.0 1.0' : '0.0 0.0 0.0 1.0';
        const bgColor = qrBgColorInput ? qrBgColorInput.value.trim() || '1.0 1.0 1.0 1.0' : '1.0 1.0 1.0 1.0';

        if (!content) {
            addToCommandLog('生成二维码', 'error', '请输入二维码内容');
            return;
        }

        if (size < 64 || size > 2048) {
            addToCommandLog('生成二维码', 'error', '二维码尺寸必须在64-2048之间');
            return;
        }

        // 切片模式下不传 position/layer_size/priority，避免后端修改主图层导致主图全屏
        let payload = {
            content,
            qr_text: qrText,
            qr_size: size,
            error_correction: errorCorrection,
            fg_color: fgColor,
            bg_color: bgColor
        };
        if (!isSliceMode) {
            const xInput = document.getElementById('qr-x') || document.getElementById('layer-x');
            const yInput = document.getElementById('qr-y') || document.getElementById('layer-y');
            const widthInput = document.getElementById('qr-width') || document.getElementById('layer-width');
            const heightInput = document.getElementById('qr-height') || document.getElementById('layer-height');
            const priorityInput = document.getElementById('qr-priority') || document.getElementById('layer-priority');
            const currentX = xInput ? (xInput.value !== '' ? parseInt(xInput.value) : 0) : 0;
            const currentY = yInput ? (yInput.value !== '' ? parseInt(yInput.value) : 0) : 0;
            const currentWidth = widthInput ? (widthInput.value !== '' ? parseInt(widthInput.value) : 0) : 0;
            const currentHeight = heightInput ? (heightInput.value !== '' ? parseInt(heightInput.value) : 0) : 0;
            const currentPriority = priorityInput ? (priorityInput.value !== '' ? parseInt(priorityInput.value) : layerId) : layerId;
            payload.position = { x: currentX, y: currentY };
            payload.layer_size = { width: currentWidth, height: currentHeight };
            payload.priority = currentPriority;
        }

        const finalCanvas = document.createElement('canvas');
        const calculatedHeight = await drawQRCodeCard(finalCanvas, size, content, qrText, fgColor, bgColor, errorCorrection);
        const base64Data = finalCanvas.toDataURL('image/png');
        payload.image_data = base64Data;

        const apiResult = await sendLayerCommand(layerId, 'generate_qrcode', payload);

        if (apiResult == null) {
            const errorMsg = '二维码生成失败';
            addToCommandLog('生成二维码', 'error', errorMsg);
            if (generateQRBtn) {
                generateQRBtn.disabled = false;
                generateQRBtn.textContent = originalBtnText;
            }
            return;
        }

        if (apiResult === null || apiResult === undefined) {
            // API调用失败或返回null
            addToCommandLog('生成二维码', 'error', '二维码生成失败：服务器未返回有效响应');
            if (generateQRBtn) {
                generateQRBtn.disabled = false;
                generateQRBtn.textContent = originalBtnText;
            }
            return;
        }

        // 成功：刷新界面
        addToCommandLog('生成二维码', 'success', `二维码并下发成功: ${content}`);

        const result = apiResult;
        await (async () => {
            // 仅主图层模式更新主图尺寸/显示，切片模式不修改主图层
            if (!isSliceMode) {
                const widthInputUI = document.getElementById('layer-width') || document.getElementById('qr-width');
                const heightInputUI = document.getElementById('layer-height') || document.getElementById('qr-height');
                if (widthInputUI && result.width !== undefined) widthInputUI.value = result.width;
                if (heightInputUI && result.height !== undefined) heightInputUI.value = result.height;
                const visibilityBtn = document.querySelector('.toggle-visibility');
                if (visibilityBtn && visibilityBtn.textContent === '显示') visibilityBtn.click();
            }

            const qrPreview = document.getElementById('qr-preview');
            if (qrPreview) {
                const qrContentInput = document.getElementById('qr-content');
                const qrTextInput = document.getElementById('qr-text');
                const qrFgColorPicker = document.getElementById('qr-fg-color-picker');
                const qrBgColorPicker = document.getElementById('qr-bg-color-picker');
                const qrFgColorInput = document.getElementById('qr-fg-color');
                const qrBgColorInput = document.getElementById('qr-bg-color-rgba') || document.getElementById('qr-bg-color');
                const qrErrorCorrection = document.getElementById('qr-error-correction');

                const content = qrContentInput ? qrContentInput.value.trim() : '';
                const text = (qrTextInput ? qrTextInput.value.trim() : '') || '微信扫码关注';
                const errorLevel = qrErrorCorrection ? parseInt(qrErrorCorrection.value) : 1;

                let fgColor = qrFgColorPicker ? qrFgColorPicker.value : (qrFgColorInput ? qrFgColorInput.value : '#000000');
                let bgColor = qrBgColorPicker ? qrBgColorPicker.value : (qrBgColorInput ? qrBgColorInput.value : '#8ba4d9');

                if (typeof QRCode !== 'undefined') {
                    let canvas = qrPreview.querySelector('canvas.preview-canvas');
                    if (!canvas) {
                        qrPreview.innerHTML = '';
                        canvas = document.createElement('canvas');
                        canvas.className = 'preview-canvas';
                        canvas.style.width = '100%';
                        canvas.style.height = 'auto';
                        canvas.style.display = 'block';
                        qrPreview.appendChild(canvas);
                    }
                    await drawQRCodeCard(canvas, 100, content, text, fgColor, bgColor, errorLevel);

                    const renderedHeight = canvas.height || 0;
                    if (renderedHeight > 0) {
                        canvas.style.setProperty('height', `${renderedHeight}px`, 'important');
                        canvas.style.setProperty('max-height', 'none', 'important');

                        qrPreview.style.setProperty('height', `${renderedHeight}px`, 'important');
                        qrPreview.style.setProperty('max-height', 'none', 'important');
                        qrPreview.style.setProperty('overflow', 'visible', 'important');

                        const card = qrPreview.closest('.qr-preview-card');
                        if (card) {
                            card.style.setProperty('height', 'auto', 'important');
                            card.style.setProperty('min-height', `${renderedHeight}px`, 'important');
                            card.style.setProperty('max-height', 'none', 'important');
                            card.style.setProperty('overflow', 'visible', 'important');
                        }

                        const previewContainer = qrPreview.closest('.qr-preview-container');
                        if (previewContainer) {
                            previewContainer.style.setProperty('height', 'auto', 'important');
                            previewContainer.style.setProperty('min-height', `${renderedHeight}px`, 'important');
                            previewContainer.style.setProperty('max-height', 'none', 'important');
                            previewContainer.style.setProperty('overflow', 'visible', 'important');
                        }
                    }
                }
            }

            if (refreshLayerInfo && typeof refreshLayerInfo === 'function') {
                await refreshLayerInfo(layerId);
            }

            if (drawCanvas && typeof drawCanvas === 'function') {
                drawCanvas();
            }

            if (updateLayerMatrix && typeof updateLayerMatrix === 'function') {
                await updateLayerMatrix();
            }

            // 注意：不再调用 showLayerProperties()
            // 这两个函数会完全重建 HTML 表单，导致用户刚配置的参数被重置为后端默认值
            // 此时 UI 表单已处于正确状态，只需 refreshLayerInfo + drawCanvas 同步数据即可
        })();
    } catch (error) {
        addToCommandLog('生成二维码', 'error', `二维码生成失败: ${error.message}`);
    } finally {
        if (generateQRBtn) {
            generateQRBtn.disabled = false;
            generateQRBtn.textContent = originalBtnText;
        }
    }
}

// 存储二维码事件监听器清理函数
let qrCodeListenersCleanup = [];

/**
 * 清理二维码事件监听器
 */
export function cleanupQRCodeListeners() {
    qrCodeListenersCleanup.forEach(cleanup => {
        try {
            cleanup();
        } catch (e) {
            // 清理二维码事件监听器时出错，忽略
        }
    });
    qrCodeListenersCleanup = [];
}

/**
 * 设置二维码Dashboard事件监听器
 */
export function setupQRCodeDashboardListeners(
    layerId, idPrefix, sliceKey, applySliceProperties, applyLayerProperties,
    generateQRCode, updateQRCodePreview, setupQRCodeColorInputs
) {
    // 先清理之前的监听器
    cleanupQRCodeListeners();

    const applyFn = sliceKey ? () => applySliceProperties(layerId, sliceKey) : () => applyLayerProperties(layerId);

    const qrContentInput = document.getElementById('qr-content');
    const generateQRBtn = document.getElementById('generate-qrcode-btn');
    const loadQrLogoBtn = document.getElementById('load-qr-logo-btn');

    if (generateQRBtn) {
        const generateHandler = () => generateQRCode(layerId, { idPrefix, sliceKey, applySliceProperties });
        generateQRBtn.onclick = generateHandler;
        qrCodeListenersCleanup.push(() => {
            generateQRBtn.onclick = null;
        });
    }

    if (loadQrLogoBtn) {
        const loadLogoHandler = () => {
            addToCommandLog('加载Logo', 'info', 'Logo加载功能');
        };
        loadQrLogoBtn.addEventListener('click', loadLogoHandler);
        qrCodeListenersCleanup.push(() => {
            loadQrLogoBtn.removeEventListener('click', loadLogoHandler);
        });
    }

    if (qrContentInput) {
        const keypressHandler = e => {
            if (e.key === 'Enter') generateQRCode(layerId, { idPrefix, sliceKey, applySliceProperties });
        };
        qrContentInput.addEventListener('keypress', keypressHandler);
        qrCodeListenersCleanup.push(() => {
            qrContentInput.removeEventListener('keypress', keypressHandler);
        });

        let qrTimer = null;
        const inputHandler = () => {
            updateQRCodePreview(drawQRCodeCard);
            clearTimeout(qrTimer);
            qrTimer = setTimeout(applyFn, 500);
        };
        qrContentInput.addEventListener('input', inputHandler);
        qrCodeListenersCleanup.push(() => {
            qrContentInput.removeEventListener('input', inputHandler);
            if (qrTimer) {
                clearTimeout(qrTimer);
                qrTimer = null;
            }
        });
    }

    const qrTextInput = document.getElementById('qr-text');
    const qrPreviewLabel = document.querySelector('.qr-preview-label');
    const qrLogoSizeInput = document.getElementById('qr-logo-size');
    const qrSizeInput = document.getElementById('qr-size');

    if (qrTextInput && qrPreviewLabel) {
        qrPreviewLabel.textContent = qrTextInput.value || '微信扫码关注';
        let qrTextTimer = null;
        const textInputHandler = () => {
            qrPreviewLabel.textContent = qrTextInput.value || '微信扫码关注';
            updateQRCodePreview(drawQRCodeCard);
            clearTimeout(qrTextTimer);
            qrTextTimer = setTimeout(applyFn, 500);
        };
        qrTextInput.addEventListener('input', textInputHandler);
        qrCodeListenersCleanup.push(() => {
            qrTextInput.removeEventListener('input', textInputHandler);
            if (qrTextTimer) {
                clearTimeout(qrTextTimer);
                qrTextTimer = null;
            }
        });
    }

    if (qrLogoSizeInput) {
        let timer = null;
        const triggerApply = () => {
            clearTimeout(timer);
            timer = setTimeout(applyFn, 300);
        };
        qrLogoSizeInput.addEventListener('change', triggerApply);
        qrLogoSizeInput.addEventListener('input', triggerApply);
        qrCodeListenersCleanup.push(() => {
            qrLogoSizeInput.removeEventListener('change', triggerApply);
            qrLogoSizeInput.removeEventListener('input', triggerApply);
            if (timer) {
                clearTimeout(timer);
                timer = null;
            }
        });
    }

    if (qrSizeInput) {
        let timer = null;
        const triggerApply = () => {
            updateQRCodePreview(drawQRCodeCard);
            clearTimeout(timer);
            timer = setTimeout(applyFn, 300);
        };
        qrSizeInput.addEventListener('change', triggerApply);
        qrSizeInput.addEventListener('input', triggerApply);
        qrCodeListenersCleanup.push(() => {
            qrSizeInput.removeEventListener('change', triggerApply);
            qrSizeInput.removeEventListener('input', triggerApply);
            if (timer) {
                clearTimeout(timer);
                timer = null;
            }
        });
    }

    setupQRCodeColorInputs(layerId, updateQRCodePreview, drawQRCodeCard);

    try {
        updateQRCodePreview(drawQRCodeCard);
    } catch (e) { }

    [`${idPrefix}-x`, `${idPrefix}-y`, `${idPrefix}-width`, `${idPrefix}-height`, `${idPrefix}-priority`].forEach(id => {
        const input = document.getElementById(id);
        if (input) {
            input.addEventListener('change', applyFn);
            qrCodeListenersCleanup.push(() => {
                input.removeEventListener('change', applyFn);
            });
        }
    });

    const qrErrorCorrection = document.getElementById('qr-error-correction');
    if (qrErrorCorrection) {
        const errorCorrectionHandler = () => {
            updateQRCodePreview(drawQRCodeCard);
            applyFn();
        };
        qrErrorCorrection.addEventListener('change', errorCorrectionHandler);
        qrCodeListenersCleanup.push(() => {
            qrErrorCorrection.removeEventListener('change', errorCorrectionHandler);
        });
    }
}

/**
 * 设置二维码颜色输入（背景 / 前景 / 文字三组），统一走 setupRgbaColorPicker。
 */
export function setupQRCodeColorInputs(layerId, updateQRCodePreview, drawQRCodeCard) {
    const onPreviewChange = () => updateQRCodePreview(drawQRCodeCard);

    setupRgbaColorPicker({
        pickerId: 'qr-bg-color-picker',
        hexId: 'qr-bg-color-hex',
        hiddenId: 'qr-bg-color',
        altHiddenId: 'qr-bg-color-rgba',
        swatchId: 'qr-bg-color-swatch',
        defaultRgba: '0 0.2 1 1', // 默认 #0051FF
        onChange: onPreviewChange,
        cleanupArray: qrCodeListenersCleanup
    });

    setupRgbaColorPicker({
        pickerId: 'qr-fg-color-picker',
        hiddenId: 'qr-fg-color',
        swatchId: 'qr-fg-color-swatch',
        defaultRgba: '0.0 0.0 0.0 1.0',
        onChange: onPreviewChange,
        cleanupArray: qrCodeListenersCleanup
    });

    setupRgbaColorPicker({
        pickerId: 'qr-text-color-picker',
        hiddenId: 'qr-text-color',
        swatchId: 'qr-text-color-swatch',
        defaultRgba: '1.0 1.0 1.0 1.0',
        onChange: onPreviewChange,
        cleanupArray: qrCodeListenersCleanup
    });
}

// 存储图层40事件监听器清理函数
let layer40ListenersCleanup = [];

const updateLayer40Config = (layerId, patch) => apiPut(`/config/layers/${layerId}`, patch);

/**
 * 清理图层40事件监听器
 */
export function cleanupLayer40Listeners() {
    layer40ListenersCleanup.forEach(cleanup => {
        try {
            cleanup();
        } catch (e) {
            // 清理图层40事件监听器时出错，忽略
        }
    });
    layer40ListenersCleanup = [];
}

/**
 * 设置图层40 / 通用文本图层的颜色输入。
 * 文字色：标准三联控件；背景色：附带"透明"开关 + 透明棋盘格预览。
 */
export function setupLayer40ColorInputs(layerId) {
    // 先清理之前的监听器
    cleanupLayer40Listeners();

    // 文字颜色 — 使用通用助手
    setupRgbaColorPicker({
        pickerId: 'text-color-picker',
        hiddenId: 'text-color',
        swatchId: 'text-color-swatch',
        defaultRgba: '1.0 1.0 1.0 1.0',
        onChange: (rgbaStr) => updateLayer40Config(layerId, { textColor: rgbaStr }),
        cleanupArray: layer40ListenersCleanup
    });

    // 背景颜色 — 透明开关需要单独处理棋盘格效果，picker 部分仍走通用助手
    const textBgTransparent = document.getElementById('text-bg-transparent');
    const textBgColorPicker = document.getElementById('text-bg-color-picker');
    const textBgColorInput = document.getElementById('text-bg-color');
    const textBgColorSwatch = document.getElementById('text-bg-color-swatch');

    const updateBgSwatchDisplay = (isTransparent, cssColor) => {
        if (!textBgColorSwatch) return;
        if (isTransparent) {
            textBgColorSwatch.style.cssText = 'background-color: #fff; background-image: linear-gradient(45deg, #ccc 25%, transparent 25%), linear-gradient(-45deg, #ccc 25%, transparent 25%), linear-gradient(45deg, transparent 75%, #ccc 75%), linear-gradient(-45deg, transparent 75%, #ccc 75%); background-size: 6px 6px; background-position: 0 0, 0 3px, 3px -3px, -3px 0;';
        } else {
            textBgColorSwatch.style.cssText = '';
            textBgColorSwatch.style.backgroundColor = cssColor;
        }
    };

    setupRgbaColorPicker({
        pickerId: 'text-bg-color-picker',
        hiddenId: 'text-bg-color',
        swatchId: 'text-bg-color-swatch',
        defaultRgba: '0.0 0.0 0.0 0.0',
        isDisabled: () => textBgTransparent && textBgTransparent.checked,
        onChange: (rgbaStr, cssColor) => {
            if (textBgTransparent && textBgTransparent.checked) return;
            updateBgSwatchDisplay(false, cssColor);
            updateLayer40Config(layerId, { bgColor: rgbaStr });
        },
        cleanupArray: layer40ListenersCleanup
    });

    if (textBgTransparent && textBgColorPicker && textBgColorInput) {
        const bgTransparentHandler = () => {
            const isTransparent = textBgTransparent.checked;
            textBgColorPicker.disabled = isTransparent;
            if (isTransparent) {
                textBgColorInput.value = '0.0 0.0 0.0 0.0';
                updateBgSwatchDisplay(true, '');
                updateLayer40Config(layerId, { bgColor: '0.0 0.0 0.0 0.0' });
            } else {
                const cssColor = textBgColorPicker.value;
                const rgbaStr = cssColorToRgbaString(cssColor);
                textBgColorInput.value = rgbaStr;
                updateBgSwatchDisplay(false, cssColor);
                updateLayer40Config(layerId, { bgColor: rgbaStr });
            }
        };
        textBgTransparent.addEventListener('change', bgTransparentHandler);
        layer40ListenersCleanup.push(() => {
            textBgTransparent.removeEventListener('change', bgTransparentHandler);
        });
    }
}

