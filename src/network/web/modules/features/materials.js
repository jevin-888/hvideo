// 素材管理模块
import { apiGet, apiPost, sendLayerCommand, apiAction } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { getCurrentPlaylistId, updatePlaylistContent, updatePlaylistList, getPlaylistTargetLayer } from './playlist.js';
import { showInfo, showConfirm, showPrompt, showError, showWarning, updateConfirmProgress, escapeHtml } from '../components/toast.js';
import { getSelectedLayer } from '../pages/layerMatrix.js';
import { toggleButtonGroupActive } from '../components/buttonGroups.js';
import { extractFolderName, normalizePath } from '../utils/pathHelpers.js';
import { clearContainer } from '../utils/domHelpers.js';
import { handleApiOperation } from '../utils/apiHelpers.js';
import { openVideoPreview } from './videoPreview.js';
import { setPreviewMode } from '../pages/videoControl.js?v=2.98';
import { parseApiResponse } from '../utils/apiResponseParser.js';

// 发送图片播放命令到图层60
// 注意：loadImage 是图层渲染命令 (code: 0x03)，在 handleLayerRender 中处理
async function sendImagePlayCommand(filePath, fileName) {
    const layerId = 60; // 图片专用图层
    if (isFusionBackgroundMaterialPath(filePath)) {
        const message = 'gb_fusion 是融合背景目录，不发送到播放图层';
        addToCommandLog('加载图片', 'warning', message);
        showWarning(message);
        return;
    }
    addToCommandLog('加载图片', 'info', `正在加载图片: ${fileName}`);

    // 从完整路径中提取相对于 Image/ 目录的相对路径（支持子目录）
    // 例如：/sdcard/huoshan/Image/gb_fusion/19.jpg → gb_fusion/19.jpg
    let imageFile = fileName;
    const imageDir = '/Image/';
    const imageDirIdx = filePath.indexOf(imageDir);
    if (imageDirIdx !== -1) {
        imageFile = filePath.substring(imageDirIdx + imageDir.length);
    }

    try {
        // 使用图层渲染命令 (0x03)，对应后端 handleLayerRender
        // 后端接受相对于 Image/ 目录的路径（支持子目录，如 gb_fusion/19.jpg）
        const result = await apiAction('rendering', 'loadImage', {
            layerId,
            image_file: imageFile
        });
        if (result === null) {
            throw new Error('API 请求失败');
        }
        addToCommandLog('加载图片', 'success', `图片加载成功: ${fileName}`);
    } catch (error) {
        addToCommandLog('加载图片', 'error', `图片加载失败: ${error.message}`);
        throw error;
    }
}


// 全局变量
let currentMaterialType = 'video';
let currentViewMode = 'grid';
let currentFolderFilter = ''; // 当前选中的文件夹过滤器
let selectedMaterials = new Set(); // 存储选中的素材路径

const FUSION_BACKGROUND_FOLDER = 'gb_fusion';
const IMAGE_FILE_EXTENSIONS = new Set(['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp', 'svg', 'tif', 'tiff']);

function isFusionBackgroundFolder(folderName) {
    return String(folderName || '').toLowerCase() === FUSION_BACKGROUND_FOLDER;
}

function isFusionBackgroundMaterialPath(filePath) {
    const normalized = String(filePath || '').replace(/\\/g, '/');
    const lower = normalized.toLowerCase();
    return isFusionBackgroundFolder(extractFolderName(normalized)) ||
        lower.includes('/image/gb_fusion/') ||
        lower.startsWith('gb_fusion/');
}

function isFusionBackgroundMaterial(file) {
    return currentMaterialType === 'image' &&
        (isFusionBackgroundFolder(file?.folder) || isFusionBackgroundMaterialPath(file?.path));
}

function isImageFilePath(filePath) {
    const name = String(filePath || '').split(/[\\/]/).pop() || '';
    const dotIndex = name.lastIndexOf('.');
    if (dotIndex < 0) return false;
    return IMAGE_FILE_EXTENSIONS.has(name.slice(dotIndex + 1).toLowerCase());
}

const DEFAULT_SOFT_BLOCK_MESSAGE = '当前设备不能同时播放两个4K视频';

function isSoftBlockedPlaybackResult(result) {
    return result && typeof result === 'object' &&
        (result.state === 'blocked' || result.softBlocked === true || result.notice === true);
}

function getPlaybackResultMessage(result, fallback = DEFAULT_SOFT_BLOCK_MESSAGE) {
    if (result && typeof result === 'object' &&
        typeof result.message === 'string' &&
        result.message.trim()) {
        return result.message;
    }
    return fallback;
}

// 更新上传按钮文本 - 根据当前素材类型
function updateUploadButtonLabels() {
    const uploadBtn = document.getElementById('upload-btn');
    const uploadFolderBtn = document.getElementById('upload-folder-btn');

    if (currentMaterialType === 'video') {
        if (uploadBtn) uploadBtn.textContent = '上传视频';
        if (uploadFolderBtn) uploadFolderBtn.textContent = '上传视频文件夹';
    } else if (currentMaterialType === 'image') {
        if (uploadBtn) uploadBtn.textContent = '上传图片';
        if (uploadFolderBtn) uploadFolderBtn.textContent = '上传图片文件夹';
    } else if (currentMaterialType === 'audio') {
        if (uploadBtn) uploadBtn.textContent = '上传音乐';
        if (uploadFolderBtn) uploadFolderBtn.textContent = '上传音乐文件夹';
    }
}

// SSE事件源（用于接收缩略图生成通知）
let thumbnailEventSource = null;

// 初始化SSE连接以接收缩略图生成通知
function initializeThumbnailSSE() {
    // 如果已经存在连接，先关闭
    if (thumbnailEventSource) {
        thumbnailEventSource.close();
    }

    try {
        // 创建SSE连接
        // window.apiBaseUrl 固定为 /api/v1，这里只追加资源路径。
        thumbnailEventSource = new EventSource(`${window.apiBaseUrl}/events`);

        // 监听缩略图生成完成事件
        thumbnailEventSource.addEventListener('thumbnail', function (event) {

            try {
                // 检查 event.data 是否存在且不为空
                if (!event.data || typeof event.data !== 'string' || event.data.trim() === '') {
                    return;
                }

                // 清理数据（移除可能的换行符、空格等）
                const cleanData = event.data.trim();

                // 尝试解析 JSON
                let data;
                try {
                    data = JSON.parse(cleanData);
                } catch (parseError) {
                    // JSON解析失败，忽略该消息
                    return;
                }

                // 验证数据格式
                if (data && data.type === 'thumbnail_ready' && data.path) {
                    // 更新对应路径的缩略图
                    updateThumbnailForPath(data.path);
                }
            } catch (e) {
                // 处理过程中出错，忽略该消息
            }
        });

        // 监听转码状态变化
        thumbnailEventSource.addEventListener('transcode_status', function (event) {
            try {
                if (!event.data) return;
                const data = JSON.parse(event.data);
                handleTranscodeStatusPayload(data);
            } catch (e) {
                console.error('Error handling transcode_status SSE:', e);
            }
        });

        // 监听连接错误
        thumbnailEventSource.onerror = function (event) {
            // 连接断开后，5秒后重试
            setTimeout(() => {
                if (!thumbnailEventSource || thumbnailEventSource.readyState === EventSource.CLOSED) {
                    initializeThumbnailSSE();
                }
            }, 5000);
        };

    } catch (e) {
        // SSE连接初始化失败，将在onerror中重试
    }
}

// 更新指定路径的缩略图
function updateThumbnailForPath(filePath) {
    const listContainer = document.getElementById('material-list');
    if (!listContainer) {
        return;
    }

    // 标准化路径（使用统一的路径规范化函数）
    const normalizedFilePath = normalizePath(filePath);

    // 查找所有匹配的缩略图元素
    const items = listContainer.querySelectorAll('.material-item');
    let found = false;

    items.forEach(item => {
        const preview = item.querySelector('.material-preview');
        if (preview) {
            // 尝试从data-path属性获取路径
            let itemPath = null;
            if (preview.dataset.path) {
                itemPath = decodeURIComponent(preview.dataset.path);
            } else {
                // 如果没有data-path，尝试从缩略图URL中提取路径
                const thumbnail = item.querySelector('img.material-thumbnail');
                if (thumbnail && thumbnail.src) {
                    try {
                        const url = new URL(thumbnail.src);
                        const pathParam = url.searchParams.get('path');
                        if (pathParam) {
                            itemPath = decodeURIComponent(pathParam);
                        }
                    } catch (e) {
                        // URL解析失败，忽略
                    }
                }
            }

            if (!itemPath) return;

            const normalizedItemPath = normalizePath(itemPath);

            // 路径匹配（支持多种匹配方式）
            const isMatch =
                normalizedItemPath === normalizedFilePath ||  // 完全匹配
                normalizedItemPath.endsWith(normalizedFilePath) ||  // itemPath 以 filePath 结尾
                normalizedFilePath.endsWith(normalizedItemPath) ||  // filePath 以 itemPath 结尾
                normalizedItemPath.includes(normalizedFilePath) ||  // itemPath 包含 filePath
                normalizedFilePath.includes(normalizedItemPath);    // filePath 包含 itemPath (新增)

            if (isMatch) {
                found = true;
                const thumbnail = item.querySelector('img.material-thumbnail');
                if (thumbnail) {
                    // 使用匹配到的 itemPath 来构建 URL（确保路径一致）
                    const thumbnailUrl = `${window.apiBaseUrl}/materials/thumbnail?path=${encodeURIComponent(itemPath)}&t=${Date.now()}`;
                    thumbnail.src = thumbnailUrl;
                    // 移除占位符（如果存在）
                    const placeholder = preview.querySelector('.thumbnail-placeholder');
                    if (placeholder) {
                        placeholder.remove();
                    }
                    // 显示缩略图
                    thumbnail.style.display = '';
                }
            }
        }
    });
}

