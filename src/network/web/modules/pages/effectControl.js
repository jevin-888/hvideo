// 特效管理页面模块
import { apiGet, apiPost, apiPut, getLastApiError } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { clearContainer } from '../utils/domHelpers.js';
import { toggleButtonGroupActive } from '../components/buttonGroups.js';
import { showWarning } from '../components/toast.js';
import { ConnectionState, getConnectionState } from '../core/connectionManager.js';

// 全局变量
let selectedEffect = null;
let selectedLayerId = null;
let videoLayers = [];

// 特效模块授权状态：未授权时隐藏 UI、不初始化任何特效相关逻辑。
// 由 loadEffectConfig 首次探测到 403 时置为 true。
let effectsLicenseDenied = false;

export function isEffectsLicenseDenied() {
    return effectsLicenseDenied;
}

export async function syncEffectLicenseVisibility() {
    try {
        const license = await apiGet('/system/license');
        if (!license || license.status === 'unlicensed' || license.status === 'expired') {
            effectsLicenseDenied = true;
            hideEffectsUI();
            return false;
        }
        const modules = Array.isArray(license.modules) ? license.modules : [];
        const enabled = modules.some(module => String(module).toLowerCase() === 'effects');
        effectsLicenseDenied = !enabled;
        if (!enabled) {
            hideEffectsUI();
        } else {
            showEffectsUI();
        }
        return enabled;
    } catch (error) {
        return !effectsLicenseDenied;
    }
}

// 本地特效配置（支持每个图层多个特效）
let localEffectConfig = {
    enabled: false,
    blendMode: 'sequential',  // 叠加模式: 'sequential'（顺序） 或 'parallel'（并行）
    layers: {},     // { layerId: [特效类型1, 特效类型2, ...] }
    layerColors: {}, // { layerId: "#rrggbb" }  描边/流光/霓虹/分割条颜色；不设=默认模式
    layerWidths: {}  // { layerId: number } 描边/流光宽度，短边百分比
};

const DEFAULT_EFFECT_WIDTH = 2.5;

function normalizeEffectWidth(value) {
    const width = Number(value);
    if (!Number.isFinite(width)) return DEFAULT_EFFECT_WIDTH;
    return Math.min(12, Math.max(0.5, Math.round(width * 10) / 10));
}

function getLayerEffectWidth(layerId) {
    return normalizeEffectWidth(
        localEffectConfig.layerWidths && localEffectConfig.layerWidths[layerId] !== undefined
            ? localEffectConfig.layerWidths[layerId]
            : DEFAULT_EFFECT_WIDTH
    );
}

// 音频联动特效列表
const audioEffects = [
    { id: 1, name: '闪白', type: 'flash_white', description: '音频节拍闪白' },
    { id: 2, name: '闪黑', type: 'flash_black', description: '音频节拍闪黑' },
    { id: 3, name: '红色闪烁', type: 'red', description: '红色脉冲' },
    { id: 4, name: '绿色闪烁', type: 'green', description: '绿色脉冲' },
    { id: 5, name: '蓝色闪烁', type: 'blue', description: '蓝色脉冲' },
    { id: 6, name: '黑条扫描', type: 'scan_bar', description: '每次重鼓点触发一次全屏左→右扫描（Q 弹减速）' },
    { id: 7, name: '中心散开', type: 'iris', description: '击鼓时收缩成中心小圈' },
    { id: 8, name: 'RGB 描边', type: 'rgb_split', description: 'R/B 通道分离形成彩色描边' },
    { id: 9, name: '反色', type: 'invert', description: '颜色取反，霓虹感' },
    { id: 10, name: '扫描线', type: 'scanlines', description: '经典电视横条纹' },
    { id: 12, name: '流光', type: 'chase_segments', description: '单段流光沿图层边缘平滑流动' },
    { id: 13, name: '帘幕对开', type: 'curtain_split', description: '全黑屏从中心向两边拉开露出视频' },
    { id: 14, name: '缩放', type: 'dmx_scale', description: 'DMX/手动触发画面缩放呼吸' },
    { id: 15, name: '旋转', type: 'dmx_rotate', description: '画面持续旋转：普通模式4秒一圈，BPM模式12拍一圈' },
    { id: 16, name: '色彩扫光', type: 'color_sweep', description: '彩色光带横向扫过画面，鼓点时增强' },
    { id: 17, name: '自动分屏', type: 'auto_split', description: '根据音频强度自动切换4/6/9/12分屏' },
    { id: 18, name: '形状: 圆形', type: 'shape_circle', description: '临时把目标图层裁成圆形' },
    { id: 19, name: '形状: 三角形', type: 'shape_triangle', description: '临时把目标图层裁成三角形' },
    { id: 20, name: '形状: 圆角矩形', type: 'shape_round_rect', description: '临时把目标图层裁成圆角矩形' },
    { id: 21, name: '形状: 星形', type: 'shape_star', description: '临时把目标图层裁成星形' },
    { id: 22, name: '形状: 六边形', type: 'shape_hexagon', description: '临时把目标图层裁成六边形' },
    { id: 23, name: '形状: 菱形', type: 'shape_diamond', description: '临时把目标图层裁成菱形' },
    { id: 24, name: '形状: 心形', type: 'shape_heart', description: '临时把目标图层裁成心形' },
    { id: 25, name: '形状: 花瓣', type: 'shape_petal', description: '临时把目标图层裁成花瓣' },
    { id: 26, name: 'LOGO演艺', type: 'logo_show', description: '酒吧开场徽标光阵，纯GPU全局炸场效果' },
    { id: 27, name: '感情', type: 'old_heart', description: '旧项目移植：红色爱心环绕叠加' },
    { id: 28, name: '灵魂出窍', type: 'old_soul', description: '旧项目移植：中心放大残影' },
    { id: 29, name: '抖动', type: 'old_shake', description: '旧项目移植：RGB 通道错位抖动' },
    { id: 31, name: '毛刺', type: 'old_glitch', description: '旧项目移植：横向线性错位和色散' },
    { id: 32, name: '幻觉', type: 'old_hallucination', description: '旧项目移植：彩色拖影幻觉' },
    { id: 33, name: '立方体', type: 'old_cube', description: '旧项目移植：视频贴图立方体旋转' },
    { id: 34, name: '边缘跑马', type: 'edge_marquee', description: '多段光条绕图层四周边缘跑马' },
    { id: 35, name: '卡点残影', type: 'beat_echo', description: '鼓点触发多层定格残影和色彩回声' },
    { id: 36, name: '霓虹描边', type: 'neon_outline', description: '常驻霓虹边缘发光，鼓点时增强亮度' },
    { id: 37, name: '液态玻璃', type: 'liquid_glass', description: '水波玻璃折射，音频控制扭曲强度' },
    { id: 38, name: '万花筒镜像', type: 'kaleidoscope', description: '多片镜像旋转，节拍增强片数和速度' },
    { id: 39, name: '卡点形状分割', type: 'beat_shape_split', description: '鼓点触发条纹、扇形、网格、星形分割；选色后为黑底彩色分割条' },
    { id: 40, name: '形状拼接', type: 'shape_mosaic_stitch', description: '黑底遮罩，鼓点逐个把圆形窗口拉伸成竖向圆角长方形' }
];

