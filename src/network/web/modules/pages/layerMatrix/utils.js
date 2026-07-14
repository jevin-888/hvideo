// 图层矩阵工具函数模块

// 验证配置是否有效
export function isConfigValid(canvasConfig) {
    return canvasConfig.loaded &&
        canvasConfig.width > 0 && canvasConfig.height > 0 &&
        canvasConfig.cols > 0 && canvasConfig.rows > 0;
}

// 验证配置并显示错误（用于需要阻止操作的场景）
export function validateConfig(canvasConfig, addToCommandLog) {
    if (!isConfigValid(canvasConfig)) {
        if (typeof addToCommandLog === 'function') {
            addToCommandLog('配置错误', 'error', '配置未加载，无法执行操作');
        }
        return false;
    }
    return true;
}

// 从名称字符串中提取“纯名称”部分（去掉已有的 Layer/图层 前缀）
function getLayerNamePart(str) {
    if (!str || typeof str !== 'string') return '图层';
    const cleaned = str.replace(/^(?:Layer|图层)\d*\s*/i, '').trim();
    return cleaned || '图层';
}

// 去掉名称中重复的 "Layer"，因为前面已有 "Layer{id}"
function stripRedundantLayer(namePart) {
    if (!namePart || typeof namePart !== 'string') return namePart || '图层';
    const stripped = namePart.replace(/Layer/gi, '').trim();
    return stripped || namePart || '图层';
}

/** 获取图层模板的显示名称（用于矩阵卡片等），格式 "Layer{id} 名称" */
export function getLayerTemplateDisplayName(template) {
    if (!template || template.id == null) return '图层';
    const namePart = stripRedundantLayer(getLayerNamePart(template.name || template.alias || ''));
    return `Layer${template.id} ${namePart}`;
}

// 获取图层的显示名称，统一为 "Layer{id} 名称" 格式
export function getLayerDisplayName(layer, ALL_AVAILABLE_LAYERS) {
    // 如果 layer.name 已包含 "切片"，且已是 "Layer..." 格式，去掉名称中重复的 Layer 后返回
    if (layer.name && layer.name.includes('切片') && /^Layer\d+/i.test(layer.name)) {
        const m = layer.name.match(/^(Layer\d+)\s*(.*)$/i);
        if (m) return `${m[1]} ${stripRedundantLayer(m[2])}`.trim();
        return layer.name;
    }
    if (layer.name && layer.name.includes('切片')) {
        let namePart = getLayerNamePart(layer.name);
        namePart = stripRedundantLayer(namePart);
        return `Layer${layer.id} ${namePart}`;
    }

    let namePart;
    const layerTemplate = ALL_AVAILABLE_LAYERS && ALL_AVAILABLE_LAYERS.find(tpl => tpl.id === layer.id);
    if (layerTemplate && (layerTemplate.name || layerTemplate.alias)) {
        namePart = getLayerNamePart(layerTemplate.name || layerTemplate.alias);
    } else {
        namePart = getLayerNamePart(layer.name || '');
    }

    // 图层21特殊处理：显示为 "Layer21 lyrics"
    if (layer.id === 21) {
        namePart = (namePart || '').replace(/\s*-\s*text/i, 'lyrics').replace(/\s*-\s*文本/i, 'lyrics');
        if (!namePart || !/lyrics/i.test(namePart)) namePart = 'lyrics';
    }

    namePart = stripRedundantLayer(namePart);
    return `Layer${layer.id} ${namePart}`.trim();
}

// 解析字符串格式的position ("x y")
export function parsePosition(position) {
    if (typeof position === 'string') {
        const parts = position.trim().split(/\s+/);
        return {
            x: parseFloat(parts[0]) || 0,
            y: parseFloat(parts[1]) || 0
        };
    }
    return position || { x: 0, y: 0 };
}

// 解析字符串格式的size ("宽度 高度")
export function parseSize(size) {
    if (typeof size === 'string') {
        const parts = size.trim().split(/\s+/);
        return {
            width: parseFloat(parts[0]) || 0,
            height: parseFloat(parts[1]) || 0
        };
    }
    return size || { width: 0, height: 0 };
}

// 解析字符串格式的颜色 ("R G B A")
export function parseColor(colorStr, defaultValue = "1.0 1.0 1.0 1.0") {
    if (!colorStr || typeof colorStr !== 'string') {
        return defaultValue;
    }
    return colorStr.trim();
}

/**
 * 解析切片的位置和尺寸（兼容多种格式：coordinate, range, position+size）
 * @param {Object} sliceData - 切片数据对象
 * @param {Object} fallbackSize - 回退尺寸对象 {宽度, 高度}
 * @returns {Object} {x, y, 宽度, 高度} 或 null（如果无法解析）
 */