// 更新指定路径的转码状态
function updateTranscodeStateForPath(filePath, status, progress, message, encoder) {
    const listContainer = document.getElementById('material-list');
    if (!listContainer) return;

    const normalizedFilePath = normalizePath(filePath);
    const items = listContainer.querySelectorAll('.material-item');

    items.forEach(item => {
        const itemPath = item.dataset.path;
        if (!itemPath) return;

        const normalizedItemPath = normalizePath(itemPath);
        const isMatch =
            normalizedItemPath === normalizedFilePath ||
            normalizedItemPath.endsWith(normalizedFilePath) ||
            normalizedFilePath.endsWith(normalizedItemPath) ||
            normalizedItemPath.includes(normalizedFilePath) ||
            normalizedFilePath.includes(normalizedItemPath);

        if (isMatch) {
            if (status === 'started') {
                item.classList.add('transcoding');
                // 确保没有重复的overlay
                ensureTranscodingOverlay(item);
            } else if (status === 'progress') {
                // 更新进度显示
                item.classList.add('transcoding');
                const overlay = ensureTranscodingOverlay(item);
                if (overlay) {
                    const progressText = overlay.querySelector('.transcoding-progress');
                    if (progressText && progress !== undefined) {
                        const percent = Math.round(progress);
                        progressText.textContent = `${percent}%`;
                    }
                    // 更新文本消息
                    const textEl = overlay.querySelector('.transcoding-text');
                    if (textEl && message) {
                        textEl.textContent = message;
                    }
                    // 更新编码器信息
                    const encoderEl = overlay.querySelector('.transcoding-encoder');
                    if (encoderEl && encoder) {
                        // 将编码器名称转换为友好显示
                        let encoderDisplay = '';
                        if (encoder.includes('rkmpp')) {
                            encoderDisplay = '🟢 RK MPP 硬件编码';
                        } else {
                            encoderDisplay = encoder;
                        }
                        encoderEl.textContent = encoderDisplay;
                    }
                }
                if (message && message.includes('跳过')) {
                    setTimeout(() => {
                        item.classList.remove('transcoding');
                        const currentOverlay = item.querySelector('.transcoding-overlay');
                        if (currentOverlay) currentOverlay.remove();
                    }, 1500);
                }
            } else {
                // finished 或 failed
                item.classList.remove('transcoding');
                const overlay = item.querySelector('.transcoding-overlay');
                if (overlay) overlay.remove();

                if (status === 'finished') {
                    // 重新刷新一下缩略图
                    setTimeout(() => updateThumbnailForPath(itemPath), 1000);
                    // 修复：showSuccess改为showInfo
                    showInfo('视频编码完成', 3000);
                } else if (status === 'failed') {
                    showError('视频转码失败，请检查文件是否损坏', 5000);
                }
            }
        }
    });
}

function ensureTranscodingOverlay(item) {
    let overlay = item.querySelector('.transcoding-overlay');
    if (overlay) {
        if (!overlay.querySelector('.transcoding-progress')) {
            const progressEl = document.createElement('div');
            progressEl.className = 'transcoding-progress';
            progressEl.textContent = '0%';
            overlay.appendChild(progressEl);
        }
        if (!overlay.querySelector('.transcoding-encoder')) {
            const encoderEl = document.createElement('div');
            encoderEl.className = 'transcoding-encoder';
            overlay.appendChild(encoderEl);
        }
        return overlay;
    }

    overlay = document.createElement('div');
    overlay.className = 'transcoding-overlay';
    overlay.innerHTML = `
        <div class="transcoding-spinner"></div>
        <div class="transcoding-text">转码中...</div>
        <div class="transcoding-progress">0%</div>
        <div class="transcoding-encoder"></div>
    `;
    item.appendChild(overlay);
    return overlay;
}

let transcodeProgressHideTimer = null;

function ensureTranscodeProgressPanel() {
    let panel = document.getElementById('transcode-progress-panel');
    if (panel) return panel;

    const toolbar = document.querySelector('.material-toolbar');
    const folderList = document.getElementById('material-folder-list');
    const parent = folderList?.parentNode || toolbar?.parentNode;
    if (!parent) return null;

    panel = document.createElement('div');
    panel.id = 'transcode-progress-panel';
    panel.className = 'transcode-progress-panel hidden';
    panel.innerHTML = `
        <div class="transcode-progress-header">
            <span id="transcode-progress-title">转码中</span>
            <span id="transcode-progress-percent">0%</span>
        </div>
        <div class="transcode-progress-bar-track">
            <div id="transcode-progress-bar" class="transcode-progress-bar"></div>
        </div>
        <div id="transcode-progress-detail" class="transcode-progress-detail"></div>
    `;

    if (folderList) {
        parent.insertBefore(panel, folderList);
    } else {
        parent.appendChild(panel);
    }
    return panel;
}

function updateTranscodeProgressPanel(title, percent, detail, autoHide = false) {
    const panel = ensureTranscodeProgressPanel();
    if (!panel) return;

    if (transcodeProgressHideTimer) {
        clearTimeout(transcodeProgressHideTimer);
        transcodeProgressHideTimer = null;
    }

    const safePercent = Math.round(clampProgress(percent));
    panel.classList.remove('hidden');
    const titleEl = panel.querySelector('#transcode-progress-title');
    const percentEl = panel.querySelector('#transcode-progress-percent');
    const barEl = panel.querySelector('#transcode-progress-bar');
    const detailEl = panel.querySelector('#transcode-progress-detail');

    if (titleEl) titleEl.textContent = title || '转码中';
    if (percentEl) percentEl.textContent = `${safePercent}%`;
    if (barEl) barEl.style.width = `${safePercent}%`;
    if (detailEl) detailEl.textContent = detail || '';

    if (autoHide) {
        transcodeProgressHideTimer = setTimeout(() => {
            panel.classList.add('hidden');
            transcodeProgressHideTimer = null;
        }, 4000);
    }
}

let transcodeStatusPollTimer = null;
let lastTranscodeSnapshotSequence = 0;
let lastHandledBatchSequence = 0;
let transcodeStatusIdleCount = 0;
let transcodeStatusExpectTerminal = false;

function isActiveTranscodeEvent(data) {
    if (!data || !data.status) return false;
    return data.status === 'started' ||
        data.status === 'progress' ||
        data.status === 'batch_start' ||
        data.status === 'batch_progress';
}

function stopTranscodeStatusPolling() {
    if (transcodeStatusPollTimer) {
        clearInterval(transcodeStatusPollTimer);
        transcodeStatusPollTimer = null;
    }
}

function startTranscodeStatusPolling(expectTerminal = true) {
    if (expectTerminal) {
        transcodeStatusExpectTerminal = true;
    }
    transcodeStatusIdleCount = 0;
    if (transcodeStatusPollTimer) return;

    pollTranscodeStatusSnapshot();
    transcodeStatusPollTimer = setInterval(pollTranscodeStatusSnapshot, 1000);
}

async function pollTranscodeStatusSnapshot() {
    const snapshot = await apiGet('/video/transcode_status', {
        timeoutMs: 3000,
        retries: 0,
        retryOnHttp: false
    });

    if (!snapshot || typeof snapshot !== 'object') {
        transcodeStatusIdleCount++;
        if (transcodeStatusIdleCount >= 5) {
            stopTranscodeStatusPolling();
        }
        return;
    }

    const states = Array.isArray(snapshot.states) ? snapshot.states : [];
    const batch = snapshot.batch && typeof snapshot.batch === 'object' ? snapshot.batch : null;
    const hasActiveStates = states.some(isActiveTranscodeEvent);
    const batchActive = batch ? isActiveTranscodeEvent(batch) : false;
    const hasWork = !!snapshot.active || hasActiveStates || batchActive;

    if (hasActiveStates || batchActive) {
        transcodeStatusExpectTerminal = true;
    }

    const sequence = Number(snapshot.sequence) || 0;
    const hasNewSnapshot = sequence === 0 || sequence > lastTranscodeSnapshotSequence;
    const batchSequence = Number(batch?.sequence) || 0;
    const hasUnhandledBatchState = batch && (batchSequence === 0 || batchSequence > lastHandledBatchSequence);

    if (hasNewSnapshot) {
        states.forEach(state => handleTranscodeStatusPayload(state));
        const lastSequence = Number(snapshot.last?.sequence) || 0;
        const hasNewLastState = lastSequence === 0 || lastSequence > lastTranscodeSnapshotSequence;
        if (snapshot.last && hasNewLastState && transcodeStatusExpectTerminal && !hasActiveStates && !batchActive) {
            handleTranscodeStatusPayload(snapshot.last);
        }
        if (sequence > 0) {
            lastTranscodeSnapshotSequence = sequence;
        }
    }

    if (hasUnhandledBatchState && (batchActive || isBatchTranscoding || transcodeStatusExpectTerminal)) {
        handleTranscodeStatusPayload(batch);
    }

    if (hasWork) {
        transcodeStatusIdleCount = 0;
        return;
    }

    transcodeStatusIdleCount++;
    if (transcodeStatusIdleCount >= 3) {
        transcodeStatusExpectTerminal = false;
        stopTranscodeStatusPolling();
    }
}