// 初始化特效页面
export async function initializeEffects() {
    // 已检测到未授权 → 直接跳过，不发任何请求、不渲染 UI
    if (effectsLicenseDenied) {
        hideEffectsUI();
        return;
    }

    await loadEffectConfig();  // 先加载配置（内部会探测 403 并设 effectsLicenseDenied）

    // 首次探测即拒绝 → 隐藏 UI 并跳过所有后续初始化
    if (effectsLicenseDenied) {
        hideEffectsUI();
        return;
    }

    await loadVideoLayers();
    renderEffectList();

    // 保留配置文件中的启用状态
    // 如果配置中 enabled=true，保持启用状态并应用效果
    if (localEffectConfig.enabled) {
        try {
            await applyAllEffects();
            addToCommandLog('初始化', 'success', '已从配置文件恢复特效启用状态');
        } catch (error) {
            addToCommandLog('初始化', 'warning', '恢复特效状态失败: ' + error.message);
        }
    }
}

// 未授权时隐藏特效相关 UI（菜单项 + 页面容器），防止用户进入后触发更多 403
function hideEffectsUI() {
    // 隐藏左侧菜单项
    document.querySelectorAll('.menu-item[data-page="effect-control"]').forEach(el => {
        el.style.display = 'none';
    });
    // 隐藏页面容器
    const page = document.getElementById('effect-control-page');
    if (page) {
        page.style.display = 'none';
        page.classList.remove('active');
    }
    // 同步停止音频反应轮询（即使被其他路径唤起也不再启动）
    stopAudioReactiveRefresh();
}

function showEffectsUI() {
    document.querySelectorAll('.menu-item[data-page="effect-control"]').forEach(el => {
        el.style.display = '';
    });
    const page = document.getElementById('effect-control-page');
    if (page) {
        page.style.display = '';
    }
}

// 规范化效果配置：将字符串/对象格式统一转换为数字ID数组
function normalizeEffectConfig(config) {
    if (!config || !config.layers) return config;

    const normalized = { ...config };
    normalized.layers = {};

    for (const [layerId, effects] of Object.entries(config.layers)) {
        if (Array.isArray(effects)) {
            normalized.layers[layerId] = effects.map(effect => {
                if (typeof effect === 'number') return effect;
                if (typeof effect === 'string') return getEffectTypeId(effect);
                if (effect && typeof effect === 'object' && effect.type !== undefined) return getEffectTypeId(effect.type);
                return getEffectTypeId(effect);
            }).filter(id => id !== 0);
        }
    }

    return normalized;
}

// 加载特效配置
async function loadEffectConfig() {
    try {
        const response = await apiGet('/effect-config');
        if (response) {
            if (response.licensed === false) {
                effectsLicenseDenied = true;
                return;
            }
            const normalized = normalizeEffectConfig({
                enabled: response.enabled !== undefined ? response.enabled : false,  // 默认关闭，避免崩溃
                blendMode: response.blendMode || 'sequential',
                layers: response.layers || {},
                layerColors: response.layerColors || {},
                layerWidths: response.layerWidths || {}
            });
            localEffectConfig = normalized;
        } else {
            // 探测到 license 未包含特效模块（403）→ 永久标记 denied
            const err = getLastApiError();
            if (err && err.reason === 'forbidden') {
                effectsLicenseDenied = true;
                return;
            }
            localEffectConfig.enabled = false;
        }
    } catch (error) {
        console.error('[EffectControl] 加载配置失败:', error);
        // 加载失败时默认关闭，避免崩溃
        localEffectConfig.enabled = false;
    }
}

// 保存特效配置
async function saveEffectConfig() {
    try {
        const response = await apiPost('/effect-config', localEffectConfig);
        if (response) {
            addToCommandLog('保存配置', 'success', '特效配置已保存');

            // 如果已启用联动，重新应用效果（使叠加模式等设置立即生效）
            if (localEffectConfig.enabled) {
                try {
                    await applyAllEffects();
                    addToCommandLog('应用配置', 'success', '效果配置已重新应用');
                } catch (applyError) {
                    addToCommandLog('应用配置', 'error', '重新应用效果失败: ' + applyError.message);
                }
            }
        } else {
            addToCommandLog('保存配置', 'error', '保存失败: 服务器无响应');
        }
    } catch (error) {
        addToCommandLog('保存配置', 'error', '保存失败: ' + error.message);
    }
}

// 应用所有已配置的效果到后端
async function applyAllEffects() {
    const layers = localEffectConfig.layers;
    let appliedCount = 0;

    for (const [layerId, effects] of Object.entries(layers)) {
        if (Array.isArray(effects) && effects.length > 0) {
            // 统一使用 effects 数组格式（无论单效果还是多效果）
            const effectsArray = effects.map(effectId => {
                const effectTypeStr = getEffectTypeString(effectId);
                return {
                    type: effectTypeStr,
                    enabled: true,
                    intensity_scale: 1.0,
                    threshold: 0.3,
                    priority: 0,
                    params: [0, 0, 0, 0]
                };
            });

            const layerColor = localEffectConfig.layerColors && localEffectConfig.layerColors[layerId];
            const payload = {
                layerId: parseInt(layerId),
                effects: effectsArray,
                blend_mode: localEffectConfig.blendMode || 'sequential'
            };
            if (layerColor) payload.effectColor = layerColor;
            payload.effectWidth = getLayerEffectWidth(layerId);
            try {
                const response = await apiPost('/audio-effect/enable', payload);
                if (response) {
                    appliedCount++;
                } else {
                    console.error(`[EffectControl] 应用效果失败 (图层${layerId}): 无响应`);
                    addToCommandLog('应用效果', 'error', `图层${layerId} 应用效果失败: 服务器无响应`);
                }
            } catch (error) {
                console.error(`[EffectControl] 应用效果失败 (图层${layerId}):`, error);
                addToCommandLog('应用效果', 'error', `图层${layerId} 应用效果失败: ${error.message}`);
            }
        }
    }

    if (appliedCount > 0) {
        addToCommandLog('启用效果', 'success', `已应用 ${appliedCount} 个图层的音频效果`);
    } else {
        addToCommandLog('启用效果', 'warning', '没有已配置的效果可应用');
    }
}

async function previewEffect(layerId, effectIdOrType) {
    const effectTypeStr = getEffectTypeString(effectIdOrType);
    if (!layerId || effectTypeStr === 'none') return false;

    const payload = {
        layerId: parseInt(layerId),
        effectType: effectTypeStr,
        type: effectTypeStr
    };
    const layerColor = localEffectConfig.layerColors && localEffectConfig.layerColors[layerId];
    if (layerColor) payload.effectColor = layerColor;
    payload.effectWidth = getLayerEffectWidth(layerId);

    const response = await apiPost('/audio-effect/preview', payload);
    return !!response;
}

// 辅助函数：将效果ID转换为字符串
function getEffectTypeString(effectId) {
    // 如果已经是字符串，直接返回
    if (typeof effectId === 'string') {
        return effectId;
    }
    const effectMap = {
        0: 'none',
        1: 'flash_white',
        2: 'flash_black',
        3: 'red',
        4: 'green',
        5: 'blue',
        6: 'scan_bar',
        7: 'iris',
        8: 'rgb_split',
        9: 'invert',
        10: 'scanlines',
        12: 'chase_segments',
        13: 'curtain_split',
        14: 'dmx_scale',
        15: 'dmx_rotate',
        16: 'color_sweep',
        17: 'auto_split',
        18: 'shape_circle',
        19: 'shape_triangle',
        20: 'shape_round_rect',
        21: 'shape_star',
        22: 'shape_hexagon',
        23: 'shape_diamond',
        24: 'shape_heart',
        25: 'shape_petal',
        26: 'logo_show',
        27: 'old_heart',
        28: 'old_soul',
        29: 'old_shake',
        31: 'old_glitch',
        32: 'old_hallucination',
        33: 'old_cube',
        34: 'edge_marquee',
        35: 'beat_echo',
        36: 'neon_outline',
        37: 'liquid_glass',
        38: 'kaleidoscope',
        39: 'beat_shape_split',
        40: 'shape_mosaic_stitch'
    };
    return effectMap[effectId] || 'none';
}

