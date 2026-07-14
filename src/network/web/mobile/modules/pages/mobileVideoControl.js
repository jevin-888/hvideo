/**
 * @file mobileVideoControl.js（文件名）
 * @brief 移动端控制核心 - 与电脑端功能保持一致
 */

import { apiGet, apiPost, apiPut, apiAction, apiActionOrThrow } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';

async function postVideoCommand(action, param = {}) {
    const videoEndpointActionMap = {
        lockPlayback: 'lock',
        muteToggle: 'mute/toggle',
        unlockPlayback: 'unlock',
        volumeDown: 'volume/down',
        volumeUp: 'volume/up',
        getPlaybackLock: 'lock/status',
        setVolume: 'volume',
        getSystemVolume: 'getSystemVolume',
        setSystemVolume: 'systemVolume'
    };
    const videoAction = videoEndpointActionMap[action] || action;
    return await apiPost(`/video/${videoAction}`, param);
}

/**
 * 将文件转换为base64字符串
 * @param {File} file - 图片文件
 * @returns {Promise<string>} 返回base64编码的图片数据
 */
function fileToBase64(file) {
    return new Promise((resolve, reject) => {
        if (!file) {
            reject(new Error('文件对象为空'));
            return;
        }

        const reader = new FileReader();

        reader.onload = () => {
            try {
                if (!reader.result) {
                    reject(new Error('文件读取结果为空'));
                    return;
                }
                // reader.result 是 data:image/png;base64,xxxxx 格式
                // 直接返回完整字符串，后端会处理
                resolve(reader.result);
            } catch (err) {
                reject(new Error('处理文件读取结果失败: ' + err.message));
            }
        };

        reader.onerror = (error) => {
            console.error('[fileToBase64] 文件读取错误:', error);
            reject(new Error('文件读取失败: ' + (error.message || '未知错误')));
        };

        reader.onabort = () => {
            console.error('[fileToBase64] 文件读取被中止');
            reject(new Error('文件读取被中止'));
        };

        try {
            reader.readAsDataURL(file);
        } catch (err) {
            reject(new Error('启动文件读取失败: ' + err.message));
        }
    });
}

// 基础状态
let currentLayerId = 1;
let isPlaying = false;
let isMuted = false;
let videoList = [];
let imageUploadListenerAttached = false; // 标志：图片上传事件监听器是否已绑定
let mediaTypeListenerAttached = false;

// 当前选中的播放列表ID
let currentPlaylistId = null;

// 媒体来源：本地素材库(local) 或 U 盘(usb)
let currentSource = 'local';

// U盘播放模式：sequence | random | loop | single
let usbPlayMode = 'sequence';

// U盘数据缓存
let usbDataCache = {
    files: null,           // 缓存的文件列表
    folderMap: null,       // 缓存的文件夹映射
    meta: null,            // 扫描元信息
    timestamp: 0,          // 缓存时间戳
    isValid: false         // 缓存是否有效
};

const USB_CACHE_DURATION = 5 * 60 * 1000; // 缓存有效期：5分钟
const USB_SCAN_LIMIT = 800;
const USB_RENDER_PAGE_SIZE = 80;
const USB_MAX_FOLDERS_IN_NAV = 80;
const USB_SCAN_TIMEOUT_MS = 15000;
const USB_CREATE_TIMEOUT_MS = 20000;

/**
 * 获取当前播放列表ID
 */
export function getCurrentPlaylistId() {
    return currentPlaylistId;
}

/**
 * 设置当前播放列表ID
 */
export function setCurrentPlaylistId(playlistId) {
    currentPlaylistId = playlistId;
}

/**
 * 获取播放列表绑定的目标图层
 */
async function getPlaylistTargetLayer(playlistId) {
    if (!playlistId) return 1;
    try {
        const playlists = await apiGet('/playlists');
        if (playlists && Array.isArray(playlists)) {
            const playlist = playlists.find(p => p.id === playlistId);
            if (playlist && playlist.targetLayerId) {
                return playlist.targetLayerId;
            }
        }
    } catch (e) {
        console.error('获取播放列表目标图层失败:', e);
    }
    return 1;
}

/**
 * 提取路径中的分类文件夹
 */
function getCatFolderName(path) {
    if (!path) return 'defaultVideo';
    const p = path.replace(/\\/g, '/');
    const segs = p.split('/').filter(s => s.length > 0);
    if (segs.length <= 1) return 'defaultVideo';
    const folder = segs[segs.length - 2];
    if (['media', 'materials', 'video', 'huoshan'].includes(folder.toLowerCase())) return 'defaultVideo';
    return folder;
}

/**
 * 规范化路径用于比较
 */
function normalizePath(path) {
    if (!path) return '';
    return path.replace(/\\/g, '/').replace(/\/+/g, '/').replace(/\/$/, '').toLowerCase();
}

/**
 * 从路径中提取文件名（不含后缀）
 */
function getFileNameFromPath(path) {
    if (!path) return '未知视频';
    const normalized = path.replace(/\\/g, '/');
    const parts = normalized.split('/').filter(s => s.length > 0);
    if (parts.length === 0) return '未知视频';

    let fileName = parts[parts.length - 1];
    // 移除文件后缀名
    const dotIndex = fileName.lastIndexOf('.');
    if (dotIndex > 0) {
        fileName = fileName.substring(0, dotIndex);
    }
    return fileName;
}

function isPlaybackActive(data) {
    if (!data || typeof data !== 'object') return false;
    const state = data.state || data.status;
    return state === 'playing' || state === 'paused';
}

function getImageFileParamFromPath(path) {
    if (!path) return '';
    const normalized = path.replace(/\\/g, '/');
    const imageDir = '/Image/';
    const imageDirIdx = normalized.indexOf(imageDir);
    if (imageDirIdx !== -1) {
        return normalized.substring(imageDirIdx + imageDir.length);
    }
    const parts = normalized.split('/').filter(Boolean);
    return parts.length > 0 ? parts[parts.length - 1] : '';
}

function selectCurrentLayerStatus(payload = {}) {
    const statusMap = payload.video_status || payload.videoStatus;
    if (!statusMap) return payload;
    return statusMap[currentLayerId] || statusMap[String(currentLayerId)] || {};
}

/**
 * 只同步状态，彻底删除所有进度/时间相关的 polling
 */
