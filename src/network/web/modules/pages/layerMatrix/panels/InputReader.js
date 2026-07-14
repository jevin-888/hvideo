// 图层属性面板的输入读取层。
// updateLayerFromInputs（实时画布预览）与 applyLayerProperties（同步到服务器）
// 共用此读取器，避免两处分别维护"读什么 ID / 转换规则"的重复逻辑。

const num = (id, def = 0) => {
    const el = document.getElementById(id);
    if (!el) return def;
    const v = parseFloat(el.value);
    return Number.isNaN(v) ? def : v;
};
const intNum = (id, def = 0) => {
    const el = document.getElementById(id);
    if (!el) return def;
    const v = parseInt(el.value, 10);
    return Number.isNaN(v) ? def : v;
};
const fitModeValue = (value) => Number(value) > 0 ? 1 : 0;
const has = (id) => !!document.getElementById(id);
const el = (id) => document.getElementById(id);

/**
 * 读取所有"通用图层"输入（位置 / 尺寸 / 透明度 / 优先级 / 旋转 / 形状 / 反转 /
 * 黑色透明 / 视频音量 / 高斯模糊 / 裁剪 / 漫游）。
 *
 * 仅当对应输入控件存在于当前 DOM 时才会写入返回对象，调用方可凭"键是否存在"
 * 判断该字段是否需要应用。无副作用，纯读取。
 *
 * 示例/字段：@param {number} layerId
 * @param {Object} layer 当前 layers 数组中的图层对象（用作默认值兜底）
 * @returns {Object} 以 snake_case key 表达的属性增量
 */
export function readLayerCommonInputs(layerId, layer) {
    const r = {};

    // 位置 / 尺寸（始终存在；若控件缺失则按 0 处理，保持与旧逻辑一致）
    r.position = { x: num('layer-x'), y: num('layer-y') };
    r.size = { width: num('layer-width'), height: num('layer-height') };
    r.priority = num('layer-priority', layerId);

    if (has('layer-rotation')) {
        const raw = num('layer-rotation', 0);
        r.rotation = Math.min(360, Math.max(0, Math.round(raw)));
    }

    // 透明度：layer-alpha 为 0-255，layer.alpha 为 0-1.0
    if (has('layer-alpha')) {
        const rawAlpha = num('layer-alpha', 255);
        r.alpha = rawAlpha / 255;
        if (layerId === 70) r.alpha_255 = Math.round(rawAlpha);
    }

    // 视频/采集图层音量：video-volume 0-100，layer.volume 0-1.0
    if (has('video-volume')) {
        r.volume = num('video-volume', 100) / 100;
    } else if (has('capture-volume')) {
        r.volume = num('capture-volume', 100) / 100;
    }

    if (has('video-gaussian-blur')) {
        r.gaussian_blur = Math.round(num('video-gaussian-blur', 0));
    }

    if (has('layer-shape-type')) {
        r.shape_type = intNum('layer-shape-type', 0);
        r.shape_param = num('layer-shape-param', 0.0);
    }

    if (has('layer-black-to-transparent')) {
        const e = el('layer-black-to-transparent');
        r.black_to_transparent = e.tagName === 'SELECT' ? e.value === 'true' : !!e.checked;
    }

    if (has('layer-invert')) {
        r.invert = intNum('layer-invert', 0);
    }

    if (has('video-fit-mode')) {
        r.fit_mode = fitModeValue(intNum('video-fit-mode', 0));
    }

    if (has('mirror-ready-hint-visible')) {
        r.mirror_ready_hint_visible = el('mirror-ready-hint-visible').checked !== false;
    }

    if (has('mirror-tv-vertical-crop-px')) {
        r.tv_vertical_crop_px = Math.max(
            0,
            Math.min(4000, Math.round(intNum('mirror-tv-vertical-crop-px', 0)))
        );
    }

    if (has('layer-roam-mode')) {
        const mode = intNum('layer-roam-mode', 0);
        r.roam_config = {
            enabled: true,
            mode,
            speed: num('layer-roam-speed', 100),
            rangeX: num('layer-roam-range-x', 500),
            rangeY: num('layer-roam-range-y', 500),
            radius: num('layer-roam-radius', 200),
            loop: el('roam-loop')?.checked !== false
        };
    }

    return r;
}
