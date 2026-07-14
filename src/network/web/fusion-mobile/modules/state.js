export const SIDES = ['left', 'right', 'top', 'bottom'];
export const SIDE_SHORT = { left: 'l', right: 'r', top: 't', bottom: 'b' };
export const SIDE_LABEL = { left: '左', right: '右', top: '上', bottom: '下' };

export const state = {
    mode: 'geometry',
    activeRegionId: 1,
    activeSide: 'left',
    geometryOp: 'point',
    geometryLineAxis: 'col',
    maskOp: 'point',
    moveStep: 100,
    layout: {
        canvas_in_width: 0,
        canvas_in_height: 0,
        input_total_width: 0,
        input_total_height: 0,
        canvas_out_width: 0,
        canvas_out_height: 0,
        tile_in_width: 0,
        tile_in_height: 0,
        tile_out_width: 0,
        tile_out_height: 0,
        grid_in_rows: 1,
        grid_in_cols: 1,
        grid_out_rows: 1,
        grid_out_cols: 1,
        merge_360: false,
        rotation_angle: 0,
        split_direction: 0,
        mappings: []
    },
    regions: [],
    geometry: {},
    mask: null,
    blend: {},
    color: {},
    correction: {},
    cave: {},
    blendAutoEdges: true,
    masterEnabled: false,
    managerMode: false,
    background: {
        layerId: 60,
        visible: false,
        currentIndex: 0,
        items: [],
        pausedVideoLayerIds: [],
        videoLayerIds: [],
        debugModeActive: false,
        autoDebugVisible: false,
        userVisible: false,
        debugOperation: Promise.resolve()
    },
    previewRects: []
};

export function activeRegion() {
    return state.regions.find((region) => Number(region.id) === Number(state.activeRegionId)) || state.regions[0] || null;
}

export function activeGeometry() {
    return state.geometry[state.activeRegionId] || null;
}

export function activeBlend() {
    return state.blend[state.activeRegionId] || null;
}

export function activeColor() {
    return state.color[state.activeRegionId] || null;
}

export function setActiveRegionId(regionId) {
    const numeric = Number(regionId);
    if (!Number.isFinite(numeric)) return;
    state.activeRegionId = numeric;
}

export function getRegionIds() {
    return state.regions.map((region) => Number(region.id)).filter((id) => Number.isFinite(id));
}

export function getNextRegionId(delta) {
    const ids = getRegionIds();
    if (!ids.length) return state.activeRegionId;
    const currentIndex = Math.max(0, ids.indexOf(Number(state.activeRegionId)));
    return ids[(currentIndex + delta + ids.length) % ids.length];
}
