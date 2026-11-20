function isFiniteNumber(value) {
    return typeof value === 'number' && Number.isFinite(value);
}

export function createFusionApi({ fusionCommand, apiPost }) {
    const post = (param, timeoutMs) => fusionCommand(param, timeoutMs);
    let checkpointTimer = null;
    let checkpointChain = Promise.resolve();

    const clearCheckpointTimer = () => {
        if (!checkpointTimer) return;
        clearTimeout(checkpointTimer);
        checkpointTimer = null;
    };

    const runCheckpoint = () => {
        clearCheckpointTimer();
        checkpointChain = checkpointChain
            .catch(() => undefined)
            .then(() => apiPost('/fusion/checkpoint', {}));
        checkpointChain.catch((error) => {
            console.warn('fusion realtime checkpoint failed:', error);
        });
        return checkpointChain;
    };

    // Throttle instead of debounce: continuous dragging is checkpointed every
    // 400 ms rather than waiting indefinitely for the interaction to stop.
    const scheduleCheckpoint = () => {
        if (checkpointTimer) return;
        checkpointTimer = setTimeout(() => {
            void runCheckpoint();
        }, 400);
    };

    const postDurableMutation = (param, timeoutMs) => post(param, timeoutMs)
        .then((result) => {
            scheduleCheckpoint();
            return result;
        });

    return {
        persist() {
            clearCheckpointTimer();
            return checkpointChain
                .catch(() => undefined)
                .then(() => apiPost('/fusion/save', {}));
        },

        checkpoint() {
            return runCheckpoint();
        },

        reset() {
            clearCheckpointTimer();
            return checkpointChain
                .catch(() => undefined)
                .then(() => apiPost('/fusion/reset', {}));
        },

        loadRegionConfig() {
            return post({ action: 'get_region_config' });
        },

        setFlexibleMapping(config = {}) {
            return post({
                action: 'set_flexible_mapping',
                ...config
            }, 60000);
        },

        setGridVisualStyle(lineWidth, hotspotRadius) {
            return post({
                action: 'set_grid_visual_style',
                line_width: lineWidth,
                hotspot_radius: hotspotRadius
            });
        },

        loadGeometry(regionId) {
            return post({ action: 'get_geometry_state', region_id: regionId });
        },

        saveGeometry(regionId) {
            return post({ action: 'save_geometry', region_id: regionId });
        },

        showGeometryGrid(regionId, showGuide) {
            return post({
                action: 'set_geometry_display',
                region_id: regionId,
                show_grid: !!showGuide
            });
        },

        showGeometryGridAll(showGuide) {
            return post({
                action: 'set_geometry_display_all',
                show_grid: !!showGuide
            });
        },

        setGeometrySelection(regionId, row, col) {
            return post({
                action: 'set_geometry_selection',
                region_id: regionId,
                selected_row: row,
                selected_col: col
            });
        },

        setActiveRegion(regionId) {
            return post({
                action: 'set_active_region_id',
                region_id: regionId
            });
        },

        setGeometryGrid(regionId, rows, cols, interpolationMode, options = {}) {
            const param = {
                action: 'set_geometry_grid',
                region_id: regionId,
                rows,
                cols
            };
            if (typeof interpolationMode === 'number') {
                param.interpolation_mode = interpolationMode;
            }
            if (options && Object.prototype.hasOwnProperty.call(options, 'syncBlendGrid')) {
                param.sync_blend_grid = !!options.syncBlendGrid;
            }
            if (options && Object.prototype.hasOwnProperty.call(options, 'recalculateBlend')) {
                param.recalculate_blend = !!options.recalculateBlend;
            }
            return post(param);
        },

        resizeGeometry(regionId, op) {
            return post({
                action: 'geometry_resize_grid',
                region_id: regionId,
                op
            });
        },

        resizeBlendGridBatch(regionIds, op) {
            return post({
                action: 'blend_resize_grid',
                region_ids: regionIds,
                op
            });
        },

        moveGeometry(regionId, op, du, dv) {
            return postDurableMutation({
                action: 'geometry_move',
                region_id: regionId,
                op,
                du,
                dv
            });
        },

        moveManagerPoint(regionId, direction, du, dv, corner = null) {
            const param = {
                action: 'manager_move_point',
                region_id: regionId,
                direction,
                du,
                dv
            };
            if (typeof corner === 'string' && corner) param.corner = corner;
            return postDurableMutation(param);
        },

        moveManagerLine(regionId, direction, du, dv, selected = null) {
            const param = {
                action: 'manager_move_line',
                region_id: regionId,
                direction,
                du,
                dv
            };
            if (selected && Number.isInteger(selected.row) && Number.isInteger(selected.col)) {
                param.selected_row = selected.row;
                param.selected_col = selected.col;
            }
            return postDurableMutation(param);
        },

        setGeometryPoint(regionId, row, col, u, v) {
            return postDurableMutation({
                action: 'set_geometry_point',
                region_id: regionId,
                row,
                col,
                u,
                v
            });
        },

        setGeometryPoints(regionId, points, rows, cols, interpolationMode) {
            const param = {
                action: 'set_geometry_points',
                region_id: regionId,
                points
            };
            if (typeof rows === 'number') param.rows = rows;
            if (typeof cols === 'number') param.cols = cols;
            if (typeof interpolationMode === 'number') param.interpolation_mode = interpolationMode;
            return postDurableMutation(param);
        },

        resizeMask(op) {
            return postDurableMutation({
                action: 'mask_resize_grid',
                op
            });
        },

        moveMask(op, du, dv, row = undefined, col = undefined) {
            const param = {
                action: 'mask_move',
                op,
                du,
                dv
            };
            if (isFiniteNumber(row)) param.row = row;
            if (isFiniteNumber(col)) param.col = col;
            return postDurableMutation(param);
        },

        loadMask() {
            return post({ action: 'get_mask_state' });
        },

        setMask(payload = {}) {
            return postDurableMutation({
                action: 'set_mask_state',
                ...payload
            });
        },

        seedMaskFromGeometry() {
            return postDurableMutation({ action: 'seed_mask_from_geometry' });
        },

        setMaskGuideVisibility(showGrid) {
            return postDurableMutation({
                action: 'set_mask_state',
                show_guide: !!showGrid,
                selected_row: showGrid ? 0 : -1,
                selected_col: showGrid ? 0 : -1
            });
        },

        saveMask() {
            return post({ action: 'save_mask' });
        },

        loadBlend(regionId) {
            return post({
                action: 'get_region_blend',
                region_id: regionId
            });
        },

        autoRecalculateBlend() {
            return post({
                action: 'auto_recalculate_blend'
            });
        },

        saveBlend(regionId, blendPayload = {}, timeoutMs) {
            return post({
                action: 'set_region_blend',
                region_id: regionId,
                is_normalized: true,
                ...blendPayload
            }, timeoutMs);
        },

        setBlendCurveParams(regionId, side, params = {}) {
            const suffix = { left: 'left', right: 'right', top: 'top', bottom: 'bottom' }[side];
            const payload = {
                action: 'set_region_blend',
                region_id: regionId,
                is_normalized: true
            };
            if (suffix && Object.prototype.hasOwnProperty.call(params, 'gamma')) {
                payload[`edge_${suffix}_gamma`] = params.gamma;
            }
            if (suffix && Object.prototype.hasOwnProperty.call(params, 'slope')) {
                payload[`edge_${suffix}_slope`] = params.slope;
            }
            if (suffix && Object.prototype.hasOwnProperty.call(params, 'stripStart')) {
                payload[`strip_start_${suffix[0]}`] = params.stripStart;
            }
            if (suffix && Object.prototype.hasOwnProperty.call(params, 'stripEnd')) {
                payload[`strip_end_${suffix[0]}`] = params.stripEnd;
            }
            if (suffix && Object.prototype.hasOwnProperty.call(params, 'anchor')) {
                payload[`anchor_${suffix[0]}`] = params.anchor;
            }
            return post(payload);
        },

        setMaster(enabled) {
            return post({
                action: 'set_fusion_master_enabled',
                enabled
            });
        },

        getMaster() {
            return post({ action: 'get_fusion_master_enabled' });
        },

        setBlendAutoEdges(enabled) {
            return post({
                action: 'set_blend_auto_edges',
                enabled: !!enabled
            });
        },

        getBlendAutoEdges() {
            return post({ action: 'get_blend_auto_edges' });
        },

        setManagerMode(enabled, traceId = '') {
            const payload = {
                action: 'set_manager_mode',
                enabled
            };
            if (traceId) payload.trace_id = traceId;
            return post(payload);
        },

        getManagerMode() {
            return post({ action: 'get_manager_mode' });
        },

        setMergeGapBrightness(regionId, side, colorId, value) {
            return post({
                action: 'set_merge_gap_brightness',
                region_id: regionId,
                side,
                color_id: colorId,
                value
            });
        },

        loadColor(regionId) {
            return post({
                action: 'get_region_color',
                region_id: regionId
            });
        },

        saveColor(regionId, color) {
            return post({
                action: 'set_region_color',
                region_id: regionId,
                brightness: color.brightness,
                contrast: color.contrast,
                saturation: color.saturation
            });
        },

        loadCorrection(regionId) {
            return post({
                action: 'get_region_geometry_correction',
                region_id: regionId
            });
        },

        saveCorrection(regionId, correction = {}) {
            return post({
                action: 'set_region_geometry_correction',
                region_id: regionId,
                ...correction
            });
        },

        loadCave(regionId) {
            return post({
                action: 'get_cave_wall_config',
                region_id: regionId
            });
        },

        saveCave(regionId, cave = {}) {
            return post({
                action: 'set_cave_wall_config',
                region_id: regionId,
                ...cave
            });
        },

        initializeCave(regionId, wallType = 0) {
            return post({
                action: 'initialize_cave_wall',
                region_id: regionId,
                wall_type: wallType
            });
        }
    };
}