// 初始化素材管理
export function initializeMaterials() {
    // 初始化按钮选中状态，确保与currentMaterialType同步
    document.querySelectorAll('.type-btn').forEach(btn => {
        if (btn.dataset.type === currentMaterialType) {
            btn.classList.add('active');
        } else {
            btn.classList.remove('active');
        }
    });

    // 初始化上传按钮文本
    updateUploadButtonLabels();

    // 初始化SSE连接以接收缩略图生成通知
    initializeThumbnailSSE();
    startTranscodeStatusPolling(false);

    // 绑定类型切换事件
    document.querySelectorAll('.type-btn').forEach(item => {
        item.addEventListener('click', function () {
            toggleButtonGroupActive('.type-btn', this);
            currentMaterialType = this.dataset.type;
            currentFolderFilter = ''; // 切换类型时重置文件夹过滤器
            updateUploadButtonLabels(); // 更新上传按钮文本
            updateFolderList();
            updateMaterials();
            updateSelectedMaterialsToolbar();
        });
    });

    // 搜索事件
    const materialSearch = document.getElementById('material-search');
    if (materialSearch) {
        materialSearch.addEventListener('input', function () {
            updateMaterials(this.value);
        });
    }

    // 刷新全部：切换到「全部」并刷新文件夹与素材列表
    const materialRefreshBtn = document.getElementById('material-refresh-btn');
    if (materialRefreshBtn) {
        materialRefreshBtn.addEventListener('click', async () => {
            currentFolderFilter = '';
            updateFolderTagsActive();
            updateFolderActionsToolbar();
            await apiPost('/materials/refresh_index', {});
            await updateFolderList();
            await updateMaterials(document.getElementById('material-search')?.value || '');
            showInfo('已触发素材库索引刷新');
        });
    }

    // 重新编码选中视频（h264_rkmpp）
    const transcodeBtn = document.getElementById('transcode-selected-btn');
    if (transcodeBtn) {
        transcodeBtn.addEventListener('click', async () => {
            if (currentMaterialType !== 'video' || selectedMaterials.size === 0) {
                showInfo('请先选中要重新编码的视频');
                return;
            }
            const paths = Array.from(selectedMaterials);
            transcodeBtn.disabled = true;
            transcodeBtn.textContent = `提交中 0/${paths.length}...`;
            let successCount = 0;
            let skippedCount = 0;
            let failCount = 0;
            let lastErrorMsg = '';
            for (let i = 0; i < paths.length; i++) {
                const path = paths[i];
                transcodeBtn.textContent = `提交中 ${i + 1}/${paths.length}...`;
                updateTranscodeStateForPath(path, 'started', 0, '已提交，等待后台开始');
                updateTranscodeProgressPanel(
                    paths.length > 1 ? `重新编码 ${i + 1}/${paths.length}` : '重新编码',
                    0,
                    `已提交: ${getPathFileName(path)}`
                );
                try {
                    const result = await apiPost('/video/transcode', { path });
                    const resultData = result;
                    // 202 异步提交成功
                    if (result !== null && result !== undefined) {
                        if (resultData?.skipped) {
                            skippedCount++;
                            updateTranscodeStateForPath(path, 'progress', 100, '已优化，跳过');
                            updateTranscodeProgressPanel('重新编码已跳过', 100, `${getPathFileName(path)} 已优化`, true);
                            addToCommandLog('重新编码', 'info', `已跳过: ${getPathFileName(path)}（已优化）`);
                        } else {
                            successCount++;
                            startTranscodeStatusPolling(true);
                            const outPath = resultData?.output_path || '';
                            addToCommandLog('重新编码', 'success',
                                `已提交: ${getPathFileName(path)}${outPath ? ' → ' + getPathFileName(outPath) : ''}`);
                        }
                    } else {
                        failCount++;
                        const errMsg = '请求失败';
                        lastErrorMsg = errMsg;
                        updateTranscodeStateForPath(path, 'failed', 100, errMsg);
                        updateTranscodeProgressPanel('重新编码提交失败', 100, `${getPathFileName(path)} - ${errMsg}`, true);
                        addToCommandLog('重新编码', 'error', `失败: ${getPathFileName(path)} - ${errMsg}`);
                    }
                } catch (e) {
                    failCount++;
                    lastErrorMsg = e.message || '网络错误';
                    updateTranscodeStateForPath(path, 'failed', 100, lastErrorMsg);
                    updateTranscodeProgressPanel('重新编码提交失败', 100, `${getPathFileName(path)} - ${lastErrorMsg}`, true);
                    addToCommandLog('重新编码', 'error', `失败: ${getPathFileName(path)} - ${lastErrorMsg}`);
                }
            }
            transcodeBtn.disabled = false;
            transcodeBtn.textContent = '🔄 重新编码';
            if (failCount > 0 && successCount === 0 && skippedCount === 0) {
                showError(`重新编码提交失败${lastErrorMsg ? ': ' + lastErrorMsg : ''}`, 5000);
            } else if (failCount > 0) {
                showInfo(`已提交 ${successCount} 个，跳过 ${skippedCount} 个，失败 ${failCount} 个，编码在后台进行`, 5000);
            } else if (successCount === 0 && skippedCount > 0) {
                showInfo(`已跳过 ${skippedCount} 个已优化视频，无需重复编码`, 5000);
            } else {
                showInfo(`已提交 ${successCount} 个视频重新编码，跳过 ${skippedCount} 个，编码完成后原文件将自动替换（可能需要数分钟）`, 6000);
            }
        });
    }

    // 上传按钮（文件上传）
    const uploadBtn = document.getElementById('upload-btn');
    const fileInput = document.getElementById('file-input');
    if (uploadBtn && fileInput) {
        uploadBtn.addEventListener('click', () => {
            fileInput.click();
        });
    }

    // 上传文件夹按钮
    const folderInput = document.getElementById('folder-input');
    const uploadFolderBtn = document.getElementById('upload-folder-btn');
    if (folderInput && uploadFolderBtn) {
        uploadFolderBtn.addEventListener('click', () => {
            folderInput.click();
        });

        folderInput.addEventListener('change', async (e) => {
            const files = e.target.files;
            if (!files || files.length === 0) return;

            // 文件夹上传：提取文件夹名（兼容Edge浏览器）
            let folderName = '';
            if (files.length > 0) {
                // 优先使用webkitRelativePath（Chrome/Safari），回退到name属性（Edge/Firefox）
                const firstPath = files[0].webkitRelativePath || files[0].name || '';
                if (firstPath) {
                    const pathParts = firstPath.split('/');
                    if (pathParts.length > 0) {
                        folderName = pathParts[0];
                    }
                }
            }

            await handleFileUpload(Array.from(files), true, folderName, `文件夹 "${folderName}"`);

            // 清空文件选择
            e.target.value = '';
        });
    }

    // 文件输入框事件
    if (fileInput) {
        fileInput.addEventListener('change', async (e) => {
            const files = e.target.files;
            if (!files || files.length === 0) return;

            await handleFileUpload(Array.from(files), false, '', '所有文件');

            // 清空文件选择
            e.target.value = '';
        });
    }


    updateMaterials();
    updateFolderList();

    // 拖拽上传（绕开 webkitdirectory 原生确认框）
    initializeDragUpload();
}

// 更新文件夹列表
async function updateFolderList() {
    const folderContainer = document.getElementById('material-folder-list');
    if (!folderContainer) return;

    folderContainer.innerHTML = '<span class="folder-loading">加载文件夹...</span>';

    const folders = await apiGet(`/materials/folders?type=${currentMaterialType}`);
    clearContainer(folderContainer);

    // 添加"全部"选项
    const allTag = document.createElement('span');
    allTag.className = `folder-tag ${currentFolderFilter === '' ? 'active' : ''}`;
    allTag.textContent = '全部';
    allTag.addEventListener('click', () => {
        currentFolderFilter = '';
        updateFolderTagsActive();
        updateFolderActionsToolbar();
        updateMaterials(document.getElementById('material-search')?.value || '');
    });
    folderContainer.appendChild(allTag);

    if (folders && Array.isArray(folders)) {
        folders
            .filter(folder => !(currentMaterialType === 'image' && isFusionBackgroundFolder(folder?.name)))
            .forEach(folder => {
                const tagWrapper = document.createElement('span');
                tagWrapper.className = 'folder-tag-wrapper';

                const tag = document.createElement('span');
                tag.className = `folder-tag ${currentFolderFilter === folder.name ? 'active' : ''}`;
                tag.textContent = folder.name;
                tag.dataset.folder = folder.name;
                tag.title = `点击过滤: ${folder.name}`;
                tag.addEventListener('click', () => {
                    currentFolderFilter = folder.name;
                    updateFolderTagsActive();
                    updateFolderActionsToolbar();
                    updateMaterials(document.getElementById('material-search')?.value || '');
                });

                tagWrapper.appendChild(tag);
                folderContainer.appendChild(tagWrapper);
            });
    }

    // 更新文件夹操作按钮工具栏
    updateFolderActionsToolbar();
    // 更新选中素材操作工具栏
    updateSelectedMaterialsToolbar();
}

// 更新选中素材操作工具栏（视频类型且已选中时显示「重新编码」）
function updateSelectedMaterialsToolbar() {
    const toolbar = document.getElementById('selected-materials-actions-toolbar');
    if (!toolbar) return;

    const hasVideoSelected = currentMaterialType === 'video' && selectedMaterials.size > 0;
    toolbar.style.display = hasVideoSelected ? '' : 'none';
}

