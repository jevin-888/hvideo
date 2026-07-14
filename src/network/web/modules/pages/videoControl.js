/**
 * @file videoControl.js（文件名）
 * @brief 视频控制页面模块
 *
 * 本模块提供视频控制功能，包括：
 * - 视频播放、暂停、停止控制
 * - 音量控制和音轨切换
 * - 播放进度显示和控制
 * - 图层选择和控制
 */
import { apiPost, apiGet, apiPut, apiAction } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { updateButtonGroupState, getButtonGroupValue } from '../components/buttonGroups.js';
import { subscribeSSEEvent } from '../core/connectionManager.js';
import { debounce } from '../utils/domHelpers.js';
import { handleApiOperation } from '../utils/apiHelpers.js';
import { getCurrentPlaylistId, getPlaylistTargetLayer } from '../features/playlist.js';
import { extractFileName } from '../utils/pathHelpers.js';

/** 图片播放列表使用的图层 ID */
const IMAGE_PLAYLIST_LAYER_ID = 60;
const FUSION_BACKGROUND_FOLDER = 'gb_fusion';

/** 图片播放列表前端状态：当前列表 ID、项列表、当前索引 */
let imagePlaylistState = { playlistId: null, items: [], currentIndex: 0 };

function isFusionBackgroundImagePath(filePath) {
    const lower = String(filePath || '').replace(/\\/g, '/').toLowerCase();
    return lower.includes(`/image/${FUSION_BACKGROUND_FOLDER}/`) ||
        lower.startsWith(`image/${FUSION_BACKGROUND_FOLDER}/`) ||
        lower.startsWith(`${FUSION_BACKGROUND_FOLDER}/`);
}

async function postVideoCommand(action, param = {}, timeoutMs) {
    const videoEndpointActionMap = {
        lockPlayback: 'lock',
        muteToggle: 'mute/toggle',
        unlockPlayback: 'unlock',
        volumeDown: 'volume/down',
        volumeUp: 'volume/up',
        getPlaybackLock: 'lock/status',
        setVolume: 'volume',
        setPlaybackRate: 'playbackRate',
        getSystemVolume: 'getSystemVolume',
        setSystemVolume: 'systemVolume'
    };
    const videoAction = videoEndpointActionMap[action] || action;
    return timeoutMs === undefined
        ? await apiPost(`/video/${videoAction}`, param)
        : await apiPost(`/video/${videoAction}`, param, timeoutMs);
}

/**
 * 发送加载图片命令到图层 60（后端 loadImage 使用 image_file 相对路径）
 * @param {string} filePath - 图片完整路径（如 /huoshan/Image/gb_fusion/xxx.jpg）
 */
async function loadImageToLayer(filePath) {
    if (isFusionBackgroundImagePath(filePath)) {
        throw new Error('gb_fusion 是融合背景目录，不发送到播放图层');
    }

    // 提取相对于 Image/ 目录的相对路径，支持子目录（如 gb_fusion/19.jpg）
    let imageFile = extractFileName(filePath);
    const imageDir = '/Image/';
    const imageDirIdx = filePath.indexOf(imageDir);
    if (imageDirIdx !== -1) {
        imageFile = filePath.substring(imageDirIdx + imageDir.length);
    }
    await apiAction('rendering', 'loadImage', {
        layerId: IMAGE_PLAYLIST_LAYER_ID,
        image_file: imageFile
    });
}

// ============================================================================
// 播放模式追踪
// ============================================================================

/**
 * 当前播放模式状态
 * @type {{mode: '播放列表'|'preview', path: string|null, layerId: number|null}}
 */
let playbackMode = {
    mode: 'playlist',  // 默认为播放列表模式
    path: null,        // 预览模式下的文件路径
    layerId: null      // 预览模式下的图层ID
};

/**
 * 设置为预览播放模式
 * @param {string} path - 文件路径
 * @param {number} layerId - 图层ID
 */
export function setPreviewMode(path, layerId) {
    playbackMode = {
        mode: 'preview',
        path: path,
        layerId: layerId
    };
}

/**
 * 设置为播放列表播放模式
 */
export function setPlaylistMode() {
    playbackMode = {
        mode: 'playlist',
        path: null,
        layerId: null
    };
}

/**
 * 获取当前播放模式
 * @returns {{mode: '播放列表'|'preview', path: string|null, layerId: number|null}}
 */
function getPlaybackMode() {
    return playbackMode;
}

// 缓存的默认图层ID（从实际配置图层获取）
let cachedDefaultLayerId = null;
let playbackRateDragging = false;
let currentPlaybackRate = 1.0;
let seekSliderDragging = false;
let seekRequestSerial = 0;
let pendingSeekRequest = null;
let activeSeekRequest = null;
let currentSeekDuration = 0;
let seekClock = {
    position: 0,
    duration: 0,
    playbackRate: 1,
    state: 'stopped',
    updatedAt: 0
};
let seekClockTimer = null;
let videoStatusSseUnsubscribe = null;

const SEEK_END_GUARD_SECONDS = 0.25;

function clampNumber(value, min, max) {
    const number = Number(value);
    if (!Number.isFinite(number)) return min;
    return Math.min(max, Math.max(min, number));
}

function normalizeDuration(value) {
    const duration = Number(value);
    return Number.isFinite(duration) && duration > 0 ? duration : 0;
}

function sliderSecondValue(value) {
    const number = Number(value);
    return Number.isFinite(number) ? String(Math.round(number * 1000) / 1000) : '0';
}

function clampPlaybackPosition(position, duration = 0, endGuardSeconds = 0) {
    const safeDuration = normalizeDuration(duration);
    const maxPosition = safeDuration > 0
        ? Math.max(0, safeDuration - Math.max(0, endGuardSeconds))
        : Number.MAX_SAFE_INTEGER;
    return clampNumber(position, 0, maxPosition);
}

function displayPositionForDuration(position, duration) {
    const safeDuration = normalizeDuration(duration);
    const safePosition = clampPlaybackPosition(position, safeDuration, 0);
    if (safeDuration > 0 && safeDuration - safePosition <= 0.5) {
        return safeDuration;
    }
    return safePosition;
}

function isPlaybackRunning(state) {
    return state === 'playing';
}

