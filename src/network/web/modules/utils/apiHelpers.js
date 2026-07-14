/**
 * @file apiHelpers.js（文件名）
 * @brief API操作工具函数库
 * 
 * 提供统一的API操作工具函数，简化常见的数据获取和操作模式
 */

import { addToCommandLog } from '../core/commandLog.js';

// 这里原本有一组通用 CRUD 辅助函数（fetchListData / handleCreateOperation / handleUpdateOperation / handleDeleteOperation），
// 源码中已经明确标注“目前无外部使用者”，并且在整个项目中没有任何引用。
// 为减少维护负担和潜在误解，这些函数已经物理删除，仅保留通用的 handleApiOperation。

/**
 * 通用API操作处理（简化版）
 * 用于处理简单的API调用，自动处理成功/失败日志
 * apiPost/apiGet/apiPut/apiDelete 返回：成功时返回数据对象，失败时返回null
 * 
 * @param {Promise} apiCall - API调用Promise（如 apiPost(url, data)）
 * @param {string} actionName - 操作名称（用于日志）
 * @param {string} successMessage - 成功消息
 * @param {string} errorMessage - 失败消息（可选，默认使用"操作失败"）
 * @param {Function} onSuccess - 成功回调 (result) => void
 * @param {Function} onError - 错误回调 (errorMsg) => void
 * @returns {Promise<boolean>} 是否成功
 */
export async function handleApiOperation(apiCall, actionName, successMessage, errorMessage = null, onSuccess = null, onError = null) {
    try {
        const result = await apiCall;
        if (result && typeof result === 'object' &&
            (result.softBlocked === true || result.notice === true || result.state === 'blocked')) {
            const warnMsg = result.message || errorMessage || `${actionName}受限`;
            addToCommandLog(actionName, 'warning', warnMsg);
            return false;
        }
        if (result !== null && result !== undefined) {
            // 成功：result不为null/undefined
            // 只有当successMessage不为null时才自动记录成功消息
            // 如果为null，说明onSuccess回调会自己处理日志
            if (successMessage !== null) {
                addToCommandLog(actionName, 'success', successMessage);
            }
            if (onSuccess) {
                await onSuccess(result);
            }
            return true;
        } else {
            // 失败：result为null或undefined
            const errorMsg = errorMessage || `${actionName}失败`;
            addToCommandLog(actionName, 'error', errorMsg);
            if (onError) {
                await onError(errorMsg);
            }
            return false;
        }
    } catch (error) {
        // 异常情况
        const errorMsg = error.message || (errorMessage || `${actionName}失败`);
        addToCommandLog(actionName, 'error', errorMsg);
        if (onError) {
            await onError(errorMsg);
        }
        return false;
    }
}

