import {
    ensureBlendRegion,
    ensureColorRegion,
    patchPage,
    setActiveRegion,
    setFocusMode,
    setInitialized,
    getState
} from './actions.js?v=2.95';
import { TABS, FOCUS_MODES, BLEND_SIDES } from './types.js';
import {
    getManagerCornerName,
    hydrateGeometryRegion,
    initializeGeometryRegion,
    snapManagerCornerSelection
} from './geometry/actions.js?v=2.95';
import { initializeMask } from './mask/actions.js?v=2.95';
import { setActiveBlendCorner, setActiveBlendSide, setBlendAutoEdges, setBlendMasterEnabled } from './blend/actions.js?v=2.95';
import { patchColor } from './color/actions.js?v=2.95';
import { persistFusionConfig, getBlendAutoEdges, getFusionMaster, getManagerMode } from './api.js?v=2.95';
import { bindUi } from './ui/bindings.js?v=2.100';
import { renderFusionUi } from './ui/panel.js?v=2.100';
import { loadGeometryLayout, loadGeometryRegion, persistGeometryRegion, syncActiveGeometrySelection, flushPendingGeometryOps } from './sync/syncGeometry.js?v=2.95';
import { loadMask, persistMask } from './sync/syncMask.js?v=2.95';
import { loadBlendRegion, syncBlendRegion } from './sync/syncBlend.js?v=2.95';
import { syncColorRegion } from './sync/syncColor.js?v=2.95';
import { switchFusionTab } from './modeSwitch.js?v=2.100';
import { applyGridVisualStyle } from './canvas/visualStyle.js?v=2.81';

function ensureRegionState(regionId) {
    initializeGeometryRegion(regionId, 2, 2);
    ensureBlendRegion(regionId);
    ensureColorRegion(regionId);
}

function bootstrapDefaults() {
    patchPage({
        activeTab: TABS.GEOMETRY,
        activeRegionId: 1,
        focusMode: FOCUS_MODES.LAYOUT
    });
}

async function hydrateRegionState(regionId) {
    const [geometryResponse, blendResponse] = await Promise.allSettled([
        loadGeometryRegion(regionId),
        loadBlendRegion(regionId)
    ]);

    if (geometryResponse.status === 'rejected' || !geometryResponse.value) {
        console.warn(`fusion geometry hydrate failed for region ${regionId}:`, geometryResponse.reason || 'empty response');
    } else {
        hydrateGeometryRegion(regionId, geometryResponse.value);
    }
    if (blendResponse.status === 'rejected' || !blendResponse.value) {
        console.warn(`fusion blend hydrate failed for region ${regionId}:`, blendResponse.reason || 'empty response');
    }
}

async function hydrateGlobalMask() {
    initializeMask(2, 2);
    try {
        await loadMask();
    } catch (error) {
        console.warn('fusion global mask hydrate failed:', error);
    }
}

async function hydrateLayout(options = {}) {
    try {
        const previousActiveRegionId = getState().page.activeRegionId;
        const response = await loadGeometryLayout();
        if (response && response.regions && response.regions.length > 0) {
            applyGridVisualStyle(
                response.gridLineWidth ?? response.grid_line_width,
                response.gridHotspotRadius ?? response.grid_hotspot_radius
            );
            const regionIds = getState().layout.regionIds || [];
            if (regionIds.length === 0) {
                throw new Error('融合布局未返回有效区域');
            }
            const keepActiveRegion = options.preserveActive !== false &&
                regionIds.includes(previousActiveRegionId);
            setActiveRegion(keepActiveRegion ? previousActiveRegionId : regionIds[0]);
            regionIds.forEach((regionId) => {
                ensureRegionState(regionId);
            });
            await Promise.all([
                Promise.all(regionIds.map((regionId) => hydrateRegionState(regionId))),
                hydrateGlobalMask()
            ]);
            await syncActiveGeometrySelection(getState().page.activeRegionId);
        }
    } catch (error) {
        console.warn('fusion layout bootstrap failed:', error);
    }

    // 拉取管理模式、融合带显示开关与自动边模式，保证 UI 与后端一致
    try {
        const [mgr, master, autoEdges] = await Promise.all([
            getManagerMode(),
            getFusionMaster(),
            getBlendAutoEdges()
        ]);
        if (!mgr || typeof mgr.enabled !== 'boolean') {
            throw new Error('融合管理模式状态返回无效');
        }
        if (!master || typeof master.enabled !== 'boolean') {
            throw new Error('融合总开关状态返回无效');
        }
        if (!autoEdges || typeof autoEdges.enabled !== 'boolean') {
            throw new Error('融合边自动/手动状态返回无效');
        }
        getState().blend.managerMode = mgr.enabled;
        setBlendMasterEnabled(master.enabled);
        setBlendAutoEdges(autoEdges.enabled);
    } catch (error) {
        console.warn('fusion runtime state hydrate failed:', error);
    }
}

