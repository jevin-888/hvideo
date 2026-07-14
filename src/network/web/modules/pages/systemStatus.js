/**
 * @file systemStatus.js（文件名）
 * @brief 系统状态页面模块
 *
 * 本模块提供系统状态显示和更新功能，包括：
 * - 系统基本信息显示
 * - 实时状态更新
 * - 视频控制状态同步
 */

import { apiGet } from '../core/api.js';
import { updateVideoControlStatus } from './videoControl.js?v=2.98';
import { clearContainer } from '../utils/domHelpers.js';
import { setAppTitleVersion } from '../core/appTitle.js';

/** 资源监控更新定时器 */
let resourceUpdateInterval = null;
/** 是否已初始化 */
let isInitialized = false;

async function handleConnectionRestored() {
    await updateSystemStatus();
}

function handleStatusVisibilityChange() {
    if (document.visibilityState === 'visible') {
        updateSystemStatus();
    }
}

export function initializeStatusUpdate() {
    if (isInitialized) {
        return; // 防止重复初始化
    }
    isInitialized = true;

    // 立即更新一次系统状态
    updateSystemStatus();

    window.addEventListener('connectionRestored', handleConnectionRestored);
    document.addEventListener('visibilitychange', handleStatusVisibilityChange);

    // 资源监控更新更频繁，每2秒更新一次（资源监控不影响业务逻辑）
    updateSystemResources();
    resourceUpdateInterval = setInterval(updateSystemResources, 2000);
}

/**
 * 清理状态更新（页面卸载时调用）
 */
export function cleanupStatusUpdate() {
    if (resourceUpdateInterval) {
        clearInterval(resourceUpdateInterval);
        resourceUpdateInterval = null;
    }
    window.removeEventListener('connectionRestored', handleConnectionRestored);
    document.removeEventListener('visibilitychange', handleStatusVisibilityChange);
    isInitialized = false;
}

/**
 * @brief 更新系统状态
 *
 * 从服务器获取系统状态并更新页面显示
 */
export async function updateSystemStatus() {
    const status = await apiGet('/system/status');
    if (status) {
        const layers = await apiGet('/layers') || [];
        const videoStatus = await getVideoStatusMap(layers);
        status.layers = layers;
        status.video_status = videoStatus;

        // 更新基本信息
        const infoResolution = document.getElementById('info-resolution');
        const infoAudioType = document.getElementById('info-audio-type');
        const infoDeviceType = document.getElementById('info-device-type');
        const infoScreenRotate = document.getElementById('info-screen-rotate');
        const statusText = document.getElementById('status-text');
        const resolution = document.getElementById('resolution');

        if (infoResolution) infoResolution.textContent = formatResolution(status);
        if (infoAudioType) {
            infoAudioType.textContent = '自动管理';
        }
        if (infoDeviceType) infoDeviceType.textContent = formatDeviceType(status.device_type);
        if (infoScreenRotate) infoScreenRotate.textContent = `${status.screen_rotate || 0}°`;

        // 更新状态文本
        if (statusText) {
            statusText.textContent = '运行中';
            statusText.style.color = '#00ff00';
        }

        // 更新标题版本号：HVIDEO 音视频系统 v1.0.0
        if (status.app_version) {
            setAppTitleVersion(status.app_version);
        }

        // 更新分辨率显示（保留原有功能，但优先显示设备信息）
        if (resolution) {
            // 尝试获取网络信息并显示设备名称和IP
            updateDeviceInfo(resolution);
        }

        // 更新图层状态
        updateLayersStatus(layers);

        // 更新视频播放状态
        updateVideoStatus(videoStatus);

        // 更新视频控制页面状态
        updateVideoControlStatus(status);
    }
}

async function getVideoStatusMap(layers) {
    const videoLayers = Array.isArray(layers) ? layers.filter(layer => layer.type === 'video') : [];
    const entries = await Promise.all(videoLayers.map(async layer => {
        const layerStatus = await apiGet(`/video/status?layerId=${layer.id}`);
        return [layer.id, layerStatus || { state: 'unknown', path: '' }];
    }));
    return Object.fromEntries(entries);
}

function formatDeviceType(deviceType) {
    if (deviceType === null || deviceType === undefined || deviceType === '') return '未知';
    const type = Number(deviceType);
    if (Number.isNaN(type)) return String(deviceType);
    if (type === 0) return '标准版';
    if (type === 1) return '专业版';
    if (type === 2) return '旗舰版';
    return `类型${type}`;
}

// 更新设备信息显示（设备名称和IP）
async function updateDeviceInfo(element) {
    try {
        const networkInfo = await apiGet('/system/network/info');
        if (networkInfo) {
            const deviceName = networkInfo.device_name || '未知设备';
            const primaryIp = networkInfo.primary_ip || '未知IP';
            // 显示格式：设备名称 - IP地址
            element.textContent = `${deviceName} - ${primaryIp}`;
        } else {
            // 如果获取失败，显示默认值
            element.textContent = '未知设备 - 未知IP';
        }
    } catch (error) {
        element.textContent = '未知设备 - 未知IP';
    }
}

