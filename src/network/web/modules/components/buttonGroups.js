/**
 * 按钮组管理模块
 * 提供统一的按钮组点击处理和状态管理功能
 */
import { setAudioChannel, switchAudioTrack } from '../pages/videoControl.js?v=2.98';
import { eventManager } from '../utils/eventManager.js';

/**
 * 通用按钮组点击处理函数
 * @param {HTMLElement} groupElement - 按钮组容器元素
 * @param {Function} callback - 点击按钮时的回调函数，接收被点击的按钮元素作为参数
 * @param {string} key - 事件监听器的唯一标识（用于去重）
 */
function handleButtonGroupClick(groupElement, callback, key) {
    if (!groupElement) return;

    const handler = function (e) {
        const btn = e.target.closest('.btn');
        if (btn && groupElement.contains(btn)) {
            // 更新选中状态：移除所有按钮的激活状态，然后激活当前按钮
            groupElement.querySelectorAll('.btn').forEach(item => item.classList.remove('active'));
            btn.classList.add('active');

            // 执行回调函数
            if (callback) {
                callback(btn);
            }
        }
    };

    // 使用事件管理器添加事件，避免重复绑定
    const listenerKey = key || `buttonGroup_${groupElement.id || 'unknown'}`;
    eventManager.add(groupElement, 'click', handler, listenerKey);
}

/**
 * 更新按钮组选中状态
 * 根据指定的值激活对应的按钮
 * @param {string} groupId - 按钮组容器元素的ID
 * @param {string} value - 要激活的按钮的data-value值
 */
export function updateButtonGroupState(groupId, value) {
    const group = document.getElementById(groupId);
    if (!group) return;

    // 移除所有按钮的激活状态
    group.querySelectorAll('.btn').forEach(btn => btn.classList.remove('active'));

    // 激活指定值的按钮
    const activeBtn = group.querySelector(`[data-value="${value}"]`);
    if (activeBtn) {
        activeBtn.classList.add('active');
    }
}

/**
 * 根据图层类型控制播放列表循环模式中的图片专用选项
 * 示例/字段：@param {string} groupId
 * 示例/字段：@param {boolean} visible
 */
export function setImageOnlyLoopOptionVisible(groupId, visible) {
    const group = document.getElementById(groupId);
    if (!group) return;

    group.querySelectorAll('.image-only-loop-option').forEach(btn => {
        btn.style.display = visible ? '' : 'none';
    });

    if (!visible) {
        const activeImageBtn = group.querySelector('.image-only-loop-option.btn.active');
        if (activeImageBtn) {
            activeImageBtn.classList.remove('active');
        }
        const hiddenActive = group.querySelector('.btn.active');
        if (!hiddenActive) {
            const fallback = group.querySelector('[data-value="0"]');
            if (fallback) fallback.classList.add('active');
        }
    }
}

/**
 * 获取按钮组选中值
 * @param {string} groupId - 按钮组容器元素的ID
 * @returns {string|null} 返回当前激活按钮的data-value值，如果没有激活按钮则返回null
 */
export function getButtonGroupValue(groupId) {
    const group = document.getElementById(groupId);
    if (!group) return null;

    const activeBtn = group.querySelector('.btn.active');
    if (!activeBtn) return null;

    return activeBtn.dataset.value;
}

/**
 * 切换按钮组激活状态（用于简单的切换场景）
 * @param {string} selector - 按钮选择器（如 '.type-btn'）
 * @param {HTMLElement} activeElement - 要激活的元素
 */
export function toggleButtonGroupActive(selector, activeElement) {
    if (!activeElement) return;
    document.querySelectorAll(selector).forEach(btn => btn.classList.remove('active'));
    activeElement.classList.add('active');
}

/**
 * 初始化所有按钮组的事件监听器
 * 包括循环模式、音轨选择、声道设置等按钮组
 */
export function initializeButtonGroups() {
    // 新建播放列表循环模式按钮组
    handleButtonGroupClick(document.getElementById('new-playlist-loop-mode'), null, 'new-playlist-loop-mode-group');

    // 播放列表设置循环模式按钮组
    handleButtonGroupClick(document.getElementById('playlist-loop-mode'), null, 'playlist-loop-mode-group');

    // 音轨选择按钮组
    handleButtonGroupClick(
        document.getElementById('audio-track-select'),
        (target) => {
            const currentTrack = parseInt(target.dataset.value || '0');
            const nextTrack = currentTrack === 0 ? 1 : 0;
            switchAudioTrack(nextTrack);
        },
        'audio-track-select-group'
    );

    // 声道设置按钮组
    handleButtonGroupClick(
        document.getElementById('audio-channel-select'),
        (target) => {
            const channel = target.dataset.value;
            setAudioChannel(channel);
        },
        'audio-channel-select-group'
    );
}
