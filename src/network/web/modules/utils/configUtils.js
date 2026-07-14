/**
 * @file configUtils.js（文件名）
 * @brief 配置序列化相关工具（如减少浮点精度噪音，便于 config.json 可读）
 */

/** 默认保留的小数位数，避免 0.98 -> 0.9800000190734863 这类浮点噪音 */
const DEFAULT_DECIMALS = 2;

/**
 * 递归遍历对象，将数字类型值四舍五入到指定小数位（不修改原对象，返回新对象）。
 * 用于在保存 config.json 前整理浮点精度，使文件更易读。
 * @param {*} obj - 任意值（对象/数组/原始类型）
 * @param {number} decimals - 保留的小数位数，默认 8
 * @returns {*} 整理后的副本
 */
export function roundNumbersInObject(obj, decimals = DEFAULT_DECIMALS) {
    if (obj === null || typeof obj !== 'object') {
        if (typeof obj === 'number' && Number.isFinite(obj)) {
            const factor = Math.pow(10, decimals);
            return Math.round(obj * factor) / factor;
        }
        return obj;
    }
    if (Array.isArray(obj)) {
        return obj.map((item) => roundNumbersInObject(item, decimals));
    }
    const out = {};
    for (const key of Object.keys(obj)) {
        out[key] = roundNumbersInObject(obj[key], decimals);
    }
    return out;
}