// 辅助函数：将效果类型字符串转换为ID
function getEffectTypeId(effectTypeStr) {
    const effectMap = {
        'none': 0,
        'flash_white': 1,
        'flash_black': 2,
        'red': 3,
        'green': 4,
        'blue': 5,
        'scan_bar': 6,
        'iris': 7,
        'rgb_split': 8,
        'invert': 9,
        'scanlines': 10,
        'chase_segments': 12,
        'curtain_split': 13,
        'dmx_scale': 14,
        'dmx_rotate': 15,
        'color_sweep': 16,
        'auto_split': 17,
        'shape_circle': 18,
        'shape_triangle': 19,
        'shape_round_rect': 20,
        'shape_star': 21,
        'shape_hexagon': 22,
        'shape_diamond': 23,
        'shape_heart': 24,
        'shape_petal': 25,
        'logo_show': 26,
        'old_heart': 27,
        'old_soul': 28,
        'old_shake': 29,
        'old_glitch': 31,
        'old_hallucination': 32,
        'old_cube': 33,
        'edge_marquee': 34,
        'beat_echo': 35,
        'neon_outline': 36,
        'liquid_glass': 37,
        'kaleidoscope': 38,
        'beat_shape_split': 39,
        'shape_mosaic_stitch': 40
    };
    // 如果已经是数字，直接返回
    if (typeof effectTypeStr === 'number') {
        return effectTypeStr;
    }
    return effectMap[effectTypeStr] !== undefined ? effectMap[effectTypeStr] : 0;
}

// 禁用所有已应用的效果
async function disableAllEffects() {
    let disabledCount = 0;
    let errorCount = 0;

    // 禁用所有视频图层的效果（不仅是已配置的）
    for (const layer of videoLayers) {
        try {
            const response = await apiPost('/audio-effect/disable', {
                layerId: layer.id
            });
            if (response) {
                disabledCount++;
            } else {
                console.error(`[EffectControl] 禁用效果失败 (图层${layer.id}): 无响应`);
                errorCount++;
            }
        } catch (error) {
            console.error(`[EffectControl] 禁用效果失败 (图层${layer.id}):`, error);
            errorCount++;
        }
    }

    if (disabledCount > 0) {
        addToCommandLog('禁用效果', 'success', `已禁用 ${disabledCount} 个图层的音频效果`);
    } else if (errorCount > 0) {
        addToCommandLog('禁用效果', 'error', `禁用效果失败: ${errorCount} 个图层操作失败`);
    } else {
        addToCommandLog('禁用效果', 'warning', '没有可禁用的图层');
    }
}

// 加载视频图层列表
async function loadVideoLayers() {
    try {
        // 使用 /api/layers 获取实际配置的图层列表
        const response = await apiGet('/layers');

        let layers = [];
        if (response && Array.isArray(response)) {
            // 过滤出视频和采集图层
            layers = response.filter(layer =>
                layer.type === 'video' || layer.type === 'capture'
            );
        }

        videoLayers = layers;
    } catch (error) {
        console.error('[EffectControl] Load layers error:', error);
        videoLayers = [];
    }
    renderLayerCards();
}