async function syncSimpleStatus() {
    try {
        const data = await apiGet(`/video/status?layerId=${currentLayerId}`);
        const statusText = document.getElementById('playback-status-text');
        if (!statusText) return;

        if (data && typeof data === 'object') {
            // 检查播放状态（API返回的是state字段）
            const playing = (data.state === 'playing' || data.status === 'playing');
            if (data.state !== undefined || data.status !== undefined) {
                isPlaying = playing;
                updatePlayPauseButton(isPlaying);
            }

            // 同步静音状态（检查 muted 或 isMuted 字段）
            if (data.muted !== undefined || data.isMuted !== undefined) {
                const serverMuted = data.muted === true || data.isMuted === true;
                if (serverMuted !== isMuted) {
                    isMuted = serverMuted;
                    updateMuteButton(isMuted);
                }
            }

            // 更新播放标题 - 优先使用path，如果没有path但有其他信息也尝试显示
            let displayName = '';

            if (data.path) {
                // 规范化API返回的路径
                const apiPath = normalizePath(data.path);

                // 如果视频列表已加载，尝试匹配
                if (videoList && videoList.length > 0) {
                    // 尝试从视频列表中查找对应的视频名称（支持多种路径格式匹配）
                    let video = videoList.find(v => {
                        if (!v || !v.path) return false;
                        const videoPath = normalizePath(v.path);
                        return videoPath === apiPath ||
                            videoPath.endsWith(apiPath) ||
                            apiPath.endsWith(videoPath);
                    });

                    // 如果还是找不到，尝试通过文件名匹配
                    if (!video && apiPath) {
                        const apiFileName = getFileNameFromPath(data.path);
                        video = videoList.find(v => {
                            if (!v || !v.path) return false;
                            const videoFileName = getFileNameFromPath(v.path);
                            return videoFileName === apiFileName;
                        });
                    }

                    displayName = video ? (video.name || getFileNameFromPath(data.path)) : getFileNameFromPath(data.path);
                } else {
                    // 视频列表未加载，直接使用文件名
                    displayName = getFileNameFromPath(data.path);
                }
            } else if (playing && videoList && videoList.length > 0) {
                // 如果没有path但正在播放，尝试从当前播放的视频卡片获取
                const activeCard = document.querySelector('.video-card.active');
                if (activeCard) {
                    const cardPath = activeCard.dataset.path;
                    if (cardPath) {
                        const video = videoList.find(v => normalizePath(v.path) === normalizePath(cardPath));
                        if (video) {
                            displayName = video.name || getFileNameFromPath(cardPath);
                        }
                    }
                }
            }

            // 更新显示
            if (displayName) {
                statusText.textContent = `正在播放：${displayName}`;
            } else if (data.state === 'stopped' || (!playing && data.state === 'paused')) {
                statusText.textContent = '正在播放：';
            } else if (playing) {
                // 正在播放但没有名称，显示默认文本
                statusText.textContent = '正在播放：';
            }
        } else if (data === null || data === undefined) {
            // API返回null，可能是没有视频在播放
            statusText.textContent = '正在播放：';
        }
    } catch (e) {
        // 状态同步失败时静默处理
    }
}

// SSE连接对象
let videoStatusEventSource = null;

/**
 * 初始化视频状态SSE监听
 */
function initializeVideoStatusSSE() {
    // 如果已有连接，先关闭
    if (videoStatusEventSource) {
        videoStatusEventSource.close();
        videoStatusEventSource = null;
    }

    try {
        videoStatusEventSource = new EventSource('/api/v1/events');

        videoStatusEventSource.addEventListener('video_status', (event) => {
            try {
                const data = selectCurrentLayerStatus(JSON.parse(event.data));
                // 更新播放状态
                if (data.state) {
                    const playing = data.state === 'playing';
                    isPlaying = playing;
                    updatePlayPauseButton(isPlaying);
                }

                // 更新静音状态
                if (data.isMuted !== undefined) {
                    isMuted = data.isMuted;
                    updateMuteButton(isMuted);
                }

                // 更新播放标题
                const statusText = document.getElementById('playback-status-text');
                if (statusText && data.path) {
                    let displayName = getFileNameFromPath(data.path);
                    
                    // 尝试从视频列表中查找完整名称
                    if (videoList && videoList.length > 0) {
                        const apiPath = normalizePath(data.path);
                        const video = videoList.find(v => {
                            if (!v || !v.path) return false;
                            const videoPath = normalizePath(v.path);
                            return videoPath === apiPath || 
                                   videoPath.endsWith(apiPath) || 
                                   apiPath.endsWith(videoPath);
                        });
                        if (video) {
                            displayName = video.name || displayName;
                        }
                    }

                    if (data.state === 'playing') {
                        statusText.textContent = `正在播放：${displayName}`;
                    } else if (data.state === 'paused') {
                        statusText.textContent = `已暂停：${displayName}`;
                    } else if (data.state === 'stopped') {
                        statusText.textContent = '正在播放：';
                    }
                }

                // 更新视频卡片的激活状态
                if (data.path) {
                    const apiPath = normalizePath(data.path);
                    document.querySelectorAll('.video-card').forEach(card => {
                        const cardPath = normalizePath(card.dataset.path || '');
                        card.classList.toggle('active', cardPath === apiPath);
                    });
                }
            } catch (err) {
                console.error('[SSE] 解析视频状态失败:', err);
            }
        });

        videoStatusEventSource.addEventListener('error', (err) => {
            console.error('[SSE] 连接错误:', err);
            // 连接断开后会自动重连，无需手动处理
        });

    } catch (err) {
        console.error('[SSE] 初始化失败:', err);
    }
}

// 页面卸载时关闭SSE连接
window.addEventListener('beforeunload', () => {
    if (videoStatusEventSource) {
        videoStatusEventSource.close();
    }
});

// 导出函数供外部调用
export { syncSimpleStatus };

function getFilteredVideos(videos) {
    const source = Array.isArray(videos) ? videos : [];
    // 根据媒体类型筛选
    const videoExtensions = ['.mp4', '.mkv', '.avi', '.mov', '.wmv', '.flv', '.webm', '.m4v', '.mpg', '.mpeg', '.ts', '.vob', '.dat'];
    const imageExtensions = ['.jpg', '.jpeg', '.png', '.gif', '.bmp', '.webp', '.svg'];
    const audioExtensions = ['.mp3', '.wav', '.flac', '.aac', '.ogg', '.m4a', '.wma'];

    return source.filter(v => {
        if (!v || !v.path) return false;
        const dotIndex = v.path.lastIndexOf('.');
        const ext = dotIndex >= 0 ? v.path.toLowerCase().substring(dotIndex) : '';

        // U盘模式下不进行媒体类型筛选，显示所有扫描到的文件
        let typeMatch = true;
        if (currentSource !== 'usb') {
            // 本地模式才进行媒体类型筛选
            switch (currentMediaType) {
                case 'video':
                    typeMatch = videoExtensions.includes(ext);
                    break;
                case 'image':
                    typeMatch = imageExtensions.includes(ext);
                    break;
                case 'audio':
                    typeMatch = audioExtensions.includes(ext);
                    break;
            }
        }

        // 分类筛选
        let catMatch = true;
        if (currentCategory) {
            catMatch = getCatFolderName(v.path) === currentCategory;
        }

        return typeMatch && catMatch;
    });
}

