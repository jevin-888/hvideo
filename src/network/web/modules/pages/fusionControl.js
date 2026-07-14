import '../core/interactionGuards.js';
import { initializeFusion } from './fusion/index.js?v=2.100';

let fusionControlInitialized = false;
let fusionControlBooting = null;
let backgroundControlInitialized = false;

function focusFusionPage(page = document.getElementById('fusion-page')) {
    if (!page || !page.classList.contains('active')) return;
    try {
        page.focus({ preventScroll: true });
    } catch (_) {
        page.focus();
    }
}

function ensureFusionPageShell() {
    if (document.getElementById('fusion-page')) {
        return document.getElementById('fusion-page');
    }

    const contentArea = document.querySelector('.content-area .page-content');
    if (!contentArea) return null;

    const page = document.createElement('div');
    page.id = 'fusion-page';
    page.className = 'page';
    page.tabIndex = -1;
    page.innerHTML = `
        <div class="fusion-container">
            <div class="fusion-layout-body">
                <div class="fusion-main-area">
                    <div class="fusion-canvas-panel panel--fusion-canvas">
                        <div class="panel-header">
                            <div class="panel-header__left">
                                <h3 class="panel__title">预览画布</h3>
                                <div id="fusionProjectorIndicator" class="fusion-status-badge">--</div>
                            </div>
                            <div id="fusionGridDensityInHeader" class="fusion-canvas-panel__grid-density">--</div>
                            <div class="panel-actions">
                                <div class="fusion-preset">
                                    <button id="fusionPresetButton" class="btn secondary fusion-preset__button" type="button">布局预设</button>
                                    <div id="fusionPresetMenu" class="fusion-preset__menu" aria-hidden="true">
                                        <div class="fusion-preset__items-container"></div>
                                    </div>
                                </div>
                                <button id="fusionProjectorOverviewButton" class="btn secondary" type="button">投影 1/1</button>
                            </div>
                        </div>
                        <div class="panel-body panel__body--fusion-canvas">
                            <div class="fusion-canvas-frame">
                                <canvas id="fusionCanvas" class="fusion-canvas"></canvas>
                            </div>
                            <div class="fusion-shortcuts fusion-hotkey-hint" aria-label="融合编辑快捷键">
                                <div class="fusion-hotkey-overview">
                                    <div class="fusion-hotkey-label">快捷键</div>
                                    <div class="fusion-hotkey-table">
                                        <div class="fusion-hotkey-section fusion-hotkey-section--motion">
                                            <div class="fusion-hotkey-section-title">
                                                <span>移动编辑</span>
                                                <em>几何 / 融合 / 遮罩</em>
                                            </div>
                                            <div class="fusion-hotkey-grid">
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">方向键</strong><span class="fusion-hotkey-action">选择热点</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+方向</strong><span class="fusion-hotkey-action">微调点</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+Alt+方向</strong><span class="fusion-hotkey-action">快调点 ×5</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+Shift+方向</strong><span class="fusion-hotkey-action">整行/整列</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+Shift+Alt+方向</strong><span class="fusion-hotkey-action">整投影区移动</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">A/D/W/S</strong><span class="fusion-hotkey-action">列/行增减</span></div>
                                            </div>
                                        </div>
                                        <div class="fusion-hotkey-section fusion-hotkey-section--mode">
                                            <div class="fusion-hotkey-section-title">
                                                <span>模式显示</span>
                                                <em>全局 / 编辑态</em>
                                            </div>
                                            <div class="fusion-hotkey-grid">
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">N/H</strong><span class="fusion-hotkey-action">下一个/上一个</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">K</strong><span class="fusion-hotkey-action">直线/曲线</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">G</strong><span class="fusion-hotkey-action">几何开关</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">M</strong><span class="fusion-hotkey-action">遮罩开关</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">I</strong><span class="fusion-hotkey-action">融合模式</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Alt+W</strong><span class="fusion-hotkey-action">融合带</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">-/+</strong><span class="fusion-hotkey-action">线粗细</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+-/+</strong><span class="fusion-hotkey-action">热点大小</span></div>
                                            </div>
                                        </div>
                                        <div class="fusion-hotkey-section fusion-hotkey-section--save">
                                            <div class="fusion-hotkey-section-title">
                                                <span>还原保存</span>
                                                <em>全局</em>
                                            </div>
                                            <div class="fusion-hotkey-grid">
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+Z</strong><span class="fusion-hotkey-action">撤销</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+Shift+Z</strong><span class="fusion-hotkey-action">初始化</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+G</strong><span class="fusion-hotkey-action">几何还原</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+I</strong><span class="fusion-hotkey-action">融合还原</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+M</strong><span class="fusion-hotkey-action">遮罩还原</span></div>
                                                <div class="fusion-hotkey-entry"><strong class="fusion-hotkey-kbd">Ctrl+S</strong><span class="fusion-hotkey-action">保存</span></div>
                                            </div>
                                        </div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            <div class="fusion-sidebar">
                <div class="fusion-controls-panel">
                    <div class="panel-tabs" role="tablist">
                        <button class="panel-tab active" data-fusion-tab="fusion-tab-geometry-fusion">融合</button>
                        <button class="panel-tab" data-fusion-tab="fusion-tab-color">色彩</button>
                    </div>
                    <div class="panel-body panel__body--fusion-controls">
                        <div id="fusion-tab-geometry-fusion" class="fusion-tab active">
                            <div class="control-group">
                                <div class="control-group__title">
                                    <div class="flex-between fusion-panel-header-row">
                                        <span id="fusionBlendSectionTitle">2. 融合</span>
                                        <label class="preview-switch fusion-blend-master-switch" title="显示或隐藏融合带">
                                            <input type="checkbox" id="fusionBlendMasterToggle">
                                            <span class="switch-slider"></span>
                                            <span id="fusionBlendMasterToggleLabel" class="switch-label">关</span>
                                        </label>
                                        <label class="preview-switch fusion-blend-auto-switch" title="自动按输入相邻关系打开融合边，手动可逐边开关">
                                            <input type="checkbox" id="fusionBlendAutoEdgesToggle" checked>
                                            <span class="switch-slider"></span>
                                            <span id="fusionBlendAutoEdgesLabel" class="switch-label">自动</span>
                                        </label>
                                    </div>
                                </div>
                                <div class="fusion-blending-side-selector">
                                    <button class="btn small fusion-blending-side" data-side="top"><span class="fusion-blending-side__check"></span><span>上</span></button>
                                    <button class="btn small fusion-blending-side" data-side="bottom"><span class="fusion-blending-side__check"></span><span>下</span></button>
                                    <button class="btn small fusion-blending-side" data-side="left"><span class="fusion-blending-side__check"></span><span>左</span></button>
                                    <button class="btn small fusion-blending-side" data-side="right"><span class="fusion-blending-side__check"></span><span>右</span></button>
                                </div>
                            </div>
                            <div class="control-group">
                                <div class="control-group__title">3. 融合参数</div>
                                <div class="field-group mt-10 fusion-param-grid">
                                    <label class="field">
                                        <span class="field__label" title="融合带亮度曲线的最终伽马指数，越大过渡越陡。">Gamma</span>
                                        <div class="field__input-group">
                                            <button class="btn" type="button" id="fusionGammaDecrease">−</button>
                                            <input id="fusionGammaInput" class="field__control" type="number" min="1" max="3" step="0.1" value="2.2">
                                            <button class="btn" type="button" id="fusionGammaIncrease">+</button>
                                            <button class="btn btn--icon" type="button" id="fusionGammaReset" title="重置">↻</button>
                                        </div>
                                    </label>
                                    <label class="field">
                                        <span class="field__label" title="融合曲线的幂次/斜率（1~3），越大曲线越陡、过渡越集中。">Slope</span>
                                        <div class="field__input-group">
                                            <button class="btn" type="button" id="fusionSlopeDecrease">−</button>
                                            <input id="fusionSlopeInput" class="field__control" type="number" min="1" max="3" step="0.1" value="2.0">
                                            <button class="btn" type="button" id="fusionSlopeIncrease">+</button>
                                            <button class="btn btn--icon" type="button" id="fusionSlopeReset" title="重置">↻</button>
                                        </div>
                                    </label>
                                    <label class="field fusion-gain-field">
                                        <span class="field__label" title="融合曲线中点增益（0~1），默认 0.5；过低会让曲线中段贴底。">Gain</span>
                                        <div class="field__input-group">
                                            <button class="btn" type="button" id="fusionGainDecrease">−</button>
                                            <input id="fusionGainInput" class="field__control" type="number" min="0" max="1" step="0.05" value="0.5">
                                            <button class="btn" type="button" id="fusionGainIncrease">+</button>
                                            <button class="btn btn--icon" type="button" id="fusionGainReset" title="重置">↻</button>
                                        </div>
                                    </label>
                                </div>
                            </div>
                            <div class="control-group">
                                <div class="control-group__title">4. 融合带亮度</div>
                                <div class="field-group mt-10">
                                    <label class="field">
                                        <span class="field__label">R (红)</span>
                                        <div class="field__input-group">
                                            <button class="btn" type="button" id="fusionBrightRDecrease">−</button>
                                            <div class="brightness-drag-field" data-bright-input="fusionBrightRInput">
                                                <output id="fusionBrightRInput" class="field__control" for="fusionBrightRDecrease fusionBrightRIncrease">128</output>
                                            </div>
                                            <button class="btn" type="button" id="fusionBrightRIncrease">+</button>
                                            <button class="btn btn--icon" type="button" id="fusionBrightRReset" title="重置">↻</button>
                                        </div>
                                    </label>
                                    <label class="field">
                                        <span class="field__label">G (绿)</span>
                                        <div class="field__input-group">
                                            <button class="btn" type="button" id="fusionBrightGDecrease">−</button>
                                            <div class="brightness-drag-field" data-bright-input="fusionBrightGInput">
                                                <output id="fusionBrightGInput" class="field__control" for="fusionBrightGDecrease fusionBrightGIncrease">128</output>
                                            </div>
                                            <button class="btn" type="button" id="fusionBrightGIncrease">+</button>
                                            <button class="btn btn--icon" type="button" id="fusionBrightGReset" title="重置">↻</button>
                                        </div>
                                    </label>
                                    <label class="field">
                                        <span class="field__label">B (蓝)</span>
                                        <div class="field__input-group">
                                            <button class="btn" type="button" id="fusionBrightBDecrease">−</button>
                                            <div class="brightness-drag-field" data-bright-input="fusionBrightBInput">
                                                <output id="fusionBrightBInput" class="field__control" for="fusionBrightBDecrease fusionBrightBIncrease">128</output>
                                            </div>
                                            <button class="btn" type="button" id="fusionBrightBIncrease">+</button>
                                            <button class="btn btn--icon" type="button" id="fusionBrightBReset" title="重置">↻</button>
                                        </div>
                                    </label>
                                </div>
                            </div>
                            <div class="control-group fusion-mobile-debug-group">
                                <div class="control-group__title">5. 手机调试</div>
                                <div class="fusion-mobile-debug">
                                    <a id="fusionMobileDebugLink" class="fusion-mobile-debug__qr" href="/fusion-mobile/" target="_blank" rel="noopener" aria-label="打开手机调试端">
                                        <canvas id="fusionMobileDebugQr" width="180" height="180"></canvas>
                                        <span id="fusionMobileDebugFallback" class="fusion-mobile-debug__fallback">二维码</span>
                                    </a>
                                    <div class="fusion-mobile-debug__meta">
                                        <div id="fusionMobileDebugUrl" class="fusion-mobile-debug__url">/fusion-mobile/</div>
                                    </div>
                                </div>
                            </div>
                        </div>
                        <div id="fusion-tab-color" class="fusion-tab">
                            <div class="fusion-color-section">
                                <div class="fusion-color-bars">
                                    <div class="color-bar-unit" data-channel="r">
                                        <div class="color-bar-unit__track"><div class="color-bar-unit__fill red"></div></div>
                                        <span class="color-bar-unit__label">R</span>
                                    </div>
                                    <div class="color-bar-unit" data-channel="g">
                                        <div class="color-bar-unit__track"><div class="color-bar-unit__fill green"></div></div>
                                        <span class="color-bar-unit__label">G</span>
                                    </div>
                                    <div class="color-bar-unit" data-channel="b">
                                        <div class="color-bar-unit__track"><div class="color-bar-unit__fill blue"></div></div>
                                        <span class="color-bar-unit__label">B</span>
                                    </div>
                                    <div class="color-bar-unit" data-channel="y">
                                        <div class="color-bar-unit__track"><div class="color-bar-unit__fill luma"></div></div>
                                        <span class="color-bar-unit__label">L</span>
                                    </div>
                                </div>
                                <div class="field-list mt-20">
                                    <div class="field-item">
                                        <span class="field-item__label">亮度</span>
                                        <input id="fusionColorBrightness" type="range" min="0" max="2" step="0.01" value="1">
                                    </div>
                                    <div class="field-item">
                                        <span class="field-item__label">对比度</span>
                                        <input id="fusionColorContrast" type="range" min="0" max="2" step="0.01" value="1">
                                    </div>
                                    <div class="field-item">
                                        <span class="field-item__label">饱和度</span>
                                        <input id="fusionColorSaturation" type="range" min="0" max="2" step="0.01" value="1">
                                    </div>
                                </div>
                                <button class="btn secondary w-100 mt-10">重置色彩</button>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
            </div>
        </div>
    `;

    contentArea.appendChild(page);
    return page;
}

export async function initializeFusionControl() {
    const page = ensureFusionPageShell();
    if (!page) {
        throw new Error('Fusion page host not found');
    }
    if (fusionControlBooting) return fusionControlBooting;
    if (fusionControlInitialized) return;

    fusionControlBooting = (async () => {
        await initializeFusion();
        focusFusionPage(page);
        if (!backgroundControlInitialized) {
            await import('./fusion-background.js?v=2.94');
            window.FusionBackground?.init?.();
            backgroundControlInitialized = true;
        }
        fusionControlInitialized = true;
    })().finally(() => {
        fusionControlBooting = null;
    });

    return fusionControlBooting;
}

export function refreshFusionControl() {
    const page = document.getElementById('fusion-page');
    if (page && page.classList.contains('active') && window.hsFusion) {
        focusFusionPage(page);
        if (window.hsFusion.refreshLayout) {
            void window.hsFusion.refreshLayout({ preserveActive: true });
        } else {
            window.hsFusion.redraw?.();
        }
    }
}

