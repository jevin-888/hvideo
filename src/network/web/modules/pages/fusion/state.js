import { BLEND_SIDES, FOCUS_MODES, TABS } from './types.js';
import { createGeometryPointGrid, createPointGrid } from './utils/grid.js';

function createDefaultBlendSide() {
    return {
        enabled: false,
        width: 0,
        gamma: 1.8,
        slope: 1.0,
        // 与旧项目 jumu_fusion_player 一致
        stripStart: 0,
        stripEnd: 255,
        anchor: 0.5,
        // 融合带亮度 (与旧项目 CMD_MERGE_CONTROL_BRIGHT 对应)
        bright: [128, 128, 128] // 说明：RGB 亮度值
    };
}

export function createDefaultBlendRegionState() {
    return {
        gridRows: 2,
        gridCols: 2,
        left: createDefaultBlendSide(),
        right: createDefaultBlendSide(),
        top: createDefaultBlendSide(),
        bottom: createDefaultBlendSide()
    };
}

export function createInitialState() {
    return {
        page: {
            activeTab: TABS.GEOMETRY,
            activeRegionId: 1,
            focusMode: FOCUS_MODES.LAYOUT,
            initialized: false
        },
        layout: {
            rows: 0,
            cols: 0,
            inputRows: 0,
            inputCols: 0,
            regionIds: [],
            regionCount: 0,
            inputTotalWidth: 0,
            inputTotalHeight: 0,
            canvasWidth: 0,
            canvasHeight: 0,
            outputWidth: 0,
            outputHeight: 0,
            tileInWidth: 0,
            tileInHeight: 0,
            tileOutWidth: 0,
            tileOutHeight: 0,
            merge360: false,
            mirrorMode: 0,
            // 融合边的相邻关系以输入幕布布局为准，输出布局只控制投影摆放。
            adjacentByRegionId: {}
        },
        geometry: {
            byRegionId: {}
        },
        mask: {
            global: createDefaultMask()
        },
        blend: {
            managerMode: false,
            masterEnabled: false,
            autoEdges: true,
            activeSide: BLEND_SIDES[0],
            activeCorner: 'top-left',
            adjustMode: null,
            byRegionId: {}
        },
        color: {
            byRegionId: {}
        },
        interaction: {
            moveStep: 2,
            dragging: {
                active: false,
                target: null,
                regionId: null,
                pointIndex: -1
            }
        }
    };
}

export function createDefaultGeometryRegion() {
    return {
        loaded: false,
        rows: 2,
        cols: 2,
        interpolationMode: 0,
        showGrid: false,
        selected: { row: 0, col: 0, axis: 'col' },
        points: createGeometryPointGrid(2, 2)
    };
}

function createDefaultMask() {
    return {
        enabled: false,
        rows: 2,
        cols: 2,
        showGrid: false,
        selected: { row: 0, col: 0, axis: 'col' },
        points: createPointGrid(2, 2),
        interpolationMode: 0  // 0: 直线(Linear), 1: 曲线(Hermite/Catmull-Rom)
    };
}

export function createDefaultColorRegion() {
    return {
        brightness: 1,
        contrast: 1,
        saturation: 1
    };
}
