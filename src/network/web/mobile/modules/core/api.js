/**
 * @file api.js（文件名）
 * @brief API请求封装模块（移动端版本）
 */

import { addToCommandLog } from './commandLog.js';
import { parseApiResponse } from '../utils/apiResponseParser.js';
import { buildModuleActionRequest } from '../../../shared/moduleActions.js';

// API请求基础URL
window.apiBaseUrl = '/api/v1';

const DEFAULT_TIMEOUT_MS = 8000;
const SCENE_LOAD_TIMEOUT_MS = 15000;
const DEFAULT_RETRY_COUNT = 1;
const DEFAULT_RETRY_DELAY_MS = 180;
const RETRYABLE_STATUS_CODES = new Set([408, 425, 429, 502, 503, 504]);
const RETRYABLE_SAFE_METHODS = new Set(['GET', 'HEAD', 'OPTIONS']);

function toPositiveNumber(value, fallback) {
    const number = typeof value === 'number' ? value : Number(value);
    return Number.isFinite(number) && number > 0 ? number : fallback;
}

function normalizeRequestOptions(method, url, timeoutOrOptions) {
    const rawOptions = timeoutOrOptions && typeof timeoutOrOptions === 'object'
        ? { ...timeoutOrOptions }
        : { timeoutMs: timeoutOrOptions };
    const defaultTimeout = url.includes('/scenes/') && url.includes('/load')
        ? SCENE_LOAD_TIMEOUT_MS
        : DEFAULT_TIMEOUT_MS;
    const timeoutMs = toPositiveNumber(rawOptions.timeoutMs, defaultTimeout);
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
        retryDelayMs,
        isSafeMethod: RETRYABLE_SAFE_METHODS.has(upperMethod)
    };
}

function shouldRetryHttpStatus(method, status, requestOptions) {
    if (requestOptions.retryOnHttp === false) return false;
    if (!RETRYABLE_STATUS_CODES.has(status)) return false;
    const upperMethod = String(method || 'GET').toUpperCase();
    return RETRYABLE_SAFE_METHODS.has(upperMethod) || requestOptions.retryMutations === true;
}

function shouldRetryFetchError(method, error, requestOptions) {
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

/**
 * 内部统一 fetch 核心：处理 AbortController/超时/网络错误/读取/JSON 解析/响应解析。
 * 返回归一化的原始结果，由各导出函数映射为自己的返回结构。
 * @return 示例/字段：{Promise<{stage: string, response: Response|null, parsed: object|null, errorMsg: string|null}>}
 *   stage 取值：'network' | 'read' | 'empty' | 'parse' | 'ok' | 'abort'
 */
async function _doFetch(method, url, data, timeoutMs) {
    const requestOptions = normalizeRequestOptions(method, url, timeoutMs);

    const options = {
        method: method,
        headers: {
            'Content-Type': 'application/json',
        }
    };

    if (data) {
        options.body = JSON.stringify(data);
    }

    let response;
    for (let attempt = 0; attempt <= requestOptions.retries; attempt++) {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), requestOptions.timeoutMs);
        try {
            response = await fetch(window.apiBaseUrl + url, {
                ...options,
                signal: controller.signal
            });
            clearTimeout(timeoutId);

            if (attempt < requestOptions.retries &&
                shouldRetryHttpStatus(method, response.status, requestOptions)) {
                try { await response.text(); } catch (_) { /* 忽略重试前读取响应失败 */ }
                await delay(retryDelayForAttempt(requestOptions.retryDelayMs, attempt));
                continue;
            }
            break;
        } catch (fetchError) {
            clearTimeout(timeoutId);
            if (attempt < requestOptions.retries &&
                shouldRetryFetchError(method, fetchError, requestOptions)) {
                await delay(retryDelayForAttempt(requestOptions.retryDelayMs, attempt));
                continue;
            }
            const isTimeout = fetchError.name === 'AbortError';
            const errorMsg = isTimeout ? `请求超时 (${requestOptions.timeoutMs}ms)` : (fetchError.message || '网络连接失败');
            addToCommandLog('API请求失败', 'error', `${method} ${url}: ${errorMsg}`);
            console.error('[API请求] 网络错误:', fetchError);
            return { stage: isTimeout ? 'abort' : 'network', response: null, parsed: null, errorMsg };
        }
    }

    let responseText = '';
    try {
        responseText = await response.text();
    } catch (error) {
        addToCommandLog('API请求失败', 'error', `${method} ${url}: 读取响应失败 - ${error.message}`);
        return { stage: 'read', response, parsed: null, errorMsg: '读取响应失败' };
    }

    if (!responseText || responseText.trim() === '') {
        addToCommandLog('API请求失败', 'error', `${method} ${url}: 响应为空`);
        return { stage: 'empty', response, parsed: null, errorMsg: '响应为空' };
    }

    let jsonData = null;
    try {
        jsonData = JSON.parse(responseText);
    } catch (parseError) {
        addToCommandLog('API请求失败', 'error', `${method} ${url}: JSON解析失败 - ${parseError.message}`);
        return { stage: 'parse', response, parsed: null, errorMsg: 'JSON解析失败' };
    }

    const parsed = parseApiResponse(jsonData);
    return { stage: 'ok', response, parsed, errorMsg: null };
}

