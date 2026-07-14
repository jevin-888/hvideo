import { getState, patchPage } from '../actions.js?v=2.95';
import { getActiveBlend, getActiveColor, getActiveGeometry, getActiveMask } from '../selectors.js?v=2.95';
import { drawBlendScene, drawGridScene, getInputCanvasLayoutMetrics, resizeCanvasToDisplaySize } from '../canvas/renderer.js?v=2.101';
import { BLEND_SIDE_LABELS, TABS } from '../types.js';

export function switchTab(tab) {
    patchPage({ activeTab: tab });
}

export function switchToColorTab() {
    switchTab(TABS.COLOR);
}

function getModeLabel(state) {
    if (state.page.activeTab === TABS.MASK) return '遮罩模式';
    if (state.page.activeTab === TABS.BLEND) return state.blend.managerMode ? '融合模式' : '融合页面';
    if (state.page.activeTab === TABS.COLOR) return '色彩模式';
    return '几何模式';
}

function getBlendDisplayLabel(state) {
    return state.blend.masterEnabled ? '融合带开' : '融合带关';
}

function isBlendSideOn(blend, side) {
    const sideState = blend?.[side];
    return !!sideState?.enabled;
}

function syncTabButtons() {
    const state = getState();
    document.querySelectorAll('[data-fusion-tab]').forEach((button) => {
        const tabId = button.getAttribute('data-fusion-tab');
        const active = (tabId === 'fusion-tab-geometry-fusion' && state.page.activeTab !== TABS.COLOR) || (tabId === 'fusion-tab-color' && state.page.activeTab === TABS.COLOR);
        button.classList.toggle('active', active);
    });

    const geometryPanel = document.getElementById('fusion-tab-geometry-fusion');
    const colorPanel = document.getElementById('fusion-tab-color');
    if (geometryPanel) geometryPanel.classList.toggle('active', state.page.activeTab !== TABS.COLOR);
    if (colorPanel) colorPanel.classList.toggle('active', state.page.activeTab === TABS.COLOR);
}

function syncHeader() {
    const state = getState();
    const indicator = document.getElementById('fusionProjectorIndicator');
    const density = document.getElementById('fusionGridDensityInHeader');
    const overviewButton = document.getElementById('fusionProjectorOverviewButton');
    const presetButton = document.getElementById('fusionPresetButton');
    const modeLabel = getModeLabel(state);
    const regionIds = state.layout.regionIds || [];
    const regionTotal = Math.max(1, regionIds.length || Number(state.layout.regionCount) || 1);
    const activeRegionLabel = `投影 ${state.page.activeRegionId}/${regionTotal}`;
    const maskCanvasLabel = '输入幕布';

    if (indicator) {
        indicator.textContent = state.page.activeTab === TABS.MASK
            ? `${maskCanvasLabel} · ${modeLabel}`
            : `${activeRegionLabel} · ${modeLabel}`;
    }

    if (overviewButton) {
        overviewButton.textContent = state.page.activeTab === TABS.MASK
            ? maskCanvasLabel
            : activeRegionLabel;
        overviewButton.classList.toggle('primary', false);
        overviewButton.classList.toggle('secondary', true);
    }

    if (presetButton) {
        presetButton.textContent = state.page.activeTab === TABS.MASK
            ? maskCanvasLabel
            : activeRegionLabel;
    }

    if (density) {
        if (state.page.activeTab === TABS.COLOR) {
            const color = getActiveColor();
            density.textContent = `B ${color.brightness.toFixed(2)} · C ${color.contrast.toFixed(2)} · S ${color.saturation.toFixed(2)}`;
        } else {
            const active = state.page.activeTab === TABS.MASK ? getActiveMask() : state.geometry.byRegionId[state.page.activeRegionId];
            const blendDisplay = state.page.activeTab === TABS.BLEND ? ` · ${getBlendDisplayLabel(state)}` : '';
            if (state.page.activeTab === TABS.MASK && active) {
                const input = getInputCanvasLayoutMetrics();
                // 遮罩状态栏先显示输入幕布布局，避免把遮罩控制点 2x2 误认为输出矩阵。
                density.textContent = `${modeLabel} · 幕布 ${input.rows} × ${input.cols} · 控制点 ${active.rows} × ${active.cols} · 网格${active.showGrid ? '开' : '关'}`;
            } else if (state.page.activeTab === TABS.BLEND) {
                const blend = getActiveBlend();
                const geometry = getActiveGeometry();
                const geometryGrid = geometry ? ` · 几何 ${geometry.rows} × ${geometry.cols}` : '';
                density.textContent = blend
                    ? `${modeLabel} · 融合 ${blend.gridRows} × ${blend.gridCols}${geometryGrid}${blendDisplay}`
                    : `${modeLabel}${blendDisplay}`;
            } else {
                density.textContent = active ? `${activeRegionLabel} · ${modeLabel} · ${active.rows} × ${active.cols} · 网格${active.showGrid ? '开' : '关'}${blendDisplay}` : modeLabel;
            }
        }
    }
}