function createVideoCard(v) {
    // 根据后缀判断是否为音频，音频不再请求缩略图接口，避免产生 404 日志
    const videoExtensions = ['.mp4', '.mkv', '.avi', '.mov', '.wmv', '.flv', '.webm', '.m4v', '.mpg', '.mpeg', '.ts', '.vob', '.dat'];
    const imageExtensions = ['.jpg', '.jpeg', '.png', '.gif', '.bmp', '.webp', '.svg'];
    const audioExtensions = ['.mp3', '.wav', '.flac', '.aac', '.ogg', '.m4a', '.wma'];
    const ext = v.path.toLowerCase().substring(v.path.lastIndexOf('.'));
    const isAudio = audioExtensions.includes(ext);

    const card = document.createElement('div');
    card.className = 'video-card';
    card.dataset.path = v.path;

    const thumbnailHtml = isAudio
        ? `
            <div class="audio-thumbnail-placeholder">
                <svg viewBox="0 0 24 24" width="32" height="32" fill="currentColor" opacity="0.8">
                    <path d="M12 3v10.55c-.59-.34-1.27-.55-2-.55-2.21 0-4 1.79-4 4s1.79 4 4 4 4-1.79 4-4V7h4V3h-6z"/>
                </svg>
            </div>
        `
        : `<img src="/api/v1/materials/thumbnail?path=${encodeURIComponent(v.path)}" alt="" onerror="this.style.display='none';">`;

    card.innerHTML = `
        <div class="preview-box">
            ${thumbnailHtml}
            <div class="play-icon-overlay">
                <svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z" fill="white"/></svg>
            </div>
        </div>
        <div class="video-info">
            <div class="video-title" title="${(v.name || '未知视频').replace(/"/g, '&quot;')}">${v.name || '未知视频'}</div>
            <div class="video-meta">
                <span class="video-size">${((v.size || 0) / (1024 * 1024)).toFixed(1)} MB</span>
                <span class="video-tag">${v.type || 'defaultVideo'}</span>
            </div>
        </div>`;
    card.addEventListener('click', async () => {
        // 判断媒体类型
        const ext = v.path.toLowerCase().substring(v.path.lastIndexOf('.'));
        const isImage = imageExtensions.includes(ext);

        if (isImage) {
            // 图片类型：使用图层60，发送 loadImage 命令
            const layerId = 60; // 图片专用图层
            try {
                const param = {
                    action: 'loadImage',
                    layerId: layerId
                };
                if (currentSource === 'usb') {
                    param.path = v.path;
                } else {
                    param.image_file = getImageFileParamFromPath(v.path);
                }
                const { action, ...imageParams } = param;
                const res = await apiAction('rendering', action, imageParams);
                if (res !== null) {
                    const statusText = document.getElementById('playback-status-text');
                    if (statusText) {
                        const displayName = v.name || getFileNameFromPath(v.path);
                        statusText.textContent = `已加载图片：${displayName}`;
                    }
                    // 设置当前卡片为选中状态
                    document.querySelectorAll('.video-card').forEach(c => c.classList.remove('active'));
                    card.classList.add('active');
                }
            } catch (err) {
                console.error('加载图片失败:', err);
            }
        } else {
            // 视频/音频类型：使用 /video/play 接口
            // 获取目标图层：优先使用播放列表绑定的图层，否则使用默认图层
            let targetLayerId = currentLayerId;
            if (currentPlaylistId) {
                targetLayerId = await getPlaylistTargetLayer(currentPlaylistId);
            }

            // 构建播放参数（与电脑端一致）
            const playParams = {
                layerId: targetLayerId,
                path: v.path,
                loop: 2,  // 预览/直播放模式：单个循环
                playbackRate: 1.0  // 播放速率
            };

            // 如果有播放列表，添加 播放列表Id
            if (currentPlaylistId) {
                playParams.playlistId = currentPlaylistId;
            }

            try {
                const res = await apiPost('/video/play', playParams);
                if (res == null) {
                    const errorMsg = '播放失败';
                    const statusText = document.getElementById('playback-status-text');
                    if (statusText) {
                        statusText.textContent = `播放失败：${errorMsg}`;
                    }
                    addToCommandLog('播放视频', 'error', `播放失败: ${errorMsg}`);
                    return;
                }
                if (res && typeof res === 'object' && (res.state === 'blocked' || res.softBlocked || res.notice)) {
                    const blockedMsg = res.message || '当前设备不能同时播放两个4K视频';
                    const statusText = document.getElementById('playback-status-text');
                    if (statusText) {
                        statusText.textContent = `播放受限：${blockedMsg}`;
                    }
                    addToCommandLog('播放视频', 'warning', blockedMsg);
                    return;
                }
                if (res !== null) {
                    isPlaying = true;

                    updatePlayPauseButton(true);
                    const statusText = document.getElementById('playback-status-text');
                    if (statusText) {
                        const displayName = v.name || getFileNameFromPath(v.path);
                        statusText.textContent = `正在播放：${displayName}`;
                    }
                    document.querySelectorAll('.video-card').forEach(c => c.classList.toggle('active', c.dataset.path === v.path));
                    setTimeout(syncSimpleStatus, 300);
                }
            } catch (err) {
                console.error('播放视频失败:', err);
            }
        }
    });
    return card;
}

function renderVideoGrid(videos) {
    const grid = document.getElementById('video-grid');
    if (!grid) return;
    grid.innerHTML = '';

    const filtered = getFilteredVideos(videos);

    if (filtered.length === 0) {
        const typeNames = { video: '视频', image: '图片', audio: '音频' };
        grid.innerHTML = `<div class="grid-status-tip">暂无${typeNames[currentMediaType] || ''}素材</div>`;
        return;
    }

    if (currentSource !== 'usb') {
        filtered.forEach(v => grid.appendChild(createVideoCard(v)));
        return;
    }

    let rendered = 0;
    const renderMore = () => {
        const next = filtered.slice(rendered, rendered + USB_RENDER_PAGE_SIZE);
        next.forEach(v => grid.appendChild(createVideoCard(v)));
        rendered += next.length;
        if (rendered < filtered.length) {
            const moreBtn = document.createElement('button');
            moreBtn.className = 'usb-load-more-btn';
            moreBtn.textContent = `加载更多 (${rendered}/${filtered.length})`;
            moreBtn.addEventListener('click', () => {
                moreBtn.remove();
                renderMore();
            });
            grid.appendChild(moreBtn);
        }
    };
    renderMore();
}

// 当前选中的分类
let currentCategory = '';

/**
 * 生成分类导航
 */
