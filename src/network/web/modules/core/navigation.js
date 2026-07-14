/**
 * @file navigation.js（文件名）
 * @brief 页面导航模块
 * 
 * 本模块提供页面导航功能，包括：
 * - 页面切换和显示
 * - 菜单项激活状态管理
 * - 页面内容自动更新
 */

// 导入页面模块更新函数
import { initializeStatusUpdate, cleanupStatusUpdate } from '../pages/systemStatus.js';
import { initializeLayerMatrix, updateLayerMatrix } from '../pages/layerMatrix.js';
import { updateSceneTemplates } from '../pages/sceneTemplates.js';
import { initializeEffects, isEffectsLicenseDenied, stopAudioReactiveRefresh, syncEffectLicenseVisibility } from '../pages/effectControl.js?v=2.99';
import { updateSettings } from '../pages/systemSettings.js?v=2.66';
import { initializeVideoControl, updateVideoControl } from '../pages/videoControl.js?v=2.98';
import { updateMaterials } from '../features/materials.js';
import { updatePlaylistList } from '../features/playlist.js';
import { initializeFusionControl, refreshFusionControl } from '../pages/fusionControl.js?v=2.100';
import { initializeProjectionCorrection, refreshProjectionCorrection } from '../pages/projectionCorrection.js?v=1.5';

/** 当前活动页面标识 */
let currentPage = 'system-status';
let layerMatrixInitialized = false;
let videoControlInitialized = false;
let fusionControlInitialized = false;
let projectionCorrectionInitialized = false;

/**
 * @brief 初始化页面导航
 * 
 * 为所有菜单项添加点击事件监听器
 */
export function initializeNavigation() {
    const menuItems = document.querySelectorAll('.menu-item');
    menuItems.forEach(item => {
        item.addEventListener('click', function () {
            const page = this.dataset.page;
            showPage(page);
        });
    });
}

/**
 * @brief 显示指定页面
 * @param {string} page 页面标识符
 */
export async function showPage(page) {
    if (page === 'effect-control') {
        const effectsEnabled = await syncEffectLicenseVisibility();
        if (!effectsEnabled || isEffectsLicenseDenied()) {
            page = 'system-status';
        }
    }

    // 如果切换到非效果控制页面，停止音频反应指标刷新
    if (currentPage === 'effect-control' && page !== 'effect-control') {
        stopAudioReactiveRefresh();
    }

    // 如果切换到非系统状态页面，停止系统状态更新
    if (currentPage === 'system-status' && page !== 'system-status') {
        cleanupStatusUpdate();
    }

    // 更新当前页面
    currentPage = page;

    // 移除所有菜单和页面的active类
    document.querySelectorAll('.menu-item').forEach(item => {
        item.classList.remove('active');
    });
    document.querySelectorAll('.page').forEach(pageElement => {
        pageElement.classList.remove('active');
    });

    // 添加active类到当前菜单和页面
    const menuItem = document.querySelector(`[data-page="${page}"]`);
    const pageElement = document.getElementById(`${page}-page`);
    if (menuItem) {
        menuItem.classList.add('active');
    }
    if (pageElement) {
        pageElement.classList.add('active');
    }

    // 更新页面内容（特效页需等待初始化完成，确保配置、图层列表、启用开关等组件就绪）
    switch (page) {
        case 'system-status':
            initializeStatusUpdate();
            break;
        case 'layer-matrix':
            if (!layerMatrixInitialized) {
                layerMatrixInitialized = true;
                await initializeLayerMatrix();
            } else {
                updateLayerMatrix();
            }
            break;
        case 'scene-template':
            updateSceneTemplates();
            break;
        case 'effect-control':
            await initializeEffects();
            break;
        case 'fusion':
            if (!fusionControlInitialized) {
                document.querySelector(`[data-page="${page}"]`)?.classList.add('active');
                document.getElementById(`${page}-page`)?.classList.add('active');
                await initializeFusionControl();
                fusionControlInitialized = true;
            } else {
                refreshFusionControl();
            }
            document.querySelector(`[data-page="${page}"]`)?.classList.add('active');
            document.getElementById(`${page}-page`)?.classList.add('active');
            break;
        case 'projection-correction':
            if (!projectionCorrectionInitialized) {
                projectionCorrectionInitialized = true;
                await initializeProjectionCorrection();
            } else {
                await refreshProjectionCorrection();
            }
            document.querySelector(`[data-page="${page}"]`)?.classList.add('active');
            document.getElementById(`${page}-page`)?.classList.add('active');
            break;
        case 'system-settings':
            updateSettings();
            break;
        case 'video-control':
            if (!videoControlInitialized) {
                videoControlInitialized = true;
                initializeVideoControl();
            } else {
                updateVideoControl();
            }
            updateMaterials();
            updatePlaylistList();
            break;
    }
}

// 获取当前页面
export function getCurrentPage() {
    return currentPage;
}