function getLiveSeekPosition() {
    const base = displayPositionForDuration(seekClock.position, seekClock.duration);
    if (!isPlaybackRunning(seekClock.state) || !seekClock.updatedAt || seekClock.duration <= 0) {
        return base;
    }
    const elapsedSeconds = (performance.now() - seekClock.updatedAt) / 1000;
    return displayPositionForDuration(base + elapsedSeconds * seekClock.playbackRate, seekClock.duration);
}

function renderSeekClock() {
    if (seekSliderDragging || activeSeekRequest || seekClock.duration <= 0) return;
    const seekSlider = document.getElementById('seek-position-slider');
    if (!seekSlider || !seekSlider.offsetParent) return;
    const position = getLiveSeekPosition();
    seekSlider.step = '0.1';
    seekSlider.max = sliderSecondValue(seekClock.duration);
    seekSlider.disabled = false;
    seekSlider.value = sliderSecondValue(clampPlaybackPosition(position, seekClock.duration, 0));
    updateSliderProgress(seekSlider, seekSlider.value);
    updateSeekValueDisplay(position, seekClock.duration);
}

function syncSeekClock(layerStatus, position, duration) {
    seekClock = {
        position: displayPositionForDuration(position, duration),
        duration,
        playbackRate: Math.max(0, Number(layerStatus.playbackRate ?? 1) || 1),
        state: layerStatus.state || layerStatus.status || 'stopped',
        updatedAt: performance.now()
    };
    if (!seekClockTimer) {
        seekClockTimer = window.setInterval(renderSeekClock, 250);
    }
}

function clearSeekClockDisplay(layerStatus = {}) {
    const state = layerStatus.state || layerStatus.status || 'stopped';
    currentSeekDuration = 0;
    seekClock = {
        position: 0,
        duration: 0,
        playbackRate: 1,
        state,
        updatedAt: performance.now()
    };

    const seekSlider = document.getElementById('seek-position-slider');
    if (seekSlider) {
        seekSlider.step = '0.1';
        seekSlider.max = '0';
        seekSlider.value = '0';
        seekSlider.disabled = true;
        updateSliderProgress(seekSlider, 0);
    }
    updateSeekValueDisplay(0, 0);
}

function shouldClearSeekForStatus(layerStatus = {}) {
    const state = layerStatus.state || layerStatus.status;
    return state === 'stopped' || state === 'capturing' || state === 'no_signal_placeholder';
}

function statusPositionFromPayload(layerStatus) {
    return layerStatus?.current_position ?? layerStatus?.currentPosition ?? layerStatus?.position;
}

function syncVideoProgressPayload(layerStatus) {
    if (!layerStatus) return;
    const statusPosition = statusPositionFromPayload(layerStatus);
    if (seekSliderDragging || activeSeekRequest) return;
    if (layerStatus.duration === undefined || statusPosition === undefined) {
        if (shouldClearSeekForStatus(layerStatus)) {
            clearSeekClockDisplay(layerStatus);
        } else if (layerStatus.state || layerStatus.status) {
            seekClock = {
                ...seekClock,
                state: layerStatus.state || layerStatus.status,
                updatedAt: performance.now()
            };
        }
        return;
    }
    const duration = normalizeDuration(layerStatus.duration);
    const position = displayPositionForDuration(statusPosition, duration);
    if (duration <= 0 && shouldClearSeekForStatus(layerStatus)) {
        clearSeekClockDisplay(layerStatus);
        return;
    }
    currentSeekDuration = duration;
    syncSeekClock(layerStatus, position, duration);
    renderSeekClock();
}

function handleVideoStatusSSE(event) {
    try {
        const data = JSON.parse(event.data || '{}');
        const playlistId = getCurrentPlaylistId();
        const layerSelectVal = getButtonGroupValue('layer-select');
        const currentLayerId = Number(playlistId ? getPlaylistTargetLayer(playlistId) : (layerSelectVal || cachedDefaultLayerId));
        if (!currentLayerId) return;
        const statusMap = data.video_status || data.videoStatus;
        const layerStatus = statusMap
            ? (statusMap[currentLayerId] || statusMap[String(currentLayerId)])
            : (Number(data.layerId) === currentLayerId ? data : null);
        if (!layerStatus) return;
        syncVideoProgressPayload(layerStatus);
    } catch (error) {
        console.warn('[video_status] 解析失败', error);
    }
}

function ensureVideoStatusSSE() {
    if (videoStatusSseUnsubscribe) return;
    videoStatusSseUnsubscribe = subscribeSSEEvent('video_status', handleVideoStatusSSE);
}

function makeSeekTraceId(layerId, position) {
    const safeLayer = layerId ?? 'none';
    const safePosition = Number.isFinite(position) ? Math.round(position * 1000) : 0;
    return `web-seek-${Date.now()}-${++seekRequestSerial}-L${safeLayer}-P${safePosition}`;
}

function logSeekDiagnostic(stage, details = {}) {
    const payload = Object.entries(details)
        .map(([key, value]) => `${key}=${value}`)
        .join(' ');
    console.info(`[SEEK_DIAG][web] ${stage}${payload ? ` ${payload}` : ''}`);
}

/**
 * 获取默认可用图层ID（从缓存或配置中获取）
 * @returns {number|null} 图层ID或null
 */
async function getDefaultLayerId() {
    if (cachedDefaultLayerId !== null) {
        // 验证缓存的图层ID是否实际存在
        try {
            const layerInfo = await apiGet(`/layers/${cachedDefaultLayerId}`);
            if (layerInfo && layerInfo.ok !== false) {
                return cachedDefaultLayerId;
            }
        } catch (error) {
            // 图层不存在，清除缓存
            cachedDefaultLayerId = null;
        }
    }
    try {
        // 使用 /api/layers 获取实际配置的图层列表
        const layers = await apiGet('/layers');
        if (layers && Array.isArray(layers) && layers.length > 0) {
            const videoLayer = layers.find(l => l.type === 'video');
            cachedDefaultLayerId = videoLayer?.id || layers[0]?.id || null;
            return cachedDefaultLayerId;
        }
    } catch (error) {
        // 静默处理
    }
    return null;
}