function renderCategoryNav(videos) {
    const navContainer = document.getElementById('category-nav');
    if (!navContainer) return;

    // U 盘模式下不展示原有分类行
    if (currentSource === 'usb') {
        navContainer.innerHTML = '';
        navContainer.style.display = 'none';
        return;
    } else {
        navContainer.style.display = '';
    }

    // 获取所有分类
    const categories = new Set();
    videos.forEach(v => {
        const cat = getCatFolderName(v.path);
        if (cat) {
            categories.add(cat);
        }
    });

    navContainer.innerHTML = '';

    // 添加"全部"按钮
    const allBtn = document.createElement('button');
    allBtn.className = 'cat-tab' + (currentCategory === '' ? ' active' : '');
    allBtn.dataset.category = '';
    allBtn.textContent = '全部';
    allBtn.addEventListener('click', () => selectCategory(''));
    navContainer.appendChild(allBtn);

    // 添加各分类按钮
    categories.forEach(cat => {
        const btn = document.createElement('button');
        btn.className = 'cat-tab' + (currentCategory === cat ? ' active' : '');
        btn.dataset.category = cat;
        btn.textContent = cat;
        btn.addEventListener('click', () => selectCategory(cat));
        navContainer.appendChild(btn);
    });
}

/**
 * 选择分类
 */
export function selectCategory(cat) {
    currentCategory = cat;
    document.querySelectorAll('.cat-tab').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.category === cat);
    });
    renderVideoGrid(videoList);
}

/**
 * 加载素材（根据当前选择的媒体类型）
 */
export async function loadVideos() {
    const grid = document.getElementById('video-grid');
    if (!grid) return;

    // 显示加载状态
    grid.innerHTML = '<div class="grid-status-tip">加载中...</div>';

    // 根据当前媒体类型调用对应的 API
    const apiType = currentMediaType === 'image' ? 'image' :
        currentMediaType === 'audio' ? 'audio' : 'video';

    try {
        const res = await apiGet(`/materials?type=${apiType}`);

        if (res === null) {
            // API请求失败（网络错误、超时等）
            grid.innerHTML = '<div class="grid-status-tip">加载失败：无法连接到服务器<br>请检查网络连接</div>';
            console.error('[loadVideos] API请求返回null，可能原因：网络错误、服务器未响应或API路径错误');
            return;
        }

        if (!Array.isArray(res)) {
            // 返回数据格式错误
            grid.innerHTML = '<div class="grid-status-tip">加载失败：数据格式错误</div>';
            console.error('[loadVideos] API返回的数据不是数组:', res);
            return;
        }

        // 数据加载成功
        videoList = res;
        renderCategoryNav(videoList);
        renderVideoGrid(videoList);

        // 列表加载后，立即同步一次状态以更新显示
        setTimeout(() => {
            syncSimpleStatus();
        }, 100);
    } catch (error) {
        // 捕获异常
        grid.innerHTML = '<div class="grid-status-tip">加载失败：' + (error.message || '未知错误') + '</div>';
        console.error('[loadVideos] 加载素材时发生异常:', error);
    }
}

/**
 * 清除U盘缓存
 */
function clearUsbCache() {
    usbDataCache = {
        files: null,
        folderMap: null,
        meta: null,
        timestamp: 0,
        isValid: false
    };
}

/**
 * 检查U盘缓存是否有效
 */
function isUsbCacheValid() {
    if (!usbDataCache.isValid || !usbDataCache.files) {
        return false;
    }
    const now = Date.now();
    const elapsed = now - usbDataCache.timestamp;
    return elapsed < USB_CACHE_DURATION;
}

function normalizeUsbScanResult(result) {
    let files = [];
    let meta = null;

    if (Array.isArray(result)) {
        files = result;
    } else if (result && typeof result === 'object') {
        if (Array.isArray(result.files)) {
            files = result.files;
            meta = result.meta || null;
        } else if (result.data && Array.isArray(result.data.files)) {
            files = result.data.files;
            meta = result.data.meta || null;
        }
    }

    const seen = new Set();
    const normalizedFiles = [];
    for (const file of files) {
        if (!file || !file.path || seen.has(file.path)) continue;
        seen.add(file.path);
        normalizedFiles.push({
            path: file.path,
            name: file.name || getFileNameFromPath(file.path),
            size: Number(file.size) || 0,
            type: file.type || ''
        });
        if (normalizedFiles.length >= USB_SCAN_LIMIT) break;
    }

    return { files: normalizedFiles, meta };
}

function buildUsbFolderMap(files) {
    const folderMap = {};
    files.forEach(file => {
        if (!file || !file.path) return;
        const pathParts = file.path.split('/').filter(Boolean);
        pathParts.pop();
        const folderPath = '/' + pathParts.join('/');
        const folderName = pathParts[pathParts.length - 1] || '根目录';

        if (!folderMap[folderPath]) {
            folderMap[folderPath] = {
                name: folderName,
                path: folderPath,
                files: []
            };
        }
        folderMap[folderPath].files.push(file);
    });
    return folderMap;
}

function buildUsbScanMessage(files, meta) {
    const count = files.length;
    if (!meta || !meta.truncated) {
        return `已扫描到 ${count} 个 U 盘媒体文件`;
    }

    const reasonText = {
        max_files: '数量较多，已按上限截断',
        max_visited_files: '文件过多，已停止深度扫描',
        max_visited_dirs: '目录过多，已停止深度扫描',
        timeout: '扫描耗时较长，已提前结束'
    }[meta.truncated_reason] || '已提前结束扫描';
    return `${reasonText}，当前显示 ${count} 个`;
}

/**
 * 加载 U 盘媒体（视频）
 * 仅负责扫描和渲染，不直接控制播放列表逻辑
 * @param {boolean} forceRefresh - 是否强制刷新，忽略缓存
 */
