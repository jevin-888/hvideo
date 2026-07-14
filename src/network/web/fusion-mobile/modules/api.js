import { createFusionApi } from '../../modules/pages/fusion/apiCore.js?v=2.95';
import { buildModuleActionRequest } from '../../shared/moduleActions.js';
import { parseApiResponse } from '../../shared/apiResponseParser.js';

const API_BASE = '/api/v1';


async function request(method, path, data = null, timeoutMs = 6000) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    const options = {
        method,
        headers: { 'Content-Type': 'application/json' },
        signal: controller.signal
    };
    if (data !== null) options.body = JSON.stringify(data);

    try {
        const response = await fetch(API_BASE + path, options);
        const text = await response.text();
        if (!text) {
            throw new Error('后端返回空响应，不符合统一 API contract');
        }

        let json;
        try {
            json = JSON.parse(text);
        } catch (error) {
            throw new Error('后端返回非 JSON 响应，不符合统一 API contract');
        }

        const parsed = parseApiResponse(json);
        if (!response.ok || !parsed.ok) {
            throw new Error(parsed.error?.message || `HTTP ${response.status}`);
        }
        return parsed.data;
    } finally {
        clearTimeout(timer);
    }
}

export function apiGet(path, timeoutMs) {
    return request('GET', path, null, timeoutMs);
}

export function apiPost(path, data, timeoutMs) {
    return request('POST', path, data, timeoutMs);
}

export function apiAction(moduleName, action, params = {}, timeoutMs) {
    const { path, payload } = buildModuleActionRequest(moduleName, action, params);
    return apiPost(path, payload, timeoutMs);
}

export function apiPut(path, data, timeoutMs) {
    return request('PUT', path, data, timeoutMs);
}

export function fusionCommand(param, timeoutMs) {
    const { action, ...params } = param;
    return apiAction('regions', action, params, timeoutMs);
}

export function moduleAction(moduleName, param, timeoutMs) {
    const { action, ...params } = param;
    return apiAction(moduleName, action, params, timeoutMs);
}

export const layerApi = {
    list: () => apiGet('/layers'),
    listRuntime: () => apiGet('/runtime/layers'),
    getRuntime: (layerId) => apiGet(`/runtime/layers/${layerId}`),
    updateRuntime: (layerId, payload) => apiPut(`/runtime/layers/${layerId}`, payload),
    updateConfig: (layerId, payload) => apiPut(`/config/layers/${layerId}`, payload)
};

export const videoApi = {
    status: (layerId) => apiGet(`/video/status?layerId=${layerId}`),
    command: (action, payload = {}, timeoutMs) => {
        const actionMap = {
            muteToggle: 'mute/toggle',
            volumeDown: 'volume/down',
            volumeUp: 'volume/up',
            getSystemVolume: 'getSystemVolume',
            setSystemVolume: 'systemVolume',
            setVolume: 'volume'
        };
        return apiPost(`/video/${actionMap[action] || action}`, payload, timeoutMs);
    }
};

export const peripheralApi = {
    dmxStatus: () => apiGet('/dmx/status'),
    dmxChannels: () => apiGet('/dmx/channels'),
    setDmxChannel: (offset, value) => moduleAction('peripheral-events', {
        action: 'set_channel',
        offset,
        value
    }),
    listSerialPorts: () => moduleAction('peripherals', { action: 'list_serial_ports' })
};

export const systemApi = {
    status: () => apiGet('/system/status'),
    resources: () => apiGet('/system/resources'),
    networkInfo: () => apiGet('/system/network/info'),
    deviceInfo: () => apiGet('/system/device-info')
};

export const fusionApi = createFusionApi({ fusionCommand, apiPost });

export const backgroundApi = {
    listImages: () => apiGet('/materials?type=image&folder=gb_fusion'),
    listAllImages: () => apiGet('/materials?type=image'),
    indexStatus: () => apiGet('/materials/index_status'),
    refreshMaterials: () => apiPost('/materials/refresh_index', {}),
    getCanvasStatus: () => apiGet('/system/status'),
    getLayer: (layerId) => apiGet(`/runtime/layers/${layerId}`),
    updateLayer: (layerId, payload) => apiPut(`/runtime/layers/${layerId}`, payload),
    listRuntimeLayers: () => apiGet('/runtime/layers'),
    listLayers: () => apiGet('/layers'),
    createRuntimeImageLayer: (layerId, priority = 0) => moduleAction('layers', {
        action: 'create_runtime_layer',
        layerId,
        layer_type: 'image',
        visible: false,
        priority
    }),
    removeRuntimeLayer: (layerId) => moduleAction('layers', {
        action: 'remove_runtime_layer',
        layerId
    }),
    loadImage: (layerId, imageFile) => moduleAction('rendering', {
        action: 'loadImage',
        layerId,
        image_file: imageFile
    }),
    videoCommand: (layerId, action) => moduleAction('playback', { action, layerId })
};