/**
 * 获取当前工作图层ID（优先从播放列表，否则从配置）
 * @returns {Promise<number|null>} 图层ID或null
 */
async function getCurrentLayerId() {
    const playlistId = getCurrentPlaylistId();
    if (playlistId) {
        const targetLayer = getPlaylistTargetLayer(playlistId);
        if (targetLayer) return targetLayer;
    }
    return await getDefaultLayerId();
}

/**
 * 同步获取当前工作图层ID（使用缓存，用于防抖回调等场景）
 * @returns {number|null} 图层ID或null
 */
function getCurrentLayerIdSync() {
    const playlistId = getCurrentPlaylistId();
    if (playlistId) {
        const targetLayer = getPlaylistTargetLayer(playlistId);
        if (targetLayer) return targetLayer;
    }
    return cachedDefaultLayerId;
}

/**
 * 更新滑块进度样式
 * @param {HTMLElement} slider - 滑块元素
 * @param {number} value - 进度值（0-100）
 */
function updateSliderProgress(slider, value) {
    if (slider) {
        const min = parseFloat(slider.min) || 0;
        const max = parseFloat(slider.max) || 100;
        const current = parseFloat(value);
        const ratio = max > min && Number.isFinite(current)
            ? ((current - min) / (max - min)) * 100
            : 0;
        slider.style.setProperty('--progress', `${clampNumber(ratio, 0, 100)}%`);
    }
}

/**
 * 初始化视频控制
 * 设置所有视频控制相关的事件监听器
 */
export function initializeVideoControl() {
    // 初始化图层选择按钮组
    updateVideoControl();
    ensureVideoStatusSSE();

    // 初始化图层音量滑块事件
    const layerVolumeSlider = document.getElementById('layer-volume-slider');
    if (layerVolumeSlider) {
        // 初始化音量滑块进度
        updateSliderProgress(layerVolumeSlider, layerVolumeSlider.value);

        layerVolumeSlider.addEventListener('input', function () {
            const volume = parseInt(this.value);
            const volumeValue = document.getElementById('layer-volume-value');
            if (volumeValue) {
                volumeValue.textContent = `${volume}%`;
            }
            // 更新滑块进度样式
            updateSliderProgress(this, volume);
            // 使用防抖，避免频繁请求
            setVolume(volume / 100, false);
        });
    }

    // 初始化系统音量滑块事件
    const systemVolumeSlider = document.getElementById('system-volume-slider');
    if (systemVolumeSlider) {
        // 初始化音量滑块进度
        updateSliderProgress(systemVolumeSlider, systemVolumeSlider.value);

        systemVolumeSlider.addEventListener('input', function () {
            const volume = parseInt(this.value);
            const volumeValue = document.getElementById('system-volume-value');
            if (volumeValue) {
                volumeValue.textContent = `${volume}%`;
            }
            // 更新滑块进度样式
            updateSliderProgress(this, volume);
            setSystemVolume(volume / 100);
        });

        // 加载系统音量初始值
        loadSystemVolume();
    }

    const playbackRateDownBtn = document.getElementById('playback-rate-down-btn');
    const playbackRateUpBtn = document.getElementById('playback-rate-up-btn');
    if (playbackRateDownBtn) {
        playbackRateDownBtn.addEventListener('click', () => adjustPlaybackRate(-0.05));
    }
    if (playbackRateUpBtn) {
        playbackRateUpBtn.addEventListener('click', () => adjustPlaybackRate(0.05));
    }

    const seekPositionSlider = document.getElementById('seek-position-slider');
    if (seekPositionSlider) {
        seekPositionSlider.step = '0.1';
        updateSliderProgress(seekPositionSlider, seekPositionSlider.value);
        seekPositionSlider.addEventListener('pointerdown', () => {
            seekSliderDragging = true;
            logSeekDiagnostic('pointerdown', {
                value: seekPositionSlider.value,
                max: seekPositionSlider.max
            });
        });
        seekPositionSlider.addEventListener('input', function () {
            const position = displayPositionForDuration(this.value, currentSeekDuration);
            updateSeekValueDisplay(position, currentSeekDuration);
            updateSliderProgress(this, this.value);
        });
        seekPositionSlider.addEventListener('change', async function () {
            seekSliderDragging = false;
            const position = clampPlaybackPosition(this.value, currentSeekDuration, SEEK_END_GUARD_SECONDS);
            this.value = sliderSecondValue(position);
            updateSeekValueDisplay(displayPositionForDuration(position, currentSeekDuration), currentSeekDuration);
            updateSliderProgress(this, this.value);
            logSeekDiagnostic('change', {
                value: this.value,
                max: this.max,
                duration: currentSeekDuration
            });
            await seekTestVideo(position);
        });
        seekPositionSlider.addEventListener('pointerup', () => {
            seekSliderDragging = false;
            logSeekDiagnostic('pointerup', {
                value: seekPositionSlider.value,
                max: seekPositionSlider.max
            });
        });
        seekPositionSlider.addEventListener('pointercancel', () => {
            seekSliderDragging = false;
            logSeekDiagnostic('pointercancel', {
                value: seekPositionSlider.value,
                max: seekPositionSlider.max
            });
        });
    }

    const dspAudioRouteTest = document.getElementById('dsp-audio-route-test');
    if (dspAudioRouteTest) {
        dspAudioRouteTest.addEventListener('click', testDspAudioRoute);
    }

    // 初始化播放/暂停按钮事件
    const playPauseBtn = document.getElementById('play-pause-btn');
    if (playPauseBtn) {
        playPauseBtn.addEventListener('click', togglePlayPause);
    }

    // 初始化重播按钮事件
    const replayBtn = document.getElementById('replay-btn');
    if (replayBtn) {
        replayBtn.addEventListener('click', replayVideo);
    }

    // 初始化下一曲按钮事件
    const nextBtn = document.getElementById('next-btn');
    if (nextBtn) {
        nextBtn.addEventListener('click', nextVideo);
    }

    const playbackLockBtn = document.getElementById('playback-lock-btn');
    if (playbackLockBtn) {
        playbackLockBtn.addEventListener('click', togglePlaybackLock);
    }

    // 初始化静音按钮事件
    const muteBtn = document.getElementById('mute-btn');
    if (muteBtn) {
        muteBtn.addEventListener('click', toggleMute);
    }

    // 初始化音量加减按钮事件（控制图层音量）
    const volumeDownBtn = document.getElementById('volume-down-btn');
    const volumeUpBtn = document.getElementById('volume-up-btn');

    if (volumeDownBtn && layerVolumeSlider) {
        volumeDownBtn.addEventListener('click', async function () {
            const currentVolume = parseInt(layerVolumeSlider.value);
            const newVolume = Math.max(0, currentVolume - 5); // 每次减5%

            // 更新UI
            layerVolumeSlider.value = newVolume;
            const volumeValue = document.getElementById('layer-volume-value');
            if (volumeValue) {
                volumeValue.textContent = `${newVolume}%`;
            }
            updateSliderProgress(layerVolumeSlider, newVolume);

            // 调用setVolume API，传入hint_type="down"强制显示提示
            const layerId = await getCurrentLayerId();
            const playlistId = getCurrentPlaylistId();
            if (layerId) {
                try {
                    await postVideoCommand('setVolume', {
                        layerId: parseInt(layerId),
                        playlistId: playlistId || undefined,
                        volume: newVolume / 100,
                        hint_type: 'down' // 强制显示音量减少提示
                    });
                } catch (error) {
                    console.error('音量减少失败:', error);
                }
            }
        });
    }

    if (volumeUpBtn && layerVolumeSlider) {
        volumeUpBtn.addEventListener('click', async function () {
            const currentVolume = parseInt(layerVolumeSlider.value);
            const newVolume = Math.min(100, currentVolume + 5); // 每次加5%

            // 更新UI
            layerVolumeSlider.value = newVolume;
            const volumeValue = document.getElementById('layer-volume-value');
            if (volumeValue) {
                volumeValue.textContent = `${newVolume}%`;
            }
            updateSliderProgress(layerVolumeSlider, newVolume);

            // 调用setVolume API，传入hint_type="up"强制显示提示
            const layerId = await getCurrentLayerId();
            const playlistId = getCurrentPlaylistId();
            if (layerId) {
                try {
                    await postVideoCommand('setVolume', {
                        layerId: parseInt(layerId),
                        playlistId: playlistId || undefined,
                        volume: newVolume / 100,
                        hint_type: 'up' // 强制显示音量增加提示
                    });
                } catch (error) {
                    console.error('音量增加失败:', error);
                }
            }
        });
    }

    // 监听播放列表选择事件，自动同步图层选择
    window.addEventListener('playlistSelected', (e) => {
        if (e.detail && e.detail.targetLayerId) {
            loadLayerVolume(e.detail.targetLayerId);
            refreshPlaybackLockState(e.detail.targetLayerId);
        }
    });

    // 监听音轨切换事件（避免循环依赖）
    window.addEventListener('nextAudioTrack', () => {
        nextAudioTrack();
    });

    // 初始化流视频测试按钮事件
    const streamTestBtn = document.getElementById('stream-test-btn');
    if (streamTestBtn) {
        streamTestBtn.addEventListener('click', testStreamPlay);
    }

    // 支持回车键触发测试
    const streamUrlInput = document.getElementById('stream-url-input');
    if (streamUrlInput) {
        streamUrlInput.addEventListener('keypress', function (e) {
            if (e.key === 'Enter') {
                testStreamPlay();
            }
        });
    }
}

