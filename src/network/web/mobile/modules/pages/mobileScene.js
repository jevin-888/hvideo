/**
 * @file mobileScene.js（文件名）
 * @brief 移动端场景模块
 */

import { apiGet, apiPost, apiAction } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { selectLayer, setCurrentPlaylistId, syncSimpleStatus } from './mobileVideoControl.js?v=115';

// 当前选中的场景
let currentScene = null;
// 是否正在切换场景（防止重复点击）
let isSwitching = false;
// 是否已经初始化事件委托
let eventDelegationInitialized = false;

/**
 * 加载场景列表
 */
export async function loadScenes() {
    const container = document.getElementById('scene-buttons');
    if (!container) return;

    container.innerHTML = '<div class="scene-loading">加载中...</div>';

    try {
        const scenes = await apiGet('/scenes');

        if (!scenes) {
            container.innerHTML = '<div class="scene-loading">加载失败：无法获取场景列表</div>';
            addToCommandLog('加载场景', 'error', 'API返回null');
            return;
        }

        if (!Array.isArray(scenes)) {
            container.innerHTML = '<div class="scene-loading">加载失败：场景列表格式错误</div>';
            addToCommandLog('加载场景', 'error', '场景列表不是数组格式');
            return;
        }

        if (scenes.length === 0) {
            container.innerHTML = '<div class="scene-loading">暂无场景</div>';
            return;
        }

        container.innerHTML = '';

        // 保存当前选中的场景名称，用于恢复选中状态
        const previousScene = currentScene;

        // 渲染场景按钮（2列网格布局）
        scenes.forEach((scene, index) => {
            if (!scene || typeof scene !== 'string') return;

            const btn = document.createElement('button');
            btn.className = 'scene-btn';
            btn.textContent = scene;
            btn.dataset.scene = scene;

            // 恢复之前的选中状态（只恢复UI状态，不设置currentScene）
            if (previousScene === scene) {
                btn.classList.add('active');
            }

            container.appendChild(btn);
        });

        // 如果之前的场景在列表中，恢复currentScene
        if (previousScene && scenes.includes(previousScene)) {
            currentScene = previousScene;
        } else {
            // 如果之前的场景不在列表中，清除选中状态
            currentScene = null;
        }

        // 初始化事件委托（只初始化一次）
        if (!eventDelegationInitialized) {
            let lastClickTime = 0;

            container.addEventListener('click', function (e) {
                const btn = e.target.closest('.scene-btn');
                if (!btn) return;

                e.preventDefault();
                e.stopPropagation();

                const now = Date.now();
                if (now - lastClickTime < 500) return;
                lastClickTime = now;

                const sceneName = btn.dataset.scene;
                if (sceneName) selectScene(sceneName);
            });

            eventDelegationInitialized = true;
        }
    } catch (error) {
        container.innerHTML = '<div class="scene-loading">加载失败</div>';
        const errorMsg = error.message || error.toString() || '未知错误';
        addToCommandLog('加载场景', 'error', errorMsg);
    }
}

/**
 * 选择场景
 * [V5.8l] 使用与电脑端相同的 API 格式 (POST /command) 确保行为一致
 */
export async function selectScene(sceneName) {
    if (!sceneName) {
        addToCommandLog('场景切换', 'error', '场景名称为空');
        return;
    }

    if (isSwitching) return;

    isSwitching = true;

    const allButtons = document.querySelectorAll('.scene-btn');
    const targetBtn = Array.from(allButtons).find(btn => btn.dataset.scene === sceneName);

    if (!targetBtn) {
        isSwitching = false;
        return;
    }

    // 更新UI状态 - 立即更新，给用户即时反馈
    allButtons.forEach(btn => {
        if (btn.dataset.scene === sceneName) {
            btn.classList.add('active');
        } else {
            btn.classList.remove('active');
        }
    });

    const previousScene = currentScene;
    currentScene = sceneName;

    try {
        const result = await apiAction('scenes', 'switch_scene', { scene_name: sceneName });

        if (result !== null) {
            addToCommandLog('场景切换', 'success', `已切换到场景: ${sceneName}`);
        } else {
            if (previousScene) {
                currentScene = previousScene;
                const buttons = document.querySelectorAll('.scene-btn');
                buttons.forEach(btn => {
                    if (btn.dataset.scene === previousScene) {
                        btn.classList.add('active');
                    } else {
                        btn.classList.remove('active');
                    }
                });
                addToCommandLog('场景切换', 'warning', `场景 ${sceneName} 加载失败，已恢复`);
            } else {
                addToCommandLog('场景切换', 'warning', `场景 ${sceneName} 加载失败`);
            }
        }
    } catch (error) {
        if (previousScene) {
            currentScene = previousScene;
            const buttons = document.querySelectorAll('.scene-btn');
            buttons.forEach(btn => {
                if (btn.dataset.scene === previousScene) {
                    btn.classList.add('active');
                } else {
                    btn.classList.remove('active');
                }
            });
        }
        addToCommandLog('场景切换', 'error', `切换场景失败: ${error.message || '未知错误'}`);
    } finally {
        isSwitching = false;
    }
}

/**
 * 获取当前场景
 */
function getCurrentScene() {
    return currentScene;
}

// 当前选中的播放列表
let currentPlaylist = null;
let playlistEventInitialized = false;
let playlistsCache = [];
let isPlaylistSwitching = false;

function findPlaylistById(playlistId) {
    return playlistsCache.find(p => String(p.id) === String(playlistId)) || null;
}

function getPlaylistName(playlist, playlistId) {
    return playlist?.name || playlistId;
}

