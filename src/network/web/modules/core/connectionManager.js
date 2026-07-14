/**
 * @file connectionManager.js（文件名）
 * @brief 连接管理模块 - 断线重连、心跳检测、网络状态监听
 */

import { addToCommandLog } from './commandLog.js';

// 连接状态
export const ConnectionState = {
    CONNECTED: 'connected',
    DISCONNECTED: 'disconnected',
    RECONNECTING: 'reconnecting',
    UNKNOWN: 'unknown'
};

// 当前连接状态
let currentState = ConnectionState.UNKNOWN;
let heartbeatInterval = null;
let reconnectTimeout = null;
let reconnectAttempts = 0;
let heartbeatFailures = 0;
let stateChangeCallbacks = [];
let isInitialized = false;  // 防止重复初始化
let sharedEventSource = null;
let sharedSSEReconnectTimer = null;
let statusSseUnsubscribe = null;  // 用于播控屏接收 KTV/其他端口触发的 status 提示
const sharedSSEListeners = new Map();

// 配置
const CONFIG = {
    heartbeatIntervalMs: 10000,      // 心跳间隔：10秒
    heartbeatTimeoutMs: 5000,        // 心跳超时：5秒
    heartbeatFailureThreshold: 2,     // 连续失败2次才判定断线，过滤瞬时网络毛刺
    maxReconnectAttempts: 999,       // 最大重连次数（无限重连）
    reconnectDelayMs: 5000,          // 重连间隔：固定 5 秒
};

/**
 * 解析 SSE status（与 9898 同一套后端约定），写入命令日志
 */
function handleStatusMessage(dataStr) {
    if (dataStr && dataStr.length) addToCommandLog('播控', 'info', dataStr);
}

function getSSEUrl() {
    const base = (typeof window !== 'undefined' && window.apiBaseUrl) ? window.apiBaseUrl : '/api/v1';
    const path = base + '/events';
    return (typeof window !== 'undefined' && window.location && window.location.origin)
        ? (window.location.origin + path)
        : path;
}

function attachSharedSSEListeners(source) {
    sharedSSEListeners.forEach((handlers, eventName) => {
        handlers.forEach(handler => source.addEventListener(eventName, handler));
    });
}

function scheduleSharedSSEReconnect() {
    if (sharedSSEReconnectTimer) return;
    sharedSSEReconnectTimer = setTimeout(() => {
        sharedSSEReconnectTimer = null;
        if (sharedSSEListeners.size > 0) {
            ensureSharedEventSource();
        }
    }, 5000);
}

function ensureSharedEventSource() {
    if (typeof EventSource === 'undefined') {
        return null;
    }
    if (sharedEventSource && sharedEventSource.readyState !== EventSource.CLOSED) {
        return sharedEventSource;
    }

    try {
        sharedEventSource = new EventSource(getSSEUrl());
        attachSharedSSEListeners(sharedEventSource);
        sharedEventSource.onopen = function () {
            addToCommandLog('连接管理', 'info', 'Web事件推送已连接');
        };
        sharedEventSource.onerror = function () {
            if (sharedEventSource && sharedEventSource.readyState === EventSource.CLOSED) {
                sharedEventSource.close();
                sharedEventSource = null;
                scheduleSharedSSEReconnect();
            }
        };
        return sharedEventSource;
    } catch (e) {
        addToCommandLog('连接管理', 'warning', 'SSE 订阅失败: ' + (e.message || ''));
        scheduleSharedSSEReconnect();
        return null;
    }
}

export function subscribeSSEEvent(eventName, handler) {
    if (!eventName || typeof handler !== 'function') {
        return () => {};
    }

    let handlers = sharedSSEListeners.get(eventName);
    if (!handlers) {
        handlers = new Set();
        sharedSSEListeners.set(eventName, handlers);
    }
    handlers.add(handler);

    const existingSource = sharedEventSource;
    const source = ensureSharedEventSource();
    if (source && source === existingSource && source.readyState !== EventSource.CLOSED) {
        source.addEventListener(eventName, handler);
    }

    return () => {
        const registeredHandlers = sharedSSEListeners.get(eventName);
        if (registeredHandlers) {
            registeredHandlers.delete(handler);
            if (registeredHandlers.size === 0) {
                sharedSSEListeners.delete(eventName);
            }
        }
        if (sharedEventSource && sharedEventSource.readyState !== EventSource.CLOSED) {
            sharedEventSource.removeEventListener(eventName, handler);
        }
    };
}