async function loadUsbMedia(forceRefresh = false) {
    const grid = document.getElementById('video-grid');
    if (!grid) {
        console.error('[loadUsbMedia] 未找到 video-grid 元素');
        return [];
    }

    // 进入 U 盘模式时，无论扫描是否成功，都不再显示原来的分类行
    const navContainer = document.getElementById('category-nav');
    if (navContainer) {
        navContainer.innerHTML = '';
        navContainer.style.display = 'none';
    }

    // 检查缓存
    if (!forceRefresh && isUsbCacheValid()) {
        // 使用缓存数据
        allUsbFiles = usbDataCache.files;
        videoList = allUsbFiles;
        currentCategory = '';
        currentSource = 'usb';
        
        // 渲染文件夹导航
        if (usbDataCache.folderMap) {
            renderUsbFolderNav(usbDataCache.folderMap);
        }
        
        // 渲染所有文件
        renderVideoGrid(allUsbFiles);
        
        // 同步状态
        setTimeout(() => {
            syncSimpleStatus();
        }, 100);
        
        return allUsbFiles;
    }

    // 需要重新扫描
    grid.innerHTML = '<div class="grid-status-tip">正在扫描 U 盘...</div>';

    let allFiles = [];
    let scanMeta = null;
    
    try {
        const result = await apiGet(
            `/filesystem/usb/media?type=all&limit=${USB_SCAN_LIMIT}&timeoutMs=9000`,
            USB_SCAN_TIMEOUT_MS
        );
        const normalized = normalizeUsbScanResult(result);
        allFiles = normalized.files;
        scanMeta = normalized.meta;
        
        // 保存所有文件到全局变量
        allUsbFiles = allFiles;
        
        // 按文件夹分组（使用所有文件）
        const folderMap = buildUsbFolderMap(allFiles);

        // 更新缓存
        usbDataCache = {
            files: allFiles,
            folderMap: folderMap,
            meta: scanMeta,
            timestamp: Date.now(),
            isValid: true
        };
        // 渲染文件夹导航（显示所有文件夹）
        renderUsbFolderNav(folderMap);
        
    } catch (error) {
        console.error('[loadUsbMedia] 扫描所有媒体类型失败:', error);
        // 扫描失败时清除缓存
        clearUsbCache();
    }

    // 使用所有扫描到的文件作为显示列表
    videoList = allUsbFiles;
    currentCategory = '';
    currentSource = 'usb';
    
    if (allUsbFiles.length === 0) {
        grid.innerHTML = '<div class="grid-status-tip">未检测到 U 盘媒体文件</div>';
        console.warn('[loadUsbMedia] U盘中没有找到媒体文件');
        return [];
    }
    
    // 渲染所有文件
    renderVideoGrid(allUsbFiles);
    if (scanMeta && scanMeta.truncated) {
        const tip = document.createElement('div');
        tip.className = 'usb-scan-warning';
        tip.textContent = buildUsbScanMessage(allUsbFiles, scanMeta);
        grid.prepend(tip);
    }

    // 列表加载后，同步一次状态
    setTimeout(() => {
        syncSimpleStatus();
    }, 100);

    return allUsbFiles;
}

/**
 * 根据当前来源显示/隐藏 U 盘工具栏
 */
function updateUsbToolbarVisibility() {
    const toolbar = document.getElementById('usb-toolbar');
    if (!toolbar) return;
    if (currentSource === 'usb') {
        toolbar.classList.remove('hidden');
    } else {
        toolbar.classList.add('hidden');
        closeUsbModeMenu();
    }
}

/**
 * 渲染U盘文件夹导航
 */
let usbFolderData = {};
let currentUsbFolder = null;
let allUsbFiles = []; // 保存所有扫描到的文件（不分类型）
let usbToolbarInitialized = false;

const USB_MODE_LABELS = {
    sequence: '顺序播放',
    random: '随机播放',
    loop: '循环播放',
    single: '单曲循环'
};

function closeUsbModeMenu() {
    const trigger = document.getElementById('usb-mode-trigger');
    const menu = document.getElementById('usb-mode-menu');
    if (menu) menu.hidden = true;
    if (trigger) trigger.setAttribute('aria-expanded', 'false');
}

function setUsbPlayMode(mode) {
    if (!USB_MODE_LABELS[mode]) return;
    usbPlayMode = mode;

    const label = document.getElementById('usb-mode-label');
    if (label) label.textContent = USB_MODE_LABELS[mode];

    document.querySelectorAll('.usb-mode-option').forEach(option => {
        const active = option.dataset.mode === mode;
        option.classList.toggle('active', active);
        option.setAttribute('aria-checked', active ? 'true' : 'false');
    });
}

function renderUsbFolderNav(folderMap) {
    usbFolderData = folderMap;
    const navContainer = document.getElementById('usb-folder-nav');
    if (!navContainer) {
        console.error('[renderUsbFolderNav] 未找到 usb-folder-nav 元素');
        return;
    }

    navContainer.innerHTML = '';
    
    const allFolders = Object.values(folderMap)
        .sort((a, b) => (b.files?.length || 0) - (a.files?.length || 0));
    const folders = allFolders.slice(0, USB_MAX_FOLDERS_IN_NAV);
    // 如果没有文件夹，不显示导航
    if (folders.length === 0) {
        return;
    }

    // 如果有多个文件夹，添加"全部"按钮
    if (folders.length > 1) {
        const allBtn = document.createElement('div');
        allBtn.className = 'usb-folder-item active';
        allBtn.textContent = '全部';
        allBtn.dataset.folder = '';
        allBtn.addEventListener('click', () => filterByUsbFolder(''));
        navContainer.appendChild(allBtn);

        // 添加分隔符
        const sep1 = document.createElement('span');
        sep1.className = 'usb-folder-separator';
        sep1.textContent = '|';
        navContainer.appendChild(sep1);
    }

    // 添加文件夹按钮
    folders.forEach((folder, index) => {
        const btn = document.createElement('div');
        btn.className = 'usb-folder-item';
        // 如果只有一个文件夹，默认激活
        if (folders.length === 1) {
            btn.classList.add('active');
        }
        btn.textContent = folder.name;
        btn.dataset.folder = folder.path;
        btn.addEventListener('click', () => filterByUsbFolder(folder.path));
        navContainer.appendChild(btn);

        // 添加分隔符（最后一个不加）
        if (index < folders.length - 1) {
            const sep = document.createElement('span');
            sep.className = 'usb-folder-separator';
            sep.textContent = '|';
            navContainer.appendChild(sep);
        }
    });

    if (allFolders.length > folders.length) {
        const more = document.createElement('span');
        more.className = 'usb-folder-more';
        more.textContent = `+${allFolders.length - folders.length}`;
        navContainer.appendChild(more);
    }
}

/**
 * 按文件夹过滤U盘文件
 */
function filterByUsbFolder(folderPath) {
    currentUsbFolder = folderPath;
    
    // 更新按钮状态
    const navContainer = document.getElementById('usb-folder-nav');
    if (navContainer) {
        navContainer.querySelectorAll('.usb-folder-item').forEach(btn => {
            if (btn.dataset.folder === folderPath) {
                btn.classList.add('active');
            } else {
                btn.classList.remove('active');
            }
        });
    }

    // 按文件夹过滤所有文件
    let filteredList;
    if (!folderPath) {
        // 显示全部文件夹
        filteredList = allUsbFiles;
    } else {
        // 显示指定文件夹的文件
        filteredList = allUsbFiles.filter(file => file.path.startsWith(folderPath + '/'));
    }

    // 渲染过滤后的列表
    renderVideoGrid(filteredList);
}

// 缓存按钮元素，避免重复查询
const buttons = {
    playPause: null,
    volumeUp: null,
    volumeDown: null,
    mute: null,
    replay: null
};

// 请求状态管理，防止重复请求
let isRequesting = false;