// 渲染图层卡片选择器（横向）
function renderLayerCards() {
    const container = document.getElementById('effect-properties-container');
    if (!container) return;

    // 使用与图层矩阵相同的样式；名称中已有 "Layer{id}" 时去掉后面重复的 "Layer"
    let cardsHtml = videoLayers.map(layer => {
        let layerName = layer.name || `图层${layer.id}`;
        const m = layerName.match(/^(Layer\d+)\s*(.*)$/i);
        if (m) layerName = `${m[1]} ${m[2].replace(/Layer/gi, '').trim()}`.trim() || m[1];
        const isSelected = selectedLayerId === layer.id;
        const typeName = layer.type === 'video' ? '视频' : layer.type === 'capture' ? '采集' : layer.type;
        // 检查该图层在本地配置中是否有特效
        const layerEffects = localEffectConfig.layers[layer.id] || [];
        const hasEffect = layerEffects.length > 0;
        const indicatorClass = hasEffect ? 'has-effect' : 'visible';
        const indicatorTitle = hasEffect ? `已配置 ${layerEffects.length} 个特效` : '无特效';
        return `
            <div class="layer-item created ${isSelected ? 'selected' : ''}" data-layer-id="${layer.id}" data-layer-type="${layer.type}">
                <div class="layer-item-header">
                    <span class="layer-name">${layerName}</span>
                </div>
                <div class="layer-item-icon">
                    <span class="layer-visibility ${indicatorClass}" title="${indicatorTitle}">●</span>
                </div>
                <div class="layer-status">
                    <span class="layer-type">${typeName}</span>
                    <span class="layer-id">ID: ${layer.id}</span>
                </div>
            </div>
        `;
    }).join('');

    container.innerHTML = `
        <h4 class="section-title">选择目标图层</h4>
        <div class="layer-matrix" style="margin-bottom: 20px;">${cardsHtml || '<span style="color:#888">无可用图层</span>'}</div>
        
        <div class="effect-toolbar-row">
            <h4 class="section-title effect-toolbar-title">已配置效果</h4>
            <div class="effect-toolbar-actions">
                <div class="effect-toolbar-group-blend-replay">
                    <select id="blend-mode-select" class="effect-toolbar-select" title="叠加方式">
                        <option value="sequential" ${localEffectConfig.blendMode === 'sequential' ? 'selected' : ''}>顺序叠加</option>
                        <option value="parallel" ${localEffectConfig.blendMode === 'parallel' ? 'selected' : ''}>并行叠加</option>
                    </select>
                    <button type="button" id="replay-test-btn" class="btn effect-replay-btn" title="重播测试">
                    <svg class="effect-replay-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M1 4v6h6M23 20v-6h-6"/><path d="M20.49 9A9 9 0 0 0 5.64 5.64L1 10m22 4l-4.64 4.36A9 9 0 0 1 3.51 15"/></svg>
                    <span>重播</span>
                </button>
                </div>
                <label class="effect-enable-label" title="描边/流光/边缘跑马/霓虹/形状分割颜色（选中图层）— 留空=默认；DMX512 CH10 可写入">
                    <span style="font-size:12px;color:#aaa;">描边色</span>
                    <input type="color" id="effect-color-picker"
                           value="${(selectedLayerId !== null && localEffectConfig.layerColors[selectedLayerId]) || '#000000'}"
                           style="width:32px;height:24px;padding:0;border:1px solid #444;background:transparent;cursor:pointer;">
                    <button type="button" id="effect-color-reset" class="btn" style="padding:2px 6px;font-size:11px;" title="切回彩虹模式">🌈</button>
                </label>
                <label class="effect-enable-label">
                    <input type="checkbox" id="effect-enable-toggle" ${localEffectConfig.enabled ? 'checked' : ''}>
                    <span>启用联动</span>
                </label>
                <button type="button" id="save-config-btn" class="btn primary">保存</button>
            </div>
        </div>
        <div class="effect-width-panel" title="RGB描边/流光/边缘跑马宽度（短边百分比）">
            <label class="effect-width-panel-label" for="effect-width-range">描边宽度</label>
            <input type="range" id="effect-width-range" min="0.5" max="12" step="0.5"
                   value="${selectedLayerId !== null ? getLayerEffectWidth(selectedLayerId) : DEFAULT_EFFECT_WIDTH}">
            <input type="number" id="effect-width-input" min="0.5" max="12" step="0.5"
                   value="${selectedLayerId !== null ? getLayerEffectWidth(selectedLayerId) : DEFAULT_EFFECT_WIDTH}">
            <span class="effect-width-panel-unit">%</span>
        </div>
        <div id="applied-effects-container" style="margin-top: 10px; display: flex; flex-wrap: wrap; gap: 8px;"></div>
    `;

    // 绑定图层卡片点击事件
    container.querySelectorAll('.layer-item').forEach(card => {
        card.addEventListener('click', () => {
            const layerId = parseInt(card.dataset.layerId);
            selectLayer(layerId);
        });
    });

    // 默认选中第一个可用图层（如果当前没有选中图层）
    if (videoLayers.length > 0 && selectedLayerId === null) {
        // 选中第一个可用图层
        selectLayer(videoLayers[0].id);
    }

    // 绑定保存按钮
    document.getElementById('save-config-btn')?.addEventListener('click', saveEffectConfig);

    // 绑定启用开关
    document.getElementById('effect-enable-toggle')?.addEventListener('change', async (e) => {
        localEffectConfig.enabled = e.target.checked;
        try {
            if (e.target.checked) {
                await applyAllEffects();
            } else {
                await disableAllEffects();
            }
            await saveEffectConfig();
        } catch (error) {
            addToCommandLog('启用联动', 'error', '切换失败: ' + error.message);
        }
    });

    // 绑定叠加模式选择
    document.getElementById('blend-mode-select')?.addEventListener('change', (e) => {
        localEffectConfig.blendMode = e.target.value;
        addToCommandLog('叠加模式', 'info', `已切换为 ${e.target.value === 'sequential' ? '顺序叠加' : '并行叠加'}（需保存配置）`);
    });

    // 绑定描边色 picker
    document.getElementById('effect-color-picker')?.addEventListener('change', async (e) => {
        if (selectedLayerId === null) return;
        const hex = e.target.value;
        localEffectConfig.layerColors[selectedLayerId] = hex;
        addToCommandLog('描边色', 'info', `图层${selectedLayerId} 描边色 → ${hex}（未保存）`);
        if (localEffectConfig.enabled) {
            await applyAllEffects();
        }
    });
    // 切回彩虹
    document.getElementById('effect-color-reset')?.addEventListener('click', async () => {
        if (selectedLayerId === null) return;
        delete localEffectConfig.layerColors[selectedLayerId];
        const picker = document.getElementById('effect-color-picker');
        if (picker) picker.value = '#000000';
        addToCommandLog('描边色', 'info', `图层${selectedLayerId} 切回彩虹（未保存）`);
        if (localEffectConfig.enabled) {
            await applyAllEffects();
        }
    });

    const syncWidthControls = (width) => {
        const normalized = normalizeEffectWidth(width);
        const range = document.getElementById('effect-width-range');
        const input = document.getElementById('effect-width-input');
        if (range) range.value = String(normalized);
        if (input) input.value = String(normalized);
        return normalized;
    };
    const applyWidthChange = async (rawWidth) => {
        if (selectedLayerId === null) return;
        const width = syncWidthControls(rawWidth);
        if (!localEffectConfig.layerWidths) localEffectConfig.layerWidths = {};
        localEffectConfig.layerWidths[selectedLayerId] = width;
        addToCommandLog('描边宽度', 'info', `图层${selectedLayerId} 描边宽度 → ${width}%（未保存）`);
        const effects = localEffectConfig.layers[selectedLayerId] || [];
        if (localEffectConfig.enabled) {
            await applyAllEffects();
        } else if (effects.length > 0) {
            await previewEffect(selectedLayerId, effects[0]);
        }
    };
    document.getElementById('effect-width-range')?.addEventListener('input', (e) => {
        syncWidthControls(e.target.value);
    });
    document.getElementById('effect-width-range')?.addEventListener('change', (e) => {
        applyWidthChange(e.target.value).catch(err => addToCommandLog('描边宽度', 'error', err.message));
    });
    document.getElementById('effect-width-input')?.addEventListener('change', (e) => {
        applyWidthChange(e.target.value).catch(err => addToCommandLog('描边宽度', 'error', err.message));
    });

    // 绑定重播测试按钮
    document.getElementById('replay-test-btn')?.addEventListener('click', async () => {
        const layerId = selectedLayerId || (videoLayers.length > 0 ? videoLayers[0].id : 1);
        try {
            const effects = localEffectConfig.layers[layerId] || [];
            if (effects.length > 0) {
                await previewEffect(layerId, effects[0]);
            }
            await apiPost('/video/replay', { layerId });
            addToCommandLog('重播测试', 'success', `图层${layerId}已重播`);
        } catch (error) {
            addToCommandLog('重播测试', 'error', '重播失败: ' + error.message);
        }
    });

    updateAppliedEffectsDisplay();
    if (videoLayers.length > 0 && !selectedLayerId) {
        // 选中第一个可用图层
        selectLayer(videoLayers[0].id);
    }

    // 渲染： AudioReactiveEngine 可视化与调参面板
    // （原「效果关联切片」每图层开关已移除：
    //   “已配置效果”列表非空 + 总开关勾上 即等于启用该图层联动）
    renderAudioReactivePanel();
}

// 选择图层
function selectLayer(layerId) {
    selectedLayerId = layerId;

    // 更新卡片选中状态
    document.querySelectorAll('.layer-item').forEach(card => {
        card.classList.toggle('selected', parseInt(card.dataset.layerId) === layerId);
    });

    // 更新描边色 picker → 显示当前图层的颜色（或彩虹默认）
    const picker = document.getElementById('effect-color-picker');
    if (picker) {
        picker.value = (localEffectConfig.layerColors && localEffectConfig.layerColors[layerId]) || '#000000';
    }
    const width = getLayerEffectWidth(layerId);
    const widthRange = document.getElementById('effect-width-range');
    const widthInput = document.getElementById('effect-width-input');
    if (widthRange) widthRange.value = String(width);
    if (widthInput) widthInput.value = String(width);

    // 更新已配置效果显示（按选中图层过滤）
    updateAppliedEffectsDisplay();
}

// 渲染特效列表
function renderEffectList() {
    const container = document.getElementById('effect-list-container');
    if (!container) return;

    clearContainer(container);

    audioEffects.forEach(effect => {
        const effectItem = document.createElement('div');
        effectItem.className = 'effect-item';
        effectItem.dataset.effectId = effect.id;
        effectItem.innerHTML = `
            <div class="effect-name">${effect.name}</div>
            <div class="effect-type">${effect.type}</div>
            <span class="effect-add-icon">+</span>
        `;

        effectItem.addEventListener('click', (e) => {
            // 如果点击的是加号，则直接应用
            if (e.target.classList.contains('effect-add-icon')) {
                selectEffect(effect.id);
                applyEffect(effect.type);
            } else {
                selectEffect(effect.id);
            }
        });
        container.appendChild(effectItem);
    });
}