/**
 * 加载图层音量值并更新UI
 * @param {number} layerId - 图层ID
 */
async function loadLayerVolume(layerId) {
    try {
        const layerInfo = await apiGet(`/layers/${layerId}`);
        const volumeSlider = document.getElementById('layer-volume-slider');
        const volumeValue = document.getElementById('layer-volume-value');

        if (!volumeSlider || !volumeValue) {
            return;
        }

        // 检查图层是否存在
        if (!layerInfo || layerInfo.ok === false) {
            // 图层不存在时，禁用音量控制并显示提示
            volumeSlider.disabled = true;
            volumeValue.textContent = '图层不存在';
            updateSliderProgress(volumeSlider, 0);
            return;
        }

        // 如果图层存在且有音量信息，使用图层音量
        let volumePercent = 100;
        if (layerInfo.volume !== undefined) {
            // 转换为百分比显示（volume是0.0-1.0，需要转换为0-100）
            volumePercent = Math.round(layerInfo.volume * 100);
        }

        volumeSlider.disabled = false;
        volumeSlider.value = volumePercent;
        volumeValue.textContent = `${volumePercent}%`;
        updateSliderProgress(volumeSlider, volumePercent);
    } catch (error) {
        // 处理错误，禁用音量控制并显示提示
        const volumeSlider = document.getElementById('layer-volume-slider');
        const volumeValue = document.getElementById('layer-volume-value');
        if (volumeSlider && volumeValue) {
            volumeSlider.disabled = true;
            volumeValue.textContent = '图层不存在';
            updateSliderProgress(volumeSlider, 0);
        }
    }
}

function updatePlaybackLockButton(locked) {
    const btn = document.getElementById('playback-lock-btn');
    if (!btn) return;
    btn.dataset.locked = locked ? 'true' : 'false';
    btn.classList.toggle('active', locked);
    btn.classList.toggle('locked', locked);
    const text = btn.querySelector('.text');
    if (text) text.textContent = locked ? '解锁' : '锁定';
}

async function refreshPlaybackLockState(layerId = null) {
    const targetLayerId = layerId || await getCurrentLayerId() || getButtonGroupValue('layer-select');
    if (!targetLayerId) {
        updatePlaybackLockButton(false);
        return;
    }

    const result = await postVideoCommand('getPlaybackLock', {
        layerId: parseInt(targetLayerId)
    });
    updatePlaybackLockButton(!!result?.locked);
}