// 更新文件夹操作按钮工具栏
async function updateFolderActionsToolbar() {
    const toolbar = document.getElementById('folder-actions-toolbar');
    if (!toolbar) return;

    // 使用工具函数清空工具栏
    clearContainer(toolbar);

    // 如果没有选中文件夹，不显示操作按钮
    if (!currentFolderFilter) {
        return;
    }
    if (currentMaterialType === 'image' && isFusionBackgroundFolder(currentFolderFilter)) {
        return;
    }

    // 获取当前选中的文件夹信息
    const folders = await apiGet(`/materials/folders?type=${currentMaterialType}`);
    if (!folders || !Array.isArray(folders)) {
        return;
    }

    const selectedFolder = folders.find(f => f.name === currentFolderFilter);
    if (!selectedFolder) {
        return;
    }

    // 重命名按钮
    const renameBtn = document.createElement('button');
    renameBtn.className = 'btn small folder-rename-btn';
    renameBtn.innerHTML = '✎ 重命名';
    renameBtn.title = '重命名文件夹';
    renameBtn.addEventListener('click', async () => {
        let basePath = '/huoshan/video';
        if (currentMaterialType === 'image') basePath = '/huoshan/Image';
        else if (currentMaterialType === 'audio') basePath = '/huoshan/audio';
        const folderPath = selectedFolder.path || `${basePath}/${selectedFolder.name}`;
        showRenameDialog(selectedFolder.name, async (newName) => {
            const success = await renameMaterial(folderPath, newName);
            if (success) {
                addToCommandLog('重命名文件夹', 'success', `成功重命名: ${selectedFolder.name} -> ${newName}`);
                // 如果重命名的是当前选中的文件夹，更新过滤器为新名称
                if (currentFolderFilter === selectedFolder.name) {
                    currentFolderFilter = newName;
                }
                // 先更新文件夹列表，再更新素材列表
                await updateFolderList();
                await updateMaterials();
            } else {
                addToCommandLog('重命名文件夹', 'error', `重命名失败: ${selectedFolder.name}`);
                showInfo('重命名文件夹失败，请检查文件夹名是否合法或权限是否足够');
            }
        });
    });

    // 删除按钮
    const deleteBtn = document.createElement('button');
    deleteBtn.className = 'btn small danger folder-delete-btn';
    deleteBtn.innerHTML = '× 删除';
    deleteBtn.title = '删除文件夹';
    deleteBtn.addEventListener('click', async () => {
        // 检查是否为默认文件夹，禁止删除
        const protectedFolders = ['defaultvideo', 'defaultimage'];
        if (protectedFolders.includes(selectedFolder.name.toLowerCase())) {
            showInfo('默认文件夹不允许删除');
            return;
        }

        let basePath = '/huoshan/video';
        if (currentMaterialType === 'image') basePath = '/huoshan/Image';
        else if (currentMaterialType === 'audio') basePath = '/huoshan/audio';
        const folderPath = selectedFolder.path || `${basePath}/${selectedFolder.name}`;
        const escapedFolderName = escapeHtml(selectedFolder.name);
        const confirmed = await showConfirm(
            `确定要删除文件夹 "${escapedFolderName}" 吗？\n文件夹内的所有文件将被删除，此操作不可恢复！`,
            '删除文件夹',
            true  // 标记需要显示进度，确认后不立即关闭对话框
        );

        if (confirmed) {
            // 对话框仍然打开，现在显示删除进度
            const dialog = document.getElementById('confirm-dialog');
            if (dialog) {
                dialog.querySelector('#confirm-message').textContent = `正在删除文件夹 "${escapedFolderName}"...`;
                dialog.querySelector('h3').textContent = '删除文件夹';
                updateConfirmProgress('正在删除文件夹...', true);
            }

            try {
                // 调用删除API
                const deleteSuccess = await deleteFolder(folderPath);

                if (deleteSuccess) {
                    // 轮询检查文件夹是否已删除
                    updateConfirmProgress('正在确认删除结果...', true);
                    let deleted = false;
                    let attempts = 0;
                    const maxAttempts = 20; // 最多检查20次，每次500ms，总共10秒

                    while (!deleted && attempts < maxAttempts) {
                        await new Promise(resolve => setTimeout(resolve, 500));
                        attempts++;

                        try {
                            const folders = await apiGet(`/materials/folders?type=${currentMaterialType}`);
                            const folderExists = folders && Array.isArray(folders) &&
                                folders.some(f => f.name === selectedFolder.name);

                            if (!folderExists) {
                                deleted = true;
                                break;
                            }

                            // 更新进度提示
                            updateConfirmProgress(`正在删除... (${attempts}/${maxAttempts})`, true);
                        } catch (error) {
                            // 如果API调用失败，继续尝试
                        }
                    }

                    if (deleted || attempts >= maxAttempts) {
                        updateConfirmProgress(deleted ? '删除成功！' : '删除操作已完成', true);
                        await new Promise(resolve => setTimeout(resolve, 500));

                        // 关闭对话框
                        const confirmDialog = document.getElementById('confirm-dialog');
                        if (confirmDialog) {
                            confirmDialog.classList.remove('active');
                            updateConfirmProgress('', false);
                        }

                        addToCommandLog('删除文件夹', 'success', `成功删除文件夹: ${selectedFolder.name}`);
                        // 如果删除的是当前选中的文件夹，重置过滤器
                        if (currentFolderFilter === selectedFolder.name) {
                            currentFolderFilter = '';
                        }
                        // 刷新文件夹列表和素材列表
                        await updateFolderList();
                        await updateMaterials();
                        // 刷新播放列表（移除已删除文件夹中的视频条目）
                        await updatePlaylistList();
                        const playlistId = getCurrentPlaylistId();
                        if (playlistId) {
                            await updatePlaylistContent(playlistId);
                        }
                    }
                } else {
                    // 删除失败
                    updateConfirmProgress('删除失败', true);
                    await new Promise(resolve => setTimeout(resolve, 1000));

                    // 关闭对话框
                    const confirmDialog = document.getElementById('confirm-dialog');
                    if (confirmDialog) {
                        confirmDialog.classList.remove('active');
                        updateConfirmProgress('', false);
                    }

                    addToCommandLog('删除文件夹', 'error', `删除失败: ${selectedFolder.name}`);
                    showInfo('删除文件夹失败，请检查文件是否被占用或权限是否足够');
                }
            } catch (error) {
                // 发生错误
                updateConfirmProgress('删除过程中发生错误', true);
                await new Promise(resolve => setTimeout(resolve, 1000));

                // 关闭对话框
                const confirmDialog = document.getElementById('confirm-dialog');
                if (confirmDialog) {
                    confirmDialog.classList.remove('active');
                    updateConfirmProgress('', false);
                }

                addToCommandLog('删除文件夹', 'error', `删除失败: ${error.message}`);
                showInfo('删除文件夹失败: ' + error.message);
            }
        }
    });

    // 添加到播放列表按钮
    const addToPlaylistBtn = document.createElement('button');
    addToPlaylistBtn.className = 'btn small add-folder-to-playlist-btn';
    addToPlaylistBtn.innerHTML = '+ 播放列表';
    addToPlaylistBtn.title = '将文件夹中的所有素材添加到播放列表';
    addToPlaylistBtn.addEventListener('click', async () => {
        const playlistId = getCurrentPlaylistId();
        if (!playlistId) {
            showInfo('请先选择一个播放列表');
            return;
        }

        // 获取当前文件夹中的所有素材
        const folderQuery = currentFolderFilter ? `&folder=${encodeURIComponent(currentFolderFilter)}` : '';
        const materials = await apiGet(`/materials?type=${currentMaterialType}${folderQuery}`);
        if (!materials || !Array.isArray(materials)) {
            showInfo('获取素材列表失败');
            return;
        }

        const folderMaterials = materials;

        if (folderMaterials.length === 0) {
            showInfo('当前文件夹中没有素材');
            return;
        }

        // 批量添加到播放列表（图片使用图层 60，视频/音频使用播放列表绑定的目标图层）
        let successCount = 0;
        let failCount = 0;
        const layerIdForAdd = currentMaterialType === 'image' ? 60 : null;
        for (const file of folderMaterials) {
            const success = await addVideoToPlaylist(playlistId, file.path, layerIdForAdd);
            if (success) {
                successCount++;
            } else {
                failCount++;
            }
        }

        if (successCount > 0) {
            addToCommandLog('添加文件夹到播放列表', 'success',
                `成功添加 ${successCount} 个素材到播放列表${failCount > 0 ? `，失败 ${failCount} 个` : ''}`);
            showInfo(`成功添加 ${successCount} 个素材到播放列表${failCount > 0 ? `，失败 ${failCount} 个` : ''}`);
            // 刷新播放列表显示
            await updatePlaylistContent(playlistId);
        } else {
            addToCommandLog('添加文件夹到播放列表', 'error', '添加失败');
            showInfo('添加素材到播放列表失败');
        }
    });

    toolbar.appendChild(renameBtn);
    toolbar.appendChild(deleteBtn);
    toolbar.appendChild(addToPlaylistBtn);
}

// 更新文件夹标签的激活状态
function updateFolderTagsActive() {
    const folderContainer = document.getElementById('material-folder-list');
    if (!folderContainer) return;

    folderContainer.querySelectorAll('.folder-tag').forEach(tag => {
        const folderName = tag.dataset.folder || '';
        if (folderName === currentFolderFilter || (folderName === '' && currentFolderFilter === '')) {
            tag.classList.add('active');
        } else {
            tag.classList.remove('active');
        }
    });

    // "全部"按钮特殊处理（第一个直接子元素，不是wrapper）
    const allTag = folderContainer.querySelector('.folder-tag:first-child');
    if (allTag && currentFolderFilter === '') {
        allTag.classList.add('active');
    }
}

// 处理文件上传（统一处理文件夹和单文件上传）
async function handleFileUpload(files, isFolderUpload, folderName, logPrefix) {
    if (!files || files.length === 0) return;

    addToCommandLog(isFolderUpload ? '上传文件夹' : '上传文件', 'info',
        isFolderUpload ? `开始上传${logPrefix}，包含 ${files.length} 个文件...` : `开始上传 ${files.length} 个文件...`);

    // 显示进度条容器
    const progressContainer = document.getElementById('upload-progress-container');
    const progressList = document.getElementById('upload-progress-list');
    progressContainer.style.display = 'block';
    clearContainer(progressList);

    // 创建所有文件的进度项
    const progressItems = {};
    for (let i = 0; i < files.length; i++) {
        const file = files[i];
        const itemId = `upload-${Date.now()}-${i}`;
        const item = createProgressItem(itemId, file.name);
        progressList.appendChild(item);

        // 初始状态显示排队中
        const info = item.querySelector('.upload-progress-text');
        if (info) info.textContent = '排队中...';

        progressItems[itemId] = { file, item, progress: 0 };
    }

    // 关闭按钮事件
    const closeBtn = document.getElementById('upload-progress-close');
    const closeHandler = () => {
        progressContainer.style.display = 'none';
    };
    closeBtn.onclick = closeHandler;

    // [核心改进] 使用队列控制并发上传数量，防止浏览器卡死和服务器 503 错误
    const MAX_CONCURRENT_UPLOADS = 2;
    const itemIds = Object.keys(progressItems);
    let currentIndex = 0;

    async function uploadNext() {
        if (currentIndex >= itemIds.length) return;

        while (currentIndex < itemIds.length) {
            const itemId = itemIds[currentIndex++];
            const { file, item } = progressItems[itemId];

            try {
                const success = await uploadFile(file, isFolderUpload ? '' : currentFolderFilter, isFolderUpload, folderName, (progress) => {
                    updateProgressItem(item, progress);
                    progressItems[itemId].progress = progress;
                });

                if (success) {
                    markProgressItemSuccess(item);
                    addToCommandLog('上传文件', 'success', `${file.name} 上传成功`);
                } else {
                    markProgressItemError(item, '上传失败');
                    addToCommandLog('上传文件', 'error', `${file.name} 上传失败`);
                }
            } catch (error) {
                markProgressItemError(item, error.message);
                addToCommandLog('上传文件', 'error', `${file.name} 上传失败: ${error.message}`);
            }
        }
    }

    // 启动指定数量的并行任务
    const workers = [];
    for (let i = 0; i < Math.min(MAX_CONCURRENT_UPLOADS, itemIds.length); i++) {
        workers.push(uploadNext());
    }

    // 等待所有上传完成
    await Promise.all(workers);

    // 统计上传结果
    const successCount = Object.values(progressItems).filter(item =>
        item.item.classList.contains('success')
    ).length;
    const totalCount = Object.keys(progressItems).length;

    // 显示完成消息
    if (successCount === totalCount) {
        addToCommandLog('上传完成', 'success', `${logPrefix}上传成功 (${successCount}/${totalCount})`);
    } else {
        addToCommandLog('上传完成', 'warning', `${logPrefix}部分文件上传失败 (成功: ${successCount}/${totalCount})`);
    }

    // 上传完成后刷新文件夹列表和素材列表
    await updateFolderList();
    await updateMaterials();

    // 3秒后自动关闭进度条（如果全部成功）
    if (successCount === totalCount) {
        setTimeout(() => {
            const allSuccess = Array.from(progressList.querySelectorAll('.upload-progress-item')).every(item =>
                item.classList.contains('success')
            );
            if (allSuccess) {
                progressContainer.style.display = 'none';
            }
        }, 3000);
    }
}