/**
 * API请求函数
 */
export async function apiRequest(method, url, data = null, timeoutMs = null) {
    try {
        const result = await _doFetch(method, url, data, timeoutMs);
        const { stage, response, parsed } = result;

        // 网络错误/读取失败/空响应/JSON解析失败：均返回 null（日志已在 _doFetch 记录）
        if (stage !== 'ok') {
            return null;
        }

        if (!response.ok) {
            if (response.status === 404) {
                return null;
            }

            if (response.status === 503 &&
                (url.includes('/system/status') || url.includes('/system/resources'))) {
                return null;
            }

            if (!parsed.ok) {
                addToCommandLog('API请求失败', 'error', `${method} ${url}: ${parsed.message}`);
            }

            if (parsed.ok) {
                return parsed.data;
            }
            return null;
        }

        if (!parsed.ok) {
            addToCommandLog('API请求失败', 'error', `${method} ${url}: ${parsed.message}`);
        }

        return parsed.ok ? (parsed.data !== undefined ? parsed.data : true) : null;
    } catch (error) {
        if (error.name === 'AbortError') {
            return null;
        }
        addToCommandLog('API请求失败', 'error', error.message);
        return null;
    }
}

/**
 * API GET请求
 */
export async function apiGet(url, timeoutMs = null) {
    return await apiRequest('GET', url, null, timeoutMs);
}

/**
 * API POST请求
 */
export async function apiPost(url, data, timeoutMs = null) {
    return await apiRequest('POST', url, data, timeoutMs);
}

export async function apiAction(moduleName, action, params = {}, timeoutMs = null) {
    const { path, payload } = buildModuleActionRequest(moduleName, action, params);
    return await apiPost(path, payload, timeoutMs);
}

/**
 * 模块 action 请求：成功直接返回业务 data，失败抛出 Error。
 */
export async function apiActionOrThrow(moduleName, action, params = {}, timeoutMs = null) {
    const { path, payload } = buildModuleActionRequest(moduleName, action, params);
    return await apiPostOrThrow(path, payload, timeoutMs);
}

export async function apiPostOrThrow(url, data, timeoutMs = null) {
    const result = await _doFetch('POST', url, data, timeoutMs);
    const { stage, response, parsed, errorMsg } = result;

    if (stage !== 'ok') {
        throw new Error(errorMsg || '请求失败');
    }

    if (!response.ok || !parsed.ok) {
        const message = parsed?.error?.message || parsed?.message || `HTTP ${response.status}`;
        addToCommandLog('API请求失败', 'error', `POST ${url}: ${message}`);
        throw new Error(message);
    }

    return parsed.data;
}

/**
 * API PUT请求
 */
export async function apiPut(url, data) {
    return await apiRequest('PUT', url, data);
}

/**
 * API DELETE请求
 */
export async function apiDelete(url) {
    return await apiRequest('DELETE', url);
}

/**
 * 发送图层操作命令
 */
export async function sendLayerCommand(layerId, action, params = {}) {
    return await apiAction('playback', action, { layerId, ...params });
}
