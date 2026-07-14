/**
 * @file script.js（文件名）
 * @brief HVIDEOEngine Web界面主入口文件
 * 
 * 本文件是Web界面的主入口，负责：
 * - 初始化所有核心模块
 * - 加载页面模块和组件模块
 * - 设置全局API接口
 * - 启动应用
 */

// 导入核心模块
import './modules/core/interactionGuards.js';
import { initializeApi, apiGet, apiPost, apiPut, apiDelete, apiAction } from './modules/core/api.js';
import { initializeNavigation, showPage } from './modules/core/navigation.js?v=3.05';
import { initializeEventListeners } from './modules/core/eventListeners.js';
import { initializeCommandLog, addToCommandLog } from './modules/core/commandLog.js';
import { initConnectionManager } from './modules/core/connectionManager.js';
import { refreshAppTitleFromLicense } from './modules/core/appTitle.js';
import { isLoggedIn, logout } from './modules/pages/login.js';

// 导入页面模块
import { initializeSceneTemplates } from './modules/pages/sceneTemplates.js';

import { initializeSettings } from './modules/pages/systemSettings.js?v=2.66';
import { initializePeripheralManagement } from './modules/pages/peripheralManagement.js?v=3.03';
import { initializeImportFolders } from './modules/pages/importFolders.js';
import { syncEffectLicenseVisibility } from './modules/pages/effectControl.js?v=2.99';
import { initializePlaylist } from './modules/features/playlist.js';
import { initializeMaterials } from './modules/features/materials.js';
// 导入组件模块
import { initializeDialogs } from './modules/components/dialogs.js';
import { initializeButtonGroups } from './modules/components/buttonGroups.js';
import { showNotification, showConfirm, showInfo, showWarning } from './modules/components/toast.js';

// 导入特色功能模块
/**
 * @brief 应用初始化函数
 * 
 * 初始化所有模块并启动应用
 */
async function initializeApp() {
    // 检查登录状态
    if (!isLoggedIn()) {
        // 未登录，跳转到登录页面
        window.location.href = '/login.html';
        return;
    }

    // 初始化API请求
    initializeApi();
    refreshAppTitleFromLicense();

    // 初始化连接管理器（断线重连、连接状态指示器）
    initConnectionManager();

    // 初始化页面导航
    initializeNavigation();

    // 根据系统授权隐藏未授权页面入口
    await syncEffectLicenseVisibility();

    // 初始化事件监听器
    initializeEventListeners();

    // 初始化场景模板列表
    initializeSceneTemplates();

    // 初始化系统设置
    initializeSettings();

    // 初始化导入文件夹功能
    initializeImportFolders();

    // 初始化中控配置
    initializePeripheralManagement();

    // 初始化命令日志
    initializeCommandLog();

    // 初始化播放列表
    initializePlaylist();

    // 初始化素材管理
    initializeMaterials();

    // 初始化对话框
    initializeDialogs();

    // 初始化按钮组
    initializeButtonGroups();

    // 将showNotification挂载到全局，供其他模块使用
    window.hsUtils = window.hsUtils || {};
    window.hsUtils.showNotification = showNotification;

    // 将API函数挂载到全局，供fusion.js等IIFE模块使用
    window.apiGet = apiGet;
    window.apiPost = apiPost;
    window.apiPut = apiPut;
    window.apiDelete = apiDelete;
    window.apiAction = apiAction;

    // 初始化重启按钮事件
    initRestartButtons();

    // 添加登出按钮
    initLogoutButton();

    // 监听连接恢复事件，自动刷新页面数据
    window.addEventListener('connectionRestored', async () => {
        await refreshAllPages();
    });

    // 显示系统状态页面
    showPage('system-status');
}

/**
 * @brief 刷新所有页面数据（重连成功后调用）
 */
