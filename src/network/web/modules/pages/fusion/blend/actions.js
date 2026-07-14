import { ensureBlendRegion, getState } from '../actions.js?v=2.95';
import { BLEND_SIDES } from '../types.js';
import { pushBlendUndo } from '../undo.js';

export function setBlendManagerMode(enabled) {
    getState().blend.managerMode = enabled;
}

export function setBlendMasterEnabled(enabled) {
    getState().blend.masterEnabled = !!enabled;
}

export function setBlendAutoEdges(enabled) {
    getState().blend.autoEdges = !!enabled;
}

export function setActiveBlendSide(side) {
    getState().blend.activeSide = side;
}

export function setActiveBlendCorner(corner) {
    getState().blend.activeCorner = corner;
}

export function pushBlendSideUndo(regionId) {
    const region = ensureBlendRegion(regionId);
    pushBlendUndo(regionId, region);
}

export function patchBlendSide(regionId, side, patch, options = {}) {
    const region = ensureBlendRegion(regionId);
    if (options.undo !== false) {
        pushBlendUndo(regionId, region);
    }
    Object.assign(region[side], patch);
    return region;
}

export function setBlendSideEnabled(regionId, side, enabled, options = {}) {
    const patch = { enabled: !!enabled };
    if (patch.enabled) {
        const region = ensureBlendRegion(regionId);
        const sideState = region?.[side];
        if (!sideState || Number(sideState.width || 0) <= 0.001) {
            patch.width = getDefaultBlendWidth(regionId, side);
        }
    }
    return patchBlendSide(regionId, side, patch, options);
}

export function getDefaultBlendWidth(regionId, side) {
    const blend = getState().blend.byRegionId[regionId];
    const gridCols = Number(blend?.gridCols) || 2;
    const gridRows = Number(blend?.gridRows) || 2;
    if ((side === 'left' || side === 'right') && gridCols > 2) return 1 / (gridCols - 1);
    if ((side === 'top' || side === 'bottom') && gridRows > 2) return 1 / (gridRows - 1);
    return 0;
}

export function ensureBlendSideWidth(regionId, side) {
    const region = ensureBlendRegion(regionId);
    const blendSide = region?.[side];
    if (!blendSide) return region;
    if (blendSide.enabled && Number(blendSide.width || 0) <= 0.001) {
        blendSide.width = getDefaultBlendWidth(regionId, side);
    }
    return region;
}

export function setBlendSidesEnabled(regionIds, enabled) {
    const ids = Array.isArray(regionIds) ? regionIds : [regionIds];
    ids.filter((regionId) => regionId).forEach((regionId) => {
        const region = ensureBlendRegion(regionId);
        BLEND_SIDES.forEach((side) => {
            const sideEnabled = !!enabled;
            region[side].enabled = sideEnabled;
            if (sideEnabled && Number(region[side].width || 0) <= 0.001) {
                region[side].width = getDefaultBlendWidth(regionId, side);
            }
        });
    });
}
