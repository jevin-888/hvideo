import {
    createDefaultBlendRegionState,
    createDefaultColorRegion,
    createDefaultGeometryRegion,
    createInitialState
} from './state.js?v=2.95';
import { setActiveRegionId as setBackendActiveRegionId } from './api.js?v=2.95';

const state = createInitialState();

export function getState() {
    return state;
}

export function setInitialized(initialized) {
    state.page.initialized = initialized;
}

export function patchPage(patch) {
    Object.assign(state.page, patch);
}

export function patchLayout(patch) {
    Object.assign(state.layout, patch);
}

export function ensureGeometryRegion(regionId) {
    if (!state.geometry.byRegionId[regionId]) {
        state.geometry.byRegionId[regionId] = createDefaultGeometryRegion();
    }
    return state.geometry.byRegionId[regionId];
}

export function ensureGlobalMask() {
    return state.mask.global;
}

export function ensureBlendRegion(regionId) {
    if (!state.blend.byRegionId[regionId]) {
        state.blend.byRegionId[regionId] = createDefaultBlendRegionState();
    }
    return state.blend.byRegionId[regionId];
}

export function ensureColorRegion(regionId) {
    if (!state.color.byRegionId[regionId]) {
        state.color.byRegionId[regionId] = createDefaultColorRegion();
    }
    return state.color.byRegionId[regionId];
}

export function patchInteraction(patch) {
    Object.assign(state.interaction.dragging, patch);
}

export function setActiveRegion(regionId) {
    state.page.activeRegionId = regionId;
    setBackendActiveRegionId(regionId).catch((error) => {
        console.error('Failed to sync active region ID to backend:', error);
    });
}

export function setFocusMode(mode) {
    state.page.focusMode = mode;
}
