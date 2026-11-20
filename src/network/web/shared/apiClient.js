import { addToCommandLog } from './commandLog.js';
import { parseApiResponse } from './apiResponseParser.js';
import { buildModuleActionRequest } from './moduleActions.js';

const API_BASE = '/api/v1';
window.apiBaseUrl = API_BASE;
const DEFAULT_TIMEOUT_MS = 8000;
const DEFAULT_RETRY_COUNT = 1;
const DEFAULT_RETRY_DELAY_MS = 180;
const RETRYABLE_STATUS_CODES = new Set([408, 425, 429, 502, 503, 504]);
const RETRYABLE_SAFE_METHODS = new Set(['GET', 'HEAD', 'OPTIONS']);

function toPositiveNumber(value, fallback) {
    const number = typeof value === 'number' ? value : Number(value);
    return Number.isFinite(number) && number > 0 ? number : fallback;
}

function normalizeRequestOptions(method, timeoutOrOptions) {
    const rawOptions = timeoutOrOptions && typeof timeoutOrOptions === 'object'
        ? { ...timeoutOrOptions }
        : { timeoutMs: timeoutOrOptions };
    const timeoutMs = toPositiveNumber(rawOptions.timeoutMs, DEFAULT_TIMEOUT_MS);
    const upperMethod = String(method || 'GET').toUpperCase();
    const defaultRetries = RETRYABLE_SAFE_METHODS.has(upperMethod) ? DEFAULT_RETRY_COUNT : 0;
    const retries = Number.isInteger(rawOptions.retries)
        ? Math.max(0, rawOptions.retries)
        : defaultRetries;
    const retryDelayMs = toPositiveNumber(rawOptions.retryDelayMs, DEFAULT_RETRY_DELAY_MS);
    return {
        ...rawOptions,
        timeoutMs,
        retries,
        retryDelayMs
    };
}

function shouldRetryHttpStatus(method, status, requestOptions) {
    if (requestOptions.retryOnHttp === false) return false;
    if (!RETRYABLE_STATUS_CODES.has(status)) return false;
    const upperMethod = String(method || 'GET').toUpperCase();
    return RETRYABLE_SAFE_METHODS.has(upperMethod) || requestOptions.retryMutations === true;
}

function shouldRetryFetchError(method, requestOptions) {
    if (requestOptions.retryOnNetwork === false) return false;
    const upperMethod = String(method || 'GET').toUpperCase();
    if (RETRYABLE_SAFE_METHODS.has(upperMethod) || requestOptions.retryMutations === true) {
        return true;
    }
    return false;
}

function retryDelayForAttempt(baseDelayMs, attempt) {
    const jitterMs = Math.floor(Math.random() * 80);
    return Math.min(1500, baseDelayMs * Math.pow(2, attempt)) + jitterMs;
}

function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function fetchWithStabilityRetry(method, url, options, requestOptions) {
    let lastError = null;

    for (let attempt = 0; attempt <= requestOptions.retries; attempt++) {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), requestOptions.timeoutMs);

        try {
            const response = await fetch(url, {
                ...options,
                signal: controller.signal
            });
            clearTimeout(timeoutId);

            if (attempt < requestOptions.retries &&
                shouldRetryHttpStatus(method, response.status, requestOptions)) {
                // 说明：重试前先读完响应，让浏览器干净地结束连接。
                try { await response.text(); } catch (_) { /* 忽略重试前读取响应失败 */ }
                await delay(retryDelayForAttempt(requestOptions.retryDelayMs, attempt));
                continue;
            }

            return response;
        } catch (error) {
            clearTimeout(timeoutId);
            lastError = error;
            if (attempt < requestOptions.retries &&
                shouldRetryFetchError(method, requestOptions)) {
                await delay(retryDelayForAttempt(requestOptions.retryDelayMs, attempt));
                continue;
            }
            throw error;
        }
    }

    throw lastError || new Error('请求失败');
}

