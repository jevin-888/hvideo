/**
 * @file script.js（文件名）
 * @brief 移动端主入口文件
 */

import './modules/core/interactionGuards.js';
import * as videoControl from './modules/pages/mobileVideoControl.js?v=117';
import * as controlPage from './modules/pages/mobileControl.js?v=117';
import * as scenePage from './modules/pages/mobileScene.js?v=117';

// 当前页面
let currentPage = 'video';

/**
 * 切换页面
 */
function switchPage(page) {
    // 隐藏所有页面
    document.querySelectorAll('.page-content').forEach(el => {
        el.classList.add('hidden');
    });

    // 显示目标页面
    const targetPage = document.getElementById(`page-${page}`);
    if (targetPage) {
        targetPage.classList.remove('hidden');
    }

    // 更新导航状态
    document.querySelectorAll('.nav-cell').forEach(item => {
        if (item.dataset.page === page) {
            item.classList.add('active');
        } else {
            item.classList.remove('active');
        }
    });

    currentPage = page;

    // 根据页面初始化
    if (page === 'video') {
        // 滚动到页面顶部，避免滚动位置导致的空白问题
        const pageVideo = document.getElementById('page-video');
        if (pageVideo) {
            pageVideo.scrollTop = 0;
        }
        window.scrollTo(0, 0);

        videoControl.loadVideos();
        // 切换到视频页面时同步播放状态
        setTimeout(() => {
            if (typeof videoControl.syncSimpleStatus === 'function') {
                videoControl.syncSimpleStatus();
            }
        }, 200);
    } else if (page === 'control') {
        window.scrollTo(0, 0);
        controlPage.initializeControlPage();
    } else if (page === 'scene') {
        window.scrollTo(0, 0);
        // 场景页面：只在首次切换时加载，避免重复加载
        if (typeof scenePage.initializeScenePage === 'function') {
            scenePage.initializeScenePage();
        } else if (typeof scenePage.loadScenes === 'function') {
            scenePage.loadScenes();
        }
    }
}

/**
 * 检查图片加载
 */
function checkImageLoad() {
    const heroImg = document.querySelector('.hero-main-img');
    if (heroImg) {
        heroImg.addEventListener('error', function () {
            // 图片加载失败时，显示渐变背景
            const overlay = document.querySelector('.hero-text-overlay');
            if (overlay) {
                overlay.style.background = 'linear-gradient(135deg, #1a3a6a 0%, #2b579a 100%)';
            }
        });
    }
}

/**
 * 初始化应用
 */
async function initializeApp() {
    // 检查图片加载
    checkImageLoad();

    // 先加载图层列表，获取正确的 currentLayerId（基于播放列表的 targetLayerId）
    await videoControl.loadLayers();

    // 初始化视频页面事件监听器（此时 currentLayerId 已正确设置）
    videoControl.initializeEventListeners();

    // 加载视频列表
    await videoControl.loadVideos();

    // 如果配置了默认播放列表，且当前图层没有播放/暂停内容，初始化时自动播放。
    await videoControl.playDefaultPlaylistIfIdle();

    // 初始化缩略图SSE
    videoControl.initializeThumbnailSSE();

    // 视频列表加载完成后，立即同步状态以显示完整的视频名称
    // 使用多个延迟确保DOM和视频列表都已准备好
    if (typeof videoControl.syncSimpleStatus === 'function') {
        // 立即同步一次
        videoControl.syncSimpleStatus();

        // 延迟同步，确保视频列表已完全加载
        setTimeout(() => {
            videoControl.syncSimpleStatus();
        }, 200);

        // 再次延迟同步，确保所有数据都已准备好
        setTimeout(() => {
            videoControl.syncSimpleStatus();
        }, 800);
    }

    // 加载系统音量
    await videoControl.loadSystemVolume();

    // 默认选择defaultVideo分类
    videoControl.selectCategory('defaultVideo');

    // 初始化中控页面
    controlPage.initializeControlPage();

    // 设置底部导航事件
    document.querySelectorAll('.nav-cell').forEach(item => {
        item.addEventListener('click', function () {
            const page = this.dataset.page;
            switchPage(page);
        });
    });

    // 默认显示视频页面
    switchPage('video');

    // 页面切换后再次同步状态
    setTimeout(() => {
        if (typeof videoControl.syncSimpleStatus === 'function') {
            videoControl.syncSimpleStatus();
        }
    }, 300);
}

/**
 * 页面加载完成后初始化
 */
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initializeApp);
} else {
    initializeApp();
}

/**
 * 处理页面可见性变化（切换标签页时）
 */
document.addEventListener('visibilitychange', () => {
    if (!document.hidden) {
        // 页面重新可见时刷新数据并同步状态
        if (currentPage === 'video') {
            videoControl.loadVideos();
            // 同步播放状态
            if (typeof videoControl.syncSimpleStatus === 'function') {
                setTimeout(() => {
                    videoControl.syncSimpleStatus();
                }, 500);
            }
        }
        // 场景页面：不自动刷新，保持当前选中状态
        // 用户可以手动刷新页面来更新场景列表
    }
});

/**
 * 处理在线/离线状态
 */
window.addEventListener('online', () => {
    if (currentPage === 'video') {
        videoControl.loadVideos();
    }
    // 场景页面：不自动刷新
});

window.addEventListener('offline', () => {
    // 离线处理
});