async function refreshAllPages() {
    try {
        addToCommandLog('页面刷新', 'info', 'HTTP重连成功，开始刷新页面数据');
        await refreshAppTitleFromLicense();

        // 动态导入刷新函数
        const { updateSystemStatus } = await import('./modules/pages/systemStatus.js');
        const { updateSceneTemplates } = await import('./modules/pages/sceneTemplates.js');
        const { updateSettings } = await import('./modules/pages/systemSettings.js?v=2.66');
        const { updateVideoControl } = await import('./modules/pages/videoControl.js?v=2.98');
        const { updateMaterials } = await import('./modules/features/materials.js');
        const { updatePlaylistList } = await import('./modules/features/playlist.js');
        const { initializeEffects, syncEffectLicenseVisibility, isEffectsLicenseDenied } = await import('./modules/pages/effectControl.js?v=2.99');

        // 刷新系统状态
        try {
            await updateSystemStatus();
        } catch (e) {
            console.error('刷新系统状态失败:', e);
        }

        // 刷新图层矩阵（已在 layerMatrix/main.js 中处理，避免重复）
        // updateLayerMatrix() 已在 layerMatrix/main.js 的 connectionRestored 事件中调用

        // 刷新场景模板列表
        try {
            await updateSceneTemplates();
        } catch (e) {
            console.error('刷新场景模板列表失败:', e);
        }

        // 刷新系统设置
        try {
            await updateSettings();
        } catch (e) {
            console.error('刷新系统设置失败:', e);
        }

        // 刷新视频控制
        try {
            await updateVideoControl();
        } catch (e) {
            console.error('刷新视频控制失败:', e);
        }

        // 刷新特效配置（重新加载配置和图层）
        try {
            const effectsEnabled = await syncEffectLicenseVisibility();
            if (effectsEnabled && !isEffectsLicenseDenied()) {
                await initializeEffects();
            }
        } catch (e) {
            console.error('刷新特效配置失败:', e);
        }

        // 刷新素材列表
        try {
            await updateMaterials();
        } catch (e) {
            console.error('刷新素材列表失败:', e);
        }

        // 刷新播放列表
        try {
            await updatePlaylistList();
        } catch (e) {
            console.error('刷新播放列表失败:', e);
        }

        // 注意：中控配置模块的配置加载是内部函数，不需要在这里刷新
        // 用户切换到中控配置页面时会自动加载

        addToCommandLog('页面刷新', 'success', '所有页面数据刷新完成');
        showNotification('页面数据已刷新', 'success');
    } catch (error) {
        console.error('刷新页面数据时出错:', error);
        addToCommandLog('页面刷新', 'error', `刷新页面数据失败: ${error.message}`);
        showNotification('部分数据刷新失败', 'warning');
    }
}

/**
 * @brief 初始化重启按钮事件
 */
function initRestartButtons() {
    const postRestartCommand = (action) => {
        const body = JSON.stringify({});
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 2000);
        const token = localStorage.getItem('hvideo_auth_token');
        const headers = { 'Content-Type': 'application/json' };
        if (token) {
            headers.Authorization = `Bearer ${token}`;
        }

        void fetch(`${window.apiBaseUrl || '/api/v1'}/system/actions/${encodeURIComponent(action)}`, {
            method: 'POST',
            headers,
            body,
            signal: controller.signal,
            keepalive: true
        }).catch(() => {
            // Restart intentionally interrupts the 当前 HTTP service.
        }).finally(() => clearTimeout(timeoutId));
    };

    // 重启软件按钮
    const restartAppBtn = document.getElementById('restart-app-btn');
    if (restartAppBtn) {
        restartAppBtn.addEventListener('click', async () => {
            const confirmed = await showConfirm('确定要重启软件吗？', '重启软件');
            if (confirmed) {
                showInfo('正在重启软件...');
                postRestartCommand('restart_app');
            }
        });
    }

    // 重启系统按钮
    const restartSystemBtn = document.getElementById('restart-system-btn');
    if (restartSystemBtn) {
        restartSystemBtn.addEventListener('click', async () => {
            const confirmed = await showConfirm('确定要重启系统吗？这将重启整个设备！', '重启系统');
            if (confirmed) {
                showWarning('正在重启系统...');
                postRestartCommand('reboot');
            }
        });
    }
}

/**
 * @brief 初始化登出按钮
 */
function initLogoutButton() {
    // 在header-right区域添加登出按钮
    const headerRight = document.querySelector('.header-right');
    if (headerRight) {
        const logoutBtn = document.createElement('button');
        logoutBtn.className = 'header-btn';
        logoutBtn.title = '退出登录';
        logoutBtn.innerHTML = `
            <svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4" />
                <polyline points="16 17 21 12 16 7" />
                <line x1="21" y1="12" x2="9" y2="12" />
            </svg>
            退出登录
        `;
        logoutBtn.addEventListener('click', async () => {
            const confirmed = await showConfirm('确定要退出登录吗？', '退出登录');
            if (confirmed) {
                await logout();
            }
        });
        
        // 直接添加到header-right末尾
        headerRight.appendChild(logoutBtn);
    }
}

// DOM加载完成后初始化应用
document.addEventListener('DOMContentLoaded', function () {
    initializeApp();
});