function initButtons() {
    buttons.playPause = document.getElementById('play-pause-btn');
    buttons.volumeUp = document.getElementById('volume-up-btn');
    buttons.volumeDown = document.getElementById('volume-down-btn');
    buttons.mute = document.getElementById('mute-btn');
    buttons.replay = document.getElementById('replay-btn');
}

function updatePlayPauseButton(playing) {
    if (!buttons.playPause) return;
    buttons.playPause.dataset.state = playing ? 'playing' : 'paused';
    // 使用镂空圆环效果，让中间图标更清晰
    if (playing) {
        // 暂停图标：镂空圆环 + 两条竖线
        buttons.playPause.innerHTML = `<svg viewBox="0 0 24 24">
            <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2"/>
            <rect x="9" y="8" width="2" height="8" fill="currentColor"/>
            <rect x="13" y="8" width="2" height="8" fill="currentColor"/>
        </svg>`;
    } else {
        // 播放图标：镂空圆环 + 三角形
        buttons.playPause.innerHTML = `<svg viewBox="0 0 24 24">
            <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2"/>
            <path d="M10 8l6 4-6 4V8z" fill="currentColor"/>
        </svg>`;
    }

    // 更新标签文字
    const label = document.getElementById('play-pause-label');
    if (label) {
        label.textContent = playing ? '暂停' : '播放';
    }
}

function updateMuteButton(muted) {
    if (!buttons.mute) return;
    buttons.mute.dataset.muted = muted ? 'true' : 'false';
    if (muted) {
        buttons.mute.style.color = '#ff0000';
        // 静音时：喇叭图标 + 右侧小X
        buttons.mute.innerHTML = `<svg viewBox="0 0 24 24">
            <path d="M11 7l-4 3H5v4h2l4 3V7z" fill="currentColor" />
            <path d="M16 9.5l4 5" stroke="#ff0000" stroke-width="2" stroke-linecap="round"/>
            <path d="M20 9.5l-4 5" stroke="#ff0000" stroke-width="2" stroke-linecap="round"/>
        </svg>`;
    } else {
        buttons.mute.style.color = '';
        // 非静音时：喇叭图标 + 声波
        buttons.mute.innerHTML = `<svg viewBox="0 0 24 24">
            <path d="M11 7l-4 3H5v4h2l4 3V7z" fill="currentColor" />
            <path d="M15 9c1 1 1 5 0 6" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" fill="none"/>
            <path d="M18 7c2 2 2 8 0 10" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" fill="none"/>
        </svg>`;
    }


    // 更新标签文字
    const label = document.getElementById('mute-label');
    if (label) {
        label.textContent = muted ? '取消静音' : '静音';
    }
}

export function initializeEventListeners() {
    initButtons();
    initUsbToolbar();

    updatePlayPauseButton(isPlaying);
    updateMuteButton(isMuted);

    // 初始化SSE监听（替代轮询）
    initializeVideoStatusSSE();

    // 使用 cloneNode 替换所有按钮，从根本上移除旧的事件监听器
    function replaceButton(btn) {
        if (!btn) return null;
        const newBtn = btn.cloneNode(true);
        btn.parentNode?.replaceChild(newBtn, btn);
        return newBtn;
    }

    buttons.playPause = replaceButton(buttons.playPause);
    buttons.volumeUp = replaceButton(buttons.volumeUp);
    buttons.volumeDown = replaceButton(buttons.volumeDown);
    buttons.mute = replaceButton(buttons.mute);
    buttons.replay = replaceButton(buttons.replay);

    buttons.playPause?.addEventListener('click', async () => {
        if (isRequesting) return;
        isRequesting = true;
        try {
            const buttonState = buttons.playPause?.dataset.state || (isPlaying ? 'playing' : 'paused');
            const action = buttonState === 'playing' ? 'pause' : 'resume';
            const res = await postVideoCommand(action, { layerId: currentLayerId });
            if (res !== null) {
                if (res && (res.state === 'playing' || res.status === 'playing')) {
                    isPlaying = true;
                } else if (res && (res.state === 'paused' || res.status === 'paused')) {
                    isPlaying = false;
                } else {
                    isPlaying = action === 'resume';
                }
                updatePlayPauseButton(isPlaying);
                setTimeout(() => {
                    syncSimpleStatus();
                }, 200);
            }
        } catch (error) {
            // 静默处理错误，避免重复的错误日志
            console.error('播放/暂停操作失败:', error);
        } finally {
            isRequesting = false;
        }
    });

    buttons.volumeUp?.addEventListener('click', async () => {
        try {
            await postVideoCommand('volumeUp', { layerId: currentLayerId });
        } catch (error) {
            console.error('音量增加失败:', error);
        }
    });

    buttons.volumeDown?.addEventListener('click', async () => {
        try {
            await postVideoCommand('volumeDown', { layerId: currentLayerId });
        } catch (error) {
            console.error('音量减少失败:', error);
        }
    });

    buttons.mute?.addEventListener('click', async () => {
        try {
            const res = await postVideoCommand('muteToggle', { layerId: currentLayerId });
            if (res !== null) {
                // 使用服务器返回的状态
                isMuted = (res.isMuted === true);
                updateMuteButton(isMuted);
            }
        } catch (error) {
            console.error('静音切换失败:', error);
        }
    });

    buttons.replay?.addEventListener('click', async () => {
        try {
            await postVideoCommand('replay', { layerId: currentLayerId });
        } catch (error) {
            console.error('重播失败:', error);
        }
    });

    if (!mediaTypeListenerAttached) {
        mediaTypeListenerAttached = true;
        document.querySelectorAll('.media-type-btn').forEach(btn => {
            btn.addEventListener('click', function () {
                document.querySelectorAll('.media-type-btn').forEach(b => b.classList.remove('active'));
                this.classList.add('active');
                const mediaType = this.dataset.type;
                filterByMediaType(mediaType);
            });
        });
    }

    // 图片上屏按钮 - 确保事件监听器只绑定一次
    const imageToScreenBtn = document.getElementById('image-to-screen-btn');
    const imageUploadInput = document.getElementById('image-upload-input');

    if (imageToScreenBtn && imageUploadInput && !imageUploadListenerAttached) {
        // 标记为已绑定
        imageUploadListenerAttached = true;

        // 点击按钮时触发文件选择器
        imageToScreenBtn.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();

            if (currentMediaType !== 'image') {
                return;
            }

            imageUploadInput.click();
        });

        // 文件选择后的处理
        // 使用防抖处理，避免重复处理
        let isProcessing = false;

        imageUploadInput.addEventListener('change', async (e) => {
            if (isProcessing) {
                imageUploadInput.value = '';
                return;
            }

            const file = e.target.files[0];
            if (!file) {
                imageUploadInput.value = '';
                return;
            }

            isProcessing = true;

            // 检查文件类型
            if (!file.type.startsWith('image/')) {
                const statusText = document.getElementById('playback-status-text');
                if (statusText) {
                    statusText.textContent = '请选择图片文件';
                    setTimeout(() => {
                        statusText.textContent = '正在播放：';
                    }, 2000);
                }
                // 清空input，允许重新选择
                imageUploadInput.value = '';
                isProcessing = false;
                return;
            }

            const statusText = document.getElementById('playback-status-text');
            if (statusText) {
                statusText.textContent = `正在处理：${file.name}...`;
            }

            try {
                const imageData = await fileToBase64(file);

                if (!imageData || imageData.length === 0) {
                    throw new Error('图片转换失败：base64数据为空');
                }

                // 发送图片数据到图层60（使用临时内存方式）
                const layerId = 60; // 图片专用图层
                await apiActionOrThrow('rendering', 'loadImage_from_data', {
                    layerId,
                    image_data: imageData,
                    filename: file.name
                });

                if (statusText) {
                    statusText.textContent = `图片已上屏：${file.name}`;
                }
                imageToScreenBtn.style.background = 'rgba(0, 255, 0, 0.2)';
                setTimeout(() => {
                    imageToScreenBtn.style.background = '';
                }, 500);
            } catch (err) {
                console.error('[图片上屏] 处理失败:', err);
                const errorMessage = err.message || '未知错误';
                if (statusText) {
                    statusText.textContent = '图片上屏失败：' + errorMessage;
                    setTimeout(() => {
                        statusText.textContent = '正在播放：';
                    }, 3000);
                }
                // 添加失败反馈动画
                imageToScreenBtn.style.background = 'rgba(255, 0, 0, 0.2)';
                setTimeout(() => {
                    imageToScreenBtn.style.background = '';
                }, 500);
            } finally {
                // 清空input，允许重新选择
                imageUploadInput.value = '';
                // 重置处理标志
                isProcessing = false;
            }
        });
    }

    // 根据媒体类型显示/隐藏图片上屏按钮
    updateImageToScreenButtonVisibility();

    updateUsbToolbarVisibility();
}

