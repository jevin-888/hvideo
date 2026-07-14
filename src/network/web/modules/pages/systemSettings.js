// 系统设置页面模块
import { apiGet, apiPost, apiDelete, getLastApiError, apiAction } from '../core/api.js';
import { applyAppTitleFromLicense } from '../core/appTitle.js';
import { addToCommandLog } from '../core/commandLog.js';
import { roundNumbersInObject } from '../utils/configUtils.js';
import { parseApiResponse } from '../utils/apiResponseParser.js';
import { showSuccess, showError, showConfirm, showPrompt } from '../components/toast.js';
import { initializeFlexibleMapping, loadFlexibleMapping, setLicensedInputChannelCount } from './flexibleMapping.js?v=2.64';

// 局域网配置同步：唯一 base_path 值为 current，后端始终写入当前 ROOT_PATH。
const SYNC_CONFIG_DIRS = [
    'Effect', 'Image', 'Layout', 'Logo', 'Lyrics', 'Music', 'QRCode', 'Scene',
    'config', 'data', 'logs', 'models', 'shaders', 'singers', 'ttf', 'video', 'web'
];
const SYNC_BASE_PATH = 'current';
const SYNC_BASE_LABEL = '当前数据根目录';
const RENDER_QUALITY_SURFACE_SIZE = {
    smooth: '1920x1080',
    normal: '2560x1440',
    high: '2880x1620',
    ultra: '3840x2160'
};

// 模块名称映射
const MODULE_LABELS = {
    'TV': 'TV',
    '投屏': '投屏',
    '霸屏': '霸屏',
    'effects': '特效'
};

const USAGE_MODE_LABELS = {
    buyout: '正常销售',
    rent: '租赁',
    installment: '分期'
};

let logTree = [];
let selectedLogDate = '';
let selectedLogFile = '';
let currentLogText = '';
let logSearchIndex = -1;

// 初始化系统设置
export function initializeSettings() {
    // 初始化标签切换功能
    initializeSettingsTabs();
    // 初始化矩阵配置
    initializeFlexibleMapping();
    // 初始化局域网配置同步模块
    initializeLanSync();
    // 绑定授权管理按钮事件
    initializeLicenseButtons();
    initializeLogViewer();
    // 隐藏功能：连续点击 10 次修改设备名称
    initializeDeviceInfoSecretEdit();
    // 隐藏功能：供应商名称连续点击 10 次切换开机动画
    initializeBootAnimationSecretSwitch();
}

// 初始化设置页面标签切换
function initializeSettingsTabs() {
    const tabs = document.querySelectorAll('.settings-tab');
    const tabContents = document.querySelectorAll('.settings-tab-content');

    tabs.forEach(tab => {
        tab.addEventListener('click', async () => {
            const targetTab = tab.dataset.tab;
            if (targetTab === 'log-viewer' && !await unlockLogViewer()) {
                return;
            }

            // 移除所有活动状态
            tabs.forEach(t => t.classList.remove('active'));
            tabContents.forEach(content => content.classList.remove('active'));

            // 激活当前标签
            tab.classList.add('active');
            const targetContent = document.getElementById(`${targetTab}-tab`);
            if (targetContent) {
                targetContent.classList.add('active');
            }
            if (targetTab === 'log-viewer') {
                loadLogList();
            }
        });
    });
}

function initializeLogViewer() {
    const refreshBtn = document.getElementById('refresh-log-list-btn');
    if (refreshBtn) {
        refreshBtn.addEventListener('click', loadLogList);
    }
    const downloadBtn = document.getElementById('download-log-btn');
    if (downloadBtn) {
        downloadBtn.addEventListener('click', downloadSelectedLog);
    }
    const searchInput = document.getElementById('log-search-input');
    const prevBtn = document.getElementById('log-search-prev-btn');
    const nextBtn = document.getElementById('log-search-next-btn');
    if (searchInput) {
        searchInput.addEventListener('input', () => applyLogSearch(0));
        searchInput.addEventListener('keydown', (event) => {
            if (event.key === 'Enter') {
                event.preventDefault();
                applyLogSearch(event.shiftKey ? -1 : 1);
            }
        });
    }
    if (prevBtn) prevBtn.addEventListener('click', () => applyLogSearch(-1));
    if (nextBtn) nextBtn.addEventListener('click', () => applyLogSearch(1));
}

async function loadLogList() {
    const dateList = document.getElementById('log-date-list');
    const fileList = document.getElementById('log-file-list');
    const detail = document.getElementById('log-detail-content');
    if (!dateList || !fileList || !detail) return;

    dateList.innerHTML = '<div class="log-empty">加载中...</div>';
    fileList.innerHTML = '<div class="log-empty">请选择日期</div>';
    detail.textContent = '请选择日志文件';
    currentLogText = '';
    resetLogSearch();
    selectedLogDate = '';
    selectedLogFile = '';
    updateLogDownloadButton();

    try {
        const result = await apiGet('/logs');
        logTree = Array.isArray(result) ? result : [];
        renderLogDates();
    } catch (error) {
        dateList.innerHTML = '<div class="log-empty">日志列表加载失败</div>';
    }
}

function renderLogDates() {
    const dateList = document.getElementById('log-date-list');
    if (!dateList) return;
    if (!logTree.length) {
        dateList.innerHTML = '<div class="log-empty">暂无日志目录</div>';
        return;
    }
    dateList.innerHTML = logTree.map(item => `
        <button class="log-list-item${item.date === selectedLogDate ? ' active' : ''}" data-date="${item.date}">
            <span>${item.date}</span>
            <span class="log-delete-btn" data-date="${item.date}" title="删除">×</span>
        </button>
    `).join('');
    dateList.querySelectorAll('.log-list-item').forEach(btn => {
        btn.addEventListener('click', () => {
            selectedLogDate = btn.dataset.date || '';
            selectedLogFile = '';
            renderLogDates();
            renderLogFiles();
            const detail = document.getElementById('log-detail-content');
            if (detail) detail.textContent = '请选择日志文件';
            currentLogText = '';
            resetLogSearch();
            updateLogDownloadButton();
        });
    });
    dateList.querySelectorAll('.log-delete-btn').forEach(btn => {
        btn.addEventListener('click', async (event) => {
            event.stopPropagation();
            await deleteLogDate(btn.dataset.date || '');
        });
    });
}