export function createApiClient({ getAuthToken, handleUnauthorized, isAuthRedirecting } = {}) {
    let lastApiError = null;

    function getLastApiError() {
        return lastApiError;
    }

    function setLastApiError(reason, message) {
        lastApiError = { reason, message };
    }

    // 封装 API 请求：成功只返回业务 data，任何失败只返回 null。
    // 线上响应必须严格符合 { ok, data, error }，这里不构造第二套响应对象。
    async function apiRequest(method, url, data = null, timeoutMs = DEFAULT_TIMEOUT_MS) {
        lastApiError = null;
        if (window.location.protocol === 'file:' || isAuthRedirecting?.()) return null;

        const requestOptions = normalizeRequestOptions(method, timeoutMs);
        const isFormData = data instanceof FormData || requestOptions.isFormData;
        const token = getAuthToken?.();
        const headers = isFormData
            ? { ...(requestOptions.headers || {}) }
            : { 'Content-Type': 'application/json', ...(requestOptions.headers || {}) };
        if (token) headers.Authorization = `Bearer ${token}`;

        const options = { method, headers };
        if (data !== null && data !== undefined) {
            options.body = isFormData ? data : JSON.stringify(data);
        }

        let response;
        try {
            response = await fetchWithStabilityRetry(
                method,
                API_BASE + url,
                options,
                requestOptions
            );
        } catch (error) {
            const isTimeout = error.name === 'AbortError';
            const reason = isTimeout ? 'timeout' : 'network';
            const message = isTimeout
                ? `请求超时 (${requestOptions.timeoutMs}ms)`
                : (error.message || '网络连接失败');
            setLastApiError(reason, message);
            if (!requestOptions.silent) {
                addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
            }
            return null;
        }

        if (handleUnauthorized?.(response, token)) {
            setLastApiError('UNAUTHORIZED', '登录已失效，请重新登录');
            return null;
        }

        let responseText;
        try {
            responseText = await response.text();
        } catch (error) {
            const message = `读取响应失败: ${error.message}`;
            setLastApiError('read', message);
            if (!requestOptions.silent) {
                addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
            }
            return null;
        }

        if (!responseText || responseText.trim() === '') {
            setLastApiError('empty', '后端返回空响应，不符合统一 API contract');
            if (!requestOptions.silent) {
                addToCommandLog('API请求失败', 'error', `${method} ${url}: 响应为空`);
            }
            return null;
        }

        let jsonData;
        try {
            jsonData = JSON.parse(responseText);
        } catch (error) {
            const message = '后端返回非 JSON 响应，不符合统一 API contract';
            setLastApiError('parse', message);
            if (!requestOptions.silent) {
                addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
            }
            return null;
        }

        const parsed = parseApiResponse(jsonData);
        if (!response.ok || !parsed.ok) {
            const error = parsed.error || { code: `HTTP_${response.status}`, message: `HTTP ${response.status}` };
            const message = error.message || `HTTP ${response.status}`;
            setLastApiError(error.code || 'api', message);

            const optionalMissingCommandList = response.status === 404 &&
                url.includes('/peripherals/command-lists/read');
            const startupUnavailable = response.status === 503 &&
                (url.includes('/system/status') ||
                 url.includes('/system/resources') ||
                 url.includes('/playlists'));
            if (!requestOptions.silent && !optionalMissingCommandList && !startupUnavailable) {
                addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
            }
            return null;
        }

        return parsed.data;
    }

    /**
     * API GET请求
     * @param {string} url - 说明：API 地址
     * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
     */
    async function apiGet(url, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return await apiRequest('GET', url, null, timeoutMs);
    }

    /**
     * API POST请求
     * @param {string} url - 说明：API 地址
     * @param {Object} data - 请求数据
     * @param {number} 时间outMs - 超时时间（毫秒），默认5000ms
     * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
     */
    async function apiPost(url, data, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return await apiRequest('POST', url, data, timeoutMs);
    }

    /**
     * 调用唯一的模块动作端点。commandCode 只用于前端模块映射，不会进入网络请求。
     */
    async function apiAction(moduleName, action, params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const { path, payload } = buildModuleActionRequest(moduleName, action, params);
        return await apiPost(path, payload, timeoutMs);
    }

    /**
     * API Action请求 (抛出异常版本)
     * @param {string} moduleName - 模块名称
     * @param {string} action - 动作名称
     * @param {object} params - 参数对象
     * @param {number} timeoutMs - 超时时间(毫秒)
     * @returns {Promise<any>} 成功时返回数据对象，失败时抛出异常
     */
    async function apiActionOrThrow(moduleName, action, params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
        const { path, payload } = buildModuleActionRequest(moduleName, action, params);
        const data = await apiPost(path, payload, timeoutMs);
        if (data === null) {
            const error = getLastApiError() || { reason: 'unknown', message: 'API请求失败' };
            throw new Error(error.message);
        }
        return data;
    }

    /**
     * API PUT请求
     * @param {string} url - 说明：API 地址
     * @param {Object} data - 请求数据
     * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
     */
    async function apiPut(url, data, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return await apiRequest('PUT', url, data, timeoutMs);
    }

    /**
     * API DELETE请求
     * @param {string} url - 说明：API 地址
     * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
     */
    async function apiDelete(url, timeoutMs = DEFAULT_TIMEOUT_MS) {
        return await apiRequest('DELETE', url, null, timeoutMs);
    }

    // 发送图层操作命令
    async function sendLayerCommand(layerId, action, params = {}) {
        // 参数验证：只检查明显无效的值，允许0（虽然通常图层ID从1开始，但让后端处理）
        const numLayerId = Number(layerId);
        if (layerId === null || layerId === undefined || (isNaN(numLayerId) && layerId !== 0)) {
            console.warn(`sendLayerCommand: Invalid layerId: ${layerId}, using default 1`);
            layerId = 1;  // 使用默认值而不是抛出错误
        } else {
            layerId = numLayerId;
        }

        if (!action || typeof action !== 'string') {
            console.warn(`sendLayerCommand: Invalid action: ${action}`);
            throw new Error(`Invalid action: ${action}`);
        }

        const layerManagementActions = ['startCapture', 'stopCapture', 'restartCapture', 'restartRk628Capture'];
        const layerRenderActions = ['generate_qrcode'];
        const moduleName = layerManagementActions.includes(action)
            ? 'layers'
            : (layerRenderActions.includes(action) ? 'rendering' : 'playback');

        return await apiAction(moduleName, action, { layerId, ...params });
    }

    return {
        getLastApiError, apiRequest, apiGet, apiPost, apiPut, apiDelete,
        apiAction, apiActionOrThrow, sendLayerCommand
    };
}
