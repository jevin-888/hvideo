import { parsePosition, parseSize, parseSliceCoordinate } from './utils.js';
import { collectSliceKeys, getSliceData, hydrateSliceFields, isSliceVisible } from './sliceModel.js';

export function createLayerItem(layerId) {
    return { type: 'layer', layerId: Number(layerId) };
}

export function createSliceItem(layerId, sliceKey) {
    return { type: 'slice', layerId: Number(layerId), sliceKey: String(sliceKey) };
}

export function getItemKey(item) {
    if (!item) return '';
    if (item.type === 'slice') return `slice:${Number(item.layerId)}:${item.sliceKey}`;
    return `layer:${Number(item.layerId)}`;
}

export function normalizeLayerRect(layer, canvasConfig) {
    if (!layer) return null;
    if (!layer.position || typeof layer.position === 'string') {
        layer.position = parsePosition(layer.position);
    }
    if (!layer.size || typeof layer.size === 'string') {
        layer.size = parseSize(layer.size);
    }

    const fallbackWidth = canvasConfig?.width || 1920;
    const fallbackHeight = canvasConfig?.height || 1080;
    const width = Number(layer.size?.width) > 0 ? Number(layer.size.width) : fallbackWidth;
    const height = Number(layer.size?.height) > 0 ? Number(layer.size.height) : fallbackHeight;

    return {
        x: Math.round(Number(layer.position?.x) || 0),
        y: Math.round(Number(layer.position?.y) || 0),
        width: Math.round(width),
        height: Math.round(height)
    };
}

export function getLayerBounds(layer, canvasConfig) {
    const rect = normalizeLayerRect(layer, canvasConfig);
    return rect ? enrichBounds(rect) : null;
}

export function getSliceBounds(layer, sliceKey, canvasConfig) {
    if (!layer || !sliceKey) return null;
    hydrateSliceFields(layer);
    const sliceData = getSliceData(layer, sliceKey);
    if (!isSliceVisible(sliceData)) return null;

    const layerRect = normalizeLayerRect(layer, canvasConfig);
    const fallbackSize = layerRect ? { width: layerRect.width, height: layerRect.height } : null;
    const rect = parseSliceCoordinate(sliceData, fallbackSize);
    if (!rect || rect.width <= 0 || rect.height <= 0) return null;
    return enrichBounds(rect);
}

export function getItemBounds(item, layers, canvasConfig) {
    if (!item) return null;
    const layer = layers.find(l => Number(l.id) === Number(item.layerId));
    if (!layer) return null;
    if (item.type === 'slice') return getSliceBounds(layer, item.sliceKey, canvasConfig);
    return getLayerBounds(layer, canvasConfig);
}

export function setItemBounds(item, layers, bounds, canvasConfig) {
    if (!item || !bounds) return null;
    const layer = layers.find(l => Number(l.id) === Number(item.layerId));
    if (!layer) return null;

    const rect = {
        x: Math.round(bounds.x),
        y: Math.round(bounds.y),
        width: Math.max(1, Math.round(bounds.width)),
        height: Math.max(1, Math.round(bounds.height))
    };

    if (item.type === 'slice') {
        const sliceData = getSliceData(layer, item.sliceKey);
        if (!sliceData) return null;
        sliceData.position = `${rect.x} ${rect.y}`;
        sliceData.size = `${rect.width} ${rect.height}`;
        sliceData.coordinate = `${rect.x} ${rect.y} ${rect.width} ${rect.height}`;
        sliceData.range = sliceData.coordinate;
        sliceData.x = rect.x;
        sliceData.y = rect.y;
        sliceData.width = rect.width;
        sliceData.height = rect.height;
        return sliceData;
    }

    if (!layer.position || typeof layer.position === 'string') {
        layer.position = parsePosition(layer.position);
    }
    if (!layer.size || typeof layer.size === 'string') {
        layer.size = parseSize(layer.size);
    }
    layer.position.x = rect.x;
    layer.position.y = rect.y;
    layer.size.width = rect.width || canvasConfig?.width || 1920;
    layer.size.height = rect.height || canvasConfig?.height || 1080;
    return layer;
}

export function enrichBounds(bounds) {
    const x = Math.round(Number(bounds.x) || 0);
    const y = Math.round(Number(bounds.y) || 0);
    const width = Math.max(1, Math.round(Number(bounds.width) || 1));
    const height = Math.max(1, Math.round(Number(bounds.height) || 1));
    return {
        x,
        y,
        width,
        height,
        left: x,
        top: y,
        right: x + width,
        bottom: y + height,
        centerX: x + width / 2,
        centerY: y + height / 2
    };
}

export function translateBounds(bounds, deltaX, deltaY) {
    return enrichBounds({
        x: bounds.x + deltaX,
        y: bounds.y + deltaY,
        width: bounds.width,
        height: bounds.height
    });
}

export function getLayerItemsFromSelection(selectedItems) {
    return (selectedItems || [])
        .filter(item => item && item.type === 'layer')
        .map(item => createLayerItem(item.layerId));
}

export function selectionHasLayer(selectedItems, layerId) {
    return (selectedItems || []).some(item => item?.type === 'layer' && Number(item.layerId) === Number(layerId));
}

export function buildReferenceBounds(layers, movingItems, canvasConfig, options = {}) {
    const movingKeys = new Set((movingItems || []).map(getItemKey));
    const includeSlices = options.includeSlices === true;
    const refs = [];

    layers.forEach(layer => {
        if (!layer || layer.visible === false) return;
        const layerItem = createLayerItem(layer.id);
        if (!movingKeys.has(getItemKey(layerItem))) {
            const bounds = getLayerBounds(layer, canvasConfig);
            if (bounds) refs.push({ item: layerItem, bounds });
        }

        if (!includeSlices) return;
        hydrateSliceFields(layer);
        collectSliceKeys(layer).forEach(sliceKey => {
            const sliceItem = createSliceItem(layer.id, sliceKey);
            if (movingKeys.has(getItemKey(sliceItem))) return;
            const bounds = getSliceBounds(layer, sliceKey, canvasConfig);
            if (bounds) refs.push({ item: sliceItem, bounds });
        });
    });

    return refs;
}