function renderLogFiles() {
    const fileList = document.getElementById('log-file-list');
    if (!fileList) return;
    const dateNode = logTree.find(item => item.date === selectedLogDate);
    const files = Array.isArray(dateNode?.files) ? dateNode.files : [];
    if (!selectedLogDate) {
        fileList.innerHTML = '<div class="log-empty">请选择日期</div>';
        return;
    }
    if (!files.length) {
        fileList.innerHTML = '<div class="log-empty">暂无日志文件</div>';
        return;
    }
    fileList.innerHTML = files.map(file => `
        <button class="log-list-item${file.name === selectedLogFile ? ' active' : ''}" data-name="${file.name}">
            <span>${file.name}</span>
            <span class="log-file-actions">
                <small>${formatLogSize(file.size)}</small>
                <span class="log-delete-btn" data-name="${file.name}" title="删除">×</span>
            </span>
        </button>
    `).join('');
    fileList.querySelectorAll('.log-list-item').forEach(btn => {
        btn.addEventListener('click', async () => {
            selectedLogFile = btn.dataset.name || '';
            renderLogFiles();
            await loadLogDetail();
            updateLogDownloadButton();
        });
    });
    fileList.querySelectorAll('.log-delete-btn').forEach(btn => {
        btn.addEventListener('click', async (event) => {
            event.stopPropagation();
            await deleteLogFile(btn.dataset.name || '');
        });
    });
}

async function unlockLogViewer() {
    const password = await showPrompt('请输入日志管理密码', '日志管理', '', 'password');
    if (password === null) return false;
    if (String(password).trim() !== '9898') {
        showError('密码错误');
        return false;
    }
    return true;
}

async function deleteLogDate(date) {
    if (!date) return;
    const confirmed = await showConfirm(`确定删除日志目录 "${date}" 吗？目录内日志都会删除。`, '删除日志目录');
    if (!confirmed) return;
    const result = await apiDelete(`/logs/date?date=${encodeURIComponent(date)}`);
    if (result != null) {
        showSuccess('日志目录已删除');
        selectedLogDate = '';
        selectedLogFile = '';
        await loadLogList();
    } else {
        showError('删除日志目录失败');
    }
}

async function deleteLogFile(name) {
    if (!selectedLogDate || !name) return;
    const confirmed = await showConfirm(`确定删除日志文件 "${name}" 吗？`, '删除日志文件');
    if (!confirmed) return;
    const result = await apiDelete(`/logs/file?date=${encodeURIComponent(selectedLogDate)}&name=${encodeURIComponent(name)}`);
    if (result != null) {
        showSuccess('日志文件已删除');
        if (selectedLogFile === name) {
            selectedLogFile = '';
            const detail = document.getElementById('log-detail-content');
            if (detail) detail.textContent = '请选择日志文件';
            currentLogText = '';
            resetLogSearch();
        }
        await refreshLogListKeepDate();
    } else {
        showError('删除日志文件失败');
    }
}

async function refreshLogListKeepDate() {
    const result = await apiGet('/logs');
    logTree = Array.isArray(result) ? result : [];
    if (!logTree.some(item => item.date === selectedLogDate)) {
        selectedLogDate = '';
        selectedLogFile = '';
    }
    renderLogDates();
    renderLogFiles();
    updateLogDownloadButton();
}

async function loadLogDetail() {
    const detail = document.getElementById('log-detail-content');
    if (!detail || !selectedLogDate || !selectedLogFile) return;
    detail.textContent = '日志加载中...';
    try {
        const url = `/api/v1/logs/file?date=${encodeURIComponent(selectedLogDate)}&name=${encodeURIComponent(selectedLogFile)}`;
        const response = await fetch(url);
        currentLogText = response.ok ? await response.text() : '日志读取失败';
        detail.textContent = currentLogText;
        resetLogSearch(false);
        applyLogSearch(0);
        detail.scrollTop = detail.scrollHeight;
    } catch (error) {
        detail.textContent = '日志读取失败';
    }
}

function resetLogSearch(clearInput = true) {
    logSearchIndex = -1;
    if (clearInput) {
        const input = document.getElementById('log-search-input');
        if (input) input.value = '';
    }
    updateLogSearchCount(0, 0);
}

function applyLogSearch(direction = 0) {
    const input = document.getElementById('log-search-input');
    const detail = document.getElementById('log-detail-content');
    if (!input || !detail) return;
    const keyword = input.value.trim();
    if (!keyword || !currentLogText) {
        detail.textContent = currentLogText || detail.textContent;
        logSearchIndex = -1;
        updateLogSearchCount(0, 0);
        return;
    }
    const regex = new RegExp(escapeRegExp(keyword), 'gi');
    const matches = [...currentLogText.matchAll(regex)];
    if (!matches.length) {
        detail.textContent = currentLogText;
        logSearchIndex = -1;
        updateLogSearchCount(0, 0);
        return;
    }
    if (direction === 0 || logSearchIndex < 0) {
        logSearchIndex = 0;
    } else {
        logSearchIndex = (logSearchIndex + direction + matches.length) % matches.length;
    }
    let cursor = 0;
    let html = '';
    matches.forEach((match, index) => {
        const start = match.index;
        const end = start + match[0].length;
        html += escapeHtmlText(currentLogText.slice(cursor, start));
        html += `<mark class="log-search-hit${index === logSearchIndex ? ' current' : ''}">${escapeHtmlText(currentLogText.slice(start, end))}</mark>`;
        cursor = end;
    });
    html += escapeHtmlText(currentLogText.slice(cursor));
    detail.innerHTML = html;
    updateLogSearchCount(logSearchIndex + 1, matches.length);
    const current = detail.querySelector('.log-search-hit.current');
    if (current) current.scrollIntoView({ block: 'center' });
}

function updateLogSearchCount(current, total) {
    const count = document.getElementById('log-search-count');
    if (count) count.textContent = `${current}/${total}`;
}

