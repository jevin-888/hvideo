/**
 * @file domHelpers.js（文件名）
 * @brief DOM操作工具函数库
 * 
 * 提供统一的DOM操作工具函数，避免重复代码
 */

/**
 * 安全清空容器（避免使用innerHTML导致的问题）
 * @param {HTMLElement} container - 要清空的容器元素
 */
export function clearContainer(container) {
    if (!container) return;
    while (container.firstChild) {
        container.removeChild(container.firstChild);
    }
}

/**
 * 防抖函数
 * 在指定延迟时间内，如果函数被多次调用，只执行最后一次调用
 * @param {Function} func - 要防抖的函数
 * @param {number} delay - 延迟时间（毫秒）
 * @returns {Function} 防抖后的函数
 */
export function debounce(func, delay) {
    let timeoutId = null;
    return function (...args) {
        clearTimeout(timeoutId);
        timeoutId = setTimeout(() => {
            func.apply(this, args);
        }, delay);
    };
}