async function togglePlaybackLock() {
    const layerId = await getCurrentLayerId() || getButtonGroupValue('layer-select');
    if (!layerId) {
        addToCommandLog('播放锁定', 'error', '请选择列表或图层');
        return;
    }

    const btn = document.getElementById('playback-lock-btn');
    const locked = btn?.dataset.locked === 'true';
    const action = locked ? 'unlockPlayback' : 'lockPlayback';
    const actionName = locked ? '播放解锁' : '播放锁定';
    const result = await postVideoCommand(action, {
        layerId: parseInt(layerId)
    });

    if (result) {
        const nextLocked = !!result.locked;
        updatePlaybackLockButton(nextLocked);
        addToCommandLog(actionName, 'success', `图层${layerId}${nextLocked ? '已锁定' : '已解锁'}`);
    } else {
        addToCommandLog(actionName, 'error', `${actionName}失败`);
    }
}

/**
 * 加载系统音量值并更新UI
 */
async function loadSystemVolume() {
    try {
        const result = await postVideoCommand('getSystemVolume', {});
        if (result && result.volume !== undefined) {
            const volume = result.volume;
            const volumeSlider = document.getElementById('system-volume-slider');
            const volumeValue = document.getElementById('system-volume-value');

            if (volumeSlider && volumeValue) {
                // 转换为百分比显示（volume是0.0-1.0，需要转换为0-100）
                const volumePercent = Math.round(volume * 100);
                volumeSlider.value = volumePercent;
                volumeValue.textContent = `${volumePercent}%`;
                updateSliderProgress(volumeSlider, volumePercent);
            }
        }
    } catch (error) {
        // 静默处理错误，不显示日志
    }
}

/**
 * 设置系统音量
 * @param {number} volume - 音量值（0-1之间的浮点数）
 */
async function setSystemVolume(volume) {
    await postVideoCommand('setSystemVolume', {
        volume: volume
    });
}

async function testDspAudioRoute(event) {
    const btn = event.target.closest('button[data-dsp-type]');
    if (!btn) return;

    const dspType = parseInt(btn.dataset.dspType, 10);
    const isHdmin = parseInt(btn.dataset.isHdmin || '0', 10);
    const label = btn.textContent.trim();

    await handleApiOperation(
        apiPost('/video/dsp/audio-route', {
            dspType,
            isHdmin,
            volume: 1.0,
            label
        }),
        'DSP通道测试',
        `已切换到 ${label}`,
        `切换 ${label} 失败`,
        () => {
            document.querySelectorAll('#dsp-audio-route-test .btn').forEach(item => item.classList.remove('active'));
            btn.classList.add('active');
        }
    );
}

const setPlaybackRate = debounce(async (rate) => {
    const layerId = await getCurrentLayerId();
    const playlistId = getCurrentPlaylistId();
    if (!layerId) return;

    await postVideoCommand('setPlaybackRate', {
        layerId: parseInt(layerId),
        playlistId: playlistId || undefined,
        rate
    });
}, 120);

function updatePlaybackRateDisplay(rate) {
    currentPlaybackRate = Math.max(0.5, Math.min(2.0, rate));
    const playbackRateValue = document.getElementById('playback-rate-value');
    if (playbackRateValue) {
        playbackRateValue.textContent = `${currentPlaybackRate.toFixed(2)}x`;
    }
}

function adjustPlaybackRate(delta) {
    const nextRate = Math.max(0.5, Math.min(2.0, Math.round((currentPlaybackRate + delta) * 100) / 100));
    playbackRateDragging = true;
    updatePlaybackRateDisplay(nextRate);
    setPlaybackRate(nextRate);
    window.setTimeout(() => { playbackRateDragging = false; }, 200);
}

function updateSeekValueDisplay(position, duration = currentSeekDuration) {
    const seekPositionValue = document.getElementById('seek-position-value');
    if (seekPositionValue) {
        const safeDuration = normalizeDuration(duration);
        const displayPosition = displayPositionForDuration(position, safeDuration);
        seekPositionValue.textContent = `${formatTime(displayPosition)} / ${formatTime(safeDuration)}`;
    }
}

async function seekTestVideo(position) {
    const layerId = await getCurrentLayerId();
    const playlistId = getCurrentPlaylistId();
    if (!layerId) return;

    position = clampPlaybackPosition(position, currentSeekDuration, SEEK_END_GUARD_SECONDS);
    const traceId = makeSeekTraceId(layerId, position);
    pendingSeekRequest = {
        layerId: parseInt(layerId),
        playlistId: playlistId || undefined,
        position,
        traceId
    };
    logSeekDiagnostic('queued', {
        traceId,
        layerId,
        playlistId: playlistId || '',
        position
    });

    if (activeSeekRequest) {
        logSeekDiagnostic('coalesce', {
            activeTraceId: activeSeekRequest.traceId,
            nextTraceId: traceId,
            position
        });
        return;
    }

    while (pendingSeekRequest) {
        const request = pendingSeekRequest;
        pendingSeekRequest = null;
        activeSeekRequest = request;
        const startedAt = performance.now();
        logSeekDiagnostic('request_start', request);
        try {
            await handleApiOperation(
                postVideoCommand('seek', {
                    traceId: request.traceId,
                    layerId: request.layerId,
                    playlistId: request.playlistId,
                    position: request.position
                }, 15000),
                '播放进度',
                `已跳转到 ${formatTime(request.position)}`,
                '播放进度跳转失败'
            );
        } finally {
            logSeekDiagnostic('request_done', {
                traceId: request.traceId,
                costMs: Math.round(performance.now() - startedAt),
                queuedNext: pendingSeekRequest ? pendingSeekRequest.traceId : ''
            });
            activeSeekRequest = null;
        }
    }
}

/**
 * 更新视频控制页面状态
 * 在页面初始化或切换时调用
 */
export async function updateVideoControl() {
    // 从播放列表或配置获取当前工作图层
    const layerId = await getCurrentLayerId();

    // 只有当有可用图层时才加载音量配置
    if (layerId) {
        await loadLayerVolume(layerId);
        await refreshPlaybackLockState(layerId);
    }
}


// getButtonGroupValue 已从 buttonGroups.js 导入

/**
 * 更新按钮视觉效果
 * 根据播放状态更新播放/暂停按钮的图标和样式
 * @param {HTMLElement} btn - 按钮元素
 * @param {boolean} isPlaying - 是否正在播放
 */