export async function refreshFusionLayout(options = {}) {
    if (!getState().page.initialized) {
        return;
    }
    await hydrateLayout({
        preserveActive: options.preserveActive !== false
    });
    renderFusionUi();
}

export function switchRegion(regionId) {
    if (!regionId) return;
    const state = getState();
    const activeTab = state.page.activeTab;
    setActiveRegion(regionId);

    // 更新 activeCorner in blend 模式 to match the new region's selection
    if (activeTab === TABS.BLEND) {
        const region = state.geometry.byRegionId[regionId];
        if (region && region.selected.row >= 0 && region.selected.col >= 0) {
            snapManagerCornerSelection(regionId);
            setActiveBlendCorner(getManagerCornerName(region));
        }
    }

    renderFusionUi();
    if (activeTab === TABS.GEOMETRY || activeTab === TABS.BLEND) {
        void syncActiveGeometrySelection(regionId).catch((error) => {
            console.warn('fusion region selection sync failed:', error);
        });
    }
}

export async function setTab(tab) {
    try {
        await switchFusionTab(tab);
    } catch (e) {
        console.warn('[setTab] Sync failed:', e);
    }
}

export function toggleOverviewMode() {
    setFocusMode(FOCUS_MODES.LAYOUT);
    renderFusionUi();
}

export function setFocusModeValue(mode) {
    if (!Object.values(FOCUS_MODES).includes(mode)) return;
    setFocusMode(mode);
    renderFusionUi();
}

export function setBlendSide(side) {
    if (!BLEND_SIDES.includes(side)) return;
    setActiveBlendSide(side);
    renderFusionUi();
}

export function setMaskEnabledValue(enabled) {
    setMaskEnabled(!!enabled);
    renderFusionUi();
}

export function setColorValues(values) {
    patchColor(getState().page.activeRegionId, values || {});
    renderFusionUi();
}

export async function saveGeometry() {
    await flushPendingGeometryOps();
    return persistGeometryRegion(getState().page.activeRegionId);
}

export async function saveMask() {
    return persistMask();
}

export async function saveBlend() {
    await flushPendingGeometryOps();
    await persistGeometryRegion(getState().page.activeRegionId);
    await syncBlendRegion(getState().page.activeRegionId);
    return persistFusionConfig();
}

export async function saveColor() {
    await syncColorRegion(getState().page.activeRegionId);
    return persistFusionConfig();
}

export async function saveCurrentRegion() {
    const state = getState();
    await flushPendingGeometryOps();
    await Promise.all([
        persistGeometryRegion(state.page.activeRegionId),
        persistMask(),
        syncBlendRegion(state.page.activeRegionId),
        syncColorRegion(state.page.activeRegionId)
    ]);
    await persistFusionConfig();
}

export function getRegionIds() {
    return [...getState().layout.regionIds];
}

export function getStateSnapshot() {
    return JSON.parse(JSON.stringify(getState()));
}

function createFusionFacade() {
    return {
        init: initializeFusion,
        redraw: renderFusionUi,
        switchRegion,
        setTab,
        toggleOverviewMode,
        setFocusMode: setFocusModeValue,
        setBlendSide,
        setMaskEnabled: setMaskEnabledValue,
        setColor: setColorValues,
        refreshLayout: refreshFusionLayout,
        saveGeometry,
        saveMask,
        saveBlend,
        saveColor,
        saveCurrentRegion,
        getRegionIds,
        getStateSnapshot,
        getInited() {
            return getState().page.initialized;
        }
    };
}

export async function initializeFusion() {
    if (getState().page.initialized) {
        await refreshFusionLayout({ preserveActive: true });
        return;
    }
    bootstrapDefaults();
    await hydrateLayout();
    bindUi();
    setInitialized(true);
    renderFusionUi();

    window.hsFusion = createFusionFacade();
}

window.hsFusion = createFusionFacade();

