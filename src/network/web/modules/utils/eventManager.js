/**
 * 事件管理器 - 统一管理事件监听器，避免重复绑定和内存泄漏
 */
class EventManager {
    constructor() {
        this.listeners = new Map(); // 存储所有事件监听器
    }

    /**
     * 添加事件监听器（自动去重）
     * @param {HTMLElement} element - 目标元素
     * @param {string} event - 事件类型
     * @param {Function} handler - 事件处理函数
     * @param {string} key - 唯一标识（用于去重和清理）
     */
    add(element, event, handler, key) {
        if (!element || !event || !handler) {
            return;
        }

        const listenerKey = key || `${element.id || 'unknown'}_${event}_${Date.now()}`;
        
        // 如果已存在，先移除
        if (this.listeners.has(listenerKey)) {
            this.remove(listenerKey);
        }
        
        element.addEventListener(event, handler);
        this.listeners.set(listenerKey, { element, event, handler, key: listenerKey });
        
        return listenerKey;
    }

    /**
     * 移除事件监听器
     * @param {string} key - 唯一标识
     */
    remove(key) {
        const listener = this.listeners.get(key);
        if (listener) {
            listener.element.removeEventListener(listener.event, listener.handler);
            this.listeners.delete(key);
            return true;
        }
        return false;
    }

    /**
     * 清理所有事件监听器
     */
    clear() {
        for (const [key, listener] of this.listeners.entries()) {
            listener.element.removeEventListener(listener.event, listener.handler);
        }
        this.listeners.clear();
    }

    /**
     * 清理指定元素的所有事件监听器
     * @param {HTMLElement} element - 目标元素
     */
    clearElement(element) {
        const toRemove = [];
        for (const [key, listener] of this.listeners.entries()) {
            if (listener.element === element) {
                listener.element.removeEventListener(listener.event, listener.handler);
                toRemove.push(key);
            }
        }
        toRemove.forEach(key => this.listeners.delete(key));
        return toRemove.length;
    }

    /**
     * 清理指定事件类型的所有监听器
     * @param {string} event - 事件类型
     */
    clearEvent(event) {
        const toRemove = [];
        for (const [key, listener] of this.listeners.entries()) {
            if (listener.event === event) {
                listener.element.removeEventListener(listener.event, listener.handler);
                toRemove.push(key);
            }
        }
        toRemove.forEach(key => this.listeners.delete(key));
        return toRemove.length;
    }

    /**
     * 获取所有监听器的数量
     * @return 示例/字段：{number}
     */
    size() {
        return this.listeners.size;
    }
}

// 导出单例
export const eventManager = new EventManager();

// 页面卸载时清理所有事件
if (typeof window !== 'undefined') {
    window.addEventListener('beforeunload', () => {
        eventManager.clear();
    });
}