function updateButtonVisuals(btn, isPlaying) {
    const iconPause = btn.querySelector('.icon-pause');
    const iconPlay = btn.querySelector('.icon-play');
    const textSpan = btn.querySelector('.text');

    if (isPlaying) {
        if (iconPause) iconPause.classList.remove('hidden');
        if (iconPlay) iconPlay.classList.add('hidden');
        if (textSpan) textSpan.textContent = '暂停';
        btn.classList.remove('primary');
        btn.classList.add('playing');
        btn.dataset.state = 'playing';
    } else {
        if (iconPause) iconPause.classList.add('hidden');
        if (iconPlay) iconPlay.classList.remove('hidden');
        if (textSpan) textSpan.textContent = '播放';
        btn.classList.remove('playing');
        btn.classList.add('primary');
        btn.dataset.state = 'paused';
    }
}

/**
 * 切换播放/暂停状态
 * 根据当前状态切换视频的播放或暂停
 */
async function togglePlayPause() {
    const btn = document.getElementById('play-pause-btn');
    if (!btn) return;

    const currentState = btn.dataset.state || 'playing';

    if (currentState === 'playing') {
        // 当前是播放状态，切换到暂停
        updateButtonVisuals(btn, false);  // 先更新UI
        await pauseVideo();
    } else {
        // 当前是暂停状态，切换到播放
        // 使用 resume 命令而不是 play，避免重新加载视频
        updateButtonVisuals(btn, true);   // 先更新UI
        await resumeVideo();
    }
}

/**
 * 播放视频
 * 根据当前播放模式（预览或播放列表）播放视频
 */
export async function playVideo() {
    const currentMode = getPlaybackMode();

    // 预览模式：继续播放预览的文件
    if (currentMode.mode === 'preview' && currentMode.path && currentMode.layerId) {
        await handleApiOperation(
            postVideoCommand('play', {
                layerId: parseInt(currentMode.layerId),
                path: currentMode.path,
                loop: 2,
                playbackRate: 1.0
            }),
            '播放视频',
            '成功播放预览视频',
            '播放预览视频失败'
        );
        return;
    }

    // 播放列表模式
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('播放视频', 'error', '无法确定目标图层');
        return;
    }

    // 图片播放列表（图层 60）：应用幻灯片参数到图层，再拉取列表并加载第一张
    if (layerId === IMAGE_PLAYLIST_LAYER_ID && playlistId) {
        try {
            const config = await apiGet(`/playlists/${playlistId}/config`);
            if (config && (config.displayDuration != null || config.fadeInTime != null || config.fadeOutTime != null)) {
                const layerParams = {};
                if (config.displayDuration != null && config.displayDuration >= 0) layerParams.displayDuration = config.displayDuration;
                if (config.fadeInTime != null && config.fadeInTime >= 0) layerParams.fadeInTime = config.fadeInTime;
                if (config.fadeOutTime != null && config.fadeOutTime >= 0) layerParams.fadeOutTime = config.fadeOutTime;
                if (Object.keys(layerParams).length > 0) {
                    await apiPut('/layers/' + IMAGE_PLAYLIST_LAYER_ID, layerParams);
                }
            }
            const items = await apiGet(`/playlists/${playlistId}/items?layerId=${IMAGE_PLAYLIST_LAYER_ID}`);
            if (!items || !Array.isArray(items) || items.length === 0) {
                addToCommandLog('播放图片列表', 'error', '当前图片列表为空');
                return;
            }
            imagePlaylistState = { playlistId, items, currentIndex: 0 };
            await loadImageToLayer(items[0].path);
            setPlaylistMode();
            addToCommandLog('播放图片列表', 'success', `已播放第 1 张，共 ${items.length} 张`);
        } catch (e) {
            addToCommandLog('播放图片列表', 'error', e.message || '播放失败');
        }
        return;
    }

    // 视频播放列表：走原有视频 API
    const loopMode = 0;
    const playbackRate = 1.0;
    await handleApiOperation(
        postVideoCommand('play', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined,
            loop: loopMode,
            playbackRate: playbackRate
        }),
        '播放视频',
        `成功播放${playlistId ? '列表项目' : '图层' + layerId + '视频'}`,
        `播放${playlistId ? '列表项目' : '图层' + layerId + '视频'}失败`,
        (result) => {
            if (result && result.layerId != null) {
                loadLayerVolume(result.layerId);
            }
            if (playlistId) {
                setPlaylistMode();
            }
        }
    );
}

/**
 * 暂停视频
 * 暂停当前选择的图层视频
 */
async function pauseVideo() {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('暂停视频', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('pause', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined
        }),
        '暂停视频',
        `成功暂停${playlistId ? '列表' : '图层' + layerId}视频`,
        `暂停${playlistId ? '列表' : '图层' + layerId}视频失败`
    );
}

/**
 * 恢复播放视频
 * 从暂停状态恢复播放，不重新加载视频
 */
async function resumeVideo() {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('恢复播放', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('resume', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined
        }),
        '恢复播放',
        `成功恢复${playlistId ? '列表' : '图层' + layerId}视频`,
        `恢复${playlistId ? '列表' : '图层' + layerId}视频失败`
    );
}

/**
 * 停止视频
 * 停止当前选择的图层视频
 */
async function stopVideo() {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('停止视频', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('stop', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined
        }),
        '停止视频',
        `成功停止${playlistId ? '列表' : '图层' + layerId}视频`,
        `停止${playlistId ? '列表' : '图层' + layerId}视频失败`
    );
}

/**
 * 重播视频
 * 从头开始播放当前选择的图层视频
 */
async function replayVideo() {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('重播视频', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('replay', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined
        }),
        '重播视频',
        `成功重播${playlistId ? '列表' : '图层' + layerId}视频`,
        `重播${playlistId ? '列表' : '图层' + layerId}视频失败`
    );
}

/**
 * 下一曲
 * 切换到当前图层列表中的下一个视频/图片
 */