// 当前选中的媒体类型
let currentMediaType = 'video';

/**
 * 根据媒体类型和来源显示/隐藏图片上屏按钮
 */
function updateImageToScreenButtonVisibility() {
    const imageToScreenContainer = document.querySelector('.image-to-screen-container');
    if (imageToScreenContainer) {
        // 只有在本地模式且媒体类型为图片时才显示
        if (currentSource === 'local' && currentMediaType === 'image') {
            imageToScreenContainer.classList.remove('hidden');
        } else {
            imageToScreenContainer.classList.add('hidden');
        }
    }
}

/**
 * 切换媒体类型并重新加载数据
 */
async function filterByMediaType(type) {
    // 点击 U 盘按钮：进入U盘模式
    if (type === 'usb') {
        currentSource = 'usb';
        currentCategory = '';
        // 立即隐藏原有分类行
        const navContainer = document.getElementById('category-nav');
        if (navContainer) {
            navContainer.innerHTML = '';
            navContainer.style.display = 'none';
        }
        
        // 检查是否有缓存数据
        if (isUsbCacheValid()) {
            // 使用缓存数据，不强制刷新
            await loadUsbMedia(false);
        } else {
            // 没有缓存，提示用户点击扫描按钮
            const grid = document.getElementById('video-grid');
            if (grid) {
                grid.innerHTML = '<div class="grid-status-tip">点击右侧"扫描"开始扫描 U 盘媒体</div>';
            }
        }
        
        updateImageToScreenButtonVisibility();
        updateUsbToolbarVisibility();
        return;
    }

    // 点击视频/图片/音频按钮：切换到本地模式
    currentSource = 'local';
    currentMediaType = type;
    currentCategory = '';
    await loadVideos();
    updateImageToScreenButtonVisibility();
    updateUsbToolbarVisibility();
}


/**
 * 初始化 U盘播放模式下拉菜单和扫描按钮
 */
function initUsbToolbar() {
    const toolbar = document.getElementById('usb-toolbar');
    if (!toolbar) {
        console.warn('[initUsbToolbar] 未找到 usb-toolbar 元素');
        return;
    }

    // 避免重复绑定事件监听器
    if (usbToolbarInitialized) {
        return;
    }
    usbToolbarInitialized = true;

    const modeTrigger = document.getElementById('usb-mode-trigger');
    const modeMenu = document.getElementById('usb-mode-menu');
    if (modeTrigger && modeMenu) {
        setUsbPlayMode(usbPlayMode);

        modeTrigger.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            const opening = modeMenu.hidden;
            modeMenu.hidden = !opening;
            modeTrigger.setAttribute('aria-expanded', opening ? 'true' : 'false');
        });

        modeMenu.querySelectorAll('.usb-mode-option').forEach(option => {
            option.addEventListener('click', (e) => {
                e.preventDefault();
                e.stopPropagation();
                setUsbPlayMode(option.dataset.mode);
                closeUsbModeMenu();
            });
        });

        document.addEventListener('click', closeUsbModeMenu);
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') closeUsbModeMenu();
        });
    } else {
        console.warn('[initUsbToolbar] 未找到 U 盘播放模式菜单');
    }

    const scanBtn = document.getElementById('usb-scan-btn');
    if (scanBtn) {
        scanBtn.addEventListener('click', async () => {
            if (scanBtn.disabled) return;
            const originalText = scanBtn.textContent;
            scanBtn.disabled = true;
            scanBtn.textContent = '扫描中';
            try {
                // 强制刷新扫描 U 盘媒体（忽略缓存）
                const files = await loadUsbMedia(true);
                if (!files || files.length === 0) {
                    console.warn('[initUsbToolbar] 扫描结果为空');
                    return;
                }
                // 扫描成功后按当前模式创建播放列表并播放
                await createUsbPlaylistAndPlay(files);
            } finally {
                scanBtn.disabled = false;
                scanBtn.textContent = originalText;
            }
        });
    } else {
        console.error('[initUsbToolbar] 未找到扫描按钮 (usb-scan-btn)');
    }
}

/**
 * 根据 usbPlayMode 创建播放列表并开始播放
 */