// 更新素材列表
export async function updateMaterials(filter = '') {
    const listContainer = document.getElementById('material-list');
    if (!listContainer) {
        return;
    }
    if (currentMaterialType === 'image' && isFusionBackgroundFolder(currentFolderFilter)) {
        currentFolderFilter = '';
    }

    // 清理旧的DOM元素和图片资源
    const oldItems = listContainer.querySelectorAll('.material-item');
    oldItems.forEach(item => {
        // 清理图片资源
        const img = item.querySelector('img.material-thumbnail');
        if (img) {
            img.src = '';
            img.onerror = null;
            img.onload = null;
        }
        // 移除所有事件监听器（通过克隆节点来移除）
        const clone = item.cloneNode(false);
        item.parentNode?.replaceChild(clone, item);
    });

    // 保留选中状态：在渲染新列表时会根据selectedMaterials恢复选中状态
    // 不需要清空selectedMaterials，这样可以保持用户的选中状态

    listContainer.innerHTML = '<div class="material-loading">加载中...</div>';

    const folderQuery = currentFolderFilter ? `&folder=${encodeURIComponent(currentFolderFilter)}` : '';
    const materials = await apiGet(`/materials?type=${currentMaterialType}${folderQuery}`);

    // 使用工具函数清理，确保没有残留
    clearContainer(listContainer);

    // 收集当前列表中的所有路径，用于清理不在列表中的选中状态
    const currentPaths = new Set();

    if (materials && Array.isArray(materials)) {
        materials.forEach(file => {
            if (isFusionBackgroundMaterial(file)) {
                return;
            }

            // 文本搜索过滤
            if (filter && !file.name.toLowerCase().includes(filter.toLowerCase())) {
                return;
            }

            // 添加到当前路径集合
            currentPaths.add(file.path);

            const item = document.createElement('div');
            const isTranscoding = !!file.is_transcoding;
            const isIncompatible = !!file.is_incompatible;
            item.className = 'material-item' + (isTranscoding ? ' transcoding' : '') + (isIncompatible ? ' incompatible' : '');
            item.dataset.path = file.path; // 存储路径用于选中状态匹配

            // 格式化文件大小
            const sizeStr = formatFileSize(file.size);

            // 根据类型显示不同内容
            let previewContent;
            // 获取所属文件夹名称（从路径中提取）
            const folderName = extractFolderName(file.path);

            // 通用缩略图URL
            const thumbnailUrl = `${window.apiBaseUrl}/materials/thumbnail?path=${encodeURIComponent(file.path)}`;

            if (currentMaterialType === 'video') {
                // 视频：保持播放图标
                previewContent = `
                    <div class="material-preview video-preview" data-path="${encodeURIComponent(file.path)}">
                        <img class="material-thumbnail" src="${thumbnailUrl}" alt="" style="opacity:0;transition:opacity 0.2s;" onload="this.style.opacity=1" />
                        <div class="material-play-overlay" title="预览视频">
                            <svg class="play-icon" viewBox="0 0 24 24" fill="currentColor">
                                <path d="M8 5l12 7-12 7V5z"/>
                            </svg>
                        </div>
                    </div>
                `;
            } else if (currentMaterialType === 'image') {
                // 图片：换成查看（眼睛）图标
                previewContent = `
                    <div class="material-preview image-preview" data-path="${encodeURIComponent(file.path)}">
                        <img class="material-thumbnail" src="${thumbnailUrl}" alt="" style="opacity:0;transition:opacity 0.2s;" onload="this.style.opacity=1" />
                        <div class="material-play-overlay" title="查看图片">
                            <svg class="play-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path>
                                <circle cx="12" cy="12" r="3"></circle>
                            </svg>
                        </div>
                    </div>
                `;
            } else if (currentMaterialType === 'audio') {
                // 音频：强化占位图标和封面感
                previewContent = `
                    <div class="material-preview audio-preview" data-path="${encodeURIComponent(file.path)}">
                        <div class="thumbnail-placeholder audio-placeholder-bg">
                            <svg viewBox="0 0 24 24" width="44" height="44" fill="currentColor" style="opacity: 0.5">
                                <path d="M12 3v10.55c-.59-.34-1.27-.55-2-.55-2.21 0-4 1.79-4 4s1.79 4 4 4 4-1.79 4-4V7h4V3h-6z"/>
                            </svg>
                        </div>
                        <div class="material-play-overlay" title="播放音频">
                            <svg class="play-icon" viewBox="0 0 24 24" fill="currentColor">
                                <path d="M8 5l12 7-12 7V5z"/>
                            </svg>
                        </div>
                    </div>
                `;
            }

            item.innerHTML = `
                ${previewContent}
                <div class="material-actions-left">
                    <button class="btn small add-to-playlist-btn" title="添加到节目列表">
                        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="5" x2="12" y2="19"></line><line x1="5" y1="12" x2="19" y2="12"></line></svg>
                    </button>
                </div>
                <div class="material-info">
                    <div class="material-name" title="${file.name}">${file.name}</div>
                    <div class="material-meta">
                        <span class="material-size">${sizeStr}</span>
                        ${folderName ? `<span class="material-folder" title="所属文件夹: ${folderName}">
                            <svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor" style="margin-right:2px; vertical-align:middle; opacity:0.8;"><path d="M10 4H4c-1.1 0-1.99.9-1.99 2L2 18c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z"/></svg> 
                            ${folderName}</span>` : ''}
                    </div>
                </div>
                <div class="material-actions">
                    <button class="btn small download-material-btn" title="下载到本地">
                        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path><polyline points="7 10 12 15 17 10"></polyline><line x1="12" y1="15" x2="12" y2="3"></line></svg>
                    </button>
                    <button class="btn small rename-material-btn" title="重命名">
                        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"></path><path d="M18.5 2.5a2.121 2.121 0 1 1 3 3L12 15l-4 1 1-4 9.5-9.5z"></path></svg>
                    </button>
                    <button class="btn small danger delete-material-btn" title="删除">
                        <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path><line x1="10" y1="11" x2="10" y2="17"></line><line x1="14" y1="11" x2="14" y2="17"></line></svg>
                    </button>
                </div>
                ${isTranscoding ? `
                <div class="transcoding-overlay">
                    <div class="transcoding-spinner"></div>
                    <div class="transcoding-text">转码中...</div>
                </div>
                ` : ''}
                ${isIncompatible ? `
                <div class="incompatible-badge" title="${file.incompatible_reason ? '格式不兼容: ' + file.incompatible_reason : '格式不兼容，可能无法正常播放'}">
                    <svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-2h2v2zm0-4h-2V7h2v6z"/></svg>
                    不兼容
                </div>
                ` : ''}
            `;

            // 简化的缩略图错误处理：加载失败直接显示占位图标
            // SSE监听会在缩略图生成完成后自动更新显示
            const thumbnail = item.querySelector('img.material-thumbnail');
            if (thumbnail) {
                thumbnail.onerror = function () {
                    // 加载失败，隐藏img元素并显示占位图标
                    this.style.display = 'none';
                    const preview = this.closest('.material-preview');
                    if (preview && !preview.querySelector('.thumbnail-placeholder')) {
                        const placeholder = document.createElement('div');
                        placeholder.className = 'thumbnail-placeholder';
                        let icon = '🖼️';
                        if (currentMaterialType === 'video') icon = '🎬';
                        else if (currentMaterialType === 'audio') icon = '🎵';
                        placeholder.innerHTML = icon;
                        preview.appendChild(placeholder);
                    }
                };
            }

            // 为视频预览区域（播放图标）添加点击事件
            if (currentMaterialType === 'video') {
                const playOverlay = item.querySelector('.material-play-overlay');
                const videoPreview = item.querySelector('.video-preview');

                if (playOverlay && videoPreview) {
                    let playTimer = null;
                    const playHandler = async function (e) {
                        e.stopPropagation();

                        // 防抖处理：300ms 内的连续点击只保留最后一次
                        if (playTimer) clearTimeout(playTimer);
                        playTimer = setTimeout(async () => {
                            const filePath = decodeURIComponent(videoPreview.dataset.path);

                            // 验证文件路径
                            if (!filePath || filePath === 'undefined' || filePath === 'null') {
                                showError('播放失败: 无效的文件路径');
                                return;
                            }

                            // 检查预览开关状态
                            const previewSwitch = document.getElementById('video-preview-switch');
                            const isPreviewEnabled = previewSwitch && previewSwitch.checked;

                            // 优先使用当前播放列表绑定的图层，否则使用选中的图层
                            const playlistId = getCurrentPlaylistId();
                            let layerId = 1;
                            if (playlistId) {
                                const targetLayer = getPlaylistTargetLayer(playlistId);
                                if (targetLayer && targetLayer >= 1 && targetLayer <= 4) {
                                    layerId = targetLayer;
                                }
                            } else {
                                let selectedLayerId = (typeof getSelectedLayer === 'function' ? getSelectedLayer() : null);
                                layerId = (selectedLayerId && selectedLayerId >= 1 && selectedLayerId <= 4) ? selectedLayerId : 1;
                            }

                            // 确保 layerId 是有效数字
                            layerId = Number(layerId);
                            if (isNaN(layerId) || layerId < 1 || layerId > 4) {
                                layerId = 1;  // 默认使用图层1
                            }

                            try {
                                const playResult = await sendLayerCommand(layerId, 'play', { path: filePath, loop: 2 });
                                if (playResult === null || playResult === undefined) {
                                    showError('播放失败: 请求失败');
                                    return;
                                }
                                if (isSoftBlockedPlaybackResult(playResult)) {
                                    const blockedMsg = getPlaybackResultMessage(playResult);
                                    showWarning(`播放受限：${blockedMsg}`);
                                    addToCommandLog('播放视频', 'warning', blockedMsg);
                                    return;
                                }
                                // 设置为预览模式
                                setPreviewMode(filePath, layerId);
                                if (isPreviewEnabled) {
                                    openVideoPreview(filePath, file.name, layerId);
                                }
                            } catch (error) {
                                showError(`播放失败: ${error.message}`);
                            } finally {
                                playTimer = null;
                            }
                        }, 300);
                    };
                    playOverlay.addEventListener('click', playHandler);
                }
            }

            // 为音频预览区域（播放图标）添加点击事件
            if (currentMaterialType === 'audio') {
                const playOverlay = item.querySelector('.material-play-overlay');
                const audioPreview = item.querySelector('.audio-preview');

                if (playOverlay && audioPreview) {
                    const playHandler = async function (e) {
                        e.stopPropagation();
                        const filePath = decodeURIComponent(audioPreview.dataset.path);

                        // 优先使用当前播放列表绑定的图层，否则使用选中的图层
                        const playlistId = getCurrentPlaylistId();
                        let layerId = 1;
                        if (playlistId) {
                            layerId = getPlaylistTargetLayer(playlistId);
                        } else {
                            let selectedLayerId = (typeof getSelectedLayer === 'function' ? getSelectedLayer() : null);
                            layerId = (selectedLayerId && selectedLayerId >= 1 && selectedLayerId <= 4) ? selectedLayerId : 1;
                        }

                        try {
                            const playResult = await sendLayerCommand(layerId, 'play', { path: filePath, loop: 2 });
                            if (playResult === null || playResult === undefined) {
                                showError('播放失败: 请求失败');
                                return;
                            }
                            if (isSoftBlockedPlaybackResult(playResult)) {
                                const blockedMsg = getPlaybackResultMessage(playResult);
                                showWarning(`播放受限：${blockedMsg}`);
                                addToCommandLog('播放音频', 'warning', blockedMsg);
                                return;
                            }
                            // 设置为预览模式
                            setPreviewMode(filePath, layerId);
                        } catch (error) {
                            showError(`播放失败: ${error.message}`);
                        }
                    };
                    playOverlay.addEventListener('click', playHandler);
                }
            }

            // 为图片预览区域（播放图标）添加点击事件
            if (currentMaterialType === 'image') {
                const playOverlay = item.querySelector('.material-play-overlay');
                const imagePreview = item.querySelector('.image-preview');

                if (playOverlay && imagePreview) {
                    const playHandler = async function (e) {
                        e.stopPropagation();
                        const filePath = decodeURIComponent(imagePreview.dataset.path);

                        try {
                            await sendImagePlayCommand(filePath, file.name);
                        } catch (error) {
                            showError(`播放失败: ${error.message}`);
                        }
                    };
                    playOverlay.addEventListener('click', playHandler);
                }
            }

            // 点击素材的操作
            item.addEventListener('click', (e) => {
                // 如果点击的是按钮或其子元素，不要触发选择
                if (e.target.closest('.delete-material-btn') ||
                    e.target.closest('.rename-material-btn') ||
                    e.target.closest('.add-to-playlist-btn') ||
                    e.target.closest('.download-material-btn') ||
                    e.target.closest('.material-play-overlay') ||
                    e.target.closest('.material-actions') ||
                    e.target.closest('.material-actions-left')) {
                    return;
                }

                // 单选模式：点击一个素材时，取消其他所有选中，只选中当前这个
                const itemPath = item.dataset.path || file.path;
                const isSelected = selectedMaterials.has(itemPath);

                if (isSelected) {
                    // 如果已经选中，再次点击则取消选中
                    selectedMaterials.delete(itemPath);
                    item.classList.remove('selected');
                    addToCommandLog('选择素材', 'info', `取消选择素材: ${file.name}`);
                } else {
                    // 取消所有其他选中
                    selectedMaterials.clear();
                    document.querySelectorAll('.material-item.selected').forEach(el => {
                        el.classList.remove('selected');
                    });

                    // 选中当前项
                    selectedMaterials.add(itemPath);
                    item.classList.add('selected');
                    addToCommandLog('选择素材', 'info', `选择素材: ${file.name}`);
                }
                updateSelectedMaterialsToolbar();
            });

            // 初始化选中状态 - 在添加事件监听器之前设置
            // 使用精确路径匹配
            if (selectedMaterials.has(file.path)) {
                item.classList.add('selected');
            }

            // 添加到节目列表按钮事件
            const addToPlaylistBtn = item.querySelector('.add-to-playlist-btn');
            addToPlaylistBtn.addEventListener('click', async (e) => {
                e.stopPropagation();
                const playlistId = getCurrentPlaylistId();
                if (!playlistId) {
                    showInfo('请先选择一个播放列表');
                    return;
                }
                // 图片固定使用图层 60，视频/音频使用播放列表绑定的目标图层
                const layerIdForAdd = currentMaterialType === 'image' ? 60 : null;
                const success = await addVideoToPlaylist(playlistId, file.path, layerIdForAdd);
                if (success) {
                    addToCommandLog('添加素材', 'success', `添加素材到列表: ${file.name}`);
                    // 刷新播放列表显示
                    await updatePlaylistContent(playlistId);
                }
            });

            // 下载按钮事件
            const downloadBtn = item.querySelector('.download-material-btn');
            downloadBtn.addEventListener('click', async (e) => {
                e.stopPropagation();
                await downloadMaterial(file.path, file.name);
            });

            // 重命名按钮事件
            const renameBtn = item.querySelector('.rename-material-btn');
            renameBtn.addEventListener('click', async (e) => {
                e.stopPropagation();
                showRenameDialog(file.name, async (newName) => {
                    // 未输入后缀时自动补全原文件后缀
                    let finalName = newName;
                    const dotIndex = file.name.lastIndexOf('.');
                    if (dotIndex > 0) {
                        const ext = file.name.slice(dotIndex);
                        if (!finalName.toLowerCase().endsWith(ext.toLowerCase())) {
                            finalName = finalName + ext;
                        }
                    }
                    const success = await renameMaterial(file.path, finalName);
                    if (success) {
                        addToCommandLog('重命名', 'success', `成功重命名: ${file.name} -> ${finalName}`);
                        // 重命名后切换到「全部」并刷新，确保列表显示（避免因路径/文件夹过滤导致不显示）
                        currentFolderFilter = '';
                        updateFolderTagsActive();
                        updateFolderActionsToolbar();
                        await updateFolderList();
                        await updateMaterials(document.getElementById('material-search')?.value || '');
                    } else {
                        addToCommandLog('重命名', 'error', `重命名失败: ${file.name}`);
                        showInfo('重命名失败，请检查文件名是否合法或权限是否足够');
                    }
                });
            });

            // 删除按钮事件
            const deleteBtn = item.querySelector('.delete-material-btn');
            deleteBtn.addEventListener('click', async (e) => {
                e.stopPropagation();
                const escapedFileName = escapeHtml(file.name);
                const confirmed = await showConfirm(
                    `确定要删除 "${escapedFileName}" 吗？`,
                    '删除素材',
                    true  // 标记需要显示进度，确认后不立即关闭对话框
                );

                if (confirmed) {
                    // 对话框仍然打开，现在显示删除进度
                    const dialog = document.getElementById('confirm-dialog');
                    if (dialog) {
                        dialog.querySelector('#confirm-message').textContent = `正在删除素材 "${escapedFileName}"...`;
                        dialog.querySelector('h3').textContent = '删除素材';
                        updateConfirmProgress('正在删除素材...', true);
                    }

                    try {
                        // 调用删除API
                        const deleteSuccess = await deleteMaterial(file.path);

                        if (deleteSuccess) {
                            // 轮询检查素材是否已删除
                            updateConfirmProgress('正在确认删除结果...', true);
                            let deleted = false;
                            let attempts = 0;
                            const maxAttempts = 20; // 最多检查20次，每次500ms，总共10秒

                            while (!deleted && attempts < maxAttempts) {
                                await new Promise(resolve => setTimeout(resolve, 500));
                                attempts++;

                                try {
                                    const materials = await apiGet(`/materials?type=${currentMaterialType}`);
                                    const materialExists = materials && Array.isArray(materials) &&
                                        materials.some(m => m.path === file.path);

                                    if (!materialExists) {
                                        deleted = true;
                                        break;
                                    }

                                    // 更新进度提示
                                    updateConfirmProgress(`正在删除... (${attempts}/${maxAttempts})`, true);
                                } catch (error) {
                                    // 如果API调用失败，继续尝试
                                }
                            }

                            if (deleted || attempts >= maxAttempts) {
                                updateConfirmProgress(deleted ? '删除成功！' : '删除操作已完成', true);
                                await new Promise(resolve => setTimeout(resolve, 500));

                                // 关闭对话框
                                const confirmDialog = document.getElementById('confirm-dialog');
                                if (confirmDialog) {
                                    confirmDialog.classList.remove('active');
                                    updateConfirmProgress('', false);
                                }

                                addToCommandLog('删除素材', 'success', `成功删除: ${file.name}`);
                                // 刷新文件夹列表和素材列表
                                await updateFolderList();
                                await updateMaterials();
                                await updatePlaylistList();
                                const playlistId = getCurrentPlaylistId();
                                if (playlistId) {
                                    await updatePlaylistContent(playlistId);
                                }
                            }
                        } else {
                            // 删除失败
                            updateConfirmProgress('删除失败', true);
                            await new Promise(resolve => setTimeout(resolve, 1000));

                            // 关闭对话框
                            const confirmDialog = document.getElementById('confirm-dialog');
                            if (confirmDialog) {
                                confirmDialog.classList.remove('active');
                                updateConfirmProgress('', false);
                            }

                            addToCommandLog('删除素材', 'error', `删除失败: ${file.name}`);
                            showInfo('删除素材失败，请检查文件是否被占用或权限是否足够');
                        }
                    } catch (error) {
                        // 发生错误
                        updateConfirmProgress('删除过程中发生错误', true);
                        await new Promise(resolve => setTimeout(resolve, 1000));

                        // 关闭对话框
                        const confirmDialog = document.getElementById('confirm-dialog');
                        if (confirmDialog) {
                            confirmDialog.classList.remove('active');
                            updateConfirmProgress('', false);
                        }

                        addToCommandLog('删除素材', 'error', `删除失败: ${error.message}`);
                        showInfo('删除素材失败: ' + error.message);
                    }
                }
            });

            listContainer.appendChild(item);
        });

        // 清理不在当前列表中的选中状态（避免选中状态残留）
        selectedMaterials.forEach(path => {
            if (!currentPaths.has(path)) {
                selectedMaterials.delete(path);
            }
        });
        updateSelectedMaterialsToolbar();

        if (listContainer.children.length === 0) {
            listContainer.innerHTML = '<div class="material-empty">暂无素材</div>';
        }
    }
}