async function nextVideo() {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId() || getButtonGroupValue('layer-select');

    if (!layerId) {
        addToCommandLog('下一曲', 'error', '请选择列表或图层');
        return;
    }

    // 图片播放列表：前端切换下一张（循环）
    if (layerId === IMAGE_PLAYLIST_LAYER_ID && playlistId) {
        let { playlistId: statePlaylistId, items } = imagePlaylistState;
        if (statePlaylistId !== playlistId || !items.length) {
            items = await apiGet(`/playlists/${playlistId}/items?layerId=${IMAGE_PLAYLIST_LAYER_ID}`);
            if (!items || !Array.isArray(items) || items.length === 0) {
                addToCommandLog('下一张', 'error', '当前图片列表为空');
                return;
            }
            imagePlaylistState = { playlistId, items, currentIndex: 0 };
        }
        const nextIndex = (imagePlaylistState.currentIndex + 1) % imagePlaylistState.items.length;
        imagePlaylistState.currentIndex = nextIndex;
        const item = imagePlaylistState.items[nextIndex];
        try {
            await loadImageToLayer(item.path);
            addToCommandLog('下一张', 'success', `第 ${nextIndex + 1} 张，共 ${imagePlaylistState.items.length} 张`);
        } catch (e) {
            addToCommandLog('下一张', 'error', e.message || '切换失败');
        }
        return;
    }

    await handleApiOperation(
        postVideoCommand('next', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined
        }),
        '下一曲',
        `成功切换到下一曲 (${playlistId ? '列表' : '图层' + layerId})`,
        '切换下一曲失败',
        (result) => {
            if (result && result.layerId != null) {
                loadLayerVolume(result.layerId);
            }
        }
    );
}


// 音量设置防抖函数（延迟300ms，避免频繁请求）
let debouncedSetVolume = null;

/**
 * 设置音量
 * @param {number} volume - 音量值（0-1之间的浮点数）
 * @param {boolean} immediate - 是否立即执行（不防抖），默认false
 */
async function setVolume(volume, immediate = false) {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        return;
    }

    // 确保音量值在有效范围内
    const clampedVolume = Math.max(0, Math.min(1, volume));

    // 如果立即执行（如按钮点击），直接发送请求
    if (immediate) {
        try {
            await postVideoCommand('setVolume', {
                layerId: parseInt(layerId),
                playlistId: playlistId || undefined,
                volume: clampedVolume
            });
        } catch (error) {
            // 静默处理错误，避免重复的错误日志
            console.error('设置音量失败:', error);
        }
        return;
    }

    // 对于滑块拖动，使用防抖
    // 在防抖函数执行时重新获取最新的 layerId 与 播放列表Id，确保使用正确的图层
    if (!debouncedSetVolume) {
        debouncedSetVolume = debounce(async (vol) => {
            const currentPlaylistId = getCurrentPlaylistId();
            const currentLayerId = getCurrentLayerIdSync();

            if (!currentLayerId) {
                return;
            }

            try {
                await postVideoCommand('setVolume', {
                    layerId: parseInt(currentLayerId),
                    playlistId: currentPlaylistId || undefined,
                    volume: vol
                });
            } catch (error) {
                // 静默处理错误，避免重复的错误日志
                console.error('设置音量失败:', error);
            }
        }, 300);
    }

    debouncedSetVolume(clampedVolume);
}

// 静音状态和之前的音量值
let isMuted = false;
let previousVolume = 100;

/**
 * 切换静音
 * 在静音和取消静音之间切换，取消静音时恢复之前的音量值
 */
async function toggleMute() {
    const volumeSlider = document.getElementById('layer-volume-slider');
    const volumeValue = document.getElementById('layer-volume-value');
    const muteBtn = document.getElementById('mute-btn');

    if (!volumeSlider || !volumeValue) return;

    if (isMuted) {
        // 取消静音，恢复之前的音量
        isMuted = false;
        volumeSlider.value = previousVolume;
        volumeValue.textContent = `${previousVolume}%`;
        // 更新滑块进度样式
        updateSliderProgress(volumeSlider, previousVolume);
        // 取消静音立即执行，不使用防抖
        await setVolume(previousVolume / 100, true);
        if (muteBtn) muteBtn.textContent = '静音';
        addToCommandLog('取消静音', 'success', `恢复图层音量到 ${previousVolume}%`);
    } else {
        // 静音前保存当前音量
        previousVolume = parseInt(volumeSlider.value);
        isMuted = true;
        volumeSlider.value = 0;
        volumeValue.textContent = '0%';
        // 更新滑块进度样式
        updateSliderProgress(volumeSlider, 0);
        // 静音立即执行，不使用防抖
        await setVolume(0, true);
        if (muteBtn) muteBtn.textContent = '取消静音';
        addToCommandLog('静音', 'success', `图层已静音（之前音量: ${previousVolume}%）`);
    }
}

/**
 * 跳转到指定位置
 * @param {number} position - 目标位置（秒）
 */
async function seekVideo(position) {
    await seekTestVideo(position);
}

/**
 * 切换音轨
 * @param {number} trackId - 目标音轨ID
 */
export async function switchAudioTrack(trackId) {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('切换音轨', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('switch_audioTrack', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined,
            audioTrack: trackId
        }),
        '切换音轨',
        `成功切换到音轨${trackId}，图层${layerId}`,
        '切换音轨失败',
        () => {
            updateButtonGroupState('audio-track-select', String(trackId));
            const audioTrackBtn = document.querySelector('#audio-track-select .btn');
            if (audioTrackBtn) {
                audioTrackBtn.dataset.value = String(trackId);
                audioTrackBtn.textContent = `音轨切换(${trackId})`;
                audioTrackBtn.classList.add('active');
            }
        }
    );
}

/**
 * 设置声道
 * @param {string} channel - 声道值（如 'stereo', 'left', 'right'）
 */
export async function setAudioChannel(channel) {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('设置声道', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('set_audioChannel', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined,
            audioChannel: channel
        }),
        '设置声道',
        `成功设置声道为${channel}，图层${layerId}`,
        '设置声道失败',
        () => {
            // 更新声道选择按钮组状态
            updateButtonGroupState('audio-channel-select', channel);
        }
    );
}

/**
 * 下一音轨
 * 切换到当前视频的下一个可用音轨
 */
