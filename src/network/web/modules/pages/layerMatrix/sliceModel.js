function isSliceKey(key) {
    return !!key && key.startsWith('slice') && /^\d+$/.test(key.substring(5));
}

export function collectSliceKeys(layer) {
    const keys = [];
    const addSliceKey = (key, value) => {
        if (!isSliceKey(key) || value === null || value === undefined || keys.includes(key)) return;
        keys.push(key);
    };
    if (!layer || typeof layer !== 'object') return keys;
    Object.entries(layer).forEach(([key, value]) => addSliceKey(key, value));
    if (layer.slices && typeof layer.slices === 'object') {
        Object.entries(layer.slices).forEach(([key, value]) => addSliceKey(key, value));
    }
    keys.sort((a, b) => parseInt(a.substring(5), 10) - parseInt(b.substring(5), 10));
    return keys;
}

export function hydrateSliceFields(layer) {
    if (!layer || typeof layer !== 'object' || !layer.slices || typeof layer.slices !== 'object') return;
    Object.entries(layer.slices).forEach(([key, value]) => {
        if (isSliceKey(key) && value !== null && value !== undefined) {
            layer[key] = value;
        }
    });
}

export function getSliceData(layer, sliceKey) {
    if (!layer || !sliceKey) return undefined;
    hydrateSliceFields(layer);
    return layer[sliceKey];
}

export function isSliceVisible(sliceData) {
    return !!sliceData && sliceData.visible !== false && sliceData.enable !== false;
}

const normalizeFitMode = (value) => Number(value) > 0 ? 1 : 0;

export function normalizeSliceVisualFields(sliceConfig) {
    if (!sliceConfig || typeof sliceConfig !== 'object') return sliceConfig;

    if (sliceConfig.rotation !== undefined && sliceConfig.rotate === undefined) {
        sliceConfig.rotate = sliceConfig.rotation;
    }
    if (sliceConfig.rotate !== undefined && sliceConfig.rotation === undefined) {
        sliceConfig.rotation = sliceConfig.rotate;
    }

    if (sliceConfig.shape_type !== undefined && sliceConfig.shapeType === undefined) {
        sliceConfig.shapeType = sliceConfig.shape_type;
    }
    if (sliceConfig.shapeType !== undefined && sliceConfig.shape_type === undefined) {
        sliceConfig.shape_type = sliceConfig.shapeType;
    }

    if (sliceConfig.shape_param !== undefined && sliceConfig.shapeParam === undefined) {
        sliceConfig.shapeParam = sliceConfig.shape_param;
    }
    if (sliceConfig.shapeParam !== undefined && sliceConfig.shape_param === undefined) {
        sliceConfig.shape_param = sliceConfig.shapeParam;
    }

    if (sliceConfig.gaussian_blur !== undefined && sliceConfig.gaussianBlur === undefined) {
        sliceConfig.gaussianBlur = sliceConfig.gaussian_blur;
    }
    if (sliceConfig.gaussianBlur !== undefined && sliceConfig.gaussian_blur === undefined) {
        sliceConfig.gaussian_blur = sliceConfig.gaussianBlur;
    }

    if (sliceConfig.black_to_transparent !== undefined && sliceConfig.blackToTransparent === undefined) {
        sliceConfig.blackToTransparent = sliceConfig.black_to_transparent;
    }
    if (sliceConfig.blackToTransparent !== undefined && sliceConfig.black_to_transparent === undefined) {
        sliceConfig.black_to_transparent = sliceConfig.blackToTransparent;
    }

    if (sliceConfig.fit_mode !== undefined && sliceConfig.fitMode === undefined) {
        sliceConfig.fitMode = sliceConfig.fit_mode;
    }
    if (sliceConfig.fitMode !== undefined && sliceConfig.fit_mode === undefined) {
        sliceConfig.fit_mode = sliceConfig.fitMode;
    }
    if (sliceConfig.fit_mode !== undefined || sliceConfig.fitMode !== undefined) {
        const fitMode = normalizeFitMode(sliceConfig.fit_mode ?? sliceConfig.fitMode);
        sliceConfig.fit_mode = fitMode;
        sliceConfig.fitMode = fitMode;
    }

    if (sliceConfig.visible !== undefined && sliceConfig.enable === undefined) {
        sliceConfig.enable = sliceConfig.visible;
    }
    if (sliceConfig.enable !== undefined && sliceConfig.visible === undefined) {
        sliceConfig.visible = sliceConfig.enable;
    }

    if (sliceConfig.roam_config !== undefined && sliceConfig.roamConfig === undefined) {
        sliceConfig.roamConfig = sliceConfig.roam_config;
    }
    if (sliceConfig.roamConfig !== undefined && sliceConfig.roam_config === undefined) {
        sliceConfig.roam_config = sliceConfig.roamConfig;
    }

    if (sliceConfig.capture_type !== undefined && sliceConfig.captureType === undefined) {
        sliceConfig.captureType = sliceConfig.capture_type;
    }
    if (sliceConfig.captureType !== undefined && sliceConfig.capture_type === undefined) {
        sliceConfig.capture_type = sliceConfig.captureType;
    }
    if (sliceConfig.capture_index !== undefined && sliceConfig.captureIndex === undefined) {
        sliceConfig.captureIndex = sliceConfig.capture_index;
    }
    if (sliceConfig.captureIndex !== undefined && sliceConfig.capture_index === undefined) {
        sliceConfig.capture_index = sliceConfig.captureIndex;
    }

    return sliceConfig;
}

export function buildSliceLayerForDraw(layer, sliceData, rect, sliceName) {
    const normalized = normalizeSliceVisualFields({ ...sliceData });
    return {
        id: layer.id,
        type: layer.type,
        name: sliceName,
        position: { x: rect.x, y: rect.y },
        size: { width: rect.width, height: rect.height },
        rotation: normalized.rotation ?? normalized.rotate ?? layer.rotation ?? 0,
        scale: normalized.scale ?? layer.scale ?? 1.0,
        alpha: normalized.alpha !== undefined
            ? normalized.alpha
            : (normalized.transparency !== undefined ? normalized.transparency / 255 : (layer.alpha || 1.0)),
        visible: isSliceVisible(normalized),
        mirror: normalized.mirror || layer.mirror || false,
        shape_type: normalized.shapeType ?? normalized.shape_type ?? layer.shapeType ?? layer.shape_type ?? 0,
        shape_param: normalized.shapeParam ?? normalized.shape_param ?? layer.shapeParam ?? layer.shape_param ?? 0
    };
}