function getFileNameFromPath(path) {
    if (!path) return '';
    const normalized = String(path).replace(/\\/g, '/');
    const fileName = normalized.split('/').filter(Boolean).pop() || '';
    const dotIndex = fileName.lastIndexOf('.');
    return dotIndex > 0 ? fileName.slice(0, dotIndex) : fileName;
}

function setPlaylistButtonsActive(playlistId) {
    document.querySelectorAll('.playlist-btn').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.playlistId === playlistId);
    });
}

/**
 * 加载播放列表
 */
export async function loadPlaylists() {
    const container = document.getElementById('playlist-buttons');
    if (!container) return;

    container.innerHTML = '<div class="scene-loading">加载中...</div>';

    try {
        const playlists = await apiGet('/playlists');

        if (!playlists || !Array.isArray(playlists)) {
            playlistsCache = [];
            container.innerHTML = '<div class="scene-loading">暂无播放列表</div>';
            return;
        }

        if (playlists.length === 0) {
            playlistsCache = [];
            container.innerHTML = '<div class="scene-loading">暂无播放列表</div>';
            return;
        }

        playlistsCache = playlists;
        container.innerHTML = '';

        // 渲染播放列表按钮
        playlists.forEach(playlist => {
            const btn = document.createElement('button');
            btn.className = 'scene-btn playlist-btn';
            btn.textContent = playlist.name || playlist.id;
            btn.dataset.playlistId = playlist.id;

            // 显示视频数量
            if (playlist.count > 0) {
                btn.textContent += ` (${playlist.count})`;
            }

            // 标记默认播放列表
            if (playlist.isDefault) {
                btn.classList.add('active');
            }

            container.appendChild(btn);
        });

        // 初始化事件委托
        if (!playlistEventInitialized) {
            let lastClickTime = 0;

            container.addEventListener('click', function (e) {
                const btn = e.target.closest('.playlist-btn');
                if (!btn) return;

                e.preventDefault();
                e.stopPropagation();

                const now = Date.now();
                if (now - lastClickTime < 500) return;
                lastClickTime = now;

                const playlistId = btn.dataset.playlistId;
                if (playlistId) {
                    selectPlaylist(playlistId);
                }
            });

            playlistEventInitialized = true;
        }
    } catch (error) {
        container.innerHTML = '<div class="scene-loading">加载失败</div>';
    }
}

/**
 * 选择播放列表
 */
async function selectPlaylist(playlistId) {
    if (!playlistId) return;
    if (isPlaylistSwitching) return;

    const playlist = findPlaylistById(playlistId);
    const playlistName = getPlaylistName(playlist, playlistId);
    const targetLayerId = Number(playlist?.targetLayerId) || 1;

    setPlaylistButtonsActive(playlistId);
    currentPlaylist = playlistId;

    // 与桌面版视频控制页一致：选择列表后，后续控制使用该列表绑定的图层。
    setCurrentPlaylistId(playlistId);
    selectLayer(targetLayerId);

    isPlaylistSwitching = true;
    try {
        const result = await apiPost('/video/play', {
            playlistId: playlistId,
            layerId: targetLayerId
        });

        if (result && typeof result === 'object' &&
            (result.state === 'blocked' || result.softBlocked || result.notice)) {
            const blockedMsg = result.message || '当前设备不能同时播放两个4K视频';
            const statusText = document.getElementById('playback-status-text');
            if (statusText) statusText.textContent = `播放受限：${blockedMsg}`;
            addToCommandLog('播放播放列表', 'warning', blockedMsg);
            return;
        }

        if (result !== null) {
            const statusText = document.getElementById('playback-status-text');
            const displayName = getFileNameFromPath(result?.uri || result?.path);
            if (statusText) {
                statusText.textContent = displayName
                    ? `正在播放：${displayName}`
                    : `正在播放：${playlistName}`;
            }
            addToCommandLog('播放播放列表', 'success', `成功播放播放列表: ${playlistName}`);
            setTimeout(syncSimpleStatus, 300);
        } else {
            addToCommandLog('播放播放列表', 'error', `播放播放列表失败: ${playlistName}`);
        }
    } catch (error) {
        addToCommandLog('播放播放列表', 'error', error.message || `播放播放列表失败: ${playlistName}`);
    } finally {
        isPlaylistSwitching = false;
    }
}

// 标签切换是否已初始化
let sceneTabsInitialized = false;

/**
 * 初始化场景页面的标签切换
 */
function initSceneTabs() {
    if (sceneTabsInitialized) return;

    const pageScene = document.getElementById('page-scene');
    if (!pageScene) return;

    const tabs = pageScene.querySelectorAll('.control-tab');
    const panels = pageScene.querySelectorAll('.control-panel');

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const targetTab = tab.dataset.tab;

            // 更新标签状态
            tabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');

            // 更新面板显示
            panels.forEach(panel => {
                if (panel.id === `${targetTab}-panel`) {
                    panel.classList.add('active');
                } else {
                    panel.classList.remove('active');
                }
            });
        });
    });

    sceneTabsInitialized = true;
}

// 是否已经加载过数据
let dataLoaded = false;

/**
 * 初始化场景页面
 */
export function initializeScenePage() {
    initSceneTabs();

    if (!dataLoaded) {
        const pageScene = document.getElementById('page-scene');
        if (pageScene) {
            const tabs = pageScene.querySelectorAll('.control-tab');
            tabs.forEach(tab => {
                if (tab.dataset.tab === 'scene') {
                    tab.classList.add('active');
                } else {
                    tab.classList.remove('active');
                }
            });

            const scenePanel = document.getElementById('scene-panel');
            const playlistPanel = document.getElementById('playlist-panel');
            if (scenePanel) scenePanel.classList.add('active');
            if (playlistPanel) playlistPanel.classList.remove('active');
        }

        loadScenes();
        loadPlaylists();
        dataLoaded = true;
    }
}
