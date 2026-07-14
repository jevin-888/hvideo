// 移动端命令日志模块 - 使用Toast提示

// Toast队列管理
const toastQueue = [];
let isShowingToast = false;
const MAX_QUEUE_SIZE = 5; // 最多保留5条消息
const TOAST_DURATION = 2000; // 每条消息显示2秒
const DEBOUNCE_TIME = 500; // 相同消息500ms内去重

// 消息去重缓存
const recentMessages = new Map();

/**
 * 显示Toast消息（队列模式）
 * @param {string} message - 消息内容
 * @param {string} type - 消息类型: 'success', 'error', 'warning', 'info'
 */
function showToast(message, type = 'info') {
    // 消息去重：相同消息在短时间内只显示一次
    const messageKey = `${type}:${message}`;
    const now = Date.now();
    const lastTime = recentMessages.get(messageKey);
    
    if (lastTime && (now - lastTime) < DEBOUNCE_TIME) {
        return; // 忽略重复消息
    }
    
    recentMessages.set(messageKey, now);
    
    // 清理过期的消息记录
    for (const [key, time] of recentMessages.entries()) {
        if (now - time > DEBOUNCE_TIME) {
            recentMessages.delete(key);
        }
    }
    
    // 添加到队列，如果队列满了则移除最旧的消息
    if (toastQueue.length >= MAX_QUEUE_SIZE) {
        toastQueue.shift();
    }
    
    toastQueue.push({ message, type });
    
    // 如果当前没有显示Toast，则开始显示
    if (!isShowingToast) {
        processToastQueue();
    }
}

/**
 * 处理Toast队列
 */
function processToastQueue() {
    if (toastQueue.length === 0) {
        isShowingToast = false;
        return;
    }
    
    isShowingToast = true;
    const { message, type } = toastQueue.shift();
    
    const container = document.getElementById('toast-container');
    if (!container) {
        isShowingToast = false;
        return;
    }
    
    // 创建Toast元素
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.textContent = message;
    
    container.appendChild(toast);
    
    // 显示动画
    setTimeout(() => {
        toast.style.animation = 'toastSlideIn 0.3s ease forwards';
    }, 10);
    
    // 自动移除并显示下一条
    setTimeout(() => {
        toast.style.animation = 'toastSlideOut 0.3s ease forwards';
        setTimeout(() => {
            if (toast.parentNode) {
                toast.parentNode.removeChild(toast);
            }
            // 继续处理队列中的下一条消息
            processToastQueue();
        }, 300);
    }, TOAST_DURATION);
}

/**
 * 添加日志条目（移动端使用Toast显示）
 * @param {string} action - 操作名称
 * @param {string} type - 日志类型: 'success', 'error', 'warning', 'info'
 * @param {string} message - 日志消息
 */
export function addToCommandLog(action, type, message) {
    // 移动端使用Toast显示重要消息
    if (type === 'error' || type === 'success' || type === 'warning') {
        showToast(`${action}: ${message}`, type);
    }
    // info类型静默处理，避免过多提示
}