// 格式化文件大小
function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

// 创建进度项
function createProgressItem(id, fileName) {
    const item = document.createElement('div');
    item.className = 'upload-progress-item';
    item.id = id;
    // 转义文件名以防止XSS攻击
    const escapedFileName = escapeHtml(fileName);
    item.innerHTML = `
        <div class="upload-progress-item-name" title="${escapedFileName}">${escapedFileName}</div>
        <div class="upload-progress-bar-container">
            <div class="upload-progress-bar" style="width: 0%"></div>
        </div>
        <div class="upload-progress-status">
            <span class="upload-progress-percent">0%</span>
            <span class="upload-progress-text">准备中...</span>
        </div>
    `;
    return item;
}

// 更新进度项
function updateProgressItem(item, progress) {
    const bar = item.querySelector('.upload-progress-bar');
    const percent = item.querySelector('.upload-progress-percent');
    const text = item.querySelector('.upload-progress-text');

    if (bar) bar.style.width = `${progress}%`;
    if (percent) percent.textContent = `${Math.round(progress)}%`;
    if (text) {
        if (progress < 100) {
            text.textContent = '上传中...';
        } else {
            text.textContent = '完成';
        }
    }
}

// 标记进度项为成功
function markProgressItemSuccess(item) {
    item.classList.add('success');
    const bar = item.querySelector('.upload-progress-bar');
    const text = item.querySelector('.upload-progress-text');
    if (bar) bar.style.width = '100%';
    if (text) text.textContent = '上传成功';
    const percent = item.querySelector('.upload-progress-percent');
    if (percent) percent.textContent = '100%';
}

// 标记进度项为错误
function markProgressItemError(item, errorMsg) {
    item.classList.add('error');
    const text = item.querySelector('.upload-progress-text');
    if (text) text.textContent = errorMsg || '上传失败';
}