export async function nextAudioTrack() {
    const playlistId = getCurrentPlaylistId();
    const layerId = await getCurrentLayerId();

    if (!layerId) {
        addToCommandLog('切换音轨', 'error', '请选择列表或图层');
        return;
    }

    await handleApiOperation(
        postVideoCommand('next_audioTrack', {
            layerId: parseInt(layerId),
            playlistId: playlistId || undefined
        }),
        '切换音轨',
        null, // 将在onSuccess中自定义消息
        '切换到下一音轨失败',
        (result) => {
            // 注意：next_audioTrack可能返回包含currentTrack的数据
            const currentTrack = result?.current_track ?? 0;
            addToCommandLog('切换音轨', 'success', `成功切换到下一音轨（音轨${currentTrack}），图层${layerId}`);
            // 更新音轨选择按钮组状态
            updateButtonGroupState('audio-track-select', String(currentTrack));
            const audioTrackBtn = document.querySelector('#audio-track-select .btn');
            if (audioTrackBtn) {
                audioTrackBtn.dataset.value = String(currentTrack);
                audioTrackBtn.textContent = `音轨切换(${currentTrack})`;
                audioTrackBtn.classList.add('active');
            }
        }
    );
}

/**
 * 格式化时间
 * @param {number} seconds - 秒数
 * @returns {string} MM:SS格式的时间字符串
 */
function formatTime(seconds) {
    if (!seconds && seconds !== 0) return '00:00';
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
}

/**
 * 更新视频控制状态
 * 根据系统状态更新UI
 * @param {object} status - 系统状态对象
 */
export function updateVideoControlStatus(status) {
    if (!status || !status.video_status) return;

    const playlistId = getCurrentPlaylistId();
    const layerSelectVal = getButtonGroupValue('layer-select');
    const layerId = playlistId ? getPlaylistTargetLayer(playlistId) : (layerSelectVal || cachedDefaultLayerId);

    if (!layerId) return;

    const layerStatus = status.video_status[layerId];
    if (layerStatus) {
        // 更新播放/暂停按钮
        const btn = document.getElementById('play-pause-btn');
        if (btn) {
            updateButtonVisuals(btn, layerStatus.state === 'playing');
        }

        // 更新时间
        const statusPosition = statusPositionFromPayload(layerStatus);
        if (layerStatus.duration !== undefined && statusPosition !== undefined && !seekSliderDragging && !activeSeekRequest) {
            syncVideoProgressPayload(layerStatus);
            const seekSlider = document.getElementById('seek-position-slider');
            const duration = currentSeekDuration;
            const position = getLiveSeekPosition();
            if (seekSlider) {
                seekSlider.step = '0.1';
                seekSlider.max = duration > 0 ? sliderSecondValue(duration) : '0';
                seekSlider.disabled = duration <= 0;
                seekSlider.value = sliderSecondValue(clampPlaybackPosition(position, duration, 0));
                updateSliderProgress(seekSlider, seekSlider.value);
            }
            updateSeekValueDisplay(position, duration);
        }
        if (layerStatus.playbackRate !== undefined && !playbackRateDragging) {
            updatePlaybackRateDisplay(parseFloat(layerStatus.playbackRate) || 1.0);
        }
    }
}

/**
 * 测试流视频播放
 * 从输入框获取URL并播放
 */
async function testStreamPlay() {
    const urlInput = document.getElementById('stream-url-input');
    const statusDiv = document.getElementById('stream-test-status');

    if (!urlInput || !statusDiv) {
        console.error('测试流播放：找不到必要的DOM元素');
        return;
    }

    const streamUrl = urlInput.value.trim();
    // 使用默认值：图层1，预览按单个循环
    const layerId = 1;
    const loop = 2;

    // 验证URL
    if (!streamUrl) {
        showStreamTestStatus('请输入流URL', 'error');
        return;
    }

    // 显示加载状态
    showStreamTestStatus('正在播放...', 'loading');

    try {
        const result = await apiPost('/video/play', {
            layerId: layerId,
            path: streamUrl,
            loop: loop
        }, { retryMutations: true });

        if (result == null) {
            const errorMsg = '播放失败';
            showStreamTestStatus(`✗ 播放失败: ${errorMsg}`, 'error');
            addToCommandLog('流视频测试', 'error', `播放失败: ${errorMsg}`);
            return;
        }

        if (result && typeof result === 'object' &&
            (result.state === 'blocked' || result.softBlocked || result.notice)) {
            const warnMsg = result.message || '当前设备不能同时播放两个4K视频';
            showStreamTestStatus(`⚠ ${warnMsg}`, 'warning');
            addToCommandLog('流视频测试', 'warning', warnMsg);
            return;
        }

        if (result !== null) {
            const message = result?.message || '播放成功';
            showStreamTestStatus(`✓ ${message}！图层${layerId}正在播放`, 'success');
            addToCommandLog('流视频测试', 'success', `成功播放: ${streamUrl}`);
        } else {
            showStreamTestStatus('✗ 播放失败: 请求无响应', 'error');
            addToCommandLog('流视频测试', 'error', '播放失败: 请求无响应');
        }

    } catch (error) {
        const errorMsg = error.message || '网络错误';
        showStreamTestStatus(`✗ 播放失败: ${errorMsg}`, 'error');
        addToCommandLog('流视频测试', 'error', `播放失败: ${errorMsg}`);
        console.error('测试流播放失败:', error);
    }
}

/**
 * 显示流测试状态
 * @param {string} message - 状态消息
 * @param {string} type - 状态类型: 'success', 'error', 'loading'
 */
function showStreamTestStatus(message, type = 'loading') {
    const statusDiv = document.getElementById('stream-test-status');
    if (!statusDiv) return;

    statusDiv.style.display = 'block';
    statusDiv.textContent = message;

    // 根据类型设置颜色
    switch (type) {
        case 'success':
            statusDiv.style.color = '#4ade80';
            statusDiv.style.background = 'rgba(74, 222, 128, 0.1)';
            break;
        case 'warning':
            statusDiv.style.color = '#fbbf24';
            statusDiv.style.background = 'rgba(251, 191, 36, 0.12)';
            break;
        case 'error':
            statusDiv.style.color = '#f87171';
            statusDiv.style.background = 'rgba(248, 113, 113, 0.1)';
            break;
        case 'loading':
        default:
            statusDiv.style.color = 'rgba(255, 255, 255, 0.7)';
            statusDiv.style.background = 'rgba(0, 0, 0, 0.3)';
            break;
    }
}