// 选择特效
function selectEffect(effectId) {
    const effectItem = document.querySelector(`[data-effect-id="${effectId}"]`);
    if (effectItem) {
        toggleButtonGroupActive('.effect-item', effectItem);
        selectedEffect = effectId;
    }
}

// 更新已配置效果显示（从本地配置）
function updateAppliedEffectsDisplay() {
    const container = document.getElementById('applied-effects-container');
    if (!container) return;

    // 将本地配置转换为卡片显示（支持多效果）
    let effectCards = [];
    for (const [layerId, effects] of Object.entries(localEffectConfig.layers)) {
        if (Array.isArray(effects) && effects.length > 0) {
            const layerIdInt = parseInt(layerId);
            // 如果只有一个效果，显示单个卡片（向后兼容）
            if (effects.length === 1) {
                // 统一转换为字符串类型用于显示
                const effectTypeStr = getEffectTypeString(effects[0]);
                effectCards.push({
                    layerId: layerIdInt,
                    effect_type: effectTypeStr,
                    effect_index: 0,
                    is_multi: false
                });
            } else {
                // 多个效果：显示为效果链
                effects.forEach((effectId, index) => {
                    const effectTypeStr = getEffectTypeString(effectId);
                    effectCards.push({
                        layerId: layerIdInt,
                        effect_type: effectTypeStr,
                        effect_index: index,
                        is_multi: true,
                        total_effects: effects.length
                    });
                });
            }
        }
    }

    // 如果选中了图层，只显示该图层的效果
    if (selectedLayerId !== null) {
        effectCards = effectCards.filter(eff => eff.layerId === selectedLayerId);
    }

    // 更新图层指示器颜色
    updateLayerIndicators();

    if (effectCards.length === 0) {
        const msg = selectedLayerId !== null
            ? `图层 ${selectedLayerId} 暂无已配置效果`
            : '暂无已配置效果';
        container.innerHTML = `<span style="color:#888">${msg}</span>`;
        return;
    }

    // 按图层分组显示
    const groupedByLayer = {};
    effectCards.forEach(eff => {
        if (!groupedByLayer[eff.layerId]) {
            groupedByLayer[eff.layerId] = [];
        }
        groupedByLayer[eff.layerId].push(eff);
    });

    let html = '<div style="display: flex; flex-wrap: wrap; gap: 8px; align-items: center;">';
    for (const [layerId, effects] of Object.entries(groupedByLayer)) {
        const effectsList = localEffectConfig.layers[layerId] || [];
        const isMulti = effectsList.length > 1;

        html += `<span style="color: #888; font-size: 12px; margin-right: 4px;">图层${layerId}${isMulti ? `(${effectsList.length}个)` : ''}:</span>`;

        effects.forEach((eff, idx) => {
            html += `
                <div class="applied-effect-card" style="display: inline-flex; align-items: center; gap: 10px; padding: 12px 18px; background: #2a2a4a; border-radius: 6px; font-size: 15px;"
                     data-layer-id="${eff.layerId}" 
                     data-effect-type="${eff.effect_type}"
                     data-effect-index="${eff.effect_index}">
                    <span style="color: #fff;">${isMulti ? `${idx + 1}.` : ''}${getEffectName(eff.effect_type)}</span>
                    <span class="effect-remove-icon" title="移除效果" style="cursor: pointer; color: #f66; font-weight: bold; font-size: 18px;">×</span>
                </div>
            `;
        });
    }
    html += '</div>';

    container.innerHTML = html;

    // 绑定移除按钮事件
    container.querySelectorAll('.effect-remove-icon').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            const card = btn.closest('.applied-effect-card');
            const layerId = parseInt(card.dataset.layerId);
            const effectType = card.dataset.effectType;
            const effectIndex = parseInt(card.dataset.effectIndex || '0');
            removeEffectFromConfig(layerId, effectType, effectIndex);
        });
    });
}

// 更新图层指示器颜色
function updateLayerIndicators() {
    document.querySelectorAll('#effect-properties-container .layer-item').forEach(card => {
        const layerId = parseInt(card.dataset.layerId);
        const layerEffects = localEffectConfig.layers[layerId] || [];
        const hasEffect = layerEffects.length > 0;
        const indicator = card.querySelector('.layer-visibility');
        if (indicator) {
            indicator.classList.remove('visible', 'has-effect');
            indicator.classList.add(hasEffect ? 'has-effect' : 'visible');
            indicator.title = hasEffect ? `已配置 ${layerEffects.length} 个特效` : '无特效';
        }
    });
}

// 从本地配置中移除效果
function removeEffectFromConfig(layerId, effectType, effectIndex = null) {
    if (!localEffectConfig.layers[layerId]) return;

    const effects = localEffectConfig.layers[layerId];
    let index = -1;

    if (effectIndex !== null && effectIndex >= 0 && effectIndex < effects.length) {
        // 使用指定的索引
        index = effectIndex;
    } else {
        // 查找效果类型（统一转换为ID进行比较）
        const effectId = getEffectTypeId(effectType);
        index = effects.indexOf(effectId);
    }

    if (index > -1) {
        const removedEffectId = effects[index];
        const removedEffectStr = getEffectTypeString(removedEffectId);
        effects.splice(index, 1);
        if (effects.length === 0) {
            delete localEffectConfig.layers[layerId];
        }
        const remainingCount = effects.length;
        if (localEffectConfig.enabled) {
            if (remainingCount > 0) {
                applyAllEffects().catch(e => addToCommandLog('应用效果', 'error', '移除后重新应用失败: ' + e.message));
            } else {
                apiPost('/audio-effect/disable', { layerId: layerId }).catch(e => addToCommandLog('应用效果', 'error', '禁用图层效果失败: ' + e.message));
            }
        }
        addToCommandLog('移除效果', 'success',
            `已移除图层 ${layerId} 的 ${removedEffectStr} 效果（剩余 ${remainingCount} 个，未保存）`);
        updateAppliedEffectsDisplay();
        renderLayerCards();
    }
}

// 添加效果到本地配置
function addEffectToConfig(layerId, effectType) {
    if (effectType === 'logo_show') {
        previewGlobalEffect(effectType).catch(e => addToCommandLog('全局效果', 'error', 'LOGO演艺启动失败: ' + e.message));
        return;
    }
    if (!localEffectConfig.layers[layerId]) {
        localEffectConfig.layers[layerId] = [];
    }

    // 检查是否已达到最大数量（10个）
    if (localEffectConfig.layers[layerId].length >= 10) {
        showWarning(`图层 ${layerId} 已达到最大效果数量（10个）`);
        addToCommandLog('添加效果', 'warning', `图层 ${layerId} 已达到最大效果数量（10个）`);
        return;
    }

    // 统一转换为数字ID存储
    const effectId = getEffectTypeId(effectType);
    const effectTypeStr = getEffectTypeString(effectId);

    // 检查是否已存在（阻止添加重复效果）
    if (localEffectConfig.layers[layerId].includes(effectId)) {
        showWarning(`${effectTypeStr} 效果已存在，不能重复添加`);
        addToCommandLog('添加效果', 'warning', `${effectTypeStr} 效果已存在于图层 ${layerId}，不能重复添加`);
        return;
    }

    localEffectConfig.layers[layerId].push(effectId);
    const totalCount = localEffectConfig.layers[layerId].length;
    addToCommandLog('添加效果', 'success',
        `已添加 ${effectTypeStr} 到图层 ${layerId}（共 ${totalCount} 个效果，未保存）`);
    updateAppliedEffectsDisplay();
    renderLayerCards();
    if (localEffectConfig.enabled) {
        applyAllEffects().catch(e => addToCommandLog('应用效果', 'error', '添加后自动应用失败: ' + e.message));
    }
    previewEffect(layerId, effectId).catch(e => addToCommandLog('预览效果', 'error', '预览失败: ' + e.message));
}