// 上传文件（支持进度回调）
async function uploadFile(file, targetFolder, isFolderUpload, folderName, onProgress) {
    return new Promise((resolve, reject) => {
        try {
            // 文件夹上传时，使用当前选中的标签类型；单文件上传时，根据文件类型判断
            let type;
            if (isFolderUpload) {
                // 文件夹上传：使用当前选中的素材类型
                type = currentMaterialType;
            } else {
                // 单文件上传：根据文件类型判断
                const isVideo = file.type.startsWith('video/');
                const isImage = file.type.startsWith('image/');
                const isAudio = file.type.startsWith('audio/');

                if (!isVideo && !isImage && !isAudio) {
                    addToCommandLog('上传文件', 'error', `不支持的文件类型: ${file.type}`);
                    resolve(false);
                    return;
                }

                if (isVideo) {
                    type = 'video';
                } else if (isImage) {
                    type = 'image';
                } else if (isAudio) {
                    type = 'audio';
                }
            }

            // 构建保存路径（兼容Edge浏览器）
            let relativePath = '';
            if (isFolderUpload) {
                // 文件夹上传：使用相对路径，去掉文件夹名，保留子目录结构
                // 兼容Edge：优先使用webkitRelativePath，回退到name属性
                const filePath = file.webkitRelativePath || file.name || '';
                if (filePath) {
                    const pathParts = filePath.split('/');
                    if (pathParts.length > 1) {
                        // 去掉第一部分的文件夹名，保留后续路径
                        relativePath = pathParts.slice(1).join('/');
                    } else {
                        relativePath = file.name;
                    }
                } else {
                    relativePath = file.name;
                }
            } else {
                // 单个文件上传：保存到当前文件夹
                relativePath = file.name;
            }

            // 构建查询参数
            const params = new URLSearchParams({
                filename: relativePath,
                type: type
            });

            // 如果是单个文件，传递目标文件夹
            if (!isFolderUpload && targetFolder) {
                params.append('targetFolder', targetFolder);
            }

            // 如果是文件夹上传，传递文件夹名（作为同级目录）
            if (isFolderUpload && folderName) {
                params.append('folderName', folderName);
            }

            const url = `/materials/upload?${params.toString()}`;

            const xhr = new XMLHttpRequest();

            // 监听上传进度
            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable && onProgress) {
                    const percent = (e.loaded / e.total) * 100;
                    // 确保进度不超过100%
                    const finalPercent = Math.min(percent, 100);
                    onProgress(finalPercent);
                }
            });

            // 监听完成
            xhr.addEventListener('load', () => {
                if (xhr.status >= 200 && xhr.status < 300) {
                    try {
                        // 确保进度条显示100%
                        if (onProgress) {
                            onProgress(100);
                        }

                        const envelope = parseApiResponse(JSON.parse(xhr.responseText));
                        if (!envelope.ok) {
                            const errorMsg = envelope.error?.message || '上传失败';
                            addToCommandLog('上传文件', 'error', `${file.name}: ${errorMsg}`);
                            resolve(false);
                            return;
                        }

                        const warning = envelope.data?.warning;
                        if (warning) {
                            addToCommandLog('上传文件', 'warning',
                                `${file.name}: 已上传，但格式可能不兼容 — ${warning.message}`);
                            showWarning(`文件已上传，但可能无法正常播放\n\n${file.name}\n\n${warning.message}`);
                        } else {
                            addToCommandLog('上传文件', 'success', `文件上传成功: ${file.name}`);
                        }

                        resolve(true);
                    } catch (e) {
                        // 确保进度条显示100%
                        if (onProgress) {
                            onProgress(100);
                        }
                        addToCommandLog('上传文件', 'error', `解析响应失败: ${e.message}`);
                        resolve(false);
                    }
                } else {
                    // 确保进度条显示100%
                    if (onProgress) {
                        onProgress(100);
                    }

                    let errorMessage = `HTTP错误: ${xhr.status}`;
                    try {
                        const envelope = parseApiResponse(JSON.parse(xhr.responseText));
                        const code = envelope.error?.code || '';
                        errorMessage = envelope.error?.message || errorMessage;
                        if (code === 'UNSUPPORTED_10BIT_VIDEO' || code === '10BIT') {
                            showError(`不支持的视频格式 (10-bit): ${file.name}\n\n${errorMessage}`);
                        } else if (code === 'UNSUPPORTED_DXV_VIDEO' || code === 'UNSUPPORTED_PRO_CODEC' || code === 'CODEC_TAG_MATCH' || code === 'METADATA_TAG') {
                            showError(`不支持的专业视频编码格式: ${file.name}\n\n${errorMessage}`);
                        } else if (code === 'COLOR_CONFIG') {
                            showError(`不支持的视频色彩配置 (BT2020/高色度): ${file.name}\n\n${errorMessage}`);
                        } else if (code === 'PRO_ENCODER_STRICT') {
                            showError(`不支持的专业编码器输出格式: ${file.name}\n\n${errorMessage}`);
                        } else if (code === 'FILENAME_KEYWORD') {
                            showError(`文件名包含不支持的格式关键字: ${file.name}\n\n${errorMessage}`);
                        } else if (xhr.status === 415) {
                            showError(`不支持的视频格式: ${file.name}\n\n${errorMessage}`);
                        }
                    } catch (error) {
                        // 非 JSON 或不符合 contract 时使用 HTTP 状态消息。
                    }

                    addToCommandLog('上传文件', 'error', `${file.name}: ${errorMessage}`);
                    resolve(false);
                }
            });

            // 监听错误
            xhr.addEventListener('error', () => {
                if (onProgress) {
                    onProgress(100);
                }
                addToCommandLog('上传文件', 'error', `网络错误: ${file.name}`);
                resolve(false);
            });

            // 监听中止
            xhr.addEventListener('abort', () => {
                if (onProgress) {
                    onProgress(100);
                }
                addToCommandLog('上传文件', 'error', `上传被取消: ${file.name}`);
                resolve(false);
            });

            // 开始上传
            xhr.open('POST', window.apiBaseUrl + url);
            xhr.setRequestHeader('Content-Type', 'application/octet-stream');
            xhr.send(file);

        } catch (error) {
            addToCommandLog('上传文件', 'error', `上传失败: ${error.message}`);
            resolve(false);
        }
    });
}

// 删除素材
async function deleteMaterial(filePath) {
    try {
        const result = await apiPost('/materials/delete', { path: filePath });
        return result !== null && result !== undefined;
    } catch (error) {
        // 删除素材失败
        addToCommandLog('删除素材', 'error', `删除失败: ${error.message}`);
        return false;
    }
}

// 重命名文件或文件夹
async function renameMaterial(oldPath, newName) {
    try {
        return await handleApiOperation(
            apiPost('/materials/rename', { path: oldPath, newName: newName }),
            '重命名',
            '重命名成功',
            '重命名失败'
        );
    } catch (error) {
        addToCommandLog('重命名', 'error', `重命名失败: ${error.message}`);
        return false;
    }
}

// 删除文件夹
async function deleteFolder(folderPath) {
    try {
        return await handleApiOperation(
            apiPost('/materials/delete_folder', { folderPath: folderPath }),
            '删除文件夹',
            '删除文件夹成功',
            '删除文件夹失败'
        );
    } catch (error) {
        addToCommandLog('删除文件夹', 'error', `删除失败: ${error.message}`);
        return false;
    }
}

// 下载素材到本地电脑 - 使用浏览器原生"另存为"对话框
async function downloadMaterial(filePath, fileName) {
    try {
        addToCommandLog('下载素材', 'info', `开始下载: ${fileName}`);
        showInfo(`正在准备下载: ${fileName}`);

        // 通过后端API获取文件，触发浏览器下载
        const downloadUrl = `${window.apiBaseUrl}/materials/download_file?path=${encodeURIComponent(filePath)}`;

        // 使用fetch获取文件，然后触发下载（这样可以显示原生另存为对话框）
        const response = await fetch(downloadUrl);

        if (!response.ok) {
            throw new Error(`下载失败: ${response.status}`);
        }

        // 获取文件blob
        const blob = await response.blob();

        // 创建下载链接
        const url = window.URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = url;
        link.download = fileName;

        // 触发下载
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);

        // 清理URL对象
        window.URL.revokeObjectURL(url);

        addToCommandLog('下载素材', 'success', `下载已开始: ${fileName}`);
        showInfo(`下载已开始，请在浏览器中选择保存位置`);

        return true;
    } catch (error) {
        addToCommandLog('下载素材', 'error', `下载失败: ${error.message}`);
        showError(`下载失败: ${error.message}`);
        return false;
    }
}

// 显示重命名对话框
async function showRenameDialog(currentName, onConfirm) {
    const newName = await showPrompt('请输入新名称:', currentName, '重命名');
    if (newName !== null && newName.trim() !== '') {
        const trimmedName = newName.trim();
        // 检查名称是否包含非法字符
        if (trimmedName.includes('/') || trimmedName.includes('\\')) {
            showInfo('名称不能包含 / 或 \\ 字符');
            return;
        }
        if (trimmedName === currentName) {
            return; // 名称未改变
        }
        onConfirm(trimmedName);
    }
}

async function addVideoToPlaylist(playlistId, filePath, layerId = null) {
    try {
        if (isFusionBackgroundMaterialPath(filePath)) {
            const message = 'gb_fusion 是融合背景目录，不能加入节目列表';
            showWarning(message);
            addToCommandLog('添加到播放列表', 'warning', message);
            return false;
        }

        // 如果没有指定 layerId，则使用播放列表绑定的图层
        const effectiveLayerId = layerId || getPlaylistTargetLayer(playlistId);

        if (!effectiveLayerId) {
            addToCommandLog('添加到播放列表', 'error', '无法确定目标图层，请先绑定播放列表图层');
            return false;
        }

        if (Number(effectiveLayerId) === 60 && !isImageFilePath(filePath)) {
            const fileName = String(filePath || '').split(/[\\/]/).pop() || filePath;
            const message = `图层60是图片图层，不能加入视频/音频文件：${fileName}`;
            showWarning(message);
            addToCommandLog('添加到播放列表', 'warning', message);
            return false;
        }

        if (Number(effectiveLayerId) >= 1 && Number(effectiveLayerId) <= 4 && isImageFilePath(filePath)) {
            const fileName = String(filePath || '').split(/[\\/]/).pop() || filePath;
            const message = `图层${effectiveLayerId}是视频图层，不能加入图片文件：${fileName}`;
            showWarning(message);
            addToCommandLog('添加到播放列表', 'warning', message);
            return false;
        }

        return await handleApiOperation(
            apiPost(`/playlists/${playlistId}/items?layerId=${effectiveLayerId}`, {
                path: filePath,
                layerId: effectiveLayerId
            }),
            '添加到播放列表',
            '添加到播放列表成功',
            '添加到播放列表失败'
        );
    } catch (error) {
        // 添加到播放列表失败
        addToCommandLog('添加到播放列表', 'error', `添加失败: ${error.message}`);
        return false;
    }
}

// 递归读取 FileSystemEntry，返回 File[]（保留相对路径到 webkitRelativePath）
function readEntryRecursive(entry, basePath) {
    return new Promise((resolve) => {
        if (entry.isFile) {
            entry.file((file) => {
                // 手动补上 webkitRelativePath（拖拽时浏览器不会自动填充）
                Object.defineProperty(file, 'webkitRelativePath', {
                    value: basePath ? `${basePath}/${file.name}` : file.name,
                    writable: false,
                });
                resolve([file]);
            }, () => resolve([]));
        } else if (entry.isDirectory) {
            const reader = entry.createReader();
            const allFiles = [];
            const readBatch = () => {
                reader.readEntries(async (entries) => {
                    if (entries.length === 0) {
                        resolve(allFiles);
                        return;
                    }
                    for (const e of entries) {
                        const childPath = basePath ? `${basePath}/${e.name}` : e.name;
                        const files = await readEntryRecursive(e, childPath);
                        allFiles.push(...files);
                    }
                    readBatch(); // readEntries 每次最多返回 100 条，需要循环读完
                }, () => resolve(allFiles));
            };
            readBatch();
        } else {
            resolve([]);
        }
    });
}

