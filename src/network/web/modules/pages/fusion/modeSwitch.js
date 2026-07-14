import { getState, setActiveRegion } from './actions.js?v=2.95';
import { TABS } from './types.js';
import { getManagerCornerName, snapManagerCornerSelection, setGeometryGridVisibleAllRegions } from './geometry/actions.js?v=2.95';
import {
    flushPendingGeometryOps,
    syncActiveGeometrySelection,
    syncGeometryGuideVisibilityForRegions
} from './sync/syncGeometry.js?v=2.95';
import {
    hydrateBlendRegionsFromResponse,
    syncBlendManagerMode
} from './sync/syncBlend.js?v=2.95';
import { syncMaskGuideVisibility } from './sync/syncMask.js?v=2.95';
import { setMaskGridVisible } from './mask/actions.js?v=2.95';
import { setActiveBlendCorner, setBlendAutoEdges, setBlendManagerMode, setBlendMasterEnabled } from './blend/actions.js?v=2.95';
import { setBlendMaster } from './api.js?v=2.95';
import { switchTab, renderFusionUi } from './ui/panel.js?v=2.100';

function setDebugBackgroundActive(active, traceId = '') {
    const controller = window.FusionBackground;
    if (!controller || typeof controller.setDebugModeActive !== 'function') return;
    void controller.setDebugModeActive(active, traceId);
}

function getDefaultFocusRegionId(state) {
    const ids = Array.isArray(state.layout.regionIds) ? state.layout.regionIds : [];
    const first = ids.find((id) => Number(id) > 0);
    return Number(first || 1);
}

function resetModeFocusToDefaultRegion() {
    const state = getState();
    const defaultRegionId = getDefaultFocusRegionId(state);
    if (Number(state.page.activeRegionId) !== defaultRegionId) {
        setActiveRegion(defaultRegionId);
    }
    return defaultRegionId;
}

async function ensureBlendMasterEnabled() {
    if (getState().blend.masterEnabled) {
        setBlendMasterEnabled(true);
        return;
    }

    const response = await setBlendMaster(true);
    if (!response || response.enabled !== true) {
        throw new Error('融合总开关打开失败');
    }
    setBlendMasterEnabled(true);
    if (typeof response.blend_auto_edges === 'boolean') {
        setBlendAutoEdges(response.blend_auto_edges);
    }
    hydrateBlendRegionsFromResponse(response);
}

async function syncBlendEntryState(regionId) {
    const masterResult = ensureBlendMasterEnabled().catch((error) => {
        console.warn('ensureBlendMasterEnabled failed, proceeding anyway:', error);
        setBlendMasterEnabled(true);
    });
    await Promise.allSettled([masterResult, flushPendingGeometryOps()]);
    await syncBlendManagerMode();
    await syncActiveGeometrySelection(regionId);
    renderFusionUi();
}

async function syncMaskEntryState(regionIds) {
    const ids = Array.isArray(regionIds) && regionIds.length ? regionIds : [];
    const results = await Promise.allSettled([
        syncBlendManagerMode(),
        syncGeometryGuideVisibilityForRegions(ids, false),
        syncMaskGuideVisibility(true)
    ]);
    results.forEach((result) => {
        if (result.status === 'rejected') {
            console.warn('fusion mask entry sync failed:', result.reason);
        }
    });
}

export async function switchFusionTab(tab, traceId = '') {
    if (!Object.values(TABS).includes(tab)) return;

    const state = getState();
    const shouldResetFocus =
        tab === TABS.GEOMETRY || tab === TABS.MASK || tab === TABS.BLEND;
    const regionId = shouldResetFocus
        ? resetModeFocusToDefaultRegion()
        : state.page.activeRegionId;
    const regionIds = state.layout.regionIds?.length ? state.layout.regionIds : [regionId];
    state.interaction.dragging = { active: false, target: null, regionId: null, pointIndex: -1 };

    if (tab === TABS.GEOMETRY) {
        setDebugBackgroundActive(true, traceId);
        switchTab(tab);
        renderFusionUi();
        await syncActiveGeometrySelection(regionId);
    } else if (tab === TABS.MASK) {
        setDebugBackgroundActive(true, traceId);
        setBlendManagerMode(false);
        setGeometryGridVisibleAllRegions(regionIds, false);
        setMaskGridVisible(true);
        switchTab(tab);
        renderFusionUi();
        void syncMaskEntryState(regionIds);
    } else if (tab === TABS.BLEND) {
        setDebugBackgroundActive(true, traceId);
        setBlendMasterEnabled(true);
        setBlendManagerMode(true);
        snapManagerCornerSelection(regionId);
        setActiveBlendCorner(getManagerCornerName(state.geometry.byRegionId[regionId]));
        switchTab(tab);
        renderFusionUi();
        void syncBlendEntryState(regionId).catch((error) => {
            console.warn('fusion blend manager entry sync failed:', error);
        });
    } else {
        setDebugBackgroundActive(false, traceId);
        switchTab(tab);
        renderFusionUi();
    }
}

