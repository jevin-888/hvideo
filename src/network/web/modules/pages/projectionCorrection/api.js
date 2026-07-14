import { apiPost, apiAction } from '../../core/api.js';

export const PROJECTION_CORRECTION_ACTIONS = Object.freeze({
    LOAD_REGION_CONFIG: 'get_region_config',
    LOAD: 'get_projection_correction',
    SAVE: 'set_projection_correction',
    SAVE_CONFIG: 'save_projection_correction_config'
});

export function postProjectionCorrectionCommand(param, timeoutMs) {
    const { action, ...params } = param;
    return apiAction('regions', action, params, timeoutMs);
}

export const projectionCorrectionApi = {
    loadRegionConfig() {
        return postProjectionCorrectionCommand({
            action: PROJECTION_CORRECTION_ACTIONS.LOAD_REGION_CONFIG
        });
    },

    load(regionId) {
        return postProjectionCorrectionCommand({
            action: PROJECTION_CORRECTION_ACTIONS.LOAD,
            region_id: regionId
        });
    },

    save(regionId, payload = {}) {
        return postProjectionCorrectionCommand({
            action: PROJECTION_CORRECTION_ACTIONS.SAVE,
            region_id: regionId,
            ...payload
        });
    },

    saveConfig(regionId) {
        return postProjectionCorrectionCommand({
            action: PROJECTION_CORRECTION_ACTIONS.SAVE_CONFIG,
            region_id: regionId
        });
    }
};