// 获取效果名称
function getEffectName(type) {
    const effect = audioEffects.find(e => e.type === type);
    return effect ? effect.name : type;
}

async function previewGlobalEffect(effectType) {
    const payload = {
        global: true,
        effectType,
        type: effectType,
        intensity: 1.0
    };
    const response = await apiPost('/audio-effect/preview', payload);
    if (response) {
        addToCommandLog('全局效果', 'success', `${getEffectName(effectType)} 已启动（所有视频图层）`);
    }
    return !!response;
}

// 应用音频效果（添加到本地配置，而非立即发送到后端）
function applyEffect(effectType) {
    if (effectType === 'logo_show') {
        previewGlobalEffect(effectType).catch(e => addToCommandLog('全局效果', 'error', 'LOGO演艺启动失败: ' + e.message));
        return;
    }
    if (!selectedLayerId) {
        showWarning('请先选择目标图层');
        return;
    }

    if (effectType === 'none') {
        delete localEffectConfig.layers[selectedLayerId];
        addToCommandLog('清除效果', 'success', `已清除图层 ${selectedLayerId} 的全部效果（未保存）`);
        updateAppliedEffectsDisplay();
        renderLayerCards();
        if (localEffectConfig.enabled) {
            apiPost('/audio-effect/disable', { layerId: selectedLayerId }).catch(e => addToCommandLog('应用效果', 'error', '禁用图层效果失败: ' + e.message));
        }
        return;
    }

    addEffectToConfig(selectedLayerId, effectType);
}

// ==========================================================================
// AudioReactiveEngine 面板：控制图层在节拍/瞬态触发时的联动效果。
// ==========================================================================

/**
 * 渲染"效果关联切片"开关面板到属性区。
 * 节拍/瞬态触发由新的 AudioReactiveEngine 负责（尚未接入），此开关仅
 * 控制每个图层是否响应节拍切换切片。
 */
async function renderEffectLinkedSlicesPanel() {
    const container = document.getElementById('effect-properties-container');
    if (!container) return;

    const panel = document.createElement('div');
    panel.className = 'effect-linked-slices-panel';
    panel.innerHTML = `
        <h4 class="section-title" style="margin-top: 20px; border-top: 1px solid #333; padding-top: 15px;">
            🔗 效果关联切片
        </h4>
        <div style="background: #1a1a1a; padding: 12px; border-radius: 6px;">
            <div style="display: flex; align-items: center; justify-content: space-between;">
                <span style="font-size: 12px; color: #888;">开启后，效果触发时自动切换切片</span>
                <label style="display: flex; align-items: center; gap: 6px; cursor: pointer;">
                    <input type="checkbox" id="effect-linked-slices-toggle">
                    <span style="font-size: 11px;">启用</span>
                </label>
            </div>
            <div style="font-size: 10px; color: #666; margin-top: 8px;">
                提示：需先在图层矩阵中配置多个切片
            </div>
        </div>
    `;
    container.appendChild(panel);

    const toggle = document.getElementById('effect-linked-slices-toggle');
    if (!toggle) return;

    // 初始化时加载当前选中图层的状态
    updateEffectLinkedSlicesState();

    toggle.addEventListener('change', async () => {
        const layerId = selectedLayerId || 1;
        try {
            await apiPut(`/config/layers/${layerId}`, { effectLinkedSlices: toggle.checked });
            addToCommandLog('效果关联切片', 'success',
                `图层${layerId} 效果关联切片已${toggle.checked ? '启用' : '禁用'}（未保存）`);
        } catch (error) {
            addToCommandLog('效果关联切片', 'error', '设置失败: ' + error.message);
        }
    });
}

export function stopAudioReactiveRefresh() {
    // 停止 AudioReactiveEngine 轮询（路由切换 / 页面隐藏时调用）
    stopAudioReactivePolling(!effectsLicenseDenied);
}

// ==========================================================================
// AudioReactiveEngine 可视化与调参面板
//   - 实时显示 BPM、节拍相位、4 段瞬态指示灯、64 段频谱条、Drop 闪烁
//   - 调参：4 段瞬态阈值滑块、BPM 范围、Drop 灵敏度、频谱增益
//   - 自适应学习按钮（30s 学习当前曲风的 flux 分布并写回阈值）
// 数据源：/api/audio-reactive/{state,spectrum,config,engine,learn}
// 注：DMX 物理灯光由独立模块管理，本面板只做特征提取 → 渲染效果关联。
// ==========================================================================

let audioReactivePollTimer = null;
let audioReactiveSpectrumCtx = null;
let audioReactiveConfigCache = null;

function stopAudioReactivePolling(notifyBackend = true) {
    if (audioReactivePollTimer) {
        clearInterval(audioReactivePollTimer);
        audioReactivePollTimer = null;
    }
    audioReactiveSpectrumCtx = null;
    // 通知后端卸下 audio callback，避免离开页面后还在跑 FFT 卡音频解码线程
    if (notifyBackend && !effectsLicenseDenied) {
        try { apiPost('/audio-reactive/engine', { enabled: false }); } catch (e) {}
    }
}