/**
 * 订阅 8080 的 /api/events 的 status 事件，用于播控屏显示 KTV 等端口触发的提示
 */
function subscribeStatusSSE() {
    if (statusSseUnsubscribe) return;
    statusSseUnsubscribe = subscribeSSEEvent('status', (e) => {
        if (e && e.data) handleStatusMessage(e.data);
    });
}

/**
 * 初始化连接管理器
 */
export function initConnectionManager() {
    // 防止重复初始化
    if (isInitialized) {
        return;
    }
    isInitialized = true;

    // 检测file://协议
    if (window.location.protocol === 'file:') {
        addToCommandLog('连接管理', 'warning',
            '检测到通过 file:// 协议访问，请通过 HTTP 服务器访问（如 http://localhost:8080）');
        setConnectionState(ConnectionState.DISCONNECTED);
        // 仍然创建UI指示器，但不会进行心跳检测
        createConnectionIndicator();
        return;
    }

    // 监听浏览器在线/离线事件
    window.addEventListener('online', handleOnline);
    window.addEventListener('offline', handleOffline);

    // 监听页面可见性变化（从后台切回时检查连接）
    document.addEventListener('visibilitychange', handleVisibilityChange);

    // 启动心跳检测
    startHeartbeat();

    // 创建连接状态 UI 指示器
    createConnectionIndicator();

    // 订阅 SSE status 事件，使 8080 播控屏能显示点歌(9898)等端口触发的控制提示
    subscribeStatusSSE();

    addToCommandLog('连接管理', 'info', '连接管理器已初始化');
}

/**
 * 添加状态变化回调
 * @returns {Function} 取消订阅函数
 */
export function onConnectionStateChange(callback) {
    stateChangeCallbacks.push(callback);
    // 返回取消订阅函数，防止内存泄漏
    return () => {
        const index = stateChangeCallbacks.indexOf(callback);
        if (index > -1) {
            stateChangeCallbacks.splice(index, 1);
        }
    };
}

/**
 * 获取当前连接状态
 */
export function getConnectionState() {
    return currentState;
}

/**
 * 设置连接状态
 */
function setConnectionState(newState) {
    if (currentState !== newState) {
        const oldState = currentState;
        currentState = newState;

        // 更新 UI
        updateConnectionIndicator(newState);

        // 通知回调
        stateChangeCallbacks.forEach(cb => {
            try {
                cb(newState, oldState);
            } catch (e) {
                console.error('状态变化回调错误:', e);
            }
        });

        // 记录日志
        const stateLabels = {
            [ConnectionState.CONNECTED]: '已连接',
            [ConnectionState.DISCONNECTED]: '已断开',
            [ConnectionState.RECONNECTING]: '重连中',
            [ConnectionState.UNKNOWN]: '未知'
        };
        addToCommandLog('连接状态', newState === ConnectionState.CONNECTED ? 'success' : 'warning',
            stateLabels[newState] || newState);
    }
}

/**
 * 处理浏览器上线事件
 */
function handleOnline() {
    addToCommandLog('网络状态', 'info', '网络已恢复');
    // 立即尝试重连
    attemptReconnect();
}

/**
 * 处理浏览器离线事件
 */
function handleOffline() {
    addToCommandLog('网络状态', 'warning', '网络已断开');
    setConnectionState(ConnectionState.DISCONNECTED);
    stopHeartbeat();
}

/**
 * 处理页面可见性变化
 */
function handleVisibilityChange() {
    if (document.visibilityState === 'visible') {
        // 从后台切回，立即检查连接
        checkConnection();
    }
}

/**
 * 启动心跳检测
 */