// 更新图层状态
function updateLayersStatus(layers) {
    const container = document.getElementById('layers-status-container');
    if (!container) return;
    clearContainer(container);

    if (!Array.isArray(layers) || layers.length === 0) {
        const emptyDiv = document.createElement('div');
        emptyDiv.className = 'info-item';
        emptyDiv.innerHTML = '<span class="info-value">暂无图层数据</span>';
        container.appendChild(emptyDiv);
        return;
    }

    layers.forEach(layer => {
        const layerDiv = document.createElement('div');
        layerDiv.className = 'info-item';
        layerDiv.innerHTML = `
            <span class="info-label">图层${layer.id}:</span>
            <span class="info-value">${layer.visible !== false ? '可见' : '隐藏'}-${formatLayerType(layer.type)}</span>
        `;
        container.appendChild(layerDiv);
    });
}

function formatResolution(status) {
    if (status.width && status.height) return `${status.width}x${status.height}`;
    if (status.canvas_in_width && status.canvas_in_height) return `${status.canvas_in_width}x${status.canvas_in_height}`;
    if (typeof status.resolution === 'string') return status.resolution.trim().replace(/\s+/, 'x');
    return '未知';
}

function formatLayerType(type) {
    if (type === 'video') return '视频';
    if (type === 'image') return '图片';
    if (type === 'text') return '文本';
    if (type === 'capture') return '采集';
    if (type === 'effect') return '特效';
    return type || '未知';
}

// 更新视频播放状态
function updateVideoStatus(videoStatus) {
    const container = document.getElementById('video-status-container');
    if (!container) return;
    clearContainer(container);

    if (!videoStatus || Object.keys(videoStatus).length === 0) {
        const emptyDiv = document.createElement('div');
        emptyDiv.className = 'info-item';
        emptyDiv.innerHTML = '<span class="info-value">暂无视频图层</span>';
        container.appendChild(emptyDiv);
        return;
    }

    // 更新状态列表
    for (const [layerId, status] of Object.entries(videoStatus)) {
        const videoDiv = document.createElement('div');
        videoDiv.className = 'info-item';
        const videoName = formatVideoName(status.path || status.current_path);
        videoDiv.innerHTML = `
            <span class="info-label">图层${layerId}:</span>
            <span class="info-value">${formatVideoState(status.state)}：${videoName}</span>
        `;
        container.appendChild(videoDiv);
    }
}

function formatVideoName(path) {
    if (!path) return '无视频';
    const normalized = String(path).split('?')[0].split('#')[0];
    const parts = normalized.split(/[\\/]/);
    return decodeURIComponent(parts[parts.length - 1] || normalized);
}

function formatVideoState(state) {
    if (state === 'playing') return '播放中';
    if (state === 'paused') return '已暂停';
    if (state === 'stopped') return '已停止';
    if (state === 'loaded') return '已加载';
    return state || '未知';
}

/**
 * @brief 更新系统资源使用情况
 *
 * 获取CPU和内存使用率并更新状态栏显示
 */
async function updateSystemResources() {
    try {
        const resources = await apiGet('/system/resources');
        if (resources) {
            const cpuUsageEl = document.getElementById('cpu-usage');
            const memUsageEl = document.getElementById('mem-usage');

            // 更新CPU使用率（若为多核汇总值 >100%，按核数归一化到 0–100%）
            if (cpuUsageEl) {
                let cpuUsage = resources.cpu_usage;
                if (cpuUsage >= 0) {
                    const cores = resources.cpu_cores || 0;
                    if (cores > 0 && cpuUsage > 100) cpuUsage = cpuUsage / cores;
                    cpuUsage = Math.min(100, cpuUsage);
                    cpuUsageEl.textContent = `${cpuUsage.toFixed(1)}%`;
                    cpuUsageEl.classList.remove('low', 'medium', 'high');
                    if (cpuUsage < 50) {
                        cpuUsageEl.classList.add('low');
                    } else if (cpuUsage < 80) {
                        cpuUsageEl.classList.add('medium');
                    } else {
                        cpuUsageEl.classList.add('high');
                    }
                } else {
                    cpuUsageEl.textContent = 'N/A';
                }
            }

            // 更新内存使用率
            if (memUsageEl && resources.memory) {
                const memUsage = resources.memory.usage_percent;
                memUsageEl.textContent = `${memUsage.toFixed(1)}%`;
                // 根据使用率设置颜色等级
                memUsageEl.classList.remove('low', 'medium', 'high');
                if (memUsage < 60) {
                    memUsageEl.classList.add('low');
                } else if (memUsage < 85) {
                    memUsageEl.classList.add('medium');
                } else {
                    memUsageEl.classList.add('high');
                }
            }
        }
    } catch (error) {
        // 静默处理错误，避免干扰用户
    }
}