async function renderAudioReactivePanel() {
    stopAudioReactivePolling(false);

    const container = document.getElementById('effect-properties-container');
    if (!container) return;

    const panel = document.createElement('div');
    panel.className = 'audio-reactive-panel';
    panel.innerHTML = `
        <h4 class="section-title" style="margin-top: 20px; border-top: 1px solid #333; padding-top: 15px;">
            🎵 音频反应引擎（AudioReactiveEngine）
        </h4>
        <div style="background: #1a1a1a; padding: 14px; border-radius: 6px; display: grid; gap: 12px;">

            <!-- 实时指标条 -->
            <div style="display:grid; grid-template-columns: repeat(4, 1fr); gap: 8px; font-size: 12px;">
                <div style="background:#222; padding:8px; border-radius:4px;">
                    <div style="color:#888; font-size:10px;">BPM</div>
                    <div id="ar-bpm" style="color:#0f8; font-size:20px; font-weight:bold;">--</div>
                    <div id="ar-bpm-conf" style="color:#666; font-size:10px;">conf --</div>
                </div>
                <div style="background:#222; padding:8px; border-radius:4px;">
                    <div style="color:#888; font-size:10px;">RMS</div>
                    <div id="ar-rms-bar" style="height:6px; background:#444; border-radius:3px; margin-top:4px; overflow:hidden;">
                        <div id="ar-rms-fill" style="width:0%; height:100%; background:linear-gradient(90deg,#0f8,#ff0,#f00); transition:width 0.05s;"></div>
                    </div>
                    <div style="color:#888; font-size:10px; margin-top:4px;">Flux</div>
                    <div id="ar-flux-bar" style="height:6px; background:#444; border-radius:3px; margin-top:4px; overflow:hidden;">
                        <div id="ar-flux-fill" style="width:0%; height:100%; background:#08f; transition:width 0.05s;"></div>
                    </div>
                </div>
                <div style="background:#222; padding:8px; border-radius:4px;">
                    <div style="color:#888; font-size:10px;">Drop</div>
                    <div id="ar-drop-ind" style="margin-top:4px; padding:6px; text-align:center; border-radius:4px; background:#333; color:#666; font-size:12px; font-weight:bold;">IDLE</div>
                    <div id="ar-drop-level" style="color:#888; font-size:10px; margin-top:4px;">0%</div>
                </div>
                <div style="background:#222; padding:8px; border-radius:4px;">
                    <div style="color:#888; font-size:10px;">Beat Phase</div>
                    <div style="height:16px; background:#333; border-radius:8px; margin-top:4px; position:relative; overflow:hidden;">
                        <div id="ar-beat-cursor" style="position:absolute; width:4px; height:100%; background:#ff0; left:0%; transition:left 0.05s;"></div>
                    </div>
                    <div id="ar-beat-flash" style="margin-top:4px; height:8px; border-radius:4px; background:#333; transition:background 0.1s;"></div>
                </div>
            </div>

            <!-- 4 段瞬态指示灯 + 能量条 -->
            <div>
                <div style="font-size:11px; color:#888; margin-bottom:6px;">4 通道瞬态（sub-bass / bass / mid / high）</div>
                <div id="ar-bands" style="display:grid; grid-template-columns: repeat(4, 1fr); gap: 6px;"></div>
            </div>

            <!-- 频谱可视化 -->
            <div>
                <div style="font-size:11px; color:#888; margin-bottom:6px;">实时频谱（64 段 log-spaced）</div>
                <canvas id="ar-spectrum" width="480" height="80" style="width:100%; height:80px; background:#0a0a0a; border-radius:4px; display:block;"></canvas>
            </div>

            <!-- 自适应学习（DMX 灯光由独立模块管理，本面板只做特征提取） -->
            <div style="background:#222; padding:8px; border-radius:4px; display:flex; align-items:center; gap:6px;">
                <button type="button" id="ar-learn-btn" class="btn" style="font-size:12px;">▶ 学习当前曲风（30s）</button>
                <div id="ar-learn-progress" style="flex:1; height:4px; background:#444; border-radius:2px; overflow:hidden;">
                    <div id="ar-learn-fill" style="width:0%; height:100%; background:#ff0; transition:width 0.2s;"></div>
                </div>
            </div>

            <!-- 调参 -->
            <details style="background:#222; padding:8px; border-radius:4px;">
                <summary style="cursor:pointer; font-size:12px; color:#aaa;">⚙️ 高级调参</summary>
                <div id="ar-config-sliders" style="display:grid; gap:8px; margin-top:10px;"></div>
            </details>
        </div>
    `;
    container.appendChild(panel);

    // 渲染 4 通道瞬态指示灯
    const bandLabels = ['sub-bass', 'bass', 'mid', 'high'];
    const bandColors = ['#08f', '#0f8', '#ff0', '#f80'];
    const bandsEl = document.getElementById('ar-bands');
    if (bandsEl) {
        bandsEl.innerHTML = bandLabels.map((lbl, i) => `
            <div style="background:#1a1a2a; padding:6px; border-radius:4px;">
                <div style="display:flex; align-items:center; gap:6px;">
                    <div id="ar-band-led-${i}" style="width:10px; height:10px; border-radius:50%; background:#333; box-shadow:0 0 0 transparent;"></div>
                    <span style="font-size:11px; color:#aaa;">${lbl}</span>
                </div>
                <div style="height:6px; background:#333; border-radius:3px; margin-top:4px; overflow:hidden;">
                    <div id="ar-band-energy-${i}" style="width:0%; height:100%; background:${bandColors[i]}; transition:width 0.05s;"></div>
                </div>
                <div style="height:4px; background:#333; border-radius:2px; margin-top:2px; overflow:hidden;">
                    <div id="ar-band-trans-${i}" style="width:0%; height:100%; background:#fff; transition:width 0.05s;"></div>
                </div>
            </div>
        `).join('');
    }

    // 初始化频谱 canvas
    const canvas = document.getElementById('ar-spectrum');
    audioReactiveSpectrumCtx = canvas ? canvas.getContext('2d') : null;

    // 加载配置 → 渲染滑块
    try {
        audioReactiveConfigCache = await apiGet('/audio-reactive/config');
    } catch (e) { audioReactiveConfigCache = null; }
    renderAudioReactiveConfigSliders();

    // 学习按钮
    document.getElementById('ar-learn-btn')?.addEventListener('click', async () => {
        try {
            const status = await apiGet('/audio-reactive/learn');
            if (status && status.learning) {
                await apiPost('/audio-reactive/learn', { action: 'stop' });
                addToCommandLog('自适应学习', 'info', '已手动停止');
            } else {
                await apiPost('/audio-reactive/learn', { action: 'start', durationSec: 30, kStd: 2.5 });
                addToCommandLog('自适应学习', 'success', '已开始 30 秒学习');
            }
        } catch (err) {
            addToCommandLog('自适应学习', 'error', '操作失败: ' + err.message);
        }
    });

    // 通知后端挂上 audio callback，开始喂数据给 AudioReactiveEngine
    try { await apiPost('/audio-reactive/engine', { enabled: true }); } catch (e) {}

    // 启动 10Hz 轮询
    audioReactivePollTimer = setInterval(refreshAudioReactiveState, 100);
}

function renderAudioReactiveConfigSliders() {
    const container = document.getElementById('ar-config-sliders');
    if (!container) return;
    const c = audioReactiveConfigCache;
    if (!c) {
        container.innerHTML = '<span style="color:#666; font-size:11px;">配置加载失败</span>';
        return;
    }
    const bandNames = ['sub', 'bass', 'mid', 'high'];
    const thr = c.transientThreshold || [1.7, 1.5, 1.4, 1.4];
    const cut = c.bandCutoffHz || [80, 250, 2000];

    const slider = (id, label, val, min, max, step) => `
        <div style="display:grid; grid-template-columns: 100px 1fr 50px; gap:8px; align-items:center;">
            <span style="font-size:11px; color:#aaa;">${label}</span>
            <input type="range" id="${id}" min="${min}" max="${max}" step="${step}" value="${val}" style="width:100%;">
            <span id="${id}-val" style="font-size:11px; color:#0f8; text-align:right;">${val}</span>
        </div>
    `;
    container.innerHTML = [
        slider('ar-cut-0', 'cut sub/bass Hz', cut[0], 20, 200, 5),
        slider('ar-cut-1', 'cut bass/mid Hz', cut[1], 100, 800, 10),
        slider('ar-cut-2', 'cut mid/high Hz', cut[2], 800, 6000, 50),
        ...bandNames.map((n, i) => slider(`ar-thr-${i}`, `阈值 ${n}`, thr[i], 1.1, 3.0, 0.05)),
        slider('ar-bpm-min', 'BPM 下限', c.bpmMin || 70, 40, 120, 1),
        slider('ar-bpm-max', 'BPM 上限', c.bpmMax || 200, 120, 240, 1),
        slider('ar-drop-ratio', 'Drop RMS 比', c.dropRmsRatio || 1.65, 1.2, 4.0, 0.05),
        slider('ar-drop-decay', 'Drop 衰减(s)', c.dropDecaySec || 1.5, 0.2, 5.0, 0.1),
        slider('ar-spec-gain', '频谱增益', c.spectrumGain || 1.0, 0.5, 3.0, 0.1),
        slider('ar-dense-sub',   'Dense 中频×',      c.denseSubBassRatio   ?? 1.12, 1.02, 2.0, 0.02),
        slider('ar-dense-rms',   'Dense 高频×',      c.denseRmsRatio       ?? 1.06, 1.00, 1.6, 0.02),
        slider('ar-dense-dwell', 'Dense 进入占比',   c.denseEnterDwellMs   ?? 360,  100, 800, 20),
        slider('ar-dense-exit',  'Dense 退出占比',   c.denseExitConfirmMs  ?? 120,   20, 500, 20),
    ].join('');

    // 绑定所有滑块
    container.querySelectorAll('input[type=range]').forEach(input => {
        const valEl = document.getElementById(input.id + '-val');
        input.addEventListener('input', () => {
            if (valEl) valEl.textContent = parseFloat(input.value).toFixed(2);
        });
        input.addEventListener('change', commitAudioReactiveConfig);
    });
}