function syncControls() {
    const state = getState();
    const regionId = state.page.activeRegionId;
    const blend = state.blend.byRegionId[regionId];
    const geometry = state.geometry.byRegionId[regionId];
    const color = getActiveColor();
    const geometryEnabled = !!(geometry && geometry.showGrid);
    const blendControlsEnabled = !!state.blend.masterEnabled;
    const fusionTitle = document.getElementById('fusionBlendSectionTitle');
    if (fusionTitle) {
        fusionTitle.textContent = state.page.activeTab === TABS.MASK
            ? '2. 遮罩模式（输入幕布）'
            : state.blend.managerMode
                ? `2. 投影 ${regionId}`
                : `2. 投影 ${regionId}`;
    }

    const masterToggle = document.getElementById('fusionBlendMasterToggle');
    const masterToggleLabel = document.getElementById('fusionBlendMasterToggleLabel');
    if (masterToggle) {
        masterToggle.checked = !!state.blend.masterEnabled;
        masterToggle.disabled = false;
        masterToggle.title = state.blend.masterEnabled ? '隐藏融合带' : '显示融合带';
        const masterSwitch = masterToggle.closest('.preview-switch');
        if (masterSwitch) masterSwitch.classList.toggle('is-disabled', masterToggle.disabled);
    }
    if (masterToggleLabel) {
        masterToggleLabel.textContent = state.blend.masterEnabled ? '开' : '关';
    }

    const autoEdgesToggle = document.getElementById('fusionBlendAutoEdgesToggle');
    const autoEdgesLabel = document.getElementById('fusionBlendAutoEdgesLabel');
    if (autoEdgesToggle) {
        autoEdgesToggle.checked = !!state.blend.autoEdges;
        autoEdgesToggle.disabled = false;
        autoEdgesToggle.title = state.blend.autoEdges
            ? '自动按输入相邻关系打开融合边'
            : '手动逐边开关融合边';
        const autoSwitch = autoEdgesToggle.closest('.preview-switch');
        if (autoSwitch) autoSwitch.classList.toggle('is-disabled', autoEdgesToggle.disabled);
    }
    if (autoEdgesLabel) {
        autoEdgesLabel.textContent = state.blend.autoEdges ? '自动' : '手动';
    }

    const sideSelector = document.querySelector('.fusion-blending-side-selector');
    if (sideSelector) {
        sideSelector.style.display = state.blend.masterEnabled ? '' : 'none';
    }

    document.querySelectorAll('.fusion-blending-side').forEach((button) => {
        const side = button.getAttribute('data-side');
        const checkIcon = button.querySelector('.fusion-blending-side__check');
        const sideEnabled = isBlendSideOn(blend, side);
        button.classList.toggle('active', sideEnabled);
        button.classList.toggle('selected', state.blend.masterEnabled && state.blend.activeSide === side);
        button.disabled = !state.blend.masterEnabled;
        const sideLabel = BLEND_SIDE_LABELS[side] || side;
        button.title = !state.blend.masterEnabled
            ? '融合带已隐藏'
            : state.blend.autoEdges
                ? `自动模式：${sideLabel}侧由输入相邻关系决定，点击只选择调参边`
                : sideEnabled ? `点击关闭${sideLabel}侧融合边` : `点击打开${sideLabel}侧融合边`;
        button.classList.toggle('is-enabled', sideEnabled);
        button.classList.toggle('is-disabled', !state.blend.masterEnabled);
        if (checkIcon) checkIcon.textContent = sideEnabled ? '✓' : '';
    });

    const gammaInput = document.getElementById('fusionGammaInput');
    const slopeInput = document.getElementById('fusionSlopeInput');
    const gainInput = document.getElementById('fusionGainInput');
    if (blend && blend[state.blend.activeSide]) {
        const sideState = blend[state.blend.activeSide];
        if (gammaInput) gammaInput.value = String(sideState.gamma);
        if (slopeInput) slopeInput.value = String(sideState.slope);
        if (gainInput) gainInput.value = String(sideState.anchor ?? 0.5);
    }
    [gammaInput, slopeInput, gainInput].forEach((input) => {
        if (!input) return;
        input.disabled = !blendControlsEnabled;
        input.title = blendControlsEnabled ? '' : '请先打开融合带';
    });

    // 同步 fusion band brightness inputs
    const brightRInput = document.getElementById('fusionBrightRInput');
    const brightGInput = document.getElementById('fusionBrightGInput');
    const brightBInput = document.getElementById('fusionBrightBInput');
    const setBrightValue = (input, value) => {
        if (!input) return;
        input.value = String(value);
        input.textContent = String(value);
    };
    if (blend && blend[state.blend.activeSide] && blend[state.blend.activeSide].bright) {
        const bright = blend[state.blend.activeSide].bright;
        setBrightValue(brightRInput, bright[0] ?? 128);
        setBrightValue(brightGInput, bright[1] ?? 128);
        setBrightValue(brightBInput, bright[2] ?? 128);
        updateBrightnessDragFill(brightRInput, bright[0] ?? 128, '239, 68, 68');
        updateBrightnessDragFill(brightGInput, bright[1] ?? 128, '34, 197, 94');
        updateBrightnessDragFill(brightBInput, bright[2] ?? 128, '59, 130, 246');
    } else {
        setBrightValue(brightRInput, 128);
        setBrightValue(brightGInput, 128);
        setBrightValue(brightBInput, 128);
        updateBrightnessDragFill(brightRInput, 128, '239, 68, 68');
        updateBrightnessDragFill(brightGInput, 128, '34, 197, 94');
        updateBrightnessDragFill(brightBInput, 128, '59, 130, 246');
    }
    [brightRInput, brightGInput, brightBInput].forEach((input) => {
        if (!input) return;
        input.disabled = !blendControlsEnabled;
        input.title = blendControlsEnabled ? '' : '请先打开融合带';
    });

    const brightness = document.getElementById('fusionColorBrightness');
    const contrast = document.getElementById('fusionColorContrast');
    const saturation = document.getElementById('fusionColorSaturation');
    if (brightness) brightness.value = String(color.brightness);
    if (contrast) contrast.value = String(color.contrast);
    if (saturation) saturation.value = String(color.saturation);

}
function updateKeyboardShortcutsColors() {
    const shortcutsContainer = document.querySelector('.fusion-shortcuts');
    if (!shortcutsContainer) return;

    const sectionColorMap = {
        'fusion-hotkey-section--motion': '#60a5fa',
        'fusion-hotkey-section--mode': '#fbbf24',
        'fusion-hotkey-section--save': '#34d399'
    };

    shortcutsContainer.querySelectorAll('.fusion-hotkey-section').forEach((section) => {
        const color = Object.entries(sectionColorMap)
            .find(([className]) => section.classList.contains(className))?.[1];
        if (!color) return;
        section.style.setProperty('--hotkey-color', color);
        section.style.setProperty('--hotkey-border', `${color}5c`);
        section.style.setProperty('--hotkey-bg', `${color}1c`);
    });
}