// 初始化拖拽上传（绕开 webkitdirectory 原生确认框）
function initializeDragUpload() {
    const listContainer = document.getElementById('material-list');
    if (!listContainer) return;

    let dragCounter = 0; // 用计数器避免子元素 dragenter/dragleave 抖动

    listContainer.addEventListener('dragenter', (e) => {
        e.preventDefault();
        dragCounter++;
        listContainer.classList.add('drag-over');
    });

    listContainer.addEventListener('dragleave', () => {
        dragCounter--;
        if (dragCounter <= 0) {
            dragCounter = 0;
            listContainer.classList.remove('drag-over');
        }
    });

    listContainer.addEventListener('dragover', (e) => {
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
    });

    listContainer.addEventListener('drop', async (e) => {
        e.preventDefault();
        dragCounter = 0;
        listContainer.classList.remove('drag-over');

        const items = Array.from(e.dataTransfer.items || []);
        if (items.length === 0) return;

        // 收集所有文件，区分是否为文件夹拖入
        const allFiles = [];
        let isFolderDrop = false;
        let folderName = '';

        for (const item of items) {
            if (item.kind !== 'file') continue;
            const entry = item.webkitGetAsEntry ? item.webkitGetAsEntry() : null;
            if (!entry) {
                // 降级：直接用 getAsFile()
                const file = item.getAsFile();
                if (file) allFiles.push(file);
                continue;
            }

            if (entry.isDirectory) {
                isFolderDrop = true;
                if (!folderName) folderName = entry.name;
                const files = await readEntryRecursive(entry, entry.name);
                allFiles.push(...files);
            } else {
                const files = await readEntryRecursive(entry, '');
                allFiles.push(...files);
            }
        }

        if (allFiles.length === 0) return;

        await handleFileUpload(allFiles, isFolderDrop, folderName,
            isFolderDrop ? `文件夹 "${folderName}"` : '所有文件');
    });
}

// ========== 批量优化功能 ==========
let isBatchTranscoding = false;
let lastBatchProgressLogKey = '';

function clampProgress(value) {
    const numberValue = Number(value);
    if (!Number.isFinite(numberValue)) return 0;
    return Math.max(0, Math.min(100, numberValue));
}

function getPathFileName(filePath) {
    const parts = String(filePath || '').split(/[\\/]/);
    return parts[parts.length - 1] || '未知文件';
}

function formatBatchPosition(data) {
    const current = Number(data.current) || 0;
    const total = Number(data.total) || 0;
    if (current > 0 && total > 0) {
        return `${current}/${total}`;
    }
    return total > 0 ? `0/${total}` : '-/-';
}

function getBatchOverallProgress(data) {
    const current = Number(data.current) || 0;
    const total = Number(data.total) || 0;
    const fileProgress = clampProgress(data.progress);
    if (current > 0 && total > 0) {
        return clampProgress(((current - 1) + fileProgress / 100) / total * 100);
    }
    return fileProgress;
}

function updateBatchTranscodeButton(data) {
    const btn = document.getElementById('batch-transcode-btn');
    if (!btn) return;

    if (!data || data.status === 'batch_complete') {
        btn.textContent = '🎬 批量优化';
        return;
    }

    if (data.status === 'batch_start') {
        btn.textContent = `批量优化 ${formatBatchPosition(data)}...`;
        return;
    }

    if (data.status === 'batch_progress') {
        const fileProgress = Math.round(clampProgress(data.progress));
        btn.textContent = `批量优化 ${formatBatchPosition(data)} ${fileProgress}%`;
    }
}

function handleBatchTranscodeStatus(data) {
    if (!data || !data.status) return;

    if (data.status === 'batch_start') {
        isBatchTranscoding = true;
        lastBatchProgressLogKey = '';
        disableAllButtons();
        updateBatchTranscodeButton(data);
        updateTranscodeProgressPanel(
            `批量优化 ${formatBatchPosition(data)}`,
            getBatchOverallProgress({ ...data, progress: 0 }),
            `检查: ${getPathFileName(data.path)}`
        );
        addToCommandLog('批量优化', 'info', `[${formatBatchPosition(data)}] 检查: ${getPathFileName(data.path)}`);
        return;
    }

    if (data.status === 'batch_progress') {
        isBatchTranscoding = true;
        disableAllButtons();
        updateBatchTranscodeButton(data);

        const fileProgress = Math.round(clampProgress(data.progress));
        const overallProgress = Math.round(getBatchOverallProgress(data));
        const detail = `${getPathFileName(data.path)}${data.message ? ' - ' + data.message : ''}`;
        updateTranscodeProgressPanel(`批量优化 ${formatBatchPosition(data)}`, overallProgress, detail);
        const progressBucket = Math.floor(fileProgress / 5);
        const logKey = `${data.current || 0}:${progressBucket}:${data.message || ''}`;
        if (logKey !== lastBatchProgressLogKey) {
            lastBatchProgressLogKey = logKey;
            const message = data.message ? ` - ${data.message}` : '';
            addToCommandLog(
                '批量优化',
                'info',
                `[${formatBatchPosition(data)}] ${fileProgress}%（总进度 ${overallProgress}%）${message}`
            );
        }
        return;
    }

    if (data.status === 'batch_complete') {
        isBatchTranscoding = false;
        transcodeStatusExpectTerminal = false;
        lastBatchProgressLogKey = '';
        enableAllButtons();
        updateBatchTranscodeButton(data);

        const total = Number(data.total) || 0;
        const completed = Number(data.completed) || 0;
        const failed = Number(data.failed) || 0;
        const skipped = Number(data.skipped) || 0;
        const type = failed > 0 ? 'warning' : 'success';
        const summary = completed === 0 && skipped > 0 && failed === 0
            ? `无需处理：${skipped}/${total} 个已优化，已跳过`
            : `完成：处理 ${completed}/${total}，跳过 ${skipped}，失败 ${failed}`;
        updateTranscodeProgressPanel('批量优化完成', 100, summary, true);
        addToCommandLog('批量优化', type, summary);
        showInfo(`批量优化完成\n${summary}`, 5000);
        setTimeout(() => updateMaterials(document.getElementById('material-search')?.value || ''), 1000);
        stopTranscodeStatusPolling();
    }
}

function handleTranscodeStatusPayload(data) {
    if (!data || !data.status) return;

    if (isActiveTranscodeEvent(data)) {
        transcodeStatusExpectTerminal = true;
        startTranscodeStatusPolling(false);
    }

    if (data.status === 'batch_start' || data.status === 'batch_progress' || data.status === 'batch_complete') {
        const batchSequence = Number(data.sequence) || 0;
        if (batchSequence > 0 && batchSequence <= lastHandledBatchSequence && data.status === 'batch_complete') {
            return;
        }
        handleBatchTranscodeStatus(data);
        if (batchSequence > 0) {
            lastHandledBatchSequence = Math.max(lastHandledBatchSequence, batchSequence);
        }
    }

    if (data.path) {
        const itemStatus = data.status === 'batch_start' ? 'started' :
            data.status === 'batch_progress' ? 'progress' : data.status;
        if (itemStatus !== 'batch_complete') {
            updateTranscodeStateForPath(data.path, itemStatus, data.progress, data.message, data.encoder);
        }

        if (isBatchTranscoding) {
            return;
        }

        if (data.status === 'started') {
            updateTranscodeProgressPanel('重新编码', 0, `开始处理: ${getPathFileName(data.path)}`);
        } else if (data.status === 'progress') {
            updateTranscodeProgressPanel(
                '重新编码',
                data.progress,
                `${getPathFileName(data.path)}${data.message ? ' - ' + data.message : ''}`
            );
        } else if (data.status === 'finished') {
            updateTranscodeProgressPanel('重新编码完成', 100, getPathFileName(data.path), true);
        } else if (data.status === 'failed') {
            updateTranscodeProgressPanel('重新编码失败', 100, data.error || getPathFileName(data.path), true);
        }
    }
}

async function batchTranscodeVideos() {
    if (isBatchTranscoding) {
        showWarning('批量优化正在进行中，请等待完成');
        return;
    }

    const confirmed = await showConfirm(
        '批量优化会检查所有视频，仅对未优化的视频执行处理。\n' +
        '已优化的视频会自动跳过，不会重复编码。\n\n' +
        '处理期间将禁用上传和其他操作。\n如果有未优化的大视频，可能需要较长时间。\n\n是否继续？'
    );

    if (!confirmed) return;

    try {
        isBatchTranscoding = true;
        disableAllButtons();
        updateTranscodeProgressPanel('批量优化', 0, '正在提交检查任务...');

        const response = await apiPost('/video/transcode_batch', {});

        if (response !== null && response !== undefined) {
            transcodeStatusExpectTerminal = true;
            updateTranscodeProgressPanel('批量优化', 0, '正在检查视频优化状态...');
            startTranscodeStatusPolling(true);
            setTimeout(() => pollTranscodeStatusSnapshot(), 300);
            setTimeout(() => pollTranscodeStatusSnapshot(), 1200);
            showInfo('批量优化已启动：已优化视频会自动跳过');
            addToCommandLog('批量优化', 'success', '已启动检查');
        } else {
            isBatchTranscoding = false;
            enableAllButtons();
            updateTranscodeProgressPanel('批量优化启动失败', 100, '后端未接受检查任务', true);
            showError('批量优化启动失败');
        }
    } catch (error) {
        isBatchTranscoding = false;
        enableAllButtons();
        updateTranscodeProgressPanel('批量优化请求失败', 100, error.message, true);
        showError('批量优化请求失败: ' + error.message);
    }
}

function disableAllButtons() {
    ['upload-btn', 'upload-folder-btn', 'material-refresh-btn',
     'transcode-selected-btn', 'batch-transcode-btn'].forEach(id => {
        const btn = document.getElementById(id);
        if (btn) { btn.disabled = true; btn.style.opacity = '0.5'; }
    });
}

function enableAllButtons() {
    ['upload-btn', 'upload-folder-btn', 'material-refresh-btn',
     'transcode-selected-btn', 'batch-transcode-btn'].forEach(id => {
        const btn = document.getElementById(id);
        if (btn) { btn.disabled = false; btn.style.opacity = '1'; }
    });
}

setTimeout(() => {
    const btn = document.getElementById('batch-transcode-btn');
    if (btn) btn.addEventListener('click', batchTranscodeVideos);
}, 1000);