function startHeartbeat() {
    stopHeartbeat(); // 先停止之前的

    heartbeatInterval = setInterval(async () => {
        await checkConnection();
    }, CONFIG.heartbeatIntervalMs);

    // 立即执行一次检查
    checkConnection();
}

/**
 * 停止心跳检测
 */
function stopHeartbeat() {
    if (heartbeatInterval) {
        clearInterval(heartbeatInterval);
        heartbeatInterval = null;
    }
}

/**
 * 检查连接状态（心跳）
 */
async function checkConnection() {
    // 如果是file://协议，不进行连接检测
    if (window.location.protocol === 'file:') {
        setConnectionState(ConnectionState.DISCONNECTED);
        return false;
    }

    try {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), CONFIG.heartbeatTimeoutMs);

        // 使用专门的心跳端点，不触发任何业务逻辑
        const base = (typeof window !== 'undefined' && window.apiBaseUrl) ? window.apiBaseUrl : '/api/v1';
        const token = localStorage.getItem('hvideo_auth_token');
        const headers = token ? { Authorization: `Bearer ${token}` } : {};
        const response = await fetch(base + '/heartbeat', {
            method: 'GET',
            headers,
            cache: 'no-store',
            signal: controller.signal
        });

        clearTimeout(timeoutId);

        if (response.ok) {
            heartbeatFailures = 0;
            // 连接正常
            if (currentState !== ConnectionState.CONNECTED) {
                setConnectionState(ConnectionState.CONNECTED);
                reconnectAttempts = 0; // 重置重连计数
                // 连接恢复时触发数据重新加载
                window.dispatchEvent(new CustomEvent('connectionRestored'));
            }
            return true;
        } else {
            throw new Error(`HTTP ${response.status}`);
        }
    } catch (error) {
        // 连接失败（网络错误或超时）
        // 静默处理，避免控制台噪音，连接状态指示器会显示状态
        heartbeatFailures++;
        if (heartbeatFailures >= CONFIG.heartbeatFailureThreshold &&
            currentState !== ConnectionState.DISCONNECTED &&
            currentState !== ConnectionState.RECONNECTING) {
            setConnectionState(ConnectionState.DISCONNECTED);
            attemptReconnect();
        }
        return false;
    }
}

/**
 * 尝试重连
 */
function attemptReconnect() {
    if (reconnectTimeout) {
        return; // 已经在重连中
    }

    if (reconnectAttempts >= CONFIG.maxReconnectAttempts) {
        addToCommandLog('重连失败', 'error', `已达最大重连次数 (${CONFIG.maxReconnectAttempts})`);
        setConnectionState(ConnectionState.DISCONNECTED);
        return;
    }

    setConnectionState(ConnectionState.RECONNECTING);
    reconnectAttempts++;

    // 固定 5 秒重连间隔
    const delay = CONFIG.reconnectDelayMs;

    addToCommandLog('重连', 'info', `第 ${reconnectAttempts} 次重连，${(delay / 1000).toFixed(0)} 秒后...`);

    reconnectTimeout = setTimeout(async () => {
        reconnectTimeout = null;

        const success = await checkConnection();
        // checkConnection() 已经负责在连接恢复时触发 connectionRestored 事件
        // 这里只需要在失败时继续重连
        if (!success && currentState !== ConnectionState.CONNECTED) {
            attemptReconnect();
        }
    }, delay);
}

/**
 * 手动触发重连
 */
function forceReconnect() {
    reconnectAttempts = 0;
    if (reconnectTimeout) {
        clearTimeout(reconnectTimeout);
        reconnectTimeout = null;
    }
    attemptReconnect();
}

/**
 * 创建连接状态指示器 UI（在HTML标题栏中查找并初始化）
 */