function updateBrightnessDragFill(input, value, color) {
    const host = input?.closest?.('.brightness-drag-field');
    if (!host) return;
    const next = Math.max(0, Math.min(255, Number(value) || 0));
    host.style.setProperty('--brightness-color', color);
    host.style.setProperty('--brightness-level', `${(next / 255) * 100}%`);
}

const MOBILE_DEBUG_FALLBACK_URL = 'http://192.168.1.100:8080/fusion-mobile/';
let lastMobileDebugUrl = '';

function getMobileDebugUrl() {
    const fallback = MOBILE_DEBUG_FALLBACK_URL;
    const { location } = window;
    const host = location.hostname;
    const unusableHost = !host || host === 'localhost' || host === '127.0.0.1' || host === '::1' || host === '0.0.0.0';
    if (!location.protocol.startsWith('http') || unusableHost) return fallback;
    return `${location.protocol}//${location.host}/fusion-mobile/`;
}

function renderMobileDebugQr() {
    const canvas = document.getElementById('fusionMobileDebugQr');
    const fallback = document.getElementById('fusionMobileDebugFallback');
    const link = document.getElementById('fusionMobileDebugLink');
    const urlText = document.getElementById('fusionMobileDebugUrl');
    if (!canvas || !link || !urlText) return;

    const url = getMobileDebugUrl();
    link.href = url;
    urlText.textContent = url;

    const linkStyle = window.getComputedStyle(link);
    const horizontalPadding = parseFloat(linkStyle.paddingLeft) + parseFloat(linkStyle.paddingRight);
    const qrSize = Math.max(96, Math.min(180, Math.floor((link.clientWidth || 196) - horizontalPadding)));
    const cacheKey = `${url}|${qrSize}`;

    if (lastMobileDebugUrl === cacheKey && canvas.dataset.ready === 'true') return;
    lastMobileDebugUrl = cacheKey;

    if (!window.QRCode?.toCanvas) {
        canvas.dataset.ready = 'false';
        if (fallback) fallback.style.display = '';
        return;
    }

    window.QRCode.toCanvas(canvas, url, {
        width: qrSize,
        margin: 1,
        errorCorrectionLevel: 'M',
        color: {
            dark: '#111827',
            light: '#ffffff'
        }
    }).then(() => {
        canvas.dataset.ready = 'true';
        if (fallback) fallback.style.display = 'none';
    }).catch((error) => {
        console.warn('fusion mobile debug QR render failed:', error);
        canvas.dataset.ready = 'false';
        if (fallback) fallback.style.display = '';
    });
}

export function renderFusionUi() {
    if (!getState().page.initialized) return;
    syncTabButtons();
    syncHeader();
    syncControls();

    const canvas = document.getElementById('fusionCanvas');
    if (canvas) {
        resizeCanvasToDisplaySize(canvas);
        drawGridScene(canvas);
    }

    const blendCanvas = document.getElementById('fusionBlendingCurve');
    if (blendCanvas) {
        resizeCanvasToDisplaySize(blendCanvas, 300, 200);
        drawBlendScene(blendCanvas);
    }

    // 更新现有快捷键的颜色
    updateKeyboardShortcutsColors();
    renderMobileDebugQr();
}