function escapeRegExp(value) {
    return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function escapeHtmlText(value) {
    return String(value)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function downloadSelectedLog() {
    if (!selectedLogDate || !selectedLogFile) return;
    window.open(`/api/v1/logs/download?date=${encodeURIComponent(selectedLogDate)}&name=${encodeURIComponent(selectedLogFile)}`, '_blank');
}

function updateLogDownloadButton() {
    const btn = document.getElementById('download-log-btn');
    if (btn) btn.disabled = !selectedLogDate || !selectedLogFile;
}

function formatLogSize(size) {
    const value = Number(size) || 0;
    if (value >= 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)} MB`;
    if (value >= 1024) return `${(value / 1024).toFixed(1)} KB`;
    return `${value} B`;
}

// 绑定授权管理按钮事件
function initializeLicenseButtons() {
    // 导入授权文件按钮
    const importBtn = document.getElementById('import-license-btn');
    const fileInput = document.getElementById('license-file-input');
    if (importBtn && fileInput) {
        importBtn.addEventListener('click', () => fileInput.click());
        fileInput.addEventListener('change', importLicenseFile);
    }

    // 刷新按钮 - 同时刷新设备信息和授权状态
    const refreshBtn = document.getElementById('refresh-device-info-btn');
    if (refreshBtn) {
        refreshBtn.addEventListener('click', async () => {
            await loadDeviceInfo();
            await loadLicenseStatus();
        });
    }

    // 保存授权服务地址到 config.json
    const saveLicenseServerUrlBtn = document.getElementById('save-license-server-url-btn');
    const licenseServerUrlInput = document.getElementById('license-server-url');
    if (saveLicenseServerUrlBtn && licenseServerUrlInput) {
        saveLicenseServerUrlBtn.addEventListener('click', saveLicenseServerUrl);
    }

    // 保存公共配置（音频输出图层）
    const saveCommonConfigBtn = document.getElementById('save-common-config-btn');
    if (saveCommonConfigBtn) {
        saveCommonConfigBtn.addEventListener('click', saveCommonConfig);
    }

    // 点播模式切换显示联动
    const vodModeSelect = document.getElementById('common-vod-mode');
    if (vodModeSelect) {
        vodModeSelect.addEventListener('change', () => {
            updateVodGroupsVisibility(vodModeSelect.value);
        });
    }
    const networkIpModeSelect = document.getElementById('common-network-ip-mode');
    if (networkIpModeSelect) {
        networkIpModeSelect.addEventListener('change', async () => {
            updateNetworkIpModeVisibility(networkIpModeSelect.value);
            if (networkIpModeSelect.value === 'static') {
                await fillCurrentNetworkConfigIfEmpty();
            }
        });
    }

}

/**
 * @brief 统一配置更新助手 (获取 -> 更新 -> 保存)
 * @param {Function} updater 修改配置的回调函数
 * @param {string} logTag 日志标签
 */
async function updateConfig(updater, logTag = '系统设置') {
    try {
        const config = await apiGet('/config.json');
        if (!config || typeof config !== 'object') {
            throw new Error('无法从引擎拉取 authoritative config');
        }

        const updatedConfig = updater({ ...config });
        const result = await apiPost('/config.json', roundNumbersInObject(updatedConfig), 15000);

        if (result != null) {
            addToCommandLog(logTag, 'success', '配置已同步并持久化');
            showSuccess(`${logTag}成功`);
            return true;
        } else {
            const lastError = getLastApiError();
            throw new Error(result?.message || lastError?.message || '引擎拒绝了配置更新');
        }
    } catch (error) {
        addToCommandLog(logTag, 'error', error.message);
        showError(error.message);
        return false;
    }
}

async function saveCommonConfig() {
    const ok = await updateConfig(config => {
        const audioSelect = document.getElementById('common-audio-output-layer-id');
        const vodModeSelect = document.getElementById('common-vod-mode');
        const renderFrameRateMode = document.getElementById('common-render-frame-rate-mode');
        const renderQuality = document.getElementById('common-render-quality');
        const localSongFileScanEnabled = document.getElementById('common-local-song-file-scan-enabled');
        const appUpdateEnabled = document.getElementById('common-app-update-enabled');
        const networkIpMode = document.getElementById('common-network-ip-mode');
        const networkStaticIp = document.getElementById('common-network-static-ip');
        const networkGateway = document.getElementById('common-network-gateway');
        const networkDns = document.getElementById('common-network-dns');
        const debugHotspotEnabled = document.getElementById('common-debug-hotspot-enabled');
        const vodHost = document.getElementById('common-online-vod-host');
        const vodRoom = document.getElementById('common-online-vod-room-id');
        const powerScheduleEnabled = document.getElementById('common-power-schedule-enabled');
        const powerOnScheduleEnabled = document.getElementById('common-power-on-schedule-enabled');
        const powerOnDate = document.getElementById('common-power-on-date');
        const powerOnTime = document.getElementById('common-power-on-time');
        const powerOffScheduleEnabled = document.getElementById('common-power-off-schedule-enabled');
        const powerOffDate = document.getElementById('common-power-off-date');
        const powerOffTime = document.getElementById('common-power-off-time');

        if (audioSelect) {
            const audioLayerId = parseInt(audioSelect.value);
            config.audioOutputLayerId = Number.isNaN(audioLayerId) ? 0 : audioLayerId;
        }
        if (vodModeSelect) {
            const mode = parseInt(vodModeSelect.value) || 0;
            config.vodMode = mode;
            config.enableVod = (mode > 0);
        }
        if (renderFrameRateMode) {
            config.renderFrameRateMode = renderFrameRateMode.value === 'fixed30' ? 'fixed30' : 'auto';
        }
        if (renderQuality) {
            config.renderQuality = normalizeRenderQuality(renderQuality.value);
        }
        if (localSongFileScanEnabled) config.localSongFileScanEnabled = localSongFileScanEnabled.checked;
        if (appUpdateEnabled) config.appUpdateEnabled = appUpdateEnabled.checked;
        if (networkIpMode) config.networkIpMode = networkIpMode.value === 'static' ? 'static' : 'dynamic';
        if (networkStaticIp) config.networkStaticIp = networkStaticIp.value.trim();
        if (networkGateway) config.networkGateway = networkGateway.value.trim();
        if (networkDns) config.networkDns = networkDns.value.trim();
        if (debugHotspotEnabled) config.debugHotspotEnabled = debugHotspotEnabled.checked;
        if (vodHost) config.onlineVodHost = vodHost.value.trim();
        if (vodRoom) config.onlineVodRoomId = vodRoom.value.trim();
        if (powerScheduleEnabled) config.powerScheduleEnabled = powerScheduleEnabled.checked;
        if (powerOnScheduleEnabled) config.powerOnScheduleEnabled = powerOnScheduleEnabled.checked;
        if (powerOnDate) config.powerOnDate = powerOnDate.value;
        if (powerOnTime) config.powerOnTime = powerOnTime.value;
        if (powerOffScheduleEnabled) config.powerOffScheduleEnabled = powerOffScheduleEnabled.checked;
        if (powerOffDate) config.powerOffDate = powerOffDate.value;
        if (powerOffTime) config.powerOffTime = powerOffTime.value;

        return config;
    }, '公共配置');
    if (ok) {
        addToCommandLog('公共配置', 'info', '渲染质量和调试热点开关将在重启应用后生效');
    }
}

// 更新系统设置页面
export async function updateSettings() {
    await Promise.allSettled([
        loadDeviceInfo(),
        loadLicenseStatus(),
        loadConfigForSettings()
    ]);
    loadFlexibleMapping();
}

// ==================== 配置（config.json） ====================

// 加载 config 用于系统设置页展示（授权服务地址、公共配置等）
async function loadConfigForSettings() {
    try {
        const data = await apiGet('/config.json');
        const input = document.getElementById('license-server-url');
        if (input && data && typeof data.licenseServerUrl === 'string') {
            input.value = data.licenseServerUrl;
        } else if (input && (!data || data.licenseServerUrl === undefined)) {
            input.value = '';
        }
        const audioId = data && typeof data.audioOutputLayerId === 'number' ? data.audioOutputLayerId : 0;
        const v = (audioId >= 0 && audioId <= 4) ? String(audioId) : '0';
        const commonEl = document.getElementById('common-audio-output-layer-id');
        if (commonEl) commonEl.value = v;

        const vodModeEl = document.getElementById('common-vod-mode');
        const renderFrameRateModeEl = document.getElementById('common-render-frame-rate-mode');
        const renderQualityEl = document.getElementById('common-render-quality');
        const localSongFileScanEl = document.getElementById('common-local-song-file-scan-enabled');
        const appUpdateEnabledEl = document.getElementById('common-app-update-enabled');
        const networkIpModeEl = document.getElementById('common-network-ip-mode');
        const networkStaticIpEl = document.getElementById('common-network-static-ip');
        const networkGatewayEl = document.getElementById('common-network-gateway');
        const networkDnsEl = document.getElementById('common-network-dns');
        const debugHotspotEnabledEl = document.getElementById('common-debug-hotspot-enabled');
        const vodHostEl = document.getElementById('common-online-vod-host');
        const vodRoomEl = document.getElementById('common-online-vod-room-id');
        const powerScheduleEnabledEl = document.getElementById('common-power-schedule-enabled');
        const powerOnScheduleEnabledEl = document.getElementById('common-power-on-schedule-enabled');
        const powerOnDateEl = document.getElementById('common-power-on-date');
        const powerOnTimeEl = document.getElementById('common-power-on-time');
        const powerOffScheduleEnabledEl = document.getElementById('common-power-off-schedule-enabled');
        const powerOffDateEl = document.getElementById('common-power-off-date');
        const powerOffTimeEl = document.getElementById('common-power-off-time');

        let vodMode = 0;
        if (data && data.vodMode !== undefined) {
            vodMode = data.vodMode;
        } else if (data && data.enableVod) {
            vodMode = 2; // 默认映射为网络模式
        }

        if (vodModeEl) vodModeEl.value = String(vodMode);
        if (renderFrameRateModeEl) {
            renderFrameRateModeEl.value = data && data.renderFrameRateMode === 'fixed30' ? 'fixed30' : 'auto';
        }
        if (renderQualityEl) {
            renderQualityEl.value = normalizeRenderQuality(data && data.renderQuality);
        }
        if (localSongFileScanEl) localSongFileScanEl.checked = !!(data && data.localSongFileScanEnabled === true);
        if (appUpdateEnabledEl) appUpdateEnabledEl.checked = !(data && data.appUpdateEnabled === false);
        const networkIpMode = data && data.networkIpMode === 'static' ? 'static' : 'dynamic';
        if (networkIpModeEl) networkIpModeEl.value = networkIpMode;
        if (networkStaticIpEl) networkStaticIpEl.value = (data && data.networkStaticIp) || '';
        if (networkGatewayEl) networkGatewayEl.value = (data && data.networkGateway) || '';
        if (networkDnsEl) networkDnsEl.value = (data && data.networkDns) || '';
        if (debugHotspotEnabledEl) debugHotspotEnabledEl.checked = !!(data && data.debugHotspotEnabled === true);
        if (vodHostEl) vodHostEl.value = (data && data.onlineVodHost) || '';
        if (vodRoomEl) vodRoomEl.value = (data && data.onlineVodRoomId) || 'current';
        if (powerScheduleEnabledEl) powerScheduleEnabledEl.checked = !!(data && data.powerScheduleEnabled === true);
        if (powerOnScheduleEnabledEl) powerOnScheduleEnabledEl.checked = !!(data && data.powerOnScheduleEnabled === true);
        if (powerOnDateEl) powerOnDateEl.value = (data && data.powerOnDate) || '';
        if (powerOnTimeEl) powerOnTimeEl.value = (data && data.powerOnTime) || '';
        if (powerOffScheduleEnabledEl) powerOffScheduleEnabledEl.checked = !!(data && data.powerOffScheduleEnabled === true);
        if (powerOffDateEl) powerOffDateEl.value = (data && data.powerOffDate) || '';
        if (powerOffTimeEl) powerOffTimeEl.value = (data && data.powerOffTime) || '';

        updateVodGroupsVisibility(vodMode);
        updateNetworkIpModeVisibility(networkIpMode);
        if (networkIpMode === 'static') {
            await fillCurrentNetworkConfigIfEmpty();
        }
    } catch (_) {
        const input = document.getElementById('license-server-url');
        if (input) input.value = '';
    }
}

function normalizeRenderQuality(value) {
    return Object.prototype.hasOwnProperty.call(RENDER_QUALITY_SURFACE_SIZE, value)
        ? value
        : 'normal';
}

// 保存授权服务地址到 config.json（先拉取完整 config，修改后整份写回）
async function saveLicenseServerUrl() {
    const input = document.getElementById('license-server-url');
    if (!input) return;
    
    await updateConfig(config => {
        config.licenseServerUrl = input.value.trim();
        return config;
    }, '授权服务地址');
}

/** 控制VOD配置项的显隐联动 */
function updateVodGroupsVisibility(mode) {
    const localScanGroup = document.getElementById('local-song-file-scan-group');
    const hostGroup = document.getElementById('online-vod-host-group');
    const roomGroup = document.getElementById('online-vod-room-group');

    // 转换为数字进行比较
    const m = parseInt(mode);
    if (localScanGroup) localScanGroup.style.display = 'flex';
    
    const isNetwork = (m === 2);
    if (hostGroup) hostGroup.style.display = isNetwork ? 'flex' : 'none';
    if (roomGroup) roomGroup.style.display = isNetwork ? 'flex' : 'none';
}

function updateNetworkIpModeVisibility(mode) {
    const groups = [
        document.getElementById('common-network-static-ip-group'),
        document.getElementById('common-network-gateway-group'),
        document.getElementById('common-network-dns-group')
    ];
    groups.forEach(group => {
        if (group) group.style.display = mode === 'static' ? 'flex' : 'none';
    });
}

async function fillCurrentNetworkConfigIfEmpty() {
    const ipEl = document.getElementById('common-network-static-ip');
    const gatewayEl = document.getElementById('common-network-gateway');
    const dnsEl = document.getElementById('common-network-dns');
    if (!ipEl && !gatewayEl && !dnsEl) return;
    const hasValue = el => el && el.value && el.value.trim() !== '';
    if (hasValue(ipEl) && hasValue(gatewayEl) && hasValue(dnsEl)) return;
    try {
        const result = await apiGet('/system/network/current-ip-config');
        const data = result;
        if (ipEl && !hasValue(ipEl) && data && data.ip) ipEl.value = data.ip;
        if (gatewayEl && !hasValue(gatewayEl)) {
            if (data && data.gateway) {
                gatewayEl.value = data.gateway;
            } else if (data && data.dns && isIpv4(data.dns)) {
                gatewayEl.value = data.dns;
            } else if (ipEl && isIpv4(ipEl.value)) {
                gatewayEl.value = guessGatewayFromIp(ipEl.value);
            }
        }
        if (dnsEl && !hasValue(dnsEl) && data && data.dns) dnsEl.value = data.dns;
    } catch (_) {
    }
}

function isIpv4(value) {
    return /^(\d{1,3}\.){3}\d{1,3}$/.test(String(value || '').trim());
}

function guessGatewayFromIp(ip) {
    const parts = String(ip || '').trim().split('.');
    if (parts.length !== 4) return '';
    parts[3] = '1';
    return parts.join('.');
}

// ==================== 设备信息 ====================

// 加载设备信息
async function loadDeviceInfo() {
    try {
        const data = await apiGet('/system/device-info');
        if (data) {
            setTextContent('device-model', data.model);
            setTextContent('device-name', data.name || data.device_name || data.hostname || '--');
            setTextContent('device-serial', data.serial);
            setTextContent('device-mac', data.mac);
            setTextContent('device-ip', data.ip);
            setTextContent('device-fingerprint', data.fingerprint);
            addToCommandLog('加载设备信息', 'success', '设备信息加载成功');
        }
    } catch (error) {
        setTextContent('device-model', '待获取');
        setTextContent('device-name', '待获取');
        setTextContent('device-serial', '待获取');
        setTextContent('device-mac', '待获取');
        setTextContent('device-ip', '待获取');
        setTextContent('device-fingerprint', '待获取');
    }
}

// 隐藏功能：连续点击 10 次触发修改设备名称
function initializeDeviceInfoSecretEdit() {
    const nameEl = document.getElementById('device-name');

    function attachMultiClickEdit(element, label) {
        if (!element) return;
        let clickCount = 0;
        let clickTimer = null;
        element.style.cursor = 'default';

        element.addEventListener('click', async () => {
            clickCount++;
            if (clickTimer) clearTimeout(clickTimer);
            clickTimer = setTimeout(() => { clickCount = 0; }, 2000);

            if (clickCount >= 10) {
                clickCount = 0;
                clearTimeout(clickTimer);
                const currentValue = element.textContent || '';
                const newValue = await showPrompt(`请输入新的${label}`, `修改${label}`, currentValue, 'text', { dismissOnBackdrop: false });
                if (newValue !== null && newValue.trim() !== '') {
                    const result = await apiPost('/system/device-alias', { name: newValue.trim() });
                    if (result) {
                        element.textContent = newValue.trim();
                        addToCommandLog(`修改${label}`, 'success', `${label}已修改为: ${newValue.trim()}`);
                    } else {
                        addToCommandLog(`修改${label}`, 'error', `${label}修改失败`);
                    }
                }
            }
        });
    }

    attachMultiClickEdit(nameEl, '设备名称');
}

// 隐藏功能：连续点击供应商名称 10 次触发开机动画切换
function initializeBootAnimationSecretSwitch() {
    const supplierEl = document.getElementById('license-supplier');
    if (!supplierEl) return;

    let clickCount = 0;
    let clickTimer = null;
    supplierEl.style.cursor = 'default';

    supplierEl.addEventListener('click', async () => {
        clickCount++;
        if (clickTimer) clearTimeout(clickTimer);
        clickTimer = setTimeout(() => { clickCount = 0; }, 2000);

        if (clickCount >= 10) {
            clickCount = 0;
            clearTimeout(clickTimer);
            await showBootAnimationDialog();
        }
    });
}

async function showBootAnimationDialog() {
    let dialog = document.getElementById('boot-animation-dialog');
    if (!dialog) {
        dialog = document.createElement('div');
        dialog.id = 'boot-animation-dialog';
        dialog.className = 'dialog';
        dialog.innerHTML = `
            <div class="dialog-content">
                <h3>切换开机动画</h3>
                <div class="dialog-body">
                    <div class="dialog-input-row">
                        <select id="boot-animation-select" class="form-control"></select>
                    </div>
                    <p id="boot-animation-detail"></p>
                </div>
                <div class="dialog-footer">
                    <button id="boot-animation-cancel-btn" class="btn">取消</button>
                    <button id="boot-animation-install-btn" class="btn primary">确定</button>
                </div>
            </div>
        `;
        document.body.appendChild(dialog);

        dialog.querySelector('#boot-animation-cancel-btn').addEventListener('click', () => {
            dialog.classList.remove('active');
        });
    }

    const select = dialog.querySelector('#boot-animation-select');
    const detail = dialog.querySelector('#boot-animation-detail');
    const installBtn = dialog.querySelector('#boot-animation-install-btn');

    select.innerHTML = '<option value="">加载中...</option>';
    select.disabled = true;
    installBtn.disabled = true;
    detail.textContent = '';
    dialog.classList.add('active');

    const result = await apiGet('/system/boot-animation/list');
    if (!result || typeof result !== 'object') {
        const lastError = getLastApiError();
        select.innerHTML = '<option value="">读取失败</option>';
        detail.textContent = lastError?.message || '无法读取开机动画列表，请确认后台服务已启动。';
        return;
    }

    const animations = Array.isArray(result?.animations) ? result.animations : [];
    const validAnimations = animations.filter(item => item && item.slot >= 1 && item.slot <= 5);

    if (!validAnimations.length) {
        select.innerHTML = '<option value="">未找到可用动画</option>';
        detail.textContent = '';
        return;
    }

    select.innerHTML = validAnimations.map(item => (
        `<option value="${item.slot}">${escapeHtmlText(item.label || item.name || `Logo ${item.slot}`)}</option>`
    )).join('');
    select.disabled = false;
    installBtn.disabled = false;

    const updateDetail = () => {
        const selected = validAnimations.find(item => String(item.slot) === select.value);
        if (!selected) {
            detail.textContent = '';
            return;
        }
        detail.textContent = selected.custom
            ? `${selected.action} · ${selected.path || result.custom_path || '/sdcard/bootanimation5.zip'}`
            : selected.action;
    };
    select.onchange = updateDetail;
    updateDetail();

    installBtn.onclick = async () => {
        const slot = parseInt(select.value, 10);
        if (!slot) return;
        const selected = validAnimations.find(item => item.slot === slot);
        const confirmed = await showConfirm(
            `确定切换到 "${selected?.label || selected?.name || `Logo ${slot}`}" 吗？`,
            '切换开机动画'
        );
        if (!confirmed) return;

        installBtn.disabled = true;
        select.disabled = true;
        detail.textContent = '正在发送系统广播...';
        const installResult = await apiPost('/system/boot-animation/install', { slot }, { timeoutMs: 10000 });

        if (installResult != null) {
            dialog.classList.remove('active');
            addToCommandLog('切换开机动画', 'success', `${selected?.label || selected?.name || `Logo ${slot}`} 已发送`);
            showSuccess('开机动画切换广播已发送');
        } else {
            const lastError = getLastApiError();
            const message = installResult?.message || lastError?.message || '切换失败';
            addToCommandLog('切换开机动画', 'error', message);
            showError(message);
            detail.textContent = message;
            installBtn.disabled = false;
            select.disabled = false;
        }
    };
}

// 导入授权文件
async function importLicenseFile(event) {
    const file = event.target.files[0];
    if (!file) return;

    if (file.name !== 'license.dat') {
        addToCommandLog('导入授权文件', 'error', '请选择 license.dat 文件');
        event.target.value = '';
        return;
    }

    try {
        const formData = new FormData();
        formData.append('license', file);

        addToCommandLog('导入授权文件', 'info', `正在上传: ${file.name}`);

        const response = await fetch('/api/v1/system/license/import', {
            method: 'POST',
            body: formData
        });

        const responseText = await response.text();
        let responseJson;
        try {
            responseJson = JSON.parse(responseText);
        } catch (_) {
            throw new Error('后端返回非 JSON 响应，不符合统一 API contract');
        }
        const parsed = parseApiResponse(responseJson);
        if (response.ok && parsed.ok) {
            addToCommandLog('导入授权文件', 'success', `授权文件 ${file.name} 已导入成功`);
            // 导入成功后自动刷新授权状态
            await loadLicenseStatus();
        } else {
            const errMsg = parsed.error?.message || `HTTP ${response.status}`;
            addToCommandLog('导入授权文件', 'error', `导入失败: ${errMsg}`);
        }
    } catch (error) {
        addToCommandLog('导入授权文件', 'error', `导入授权文件失败: ${error.message || '网络或服务异常'}`);
    }

    // 重置 file input，允许重复选择同一文件
    event.target.value = '';
}

// ==================== 授权状态 ====================

// 加载授权状态
async function loadLicenseStatus() {
    try {
        const browserTime = Math.floor(Date.now() / 1000);
        const data = await apiGet(`/system/license?browser_time=${browserTime}`);
        if (data) {
            applyAppTitleFromLicense(data);
            updateStatusBadge(data.status, data.days_remaining);
            setTextContent('license-supplier', data.supplier_name || '--');
            setTextContent('license-customer', data.customer_name || '--');
            setTextContent('license-usage-mode', USAGE_MODE_LABELS[data.usage_mode] || data.usage_mode || '--');
            setTextContent('license-expiry', data.expiry_date || '--');
            const daysText = data.status === 'time_invalid' || data.status === 'time_calibrating'
                ? '等待云端校准'
                : (data.days_remaining !== undefined ? `${data.days_remaining} 天` : '--');
            setTextContent('license-days-remaining', daysText);
            const inputChannelCount = Number(data.input_channel_count) || 0;
            setTextContent('license-input-channel-count', inputChannelCount > 0 ? `${inputChannelCount}` : '不限制');
            setLicensedInputChannelCount(inputChannelCount);
            setTextContent('license-arrears', data.arrears_amount !== undefined ? `¥${data.arrears_amount.toFixed(2)}` : '--');
            updateModulesGrid(data.modules || []);
            updateLayersGrid(data.enabled_layers || []);
            addToCommandLog('加载授权状态', 'success', '授权状态加载成功');
        }
    } catch (error) {
        setTextContent('license-usage-mode', '--');
        setTextContent('license-input-channel-count', '--');
        setLicensedInputChannelCount(0);
        updateStatusBadge('unlicensed');
    }
}

// 更新状态标签
function updateStatusBadge(status, daysRemaining) {
    const badge = document.getElementById('license-status-badge');
    if (!badge) return;

    badge.classList.remove('valid', 'expiring', 'expired', 'buyout');

    if (status === 'valid') {
        if (daysRemaining !== undefined && daysRemaining <= 7) {
            badge.textContent = `即将到期 (${daysRemaining}天)`;
            badge.classList.add('expiring');
        } else {
            badge.textContent = '已授权';
            badge.classList.add('valid');
        }
    } else if (status === 'time_invalid' || status === 'time_calibrating') {
        badge.textContent = '等待云端校准';
        badge.classList.add('expiring');
    } else if (status === 'expired') {
        badge.textContent = '已过期';
        badge.classList.add('expired');
    } else {
        badge.textContent = '未授权';
    }
}

// 更新模块网格
function updateModulesGrid(enabledModules) {
    const grid = document.getElementById('license-modules-grid');
    if (!grid) return;

    grid.innerHTML = '';
    const allModules = ['TV', '投屏', '霸屏', 'effects'];

    allModules.forEach(mod => {
        const tag = document.createElement('div');
        const isEnabled = enabledModules.includes(mod);
        tag.className = `module-tag ${isEnabled ? 'enabled' : 'disabled'}`;
        tag.textContent = MODULE_LABELS[mod] || mod;
        grid.appendChild(tag);
    });
}

// 更新图层网格
function updateLayersGrid(enabledLayers) {
    const grid = document.getElementById('license-layers-grid');
    if (!grid) return;

    grid.innerHTML = '';

    if (enabledLayers.length === 0) {
        grid.innerHTML = '<span class="info-value" style="color: #888;">未配置</span>';
        return;
    }

    enabledLayers.forEach(layerId => {
        const tag = document.createElement('span');
        tag.className = 'layer-tag';
        tag.textContent = `图层 ${layerId}`;
        grid.appendChild(tag);
    });
}

// ==================== 局域网配置同步 ====================

/** 当前扫描到的同型号设备列表（ip -> { ip, name? }） */
let lanSyncDevices = [];

/** 同步方向模式：'to-device' 推送到选中设备（左侧列表可勾选）| 'to-local' 拉取到本机（右侧列表可勾选） */
let lanSyncMode = 'to-device';

/** 同步过程中箭头方向：'' 不显示 | 'to-device' 本机→设备 → | 'to-local' 设备→本机 ← */
let lanSyncArrowDirection = '';

/** 渲染扫描设备卡片：设备列表或占位 */
function renderLanSyncDeviceList(devices) {
    const container = document.getElementById('lan-sync-device-list');
    if (!container) return;

    container.innerHTML = '';
    if (!devices || devices.length === 0) {
        const ph = document.createElement('div');
        ph.className = 'lan-sync-device-placeholder';
        ph.textContent = '点击「扫描」发现设备';
        container.appendChild(ph);
        updateLanSyncDeviceFolderTitle();
        return;
    }

    devices.forEach((dev, i) => {
        const ip = dev.ip || dev.id;
        const name = dev.name || dev.ip || dev.id || `设备${i + 1}`;
        const label = document.createElement('label');
        label.className = 'lan-sync-device-item';
        label.setAttribute('data-lan-ip', ip);
        label.setAttribute('data-lan-name', String(name));
        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.value = ip;
        cb.setAttribute('data-lan-ip', ip);
        label.appendChild(cb);
        label.appendChild(document.createTextNode(name));
        container.appendChild(label);
    });

    container.querySelectorAll('input[type="checkbox"]').forEach(cb => {
        cb.addEventListener('change', updateLanSyncDeviceFolderTitle);
    });
    updateLanSyncDeviceFolderTitle();
}

/** 渲染文件夹卡片：设备侧与本机侧均为可勾选列表，由 lanSyncMode 决定当前哪侧可操作 */
function renderLanSyncFolderPanels() {
    const deviceList = document.getElementById('lan-sync-dirs-device-list');
    const localList = document.getElementById('lan-sync-dirs-local-list');
    if (!deviceList || !localList) return;

    deviceList.innerHTML = '';
    localList.innerHTML = '';

    SYNC_CONFIG_DIRS.forEach(dir => {
        const label = document.createElement('label');
        label.className = 'lan-sync-dir-item';
        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.className = 'lan-sync-dir-cb';
        cb.value = dir;
        cb.checked = true;
        cb.setAttribute('data-lan-dir', dir);
        label.appendChild(cb);
        label.appendChild(document.createTextNode(dir));
        deviceList.appendChild(label);
    });

    SYNC_CONFIG_DIRS.forEach(dir => {
        const label = document.createElement('label');
        label.className = 'lan-sync-dir-item';
        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.className = 'lan-sync-dir-cb';
        cb.value = dir;
        cb.checked = true;
        cb.setAttribute('data-lan-dir', dir);
        label.appendChild(cb);
        label.appendChild(document.createTextNode(dir));
        localList.appendChild(label);
    });

    applyLanSyncMode();
}

/** 根据 lanSyncMode 更新方向按钮高亮，以及哪一侧文件夹列表可勾选 */
function applyLanSyncMode() {
    const pullBtn = document.getElementById('lan-sync-mode-pull-btn');
    const pushBtn = document.getElementById('lan-sync-mode-push-btn');
    const devicePanel = document.querySelector('.lan-sync-folder-panel.lan-sync-folder-device');
    const localPanel = document.querySelector('.lan-sync-folder-panel.lan-sync-folder-local');

    if (pullBtn) pullBtn.classList.toggle('active', lanSyncMode === 'to-local');
    if (pushBtn) pushBtn.classList.toggle('active', lanSyncMode === 'to-device');

    if (devicePanel) {
        devicePanel.classList.toggle('lan-sync-folder-panel-selectable', lanSyncMode === 'to-device');
        devicePanel.classList.toggle('lan-sync-folder-panel-disabled', lanSyncMode === 'to-local');
    }
    if (localPanel) {
        localPanel.classList.toggle('lan-sync-folder-panel-selectable', lanSyncMode === 'to-local');
        localPanel.classList.toggle('lan-sync-folder-panel-disabled', lanSyncMode === 'to-device');
    }
}

/** 根据当前勾选的设备更新「设备文件夹」标题（选中哪个显示哪个，如 001 设备） */
function updateLanSyncDeviceFolderTitle() {
    const titleEl = document.getElementById('lan-sync-device-folder-title');
    if (!titleEl) return;

    const checked = document.querySelectorAll('#lan-sync-device-list .lan-sync-device-item input:checked');
    if (checked.length === 0) {
        titleEl.textContent = '请选择设备';
        return;
    }
    if (checked.length === 1) {
        const label = checked[0].closest('.lan-sync-device-item');
        const name = label ? (label.getAttribute('data-lan-name') || label.textContent.trim() || '设备') : '设备';
        titleEl.textContent = name;
        return;
    }
    titleEl.textContent = `已选 ${checked.length} 台设备`;
}

function initializeLanSync() {
    renderLanSyncDeviceList([]);
    renderLanSyncFolderPanels();

    const scanBtn = document.getElementById('lan-sync-scan-btn');
    if (scanBtn) {
        scanBtn.addEventListener('click', scanLanDevices);
    }

    const pullModeBtn = document.getElementById('lan-sync-mode-pull-btn');
    if (pullModeBtn) {
        pullModeBtn.addEventListener('click', () => {
            lanSyncMode = 'to-local';
            applyLanSyncMode();
        });
    }
    const pushModeBtn = document.getElementById('lan-sync-mode-push-btn');
    if (pushModeBtn) {
        pushModeBtn.addEventListener('click', () => {
            lanSyncMode = 'to-device';
            applyLanSyncMode();
        });
    }
    const execBtn = document.getElementById('lan-sync-exec-btn');
    if (execBtn) {
        execBtn.addEventListener('click', () => {
            if (lanSyncMode === 'to-device') startLanSync();
            else startLanSyncPull();
        });
    }
}

async function scanLanDevices() {
    addToCommandLog('扫描设备', 'info', '正在扫描局域网同型号设备…');
    try {
        const data = await apiGet('/system/lan-devices').catch(() => null);
        const devices = (data && Array.isArray(data.devices)) ? data.devices : (Array.isArray(data) ? data : []);

        if (devices.length > 0) {
            lanSyncDevices = devices;
            renderLanSyncDeviceList(devices);
            addToCommandLog('扫描设备', 'success', `发现 ${devices.length} 台设备`);
        } else {
            lanSyncDevices = [];
            renderLanSyncDeviceList([]);
            addToCommandLog('扫描设备', 'info', '未发现同型号设备');
        }
    } catch (e) {
        addToCommandLog('扫描设备', 'error', (e && e.message) || '扫描失败');
        lanSyncDevices = [];
        renderLanSyncDeviceList([]);
    }
}

function getSelectedLanDirs() {
    const selector = lanSyncMode === 'to-device'
        ? '#lan-sync-dirs-device-list .lan-sync-dir-cb:checked'
        : '#lan-sync-dirs-local-list .lan-sync-dir-cb:checked';
    const list = document.querySelectorAll(selector);
    return Array.from(list).map(el => el.value);
}

function getSelectedLanDevices() {
    const list = document.querySelectorAll('.lan-sync-device-item input[type="checkbox"]:checked');
    return Array.from(list).map(el => el.value);
}

/** 根据当前同步方向更新箭头显示（默认不显示，同步中显示 → 或 ←） */
function updateLanSyncArrowCells() {
    const symbol = lanSyncArrowDirection === 'to-device' ? '→' : (lanSyncArrowDirection === 'to-local' ? '←' : '');
    document.querySelectorAll('.lan-sync-arrow-cell').forEach(el => { el.textContent = symbol; });
}

async function startLanSync() {
    const dirs = getSelectedLanDirs();
    const devices = getSelectedLanDevices();
    if (dirs.length === 0) {
        showError('请至少勾选一个要同步的配置子目录', 2500);
        return;
    }
    if (lanSyncDevices.length > 0 && devices.length === 0) {
        showError('请先扫描设备并勾选要同步的目标设备', 2500);
        return;
    }

    lanSyncArrowDirection = 'to-device';
    updateLanSyncArrowCells();
    addToCommandLog('配置同步', 'info', `正在同步到选中设备: ${SYNC_BASE_LABEL} [${dirs.join(', ')}]`);
    try {
        const result = await apiAction('sync', 'sync_device', { sync_data: {
                    timestamp: Math.floor(Date.now() / 1000),
                    sync_mode: 0,
                    base_path: SYNC_BASE_PATH,
                    dirs: dirs,
                    target_ips: devices
                } });
        const completed = result != null;
        if (completed) {
            const msg = '同步请求已完成';
            showSuccess(msg, 3000);
            addToCommandLog('配置同步', 'success', msg);
        } else {
            const errMsg = result?.message || '同步失败';
            showError(errMsg, 3000);
            addToCommandLog('配置同步', 'error', errMsg);
        }
    } catch (e) {
        const errMsg = (e && e.message) ? e.message : '请求失败';
        showError(errMsg, 3000);
        addToCommandLog('配置同步', 'error', errMsg);
    } finally {
        lanSyncArrowDirection = '';
        updateLanSyncArrowCells();
    }
}

/** 从选中设备同步到本机：勾选一个设备作为来源，将对方目录拉取到本机 */
async function startLanSyncPull() {
    const dirs = getSelectedLanDirs();
    const devices = getSelectedLanDevices();
    if (dirs.length === 0) {
        showError('请至少勾选一个要同步的配置子目录', 2500);
        return;
    }
    if (devices.length !== 1) {
        showError('请勾选一个设备作为来源（从该设备同步到本机）', 2500);
        return;
    }
    const sourceIp = devices[0];

    lanSyncArrowDirection = 'to-local';
    updateLanSyncArrowCells();
    addToCommandLog('配置同步', 'info', `正在从 ${sourceIp} 同步到本机: ${SYNC_BASE_LABEL} [${dirs.join(', ')}]`);
    try {
        const result = await apiAction('sync', 'sync_device', { sync_data: {
                    timestamp: Math.floor(Date.now() / 1000),
                    sync_mode: 0,
                    base_path: SYNC_BASE_PATH,
                    dirs: dirs,
                    source_ip: sourceIp
                } });
        const completed = result != null;
        if (completed) {
            const msg = '已从设备同步到本机';
            showSuccess(msg, 3000);
            addToCommandLog('配置同步', 'success', msg);
        } else {
            const errMsg = result?.message || '拉取同步失败';
            showError(errMsg, 3000);
            addToCommandLog('配置同步', 'error', errMsg);
        }
    } catch (e) {
        const errMsg = (e && e.message) ? e.message : '请求失败';
        showError(errMsg, 3000);
        addToCommandLog('配置同步', 'error', errMsg);
    } finally {
        lanSyncArrowDirection = '';
        updateLanSyncArrowCells();
    }
}

// ==================== 工具函数 ====================

function setTextContent(id, text) {
    const el = document.getElementById(id);
    if (el) el.textContent = text || '--';
}
