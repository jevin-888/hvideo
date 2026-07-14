import { ensureColorRegion } from '../actions.js?v=2.95';
import { clamp } from '../utils/grid.js';

export function patchColor(regionId, patch) {
    const region = ensureColorRegion(regionId);
    if (patch.brightness !== undefined) region.brightness = clamp(patch.brightness, 0, 2);
    if (patch.contrast !== undefined) region.contrast = clamp(patch.contrast, 0, 2);
    if (patch.saturation !== undefined) region.saturation = clamp(patch.saturation, 0, 2);
    return region;
}

export function resetColor(regionId) {
    const region = ensureColorRegion(regionId);
    region.brightness = 1;
    region.contrast = 1;
    region.saturation = 1;
    return region;
}

