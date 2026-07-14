/**
 * 唯一的公开模块动作协议定义。
 *
 * 请求固定为：POST /api/v1/{module}/actions/{action}
 * body 只包含动作参数；模块名和动作名都不进入 body。
 */
export const ApiModule = Object.freeze({
    SYSTEM_CONFIG: 'system-config',
    LAYERS: 'layers',
    PLAYBACK: 'playback',
    RENDERING: 'rendering',
    SYNC: 'sync',
    PLAYLISTS: 'playlists',
    SCENES: 'scenes',
    REGIONS: 'regions',
    LYRICS: 'lyrics',
    SYSTEM: 'system',
    PERIPHERALS: 'peripherals',
    PERIPHERAL_EVENTS: 'peripheral-events'
});

const API_MODULES = new Set(Object.values(ApiModule));
const NAME_PATTERN = /^[A-Za-z0-9_-]+$/;
const LEGACY_PROTOCOL_FIELDS = new Set(['type', 'code', 'param', 'action']);

export function buildModuleActionRequest(moduleName, action, params = {}) {
    if (typeof moduleName !== 'string' || !API_MODULES.has(moduleName)) {
        throw new TypeError(`未注册的 API 模块: ${String(moduleName)}`);
    }
    if (typeof action !== 'string' || !NAME_PATTERN.test(action)) {
        throw new TypeError(`非法模块动作: ${String(action)}`);
    }
    if (!params || typeof params !== 'object' || Array.isArray(params)) {
        throw new TypeError('模块动作参数必须是对象');
    }

    const payload = {};
    for (const [key, value] of Object.entries(params)) {
        if (LEGACY_PROTOCOL_FIELDS.has(key)) {
            throw new TypeError(`模块动作参数禁止包含旧协议字段: ${key}`);
        }
        payload[key] = value;
    }

    return {
        path: `/${moduleName}/actions/${encodeURIComponent(action)}`,
        payload
    };
}
