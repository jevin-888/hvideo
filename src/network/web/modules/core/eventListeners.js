// 事件监听器模块
import { getCurrentPage, showPage } from './navigation.js';

// 初始化事件监听器
export function initializeEventListeners() {
    // 工具栏按钮事件
    const refreshBtn = document.getElementById('refresh-btn');
    if (refreshBtn) {
        refreshBtn.addEventListener('click', function () {
            refreshCurrentPage();
        });
    }

    const fullscreenBtn = document.getElementById('fullscreen-btn');
    if (fullscreenBtn) {
        fullscreenBtn.addEventListener('click', function () {
            toggleFullscreen();
        });
    }

    // 注意：视频控制按钮事件（播放/暂停、重播、下一曲、静音等）
    // 统一在 videoControl.js 的 initializeVideoControl() 中绑定
    // 这里不再重复绑定，避免事件触发两次

    // 声道控制事件 - 通过按钮组处理，在 buttonGroups.js 中处理




    // 屏幕旋转实时更新（热旋转）
    const screenRotateInput = document.getElementById('setting-screen-rotate');
    if (screenRotateInput) {
        let rotateTimeout = null;
        const inputHandler = function () {
            const rotateValue = parseInt(this.value) || 0;

            // 更新状态显示
            const infoRotate = document.getElementById('info-screen-rotate');
            if (infoRotate) {
                infoRotate.textContent = `${rotateValue}°`;
            }

            // 防抖：延迟300ms后调用API，避免频繁请求
            // 注意：屏幕旋转功能已移至其他模块或暂时禁用
            if (rotateTimeout) {
                clearTimeout(rotateTimeout);
            }
            rotateTimeout = setTimeout(async () => {
                // 屏幕旋转功能暂时禁用
            }, 300);
        };
        screenRotateInput.addEventListener('input', inputHandler);

        // 在页面卸载时清理定时器
        window.addEventListener('beforeunload', () => {
            if (rotateTimeout) {
                clearTimeout(rotateTimeout);
                rotateTimeout = null;
            }
        });
    }

    // 音量滑块事件和音量加减按钮事件 - 统一在 videoControl.js 的 initializeVideoControl 中处理
    // 这里不再重复绑定，避免事件触发两次

    // 进度滑块事件 - 在 videoControl.js 的 initializeVideoControl 中处理

}

// 刷新当前页面
function refreshCurrentPage() {
    const currentPage = getCurrentPage();
    showPage(currentPage);
}

// 切换全屏
function toggleFullscreen() {
    if (!document.fullscreenElement) {
        document.documentElement.requestFullscreen().catch(() => {
            // 全屏切换失败，静默处理
        });
    } else {
        if (document.exitFullscreen) {
            document.exitFullscreen();
        }
    }
}