async function commitAudioReactiveConfig() {
    const get = (id) => parseFloat(document.getElementById(id)?.value || 0);
    const cfg = {
        bandCutoffHz: [get('ar-cut-0'), get('ar-cut-1'), get('ar-cut-2')],
        transientThreshold: [get('ar-thr-0'), get('ar-thr-1'), get('ar-thr-2'), get('ar-thr-3')],
        bpmMin: get('ar-bpm-min'),
        bpmMax: get('ar-bpm-max'),
        dropRmsRatio: get('ar-drop-ratio'),
        dropDecaySec: get('ar-drop-decay'),
        spectrumGain: get('ar-spec-gain'),
        denseSubBassRatio:  get('ar-dense-sub'),
        denseRmsRatio:      get('ar-dense-rms'),
        denseEnterDwellMs:  Math.round(get('ar-dense-dwell')),
        denseExitConfirmMs: Math.round(get('ar-dense-exit')),
    };
    try {
        await apiPost('/audio-reactive/config', cfg);
        audioReactiveConfigCache = { ...(audioReactiveConfigCache || {}), ...cfg };
    } catch (e) {
        addToCommandLog('音频反应配置', 'error', '保存失败: ' + e.message);
    }
}

async function refreshAudioReactiveState() {
    // 拉状态 + 频谱 + 学习进度（按需）
    let st = null, sp = null;
    try {
        st = await apiGet('/audio-reactive/state');
    } catch (e) { return; /* 服务器没起就静默 */ }
    try { sp = await apiGet('/audio-reactive/spectrum'); } catch (e) {}

    if (!st) return;

    // BPM 节拍
    const bpmEl = document.getElementById('ar-bpm');
    const confEl = document.getElementById('ar-bpm-conf');
    if (bpmEl) bpmEl.textContent = st.bpm > 1 ? st.bpm.toFixed(1) : '--';
    if (confEl) confEl.textContent = `conf ${(st.bpmConfidence * 100).toFixed(0)}%`;

    // RMS / 频谱通量
    const rmsFill = document.getElementById('ar-rms-fill');
    if (rmsFill) rmsFill.style.width = Math.round(st.rms * 100) + '%';
    const fluxFill = document.getElementById('ar-flux-fill');
    if (fluxFill) fluxFill.style.width = Math.round(st.spectralFlux * 100) + '%';

    // Drop 段落检测
    const dropInd = document.getElementById('ar-drop-ind');
    if (dropInd) {
        dropInd.style.background = st.dropActive ? '#f04' : '#333';
        dropInd.style.color = st.dropActive ? '#fff' : '#666';
        dropInd.textContent = st.dropActive ? '🔥 DROP' : 'IDLE';
    }
    const dropLv = document.getElementById('ar-drop-level');
    if (dropLv) dropLv.textContent = Math.round(st.dropIntensity * 100) + '%';

    // 节拍相位
    const cursor = document.getElementById('ar-beat-cursor');
    if (cursor) cursor.style.left = Math.round(st.beatPhase * 100) + '%';
    const beatFlash = document.getElementById('ar-beat-flash');
    if (beatFlash) beatFlash.style.background = st.beatThisFrame ? '#ff0' : '#333';

    // 4 段
    if (Array.isArray(st.bands)) {
        for (let i = 0; i < st.bands.length && i < 4; ++i) {
            const b = st.bands[i];
            const led = document.getElementById(`ar-band-led-${i}`);
            if (led) {
                const fired = b.transientThisFrame;
                led.style.background = fired ? '#fff' : '#333';
                led.style.boxShadow = fired ? '0 0 8px #fff' : 'none';
            }
            const e = document.getElementById(`ar-band-energy-${i}`);
            if (e) e.style.width = Math.round(b.energy * 100) + '%';
            const t = document.getElementById(`ar-band-trans-${i}`);
            if (t) t.style.width = Math.round(b.transient * 100) + '%';
        }
    }

    // 频谱
    if (sp && Array.isArray(sp.spectrum) && audioReactiveSpectrumCtx) {
        const ctx = audioReactiveSpectrumCtx;
        const canvas = ctx.canvas;
        const w = canvas.width, h = canvas.height;
        ctx.clearRect(0, 0, w, h);
        const bins = sp.spectrum;
        const bw = w / bins.length;
        for (let i = 0; i < bins.length; ++i) {
            const v = Math.max(0, Math.min(1, bins[i]));
            const bh = v * h;
            // 颜色按频率渐变
            const hue = 240 - (i / bins.length) * 240;
            ctx.fillStyle = `hsl(${hue}, 80%, ${30 + v * 40}%)`;
            ctx.fillRect(i * bw, h - bh, bw - 1, bh);
        }
    }

    // 学习进度
    try {
        const learn = await apiGet('/audio-reactive/learn');
        const fill = document.getElementById('ar-learn-fill');
        const btn = document.getElementById('ar-learn-btn');
        if (learn && fill) fill.style.width = Math.round((learn.progress || 0) * 100) + '%';
        if (learn && btn) btn.textContent = learn.learning ? '⏹ 停止学习' : '▶ 学习当前曲风（30s）';
        // 学习结束后刷新一次配置
        if (learn && !learn.learning && fill && fill.style.width === '100%') {
            audioReactiveConfigCache = await apiGet('/audio-reactive/config');
            renderAudioReactiveConfigSliders();
        }
    } catch (e) {}
}

// 更新效果关联切片开关状态
async function updateEffectLinkedSlicesState() {
    const toggle = document.getElementById('effect-linked-slices-toggle');
    if (!toggle) return;

    // 使用已选择的图层，如果没有则使用 videoLayers 中的第一个
    const layerId = selectedLayerId || (videoLayers.length > 0 ? videoLayers[0].id : null);
    if (!layerId) {
        // 没有可用图层时，禁用开关并显示提示
        toggle.disabled = true;
        return;
    }
    
    try {
        const response = await apiGet(`/layers/${layerId}`);
        if (response != null) {
            toggle.disabled = false;
            toggle.checked = response.effect_linked_slices || false;
        } else {
            // 图层不存在时，禁用开关
            toggle.disabled = true;
        }
    } catch (error) {
        console.error('[EffectControl] 获取效果关联切片状态失败:', error);
        // 发生错误时，禁用开关
        toggle.disabled = true;
    }
}

// 文件结束
