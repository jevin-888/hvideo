import { ensureBlendRegion, ensureColorRegion, ensureGlobalMask, ensureGeometryRegion, getState } from './actions.js?v=2.95';

function getActiveRegionId() {
    return getState().page.activeRegionId;
}

export function getActiveGeometry() {
    return ensureGeometryRegion(getActiveRegionId());
}

export function getActiveMask() {
    return ensureGlobalMask();
}

export function getActiveBlend() {
    return ensureBlendRegion(getActiveRegionId());
}

export function getActiveBlendCorner() {
    return getState().blend.activeCorner;
}

export function getActiveColor() {
    return ensureColorRegion(getActiveRegionId());
}

export function getRegionIds() {
    return getState().layout.regionIds;
}

export function getNextRegionId(direction = 1) {
    const ids = getRegionIds();
    if (ids.length === 0) return null;
    const currentIndex = Math.max(0, ids.indexOf(getActiveRegionId()));
    return ids[(currentIndex + direction + ids.length) % ids.length];
}
