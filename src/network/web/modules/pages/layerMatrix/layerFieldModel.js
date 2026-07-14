import { parsePosition, parseSize } from './utils.js';

// 图层字段单一定义。每条记录描述一个字段在三处的名称：
//   ui:      前端/输入框使用的 snake_case 字段（layer.<ui> / properties.<ui>）。
//   config:  config.json / camelCase 后端配置字段。
//   run时间: run时间 API（/run时间/layers/{id}）使用的 camelCase 字段。若为 null 表示该字段不参与 run时间 patch。
//
// 此表是 buildRun时间LayerPatch / buildConfigLayerPatch / configLayerToUiLayer 的唯一来源。
const LAYER_FIELDS = [
    { ui: 'rotation', config: 'rotation', runtime: 'rotation' },
    { ui: 'scale', config: 'scale', runtime: 'scale' },
    { ui: 'alpha', config: 'alpha', runtime: 'alpha' },
    { ui: 'priority', config: 'priority', runtime: 'priority' },
    { ui: 'volume', config: 'volume', runtime: 'volume' },
    { ui: 'playbackRate', config: 'playbackRate', runtime: null },
    { ui: 'audioTrack', config: 'audioTrack', runtime: null },
    { ui: 'audioChannel', config: 'audioChannel', runtime: null },
    { ui: 'boundPlaylistId', config: 'boundPlaylistId', runtime: null },
    { ui: 'playlistId', config: 'playlistId', runtime: null },
    { ui: 'animated', config: 'animated', runtime: null },
    { ui: 'text', config: 'text', runtime: null },
    { ui: 'alignment', config: 'alignment', runtime: null },
    { ui: 'invert', config: 'invert', runtime: 'invert' },
    { ui: 'capture_type', config: 'captureType', runtime: null },
    { ui: 'capture_index', config: 'captureIndex', runtime: null },
    { ui: 'capture_rotation', config: 'captureRotation', runtime: 'captureRotation' },
    { ui: 'gaussian_blur', config: 'gaussianBlur', runtime: 'gaussianBlur' },
    { ui: 'fit_mode', config: 'fitMode', runtime: 'fitMode' },
    { ui: 'mirror_ready_hint_visible', config: 'mirrorReadyHintVisible', runtime: 'mirrorReadyHintVisible' },
    { ui: 'tv_vertical_crop_px', config: 'tvVerticalCropPx', runtime: 'tvVerticalCropPx' },
    { ui: 'shape_type', config: 'shapeType', runtime: 'shapeType' },
    { ui: 'shape_param', config: 'shapeParam', runtime: 'shapeParam' },
    { ui: 'black_to_transparent', config: 'blackToTransparent', runtime: 'blackToTransparent' },
    { ui: 'effect_linked_slices', config: 'effectLinkedSlices', runtime: null },
    { ui: 'filter_mode', config: 'filterMode', runtime: null },
    { ui: 'fade_in_time', config: 'fadeInTime', runtime: null },
    { ui: 'fade_out_time', config: 'fadeOutTime', runtime: null },
    { ui: 'photo_wall_mode', config: 'photoWallMode', runtime: null },
    { ui: 'scale_mode', config: 'scaleMode', runtime: null },
    { ui: 'font_file', config: 'fontFile', runtime: null },
    { ui: 'font_size', config: 'fontSize', runtime: null },
    { ui: 'text_color', config: 'textColor', runtime: null },
    { ui: 'bg_color', config: 'bgColor', runtime: null },
    { ui: 'scroll_speed', config: 'scrollSpeed', runtime: null },
    { ui: 'outline_width', config: 'outlineWidth', runtime: null },
    { ui: 'outline_color', config: 'outlineColor', runtime: null },
    { ui: 'shadow', config: 'shadow', runtime: null },
    { ui: 'bind_layerId', config: 'bindLayerId', runtime: null },
    { ui: 'subtitle_visible', config: 'subtitleVisible', runtime: null },
    { ui: 'show_count', config: 'showCount', runtime: null },
    { ui: 'display_align', config: 'displayAlign', runtime: null },
    { ui: 'start_hint_time', config: 'startHintTime', runtime: null },
    { ui: 'end_hint_time', config: 'endHintTime', runtime: null },
    { ui: 'show_list', config: 'l41ShowList', runtime: null },
    { ui: 'qr_content', config: 'qrContent', runtime: null },
    { ui: 'qr_size', config: 'qrSize', runtime: null },
    { ui: 'qr_logo_size', config: 'qrLogoSize', runtime: null },
    { ui: 'qr_text', config: 'qrText', runtime: null },
    { ui: 'qr_text_color', config: 'qrTextColor', runtime: null },
    { ui: 'qr_bg_color', config: 'qrBgColor', runtime: null },
    { ui: 'qr_fg_color', config: 'qrFgColor', runtime: null },
    { ui: 'qr_error_correction', config: 'qrErrorCorrection', runtime: null }
];