export function parseSliceCoordinate(sliceData, fallbackSize = null) {
    if (!sliceData) return null;

    let x = 0, y = 0, width = 0, height = 0;

    // 优先使用 coordinate（后端格式："x y 宽度 高度"）
    if (sliceData.coordinate && typeof sliceData.coordinate === 'string') {
        const parts = sliceData.coordinate.split(' ');
        if (parts.length >= 4) {
            x = parseInt(parts[0]) || 0;
            y = parseInt(parts[1]) || 0;
            width = parseInt(parts[2]) || 0;
            height = parseInt(parts[3]) || 0;
            return { x, y, width, height };
        }
    }

    // 其次使用 range（后端格式："x y 宽度 高度"）
    if (sliceData.range && typeof sliceData.range === 'string') {
        const parts = sliceData.range.split(' ');
        if (parts.length >= 4) {
            x = parseInt(parts[0]) || 0;
            y = parseInt(parts[1]) || 0;
            width = parseInt(parts[2]) || 0;
            height = parseInt(parts[3]) || 0;
            return { x, y, width, height };
        }
    }

    // 最后使用 position + size（分离格式）
    if (sliceData.position) {
        if (typeof sliceData.position === 'string') {
            const parts = sliceData.position.split(' ');
            if (parts.length >= 2) {
                x = parseInt(parts[0]) || 0;
                y = parseInt(parts[1]) || 0;
            }
        } else {
            x = sliceData.position.x || 0;
            y = sliceData.position.y || 0;
        }
    }

    if (sliceData.size) {
        if (typeof sliceData.size === 'string') {
            const parts = sliceData.size.split(' ');
            if (parts.length >= 2) {
                width = parseInt(parts[0]) || 0;
                height = parseInt(parts[1]) || 0;
            }
        } else {
            width = sliceData.size.width || 0;
            height = sliceData.size.height || 0;
        }
    }

    // 如果宽度或高度为0，使用回退值
    if ((width === 0 || height === 0) && fallbackSize) {
        width = width || fallbackSize.width || 0;
        height = height || fallbackSize.height || 0;
    }

    return { x, y, width, height };
}

/**
 * 获取图层类型判断对象（用于简化类型检查）
 * @param {Object} layer - 图层对象
 * @param {number} layerId - 图层ID
 * @returns {Object} 包含各种类型判断标志的对象
 */
export function getLayerTypeFlags(layer, layerId) {
    const layerType = layer?.type || 'unknown';
    const lid = Number(layerId);

    const isVideoLayer = layerType === 'video' || layerType === 'VIDEO' || (lid >= 1 && lid <= 4);
    const isCaptureLayer = lid === 10 || lid === 11;
    const isQRCodeLayer = layerType === 'qrcode' || layerType === 'QRCODE' || lid === 71;
    const isImageLayer = layerType === 'image' || layerType === 'IMAGE';
    const isTextLayer = layerType === 'text' || layerType === 'TEXT' || lid === 21 || lid === 30 || lid === 40 || lid === 41;
    const isEffectLayer = layerType === 'effect' || layerType === 'EFFECT';
    const isLyricLayer = lid === 21;
    const isLayer40 = lid === 40;
    const isLayer41 = lid === 41;

    return {
        isVideoLayer,
        isCaptureLayer,
        isQRCodeLayer,
        isImageLayer,
        isTextLayer,
        isEffectLayer,
        isLyricLayer,
        isLayer40,
        isLayer41,
        // 派生标志：纯视频图层（含视频但不含采集/二维码），用于参数面板布局判定
        isPureVideo: isVideoLayer && !isCaptureLayer && !isQRCodeLayer,
        // 派生标志：是否支持漫游配置（视频1-4、采集10/11、图像60）
        isRoamCapable: (lid >= 1 && lid <= 4) || lid === 10 || lid === 11 || lid === 60
    };
}

// 将"R G B A"格式（0.0-1.0）转换为CSS颜色格式（用于color input和色块显示，返回#RRGGBB格式）
export function rgbaStringToCssColor(rgbaStr) {
    if (!rgbaStr || typeof rgbaStr !== 'string') {
        return '#FFFFFF';
    }
    const parts = rgbaStr.trim().split(/\s+/);
    if (parts.length < 3) {
        return '#FFFFFF';
    }
    const r = Math.round(parseFloat(parts[0]) * 255);
    const g = Math.round(parseFloat(parts[1]) * 255);
    const b = Math.round(parseFloat(parts[2]) * 255);
    // 返回#RRGGBB格式（color input需要这种格式）
    return `#${r.toString(16).padStart(2, '0')}${g.toString(16).padStart(2, '0')}${b.toString(16).padStart(2, '0')}`;
}

// 将CSS颜色格式（rgb(r, g, b)）转换为"R G B A"格式（0.0-1.0）
export function cssColorToRgbaString(cssColor) {
    if (!cssColor || typeof cssColor !== 'string') {
        return '1.0 1.0 1.0 1.0';
    }
    // 支持rgb(r, g, b)格式
    const rgbMatch = cssColor.match(/rgb\((\d+),\s*(\d+),\s*(\d+)\)/);
    if (rgbMatch) {
        const r = (parseInt(rgbMatch[1]) / 255).toFixed(2);
        const g = (parseInt(rgbMatch[2]) / 255).toFixed(2);
        const b = (parseInt(rgbMatch[3]) / 255).toFixed(2);
        // 如果有alpha值则使用，否则默认1.0
        return `${r} ${g} ${b} 1.0`;
    }
    // 支持#RRGGBB格式
    const hexMatch = cssColor.match(/^#([0-9A-Fa-f]{6})$/);
    if (hexMatch) {
        const hex = hexMatch[1];
        const r = (parseInt(hex.substring(0, 2), 16) / 255).toFixed(2);
        const g = (parseInt(hex.substring(2, 4), 16) / 255).toFixed(2);
        const b = (parseInt(hex.substring(4, 6), 16) / 255).toFixed(2);
        return `${r} ${g} ${b} 1.0`;
    }
    return '1.0 1.0 1.0 1.0';
}

// 图形类型定义（与后端shader保持一致）
export const SHAPE_TYPES = [
    { value: 0, label: '默认' },
    { value: 1, label: '圆形' },
    { value: 2, label: '三角形' },
    { value: 3, label: '圆角矩形' },
    { value: 4, label: '星形' },
    { value: 5, label: '六边形' },
    { value: 6, label: '菱形' },
    { value: 7, label: '心形' },
    { value: 8, label: '花瓣' }
];