function createConnectionIndicator() {
    // 检查是否已存在
    if (document.getElementById('connection-indicator')) {
        return;
    }

    // 查找 HTML 中的 status-text 元素，在其前面插入连接指示器
    const statusText = document.getElementById('status-text');
    if (!statusText) {
        return;
    }

    // 创建连接状态指示器
    const indicator = document.createElement('span');
    indicator.id = 'connection-indicator';
    indicator.className = 'resource-item';
    indicator.style.cursor = 'pointer';
    indicator.innerHTML = `
        <span class="connection-dot" style="width: 8px; height: 8px; border-radius: 50%; background: #888; display: inline-block; margin-right: 4px;"></span>
        <span class="connection-text">检测中</span>
    `;

    // 在 status-text 之前插入连接指示器和分隔符
    const divider = document.createElement('span');
    divider.className = 'status-divider';
    divider.textContent = '|';

    statusText.parentNode.insertBefore(indicator, statusText);
    statusText.parentNode.insertBefore(divider, statusText);

    // 添加样式
    const style = document.createElement('style');
    style.id = 'connection-indicator-style';
    style.textContent = `
        #connection-indicator .connection-dot {
            transition: background 0.3s ease, box-shadow 0.3s ease;
        }
        #connection-indicator.connected .connection-dot {
            background: #4ade80 !important;
            box-shadow: 0 0 6px #4ade80;
        }
        #connection-indicator.disconnected .connection-dot {
            background: #ef4444 !important;
            box-shadow: 0 0 6px #ef4444;
            animation: pulse 1s infinite;
        }
        #connection-indicator.reconnecting .connection-dot {
            background: #facc15 !important;
            box-shadow: 0 0 6px #facc15;
            animation: pulse 0.5s infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        #connection-indicator.connected .connection-text {
            color: #4ade80;
        }
        #connection-indicator.disconnected .connection-text {
            color: #ef4444;
        }
        #connection-indicator.reconnecting .connection-text {
            color: #facc15;
        }
    `;
    document.head.appendChild(style);

    // 点击连接指示器手动重连
    indicator.addEventListener('click', () => {
        if (currentState !== ConnectionState.CONNECTED) {
            forceReconnect();
        }
    });
}

/**
 * 更新连接状态指示器
 */
function updateConnectionIndicator(state) {
    const indicator = document.getElementById('connection-indicator');
    if (!indicator) return;

    indicator.className = '';
    const textEl = indicator.querySelector('.connection-text');

    switch (state) {
        case ConnectionState.CONNECTED:
            indicator.classList.add('connected');
            textEl.textContent = '已连接';
            break;
        case ConnectionState.DISCONNECTED:
            indicator.classList.add('disconnected');
            textEl.textContent = '已断开 (点击重连)';
            break;
        case ConnectionState.RECONNECTING:
            indicator.classList.add('reconnecting');
            textEl.textContent = '重连中...';
            break;
        default:
            textEl.textContent = '检测中...';
    }
}

/**
 * 销毁连接管理器
 */
export function destroyConnectionManager() {
    stopHeartbeat();

    if (reconnectTimeout) {
        clearTimeout(reconnectTimeout);
        reconnectTimeout = null;
    }
    if (sharedSSEReconnectTimer) {
        clearTimeout(sharedSSEReconnectTimer);
        sharedSSEReconnectTimer = null;
    }
    if (statusSseUnsubscribe) {
        statusSseUnsubscribe();
        statusSseUnsubscribe = null;
    }
    if (sharedEventSource) {
        sharedEventSource.close();
        sharedEventSource = null;
    }
    sharedSSEListeners.clear();

    window.removeEventListener('online', handleOnline);
    window.removeEventListener('offline', handleOffline);
    document.removeEventListener('visibilitychange', handleVisibilityChange);

    // 移除连接指示器和相关样式
    const indicator = document.getElementById('connection-indicator');
    if (indicator) {
        // 移除前一个分隔符
        const prevSibling = indicator.previousElementSibling;
        if (prevSibling && prevSibling.className === 'status-divider') {
            prevSibling.remove();
        }
        indicator.remove();
    }

    const style = document.getElementById('connection-indicator-style');
    if (style) {
        style.remove();
    }

    // 重置所有状态
    stateChangeCallbacks = [];
    currentState = ConnectionState.UNKNOWN;
    reconnectAttempts = 0;
    heartbeatFailures = 0;
    isInitialized = false;  // 允许重新初始化
}
