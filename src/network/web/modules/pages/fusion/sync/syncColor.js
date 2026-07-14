import { loadColor, saveColor } from '../api.js?v=2.95';
import { ensureColorRegion } from '../actions.js?v=2.95';

export async function syncColorRegion(regionId) {
    const region = ensureColorRegion(regionId);
    return saveColor(regionId, region);
}

export async function loadColorRegion(regionId) {
    return loadColor(regionId);
}

