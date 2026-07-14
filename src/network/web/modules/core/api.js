/**
 * @file api.js（文件名）
 * @brief API请求封装模块
 *
 * 本模块提供统一的API请求接口，包括：
 * - API请求基础URL配置
 * - 统一的响应格式解析
 * - GET/POST/PUT/DELETE请求封装
 * - 命令日志记录
 */

import { addToCommandLog } from './commandLog.js';
import { parseApiResponse } from '../utils/apiResponseParser.js';
import { buildModuleActionRequest } from '../../shared/moduleActions.js';

// API请求基础URL
window.apiBaseUrl = '/api/v1';

/** 最近一次 API 失败原因，便于调用方展示具体错误（超时/网络/解析等） */
let lastApiError = null;

const DEFAULT_TIMEOUT_MS = 8000;
const DEFAULT_RETRY_COUNT = 1;
const DEFAULT_RETRY_DELAY_MS = 180;
const RETRYABLE_STATUS_CODES = new Set([408, 425, 429, 502, 503, 504]);
const RETRYABLE_SAFE_METHODS = new Set(['GET', 'HEAD', 'OPTIONS']);

export function getLastApiError() {
    return lastApiError;
}

function setLastApiError(reason, message) {
    lastApiError = { reason, message };
}

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
                shouldRetryFetchError(method, error, requestOptions)) {
                await delay(retryDelayForAttempt(requestOptions.retryDelayMs, attempt));
                continue;
            }
            throw error;
        }
    }

    throw lastError || new Error('请求失败');
}

// 检测是否通过file://协议访问
let isFileProtocol = false;
let fileProtocolWarningShown = false;

/**
 * @brief 检测访问协议并给出提示
 */
function checkProtocol() {
    if (window.location.protocol === 'file:') {
        isFileProtocol = true;
        if (!fileProtocolWarningShown) fileProtocolWarningShown = true;
        return true;
    }
    return false;
}

/**
 * @brief 初始化API模块
 *
 * 保留函数以保持API兼容性，当前无需特殊初始化
 */
export function initializeApi() {
    // 检测访问协议
    checkProtocol();
    // API初始化完成
}

// 封装 API 请求：成功只返回业务 data，任何失败只返回 null。
// 线上响应必须严格符合 { ok, data, error }，这里不构造第二套响应对象。
export async function apiRequest(method, url, data = null, timeoutMs = DEFAULT_TIMEOUT_MS) {
    lastApiError = null;
    if (checkProtocol()) return null;

    const requestOptions = normalizeRequestOptions(method, timeoutMs);
    const isFormData = data instanceof FormData || requestOptions.isFormData;
    const token = localStorage.getItem('hvideo_auth_token');
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
            window.apiBaseUrl + url,
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
        addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
        return null;
    }

    let responseText;
    try {
        responseText = await response.text();
    } catch (error) {
        const message = `读取响应失败: ${error.message}`;
        setLastApiError('read', message);
        addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
        return null;
    }

    if (!responseText || responseText.trim() === '') {
        setLastApiError('empty', '后端返回空响应，不符合统一 API contract');
        addToCommandLog('API请求失败', 'error', `${method} ${url}: 响应为空`);
        return null;
    }

    let jsonData;
    try {
        jsonData = JSON.parse(responseText);
    } catch (error) {
        const message = '后端返回非 JSON 响应，不符合统一 API contract';
        setLastApiError('parse', message);
        addToCommandLog('API请求失败', 'error', `${method} ${url}: ${message}`);
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
        if (!optionalMissingCommandList && !startupUnavailable) {
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
export async function apiGet(url, timeoutMs = DEFAULT_TIMEOUT_MS) {
    return await apiRequest('GET', url, null, timeoutMs);
}

/**
 * API POST请求
 * @param {string} url - 说明：API 地址
 * @param {Object} data - 请求数据
 * @param {number} 时间outMs - 超时时间（毫秒），默认5000ms
 * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
 */
export async function apiPost(url, data, timeoutMs = DEFAULT_TIMEOUT_MS) {
    return await apiRequest('POST', url, data, timeoutMs);
}

/**
 * 调用唯一的模块动作端点。commandCode 只用于前端模块映射，不会进入网络请求。
 */
export async function apiAction(moduleName, action, params = {}, timeoutMs = DEFAULT_TIMEOUT_MS) {
    const { path, payload } = buildModuleActionRequest(moduleName, action, params);
    return await apiPost(path, payload, timeoutMs);
}

/**
 * API PUT请求
 * @param {string} url - 说明：API 地址
 * @param {Object} data - 请求数据
 * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
 */
export async function apiPut(url, data, timeoutMs = DEFAULT_TIMEOUT_MS) {
    return await apiRequest('PUT', url, data, timeoutMs);
}

/**
 * API DELETE请求
 * @param {string} url - 说明：API 地址
 * @returns {Promise<any>} 成功时返回数据对象，失败时返回null
 */
export async function apiDelete(url, timeoutMs = DEFAULT_TIMEOUT_MS) {
    return await apiRequest('DELETE', url, null, timeoutMs);
}

// 发送图层操作命令
export async function sendLayerCommand(layerId, action, params = {}) {
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