const RUNTIME_KEYS_PASSTHROUGH = ['visible', 'position', 'size'];

function normalizeLayerFieldAliases(layer) {
    if (!layer || typeof layer !== 'object') return layer;
    const normalizeFitMode = (value) => Number(value) > 0 ? 1 : 0;
    const normalizeCaptureRotation = (value) => {
        const rotation = Number(value) || 0;
        return [-1, 0, 90, 180, 270].includes(rotation) ? rotation : 0;
    };

    if (layer.fit_mode !== undefined || layer.fitMode !== undefined) {
        const fitMode = normalizeFitMode(layer.fit_mode ?? layer.fitMode);
        layer.fit_mode = fitMode;
        layer.fitMode = fitMode;
    }
    if (layer.capture_type !== undefined && layer.captureType === undefined) {
        layer.captureType = layer.capture_type;
    }
    if (layer.captureType !== undefined && layer.capture_type === undefined) {
        layer.capture_type = layer.captureType;
    }
    if (layer.capture_index !== undefined && layer.captureIndex === undefined) {
        layer.captureIndex = layer.capture_index;
    }
    if (layer.captureIndex !== undefined && layer.capture_index === undefined) {
        layer.capture_index = layer.captureIndex;
    }
    if (layer.capture_rotation !== undefined || layer.captureRotation !== undefined) {
        const captureRotation = normalizeCaptureRotation(layer.capture_rotation ?? layer.captureRotation);
        layer.capture_rotation = captureRotation;
        layer.captureRotation = captureRotation;
    }
    if (layer.tv_vertical_crop_px !== undefined || layer.tvVerticalCropPx !== undefined) {
        const cropPx = Math.max(0, Math.min(4000, Math.round(Number(layer.tv_vertical_crop_px ?? layer.tvVerticalCropPx) || 0)));
        layer.tv_vertical_crop_px = cropPx;
        layer.tvVerticalCropPx = cropPx;
    }
    return layer;
}

export function buildRuntimeLayerPatch(properties) {
    const patch = {};
    RUNTIME_KEYS_PASSTHROUGH.forEach((key) => {
        if (properties[key] !== undefined) patch[key] = properties[key];
    });
    LAYER_FIELDS.forEach((field) => {
        if (!field.runtime) return;
        if (properties[field.ui] !== undefined) {
            patch[field.runtime] = properties[field.ui];
        } else if (properties[field.runtime] !== undefined) {
            patch[field.runtime] = properties[field.runtime];
        }
    });
    return patch;
}

export function buildConfigLayerPatch(properties, layerId) {
    const patch = {};
    if (properties.visible !== undefined) patch.visible = properties.visible;
    if (properties.position) {
        patch.position = `${Math.round(properties.position.x || 0)} ${Math.round(properties.position.y || 0)}`;
    }
    if (properties.size) {
        patch.size = `${Math.round(properties.size.width || 0)} ${Math.round(properties.size.height || 0)}`;
    }

    LAYER_FIELDS.forEach((field) => {
        if (properties[field.ui] !== undefined) patch[field.config] = properties[field.ui];
    });

    if (properties.display_duration !== undefined) {
        patch[layerId === 41 ? 'l41DisplayDuration' : 'displayDuration'] = properties.display_duration;
    }
    if (properties.roam_config !== undefined) {
        patch.roamConfig = JSON.stringify(properties.roam_config);
    }
    return patch;
}

export function configLayerToUiLayer(config) {
    if (!config || typeof config !== 'object') return {};
    const layer = {};

    LAYER_FIELDS.forEach((field) => {
        if (config[field.config] !== undefined) layer[field.ui] = config[field.config];
    });

    if (config.displayDuration !== undefined && layer.display_duration === undefined) {
        layer.display_duration = config.displayDuration;
    }
    if (config.l41DisplayDuration !== undefined) {
        layer.display_duration = config.l41DisplayDuration;
    }

    if (config.visible !== undefined) layer.visible = config.visible;
    if (config.position !== undefined) layer.position = parsePosition(config.position);
    if (config.size !== undefined) layer.size = parseSize(config.size);

    if (config.roamConfig) {
        try {
            layer.roam_config = typeof config.roamConfig === 'string'
                ? JSON.parse(config.roamConfig)
                : config.roamConfig;
        } catch (_) {
            layer.roam_config = config.roamConfig;
        }
    }
    if (config.slices && typeof config.slices === 'object') {
        layer.slices = config.slices;
        Object.entries(config.slices).forEach(([key, value]) => {
            if (key && key.startsWith('slice') && value !== null && value !== undefined) {
                layer[key] = value;
            }
        });
    }
    Object.keys(config).forEach((key) => {
        if (key.startsWith('slice') && /^\d+$/.test(key.substring(5)) && config[key] !== null && config[key] !== undefined) {
            layer[key] = config[key];
        }
    });

    return normalizeLayerFieldAliases(layer);
}

export { normalizeLayerFieldAliases };