async function createUsbPlaylistAndPlay(files) {
    if (!files || files.length === 0) return;

    try {
        const statusText = document.getElementById('playback-status-text');
        if (statusText) {
            statusText.textContent = '正在创建 U 盘播放列表...';
        }

        const playableFiles = files
            .filter(file => file && file.path)
            .slice(0, USB_SCAN_LIMIT);
        const wasLimited = files.length > playableFiles.length;

        // 从第一个文件路径提取U盘挂载路径
        // 例如: /storage/AAE2-941B/HS/video.mp4 -> /storage/AAE2-941B
        let usbMountPath = '';
        if (playableFiles.length > 0 && playableFiles[0].path) {
            const pathParts = playableFiles[0].path.split('/');
            if (pathParts.length >= 3 && pathParts[1] === 'storage') {
                usbMountPath = `/${pathParts[1]}/${pathParts[2]}`;
            }
        }

        const items = playableFiles.map(file => ({
            path: file.path,
            title: file.name || getFileNameFromPath(file.path),
            duration: 0
        }));

        // 1. 创建临时播放列表
        const createResult = await apiPost('/playlists', {
            name: `U盘_${Date.now()}`,
            target_layerId: currentLayerId,
            is_temporary: true,              // 标记为临时播放列表
            usb_mount_path: usbMountPath,    // 记录U盘挂载路径
            items
        }, USB_CREATE_TIMEOUT_MS);

        const playlistId = createResult?.id;
        const createdCount = createResult?.item_count;
        if (!playlistId || !Number.isInteger(createdCount)) {
            throw new Error('创建播放列表响应缺少 id 或 item_count');
        }
        if (createdCount !== items.length) {
            throw new Error(`播放列表项目数量不一致: expected=${items.length}, actual=${createdCount}`);
        }

        // 2. 设置播放模式
        const config = {};
        switch (usbPlayMode) {
            case 'random':
                config.mode = 'random';
                config.shuffle = true;
                config.loop = 0; // 列表循环
                break;
            case 'loop':
                config.mode = 'sequence';
                config.shuffle = false;
                config.loop = 0; // 列表循环
                break;
            case 'single':
                config.mode = 'sequence';
                config.shuffle = false;
                config.loop = 2; // 单曲循环
                break;
            case 'sequence':
            default:
                config.mode = 'sequence';
                config.shuffle = false;
                config.loop = 3; // 顺序循环（播完循环回第一个）
                break;
        }

        try {
            await apiPut(`/playlists/${encodeURIComponent(playlistId)}/config`, config);
        } catch (e) {
            console.error('[createUsbPlaylistAndPlay] 设置播放模式失败:', e);
        }

        // 4. 播放播放列表
        const playResult = await apiAction('playback', 'play', {
            playlistId: playlistId,
            layerId: currentLayerId,
            index: 0
        });

        if (playResult == null) {
            if (statusText) {
                statusText.textContent = '播放 U 盘播放列表失败';
            }
            addToCommandLog('播放 U 盘播放列表', 'error', '播放失败');
            return;
        }

        if (playResult && typeof playResult === 'object' && (playResult.state === 'blocked' || playResult.softBlocked || playResult.notice)) {
            const blockedMsg = playResult.message || '当前设备不能同时播放两个4K视频';
            if (statusText) {
                statusText.textContent = '播放受限：' + blockedMsg;
            }
            addToCommandLog('播放 U 盘播放列表', 'warning', blockedMsg);
            return;
        }

        if (playResult !== null) {
            currentPlaylistId = playlistId;
            if (statusText) {
                statusText.textContent = wasLimited
                    ? `正在播放：U 盘播放列表（前 ${playableFiles.length} 个）`
                    : '正在播放：U 盘播放列表';
            }
            // 稍后同步一次状态，显示实际片名
            setTimeout(syncSimpleStatus, 300);
        } else if (statusText) {
            statusText.textContent = '播放 U 盘播放列表失败';
        }
    } catch (error) {
        console.error('[createUsbPlaylistAndPlay] U 盘播放流程失败:', error);
        const statusText = document.getElementById('playback-status-text');
        if (statusText) {
            statusText.textContent = 'U 盘播放失败：' + (error.message || '未知错误');
        }
    }
}

/**
 * 加载图层列表，从播放列表获取目标图层ID
 * 这确保移动端使用正确的视频图层而不是硬编码的图层1
 */
export async function loadLayers() {
    try {
        // 从播放列表获取目标图层
        const playlists = await apiGet('/playlists');
        if (playlists && Array.isArray(playlists) && playlists.length > 0) {
            const defaultPlaylist =
                playlists.find(p => p.isDefault && Number(p.targetLayerId) === Number(currentLayerId)) ||
                playlists.find(p => p.isDefault && p.targetLayerId);
            if (defaultPlaylist && defaultPlaylist.targetLayerId) {
                currentPlaylistId = defaultPlaylist.id;
                currentLayerId = defaultPlaylist.targetLayerId;
                return;
            }
        }

        // 备选方案：从图层列表获取第一个视频图层
        const layers = await apiGet('/layers');
        if (layers && Array.isArray(layers)) {
            const videoLayer = layers.find(l => l.type === 'video' || l.name?.includes('video'));
            if (videoLayer && videoLayer.id) {
                currentLayerId = videoLayer.id;
                return;
            }
        }

        // 最后的默认值：使用图层2（常见配置）
        currentLayerId = 2;
    } catch (error) {
        console.error('[loadLayers] 加载图层失败:', error);
        currentLayerId = 2; // 失败时使用图层2作为默认值
    }
}

export function selectLayer(id) { currentLayerId = id; }

export async function playDefaultPlaylistIfIdle() {
    if (!currentPlaylistId || !currentLayerId) return false;

    try {
        const status = await apiGet(`/video/status?layerId=${currentLayerId}`);
        if (isPlaybackActive(status)) {
            return false;
        }

        const result = await apiPost('/video/play', {
            playlistId: currentPlaylistId,
            layerId: currentLayerId
        });

        if (result && typeof result === 'object' &&
            (result.state === 'blocked' || result.softBlocked || result.notice)) {
            const blockedMsg = result.message || '当前设备不能同时播放两个4K视频';
            const statusText = document.getElementById('playback-status-text');
            if (statusText) statusText.textContent = `播放受限：${blockedMsg}`;
            addToCommandLog('默认播放列表', 'warning', blockedMsg);
            return false;
        }

        if (result !== null) {
            isPlaying = true;
            updatePlayPauseButton(true);
            const statusText = document.getElementById('playback-status-text');
            const displayName = getFileNameFromPath(result?.uri || result?.path);
            if (statusText) {
                statusText.textContent = displayName ? `正在播放：${displayName}` : '正在播放：默认播放列表';
            }
            addToCommandLog('默认播放列表', 'success', '已播放默认播放列表');
            setTimeout(syncSimpleStatus, 300);
            return true;
        }
    } catch (error) {
        console.error('[playDefaultPlaylistIfIdle] 播放默认播放列表失败:', error);
        addToCommandLog('默认播放列表', 'error', error.message || '播放默认播放列表失败');
    }

    return false;
}

/**
 * 加载系统音量
 */
export async function loadSystemVolume() {
    try {
        await apiPost('/video/getSystemVolume', {});
    } catch (error) {
        console.error('[loadSystemVolume] 加载系统音量失败:', error);
    }
}

export function initializeThumbnailSSE() { }

