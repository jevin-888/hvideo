// 系统设置页面模块
import { apiGet, apiPost, apiDelete, getLastApiError, apiAction } from '../core/api.js';
import { applyAppTitleFromLicense } from '../core/appTitle.js';
import { MODULE_LABELS, applyModuleLicense, isModuleLicensed } from '../core/moduleLicense.js';
import { addToCommandLog } from '/shared/commandLog.js';
import { roundNumbersInObject } from '../utils/configUtils.js';
import { parseApiResponse } from '/shared/apiResponseParser.js';
import { showSuccess, showError, showConfirm, showPrompt } from '../components/toast.js';
import { initializeFlexibleMapping, loadFlexibleMapping, setLicensedInputChannelCount } from './flexibleMapping.js?v=2.65';
import { initializeDisplayOutput, loadDisplayOutput } from './displayOutput.js?v=2';
import { initializeDisplayResolution, loadDisplayResolution } from './displayResolution.js?v=1';

// 局域网配置同步：唯一 base_path 值为 current，后端始终写入当前 ROOT_PATH。
const SYNC_CONFIG_DIRS = [
    'Effect', 'Image', 'Layout', 'Logo', 'Lyrics', 'Music', 'QRCode', 'Scene',
    'config', 'data', 'logs', 'models', 'shaders', 'singers', 'ttf', 'video', 'web'
];
const SYNC_BASE_PATH = 'current';
const SYNC_REQUEST_TIMEOUT_MS = 3 * 60 * 60 * 1000;
const SYNC_BASE_LABEL = '当前数据根目录';
const RENDER_QUALITY_SURFACE_SIZE = {
    smooth: '1920x1080',
    normal: '2560x1440',
    high: '2880x1620',
    ultra: '3840x2160'
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
    initializeCommonConfigSwitcher();
    initializeDisplayOutput();
    initializeDisplayResolution();
    initializeLyricPreview();
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

function initializeLyricPreview() {
    const panel = document.getElementById('common-config-panel-lyric');
    if (!panel) return;
    panel.addEventListener('change', updateLyricPreview);
    updateLyricPreview();
}

function updateLyricPreview() {
    const stage = document.getElementById('lyric-preview-stage');
    if (!stage) return;
    const pinyin = document.getElementById('common-lyric-display-mode')?.value === 'pinyin';
    const headerVisible = document.getElementById('common-lyric-header-visible')?.checked === true;
    const backgroundVisible = document.getElementById('common-lyric-background-visible')?.checked === true;
    const modeLabel = pinyin ? '纯拼音 · 带声调' : '原文歌词';
    document.getElementById('lyric-preview-mode').textContent = modeLabel;
    document.getElementById('lyric-preview-header').hidden = !headerVisible;
    document.getElementById('lyric-preview-sung').textContent = pinyin ? 'ràng měi yí jù gē' : '让每一句歌';
    document.getElementById('lyric-preview-pending').textContent = pinyin ? 'dōu qīng xī dòng rén' : '都清晰动人';
    document.getElementById('lyric-preview-next').textContent = pinyin ? 'bǎ měi hǎo liú zài zhè yí kè' : '把美好留在这一刻';
    stage.classList.toggle('has-background', backgroundVisible);
    stage.classList.toggle('is-pinyin', pinyin);
    stage.setAttribute('aria-label', `${modeLabel}排版示意，歌曲信息${headerVisible ? '显示' : '隐藏'}，歌词背景${backgroundVisible ? '开启' : '关闭'}`);
}

function initializeCommonConfigSwitcher() {
    const tabs = Array.from(document.querySelectorAll('[data-common-config-target]'));
    const panels = Array.from(document.querySelectorAll('[data-common-config-panel]'));
    if (tabs.length === 0 || panels.length === 0) return;

    const activate = (target, focusTab = false) => {
        const availableTabs = tabs.filter(tab => !tab.hidden && !tab.disabled);
        const selectedTab = availableTabs.find(tab => tab.dataset.commonConfigTarget === target) || availableTabs[0];
        if (!selectedTab) return;
        const selectedTarget = selectedTab.dataset.commonConfigTarget;

        tabs.forEach(tab => {
            const active = tab === selectedTab;
            tab.classList.toggle('active', active);
            tab.setAttribute('aria-selected', String(active));
            tab.tabIndex = active ? 0 : -1;
        });
        panels.forEach(panel => {
            const active = panel.dataset.commonConfigPanel === selectedTarget;
            panel.classList.toggle('active', active);
            panel.hidden = !active;
        });

        if (focusTab) selectedTab.focus();
    };

    tabs.forEach(tab => {
        tab.addEventListener('click', () => activate(tab.dataset.commonConfigTarget));
        tab.addEventListener('keydown', event => {
            const availableTabs = tabs.filter(item => !item.hidden && !item.disabled);
            const currentIndex = availableTabs.indexOf(tab);
            let nextIndex = currentIndex;
            if (event.key === 'ArrowRight') nextIndex = (currentIndex + 1) % availableTabs.length;
            else if (event.key === 'ArrowLeft') nextIndex = (currentIndex - 1 + availableTabs.length) % availableTabs.length;
            else if (event.key === 'Home') nextIndex = 0;
            else if (event.key === 'End') nextIndex = availableTabs.length - 1;
            else return;

            event.preventDefault();
            activate(availableTabs[nextIndex]?.dataset.commonConfigTarget, true);
        });
    });

    activate(tabs.find(tab => tab.classList.contains('active'))?.dataset.commonConfigTarget);
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
        const token = localStorage.getItem('hvideo_auth_token');
        const response = await fetch(url, token
            ? { headers: { Authorization: `Bearer ${token}` } } : undefined);
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
    const syncRoleSelect = document.getElementById('common-sync-role');
    if (syncRoleSelect) {
        syncRoleSelect.addEventListener('change', updateSyncLeaderVisibility);
    }
    updateSyncLeaderVisibility();

}

function updateSyncLeaderVisibility() {
    const role = document.getElementById('common-sync-role')?.value || 'follower';
    const input = document.getElementById('common-sync-leader-ip');
    if (!input) return;
    const item = input.closest('.setting-item');
    if (item) item.style.opacity = role === 'leader' ? '0.65' : '1';
    input.placeholder = role === 'leader'
        ? '主设备可留空；从设备填写主设备 IP'
        : '填写主设备 IP，例如 192.168.2.103';
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
            // VOD 增益和歌词选项需要跨应用重启保持；保存后立即从引擎权威配置回读，
            // 防止旧缓存或并发配置写入造成页面显示成功但实际未落盘。
            const persistedKeys = ['startupIpHintEnabled', 'playbackOperationHintEnabled', 'vodVolumeGainDb', 'lyricDisplayMode', 'lyricFont', 'lyricHeaderVisible', 'lyricBackgroundVisible',
                'lyricTitleFontSize', 'lyricTitleOutlineWidth', 'lyricTextFontSize',
                'lyricTextOutlineWidth', 'lyricTextSpacing', 'lyricProducerText'];
            const needsReadback = persistedKeys.some(key => Object.prototype.hasOwnProperty.call(updatedConfig, key));
            if (needsReadback) {
                const persisted = await apiGet('/config.json');
                const expectedPersisted = roundNumbersInObject(updatedConfig);
                const mismatch = persistedKeys.some(key =>
                    Object.prototype.hasOwnProperty.call(updatedConfig, key) &&
                    persisted?.[key] !== expectedPersisted[key]);
                if (mismatch) {
                    throw new Error('公共配置回读不一致，未确认持久化，请重试');
                }
            }
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
        const originalConfig = { ...config };
        const audioSelect = document.getElementById('common-audio-output-layer-id');
        const vodModeSelect = document.getElementById('common-vod-mode');
        const renderFrameRateMode = document.getElementById('common-render-frame-rate-mode');
        const renderQuality = document.getElementById('common-render-quality');
        const playbackOperationHintEnabled = document.getElementById('common-playback-operation-hint-enabled');
        const startupIpHintEnabled = document.getElementById('common-startup-ip-hint-enabled');
        const localSongFileScanEnabled = document.getElementById('common-local-song-file-scan-enabled');
        const appUpdateEnabled = document.getElementById('common-app-update-enabled');
        const networkIpMode = document.getElementById('common-network-ip-mode');
        const networkStaticIp = document.getElementById('common-network-static-ip');
        const networkGateway = document.getElementById('common-network-gateway');
        const networkDns = document.getElementById('common-network-dns');
        const startupWirelessMode = document.getElementById('common-startup-wireless-mode');
        const usbOtgMode = document.getElementById('common-usb-otg-mode');
        const vodHost = document.getElementById('common-online-vod-host');
        const vodRoom = document.getElementById('common-online-vod-room-id');
        const powerScheduleEnabled = document.getElementById('common-power-schedule-enabled');
        const powerOnScheduleEnabled = document.getElementById('common-power-on-schedule-enabled');
        const powerOnDate = document.getElementById('common-power-on-date');
        const powerOnTime = document.getElementById('common-power-on-time');
        const powerOffScheduleEnabled = document.getElementById('common-power-off-schedule-enabled');
        const powerOffDate = document.getElementById('common-power-off-date');
        const powerOffTime = document.getElementById('common-power-off-time');
        const lyricFont = document.getElementById('common-lyric-font');
        const lyricHeaderVisible = document.getElementById('common-lyric-header-visible');
        const lyricBackgroundVisible = document.getElementById('common-lyric-background-visible');
        const lyricDisplayMode = document.getElementById('common-lyric-display-mode');
        const lyricTitleFontSize = document.getElementById('common-lyric-title-font-size');
        const lyricTitleOutlineWidth = document.getElementById('common-lyric-title-outline-width');
        const lyricTextFontSize = document.getElementById('common-lyric-text-font-size');
        const lyricTextOutlineWidth = document.getElementById('common-lyric-text-outline-width');
        const lyricTextSpacing = document.getElementById('common-lyric-text-spacing');
        const lyricProducerText = document.getElementById('common-lyric-producer-text');
        const syncEnabled = document.getElementById('common-sync-enabled');
        const syncRole = document.getElementById('common-sync-role');
        const syncGroupId = document.getElementById('common-sync-group-id');
        const syncLeaderIp = document.getElementById('common-sync-leader-ip');
        const syncMulticastAddr = document.getElementById('common-sync-multicast-addr');
        const syncInterface = document.getElementById('common-sync-interface');
        const syncTtl = document.getElementById('common-sync-ttl');

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
        if (playbackOperationHintEnabled) config.playbackOperationHintEnabled = playbackOperationHintEnabled.checked;
        if (startupIpHintEnabled) config.startupIpHintEnabled = startupIpHintEnabled.checked;
        if (localSongFileScanEnabled) config.localSongFileScanEnabled = localSongFileScanEnabled.checked;
        if (appUpdateEnabled) config.appUpdateEnabled = appUpdateEnabled.checked;
        if (networkIpMode) config.networkIpMode = networkIpMode.value === 'static' ? 'static' : 'dynamic';
        if (networkStaticIp) config.networkStaticIp = networkStaticIp.value.trim();
        if (networkGateway) config.networkGateway = networkGateway.value.trim();
        if (networkDns) config.networkDns = networkDns.value.trim();
        if (startupWirelessMode) config.startupWirelessMode = startupWirelessMode.value === 'wifi' ? 'wifi' : 'hotspot';
        if (usbOtgMode) {
            config.usbOtgMode = ['otg', 'peripheral', 'host'].includes(usbOtgMode.value)
                ? usbOtgMode.value
                : 'host';
        }
        if (vodHost) config.onlineVodHost = vodHost.value.trim();
        if (vodRoom) config.onlineVodRoomId = vodRoom.value.trim();
        const vodGain = document.getElementById('common-vod-volume-gain-db');
        if (vodGain) {
            const gainDb = Number(vodGain.value);
            if (!Number.isInteger(gainDb) || gainDb < 0 || gainDb > 10) {
                throw new Error('VOD 音量增益请选择 0 至 +10 dB');
            }
            config.vodVolumeGainDb = gainDb;
        }
        if (powerScheduleEnabled) config.powerScheduleEnabled = powerScheduleEnabled.checked;
        if (powerOnScheduleEnabled) config.powerOnScheduleEnabled = powerOnScheduleEnabled.checked;
        if (powerOnDate) config.powerOnDate = powerOnDate.value;
        if (powerOnTime) config.powerOnTime = powerOnTime.value;
        if (powerOffScheduleEnabled) config.powerOffScheduleEnabled = powerOffScheduleEnabled.checked;
        if (powerOffDate) config.powerOffDate = powerOffDate.value;
        if (powerOffTime) config.powerOffTime = powerOffTime.value;
        if (lyricFont) config.lyricFont = normalizeLyricFontValue(lyricFont.value);
        if (lyricHeaderVisible) config.lyricHeaderVisible = lyricHeaderVisible.checked;
        if (lyricBackgroundVisible) config.lyricBackgroundVisible = lyricBackgroundVisible.checked;
        if (lyricDisplayMode) config.lyricDisplayMode = lyricDisplayMode.value === 'pinyin' ? 'pinyin' : 'original';
        const readLyricNumber = (element, fallback) => {
            if (!element) return fallback;
            const value = Number(element.value);
            return Number.isFinite(value) ? value : fallback;
        };
        config.lyricTitleFontSize = readLyricNumber(lyricTitleFontSize, 150);
        config.lyricTitleOutlineWidth = readLyricNumber(lyricTitleOutlineWidth, 8);
        config.lyricTextFontSize = readLyricNumber(lyricTextFontSize, 140);
        config.lyricTextOutlineWidth = readLyricNumber(lyricTextOutlineWidth, 10);
        config.lyricTextSpacing = readLyricNumber(lyricTextSpacing, -10);
        if (lyricProducerText) config.lyricProducerText = lyricProducerText.value.slice(0, 256);
        if (!isModuleLicensed('vod')) {
            for (const key of ['audioOutputLayerId', 'vodMode', 'enableVod', 'onlineVodHost', 'onlineVodRoomId', 'vodVolumeGainDb',
                'lyricFont', 'lyricHeaderVisible', 'lyricBackgroundVisible', 'lyricDisplayMode',
                'lyricTitleFontSize', 'lyricTitleOutlineWidth', 'lyricTextFontSize',
                'lyricTextOutlineWidth', 'lyricTextSpacing', 'lyricProducerText']) {
                if (Object.prototype.hasOwnProperty.call(originalConfig, key)) config[key] = originalConfig[key];
                else delete config[key];
            }
        }
        if (syncEnabled || syncRole || syncGroupId || syncLeaderIp || syncMulticastAddr || syncInterface || syncTtl) {
            const previousSync = config.syncGroup && typeof config.syncGroup === 'object'
                ? config.syncGroup
                : {};
            const ttlValue = syncTtl ? Number.parseInt(syncTtl.value, 10) : Number(previousSync.ttl);
            config.syncGroup = {
                ...previousSync,
                enabled: syncEnabled ? syncEnabled.checked : !!previousSync.enabled,
                role: syncRole?.value === 'leader' ? 'leader' : 'follower',
                groupId: (syncGroupId?.value || previousSync.groupId || 'default').trim() || 'default',
                leaderIp: (syncLeaderIp?.value || previousSync.leaderIp || '').trim(),
                leaderDeviceId: syncLeaderIp?.value?.trim() ? '' : (previousSync.leaderDeviceId || ''),
                multicastAddr: (syncMulticastAddr?.value || previousSync.multicastAddr || '239.255.42.10').trim(),
                interface: (syncInterface?.value || previousSync.interface || '').trim(),
                port: 4322,
                ttl: Number.isInteger(ttlValue) ? Math.max(0, Math.min(255, ttlValue)) : 1
            };
        }

        return config;
    }, '公共配置');
    if (ok) {
        addToCommandLog('公共配置', 'info', '渲染质量将在重启应用后生效；启动无线模式将在重启设备后生效');
    }
}

// 更新系统设置页面
export async function updateSettings() {
    await Promise.allSettled([
        loadDeviceInfo(),
        loadLicenseStatus(),
        loadConfigForSettings(),
        loadDisplayOutput(),
        loadDisplayResolution()
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
        const playbackOperationHintEnabledEl = document.getElementById('common-playback-operation-hint-enabled');
        const startupIpHintEnabledEl = document.getElementById('common-startup-ip-hint-enabled');
        const localSongFileScanEl = document.getElementById('common-local-song-file-scan-enabled');
        const appUpdateEnabledEl = document.getElementById('common-app-update-enabled');
        const networkIpModeEl = document.getElementById('common-network-ip-mode');
        const networkStaticIpEl = document.getElementById('common-network-static-ip');
        const networkGatewayEl = document.getElementById('common-network-gateway');
        const networkDnsEl = document.getElementById('common-network-dns');
        const startupWirelessModeEl = document.getElementById('common-startup-wireless-mode');
        const usbOtgModeEl = document.getElementById('common-usb-otg-mode');
        const vodHostEl = document.getElementById('common-online-vod-host');
        const vodRoomEl = document.getElementById('common-online-vod-room-id');
        const powerScheduleEnabledEl = document.getElementById('common-power-schedule-enabled');
        const powerOnScheduleEnabledEl = document.getElementById('common-power-on-schedule-enabled');
        const powerOnDateEl = document.getElementById('common-power-on-date');
        const powerOnTimeEl = document.getElementById('common-power-on-time');
        const powerOffScheduleEnabledEl = document.getElementById('common-power-off-schedule-enabled');
        const powerOffDateEl = document.getElementById('common-power-off-date');
        const powerOffTimeEl = document.getElementById('common-power-off-time');
        const syncEnabledEl = document.getElementById('common-sync-enabled');
        const syncRoleEl = document.getElementById('common-sync-role');
        const syncGroupIdEl = document.getElementById('common-sync-group-id');
        const syncLeaderIpEl = document.getElementById('common-sync-leader-ip');
        const syncMulticastAddrEl = document.getElementById('common-sync-multicast-addr');
        const syncInterfaceEl = document.getElementById('common-sync-interface');
        const syncPortEl = document.getElementById('common-sync-port');
        const syncTtlEl = document.getElementById('common-sync-ttl');

        const syncGroup = data && data.syncGroup && typeof data.syncGroup === 'object'
            ? data.syncGroup
            : {};
        if (syncEnabledEl) syncEnabledEl.checked = syncGroup.enabled === true;
        if (syncRoleEl) syncRoleEl.value = syncGroup.role === 'leader' ? 'leader' : 'follower';
        if (syncGroupIdEl) syncGroupIdEl.value = syncGroup.groupId || 'default';
        if (syncLeaderIpEl) {
            const legacyId = syncGroup.leaderDeviceId || '';
            const legacyLooksLikeIp = /^\d{1,3}(\.\d{1,3}){3}$/.test(legacyId);
            syncLeaderIpEl.value = syncGroup.leaderIp || (legacyLooksLikeIp ? legacyId : '');
        }
        if (syncMulticastAddrEl) syncMulticastAddrEl.value = syncGroup.multicastAddr || '239.255.42.10';
        if (syncInterfaceEl) syncInterfaceEl.value = syncGroup.interface || '';
        if (syncPortEl) syncPortEl.value = 4322;
        if (syncTtlEl) syncTtlEl.value = Number.isInteger(syncGroup.ttl) ? syncGroup.ttl : 1;
        updateSyncLeaderVisibility();

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
        if (playbackOperationHintEnabledEl) playbackOperationHintEnabledEl.checked = data?.playbackOperationHintEnabled !== false;
        if (startupIpHintEnabledEl) startupIpHintEnabledEl.checked = data?.startupIpHintEnabled !== false;
        if (localSongFileScanEl) localSongFileScanEl.checked = !!(data && data.localSongFileScanEnabled === true);
        if (appUpdateEnabledEl) appUpdateEnabledEl.checked = !(data && data.appUpdateEnabled === false);
        const networkIpMode = data && data.networkIpMode === 'static' ? 'static' : 'dynamic';
        if (networkIpModeEl) networkIpModeEl.value = networkIpMode;
        if (networkStaticIpEl) networkStaticIpEl.value = (data && data.networkStaticIp) || '';
        if (networkGatewayEl) networkGatewayEl.value = (data && data.networkGateway) || '';
        if (networkDnsEl) networkDnsEl.value = (data && data.networkDns) || '';
        if (startupWirelessModeEl) startupWirelessModeEl.value = data?.startupWirelessMode === 'wifi' ? 'wifi' : 'hotspot';
        if (usbOtgModeEl) {
            usbOtgModeEl.value = data && ['otg', 'peripheral', 'host'].includes(data.usbOtgMode)
                ? data.usbOtgMode
                : 'host';
        }
        if (vodHostEl) vodHostEl.value = (data && data.onlineVodHost) || '';
        if (vodRoomEl) vodRoomEl.value = (data && data.onlineVodRoomId) || 'current';
        const vodGainEl = document.getElementById('common-vod-volume-gain-db');
        if (vodGainEl) vodGainEl.value = String(data?.vodVolumeGainDb ?? 0);
        if (powerScheduleEnabledEl) powerScheduleEnabledEl.checked = !!(data && data.powerScheduleEnabled === true);
        if (powerOnScheduleEnabledEl) powerOnScheduleEnabledEl.checked = !!(data && data.powerOnScheduleEnabled === true);
        if (powerOnDateEl) powerOnDateEl.value = (data && data.powerOnDate) || '';
        if (powerOnTimeEl) powerOnTimeEl.value = (data && data.powerOnTime) || '';
        if (powerOffScheduleEnabledEl) powerOffScheduleEnabledEl.checked = !!(data && data.powerOffScheduleEnabled === true);
        if (powerOffDateEl) powerOffDateEl.value = (data && data.powerOffDate) || '';
        if (powerOffTimeEl) powerOffTimeEl.value = (data && data.powerOffTime) || '';
        await loadLyricFontOptions(data && typeof data.lyricFont === 'string' ? data.lyricFont : '');
        const lyricHeaderVisibleEl = document.getElementById('common-lyric-header-visible');
        if (lyricHeaderVisibleEl) lyricHeaderVisibleEl.checked = data?.lyricHeaderVisible === true;
        const lyricBackgroundVisibleEl = document.getElementById('common-lyric-background-visible');
        if (lyricBackgroundVisibleEl) lyricBackgroundVisibleEl.checked = data?.lyricBackgroundVisible !== false;
        const lyricDisplayModeEl = document.getElementById('common-lyric-display-mode');
        if (lyricDisplayModeEl) lyricDisplayModeEl.value = data?.lyricDisplayMode === 'pinyin' ? 'pinyin' : 'original';
        const lyricTitleFontSizeEl = document.getElementById('common-lyric-title-font-size');
        const lyricTitleOutlineWidthEl = document.getElementById('common-lyric-title-outline-width');
        const lyricTextFontSizeEl = document.getElementById('common-lyric-text-font-size');
        const lyricTextOutlineWidthEl = document.getElementById('common-lyric-text-outline-width');
        const lyricTextSpacingEl = document.getElementById('common-lyric-text-spacing');
        const lyricProducerTextEl = document.getElementById('common-lyric-producer-text');
        if (lyricTitleFontSizeEl) lyricTitleFontSizeEl.value = data?.lyricTitleFontSize ?? 150;
        if (lyricTitleOutlineWidthEl) lyricTitleOutlineWidthEl.value = data?.lyricTitleOutlineWidth ?? 8;
        if (lyricTextFontSizeEl) lyricTextFontSizeEl.value = data?.lyricTextFontSize ?? 140;
        if (lyricTextOutlineWidthEl) lyricTextOutlineWidthEl.value = data?.lyricTextOutlineWidth ?? 10;
        if (lyricTextSpacingEl) lyricTextSpacingEl.value = data?.lyricTextSpacing ?? -10;
        if (lyricProducerTextEl) lyricProducerTextEl.value = typeof data?.lyricProducerText === 'string'
            ? data.lyricProducerText : '';
        updateLyricPreview();

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

function normalizeLyricFontValue(value) {
    if (typeof value !== 'string' || !value) return '';
    let normalized = value.replace(/\\/g, '/');
    if (normalized.startsWith('ttf/')) normalized = normalized.substring(4);
    if (!normalized || normalized.startsWith('/') || normalized.includes(':')) return '';
    const segments = normalized.split('/');
    if (segments.some(segment => !segment || segment === '.' || segment === '..')) return '';
    return /\.(ttf|otf|ttc)$/i.test(normalized) ? normalized : '';
}

function relativeFontPath(material) {
    const rawPath = typeof material?.path === 'string' ? material.path.replace(/\\/g, '/') : '';
    const markerIndex = rawPath.toLowerCase().lastIndexOf('/ttf/');
    const fromPath = markerIndex >= 0 ? rawPath.substring(markerIndex + 5) : '';
    return normalizeLyricFontValue(fromPath || material?.name || '');
}

async function loadLyricFontOptions(configuredValue) {
    const select = document.getElementById('common-lyric-font');
    if (!select) return;

    const configuredFont = normalizeLyricFontValue(configuredValue);
    select.setAttribute('aria-busy', 'true');
    select.replaceChildren();

    const automaticOption = document.createElement('option');
    automaticOption.value = '';
    automaticOption.textContent = '\u81ea\u52a8\u9009\u62e9';
    select.appendChild(automaticOption);

    try {
        const materials = await apiGet('/materials?type=font');
        if (!Array.isArray(materials)) {
            throw new Error(getLastApiError()?.message || '\u5b57\u4f53\u7d20\u6750\u63a5\u53e3\u672a\u8fd4\u56de\u5217\u8868');
        }

        const fontsByPath = new Map();
        materials.forEach(material => {
            const value = relativeFontPath(material);
            if (!value) return;
            const key = value.toLocaleLowerCase();
            if (!fontsByPath.has(key)) {
                fontsByPath.set(key, { value, name: material.name || value });
            }
        });
        const fonts = Array.from(fontsByPath.values()).sort((left, right) =>
            left.name.localeCompare(right.name, 'zh-CN', { numeric: true, sensitivity: 'base' }));

        fonts.forEach(font => {
            const option = document.createElement('option');
            option.value = font.value;
            option.setAttribute('translate', 'no');
            option.textContent = font.name;
            option.title = font.value;
            select.appendChild(option);
        });

        if (configuredFont && !fonts.some(font =>
            font.value.toLocaleLowerCase() === configuredFont.toLocaleLowerCase())) {
            const missingOption = document.createElement('option');
            missingOption.value = configuredFont;
            missingOption.textContent = `${configuredFont}\uff08\u6587\u4ef6\u4e0d\u5b58\u5728\uff09`;
            missingOption.title = configuredFont;
            select.appendChild(missingOption);
        }
        select.value = configuredFont;
        select.title = '\u9009\u62e9 /huoshan/ttf \u76ee\u5f55\u4e2d\u7684\u5b57\u4f53\uff1b\u4fdd\u5b58\u540e\u91cd\u542f\u5e94\u7528\u751f\u6548';
    } catch (error) {
        if (configuredFont) {
            const currentOption = document.createElement('option');
            currentOption.value = configuredFont;
            currentOption.textContent = configuredFont;
            select.appendChild(currentOption);
            select.value = configuredFont;
        }
        select.title = '\u5b57\u4f53\u5217\u8868\u52a0\u8f7d\u5931\u8d25\uff0c\u5df2\u4fdd\u7559\u5f53\u524d\u914d\u7f6e';
        addToCommandLog('\u6b4c\u8bcd\u5b57\u4f53', 'warn', error.message || '\u5b57\u4f53\u5217\u8868\u52a0\u8f7d\u5931\u8d25');
    } finally {
        select.removeAttribute('aria-busy');
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
                    const normalizedName = newValue.trim();
                    const result = await apiPost('/system/device-alias', {
                        device_hsname: normalizedName
                    });
                    if (result) {
                        element.textContent = normalizedName;
                        addToCommandLog(`修改${label}`, 'success', `${label}已修改为: ${normalizedName}`);
                    } else {
                        const reason = getLastApiError()?.message || `${label}修改失败`;
                        addToCommandLog(`修改${label}`, 'error', reason);
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
            headers: (() => {
                const token = localStorage.getItem('hvideo_auth_token');
                return token ? { Authorization: `Bearer ${token}` } : {};
            })(),
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
        applyModuleLicense(data);
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
            return data;
        }
        return null;
    } catch (error) {
        applyModuleLicense(null);
        setTextContent('license-usage-mode', '--');
        setTextContent('license-input-channel-count', '--');
        setLicensedInputChannelCount(0);
        updateStatusBadge('unlicensed');
        return null;
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
    const allModules = Object.keys(MODULE_LABELS);

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

/** 同步方向模式：'to-device' 推送到选中设备（左侧列表可勾选）| 'to-local' 拉取到本机（右侧列表可勾选） */
let lanSyncMode = 'to-device';

/** 同步过程中箭头方向：'' 不显示 | 'to-device' 本机→设备 ← | 'to-local' 设备→本机 → */
let lanSyncArrowDirection = '';

/** Prevent duplicate scan or sync submissions. */
let lanSyncBusy = false;

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
        const ip = typeof dev.ip === 'string' ? dev.ip : '';
        const name = dev.name || dev.ip || dev.id || `设备${i + 1}`;
        const label = document.createElement('label');
        label.className = 'lan-sync-device-item';
        label.setAttribute('data-lan-ip', ip);
        label.setAttribute('data-lan-name', String(name));
        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.value = ip;
        cb.setAttribute('data-lan-ip', ip);
        cb.setAttribute('data-lan-serial', typeof dev.serial === 'string' ? dev.serial : '');
        const compatible = Number(dev.config_sync?.protocol) === 2;
        const pending = dev.config_sync?.state === 'pending_restart';
        cb.setAttribute('data-lan-ready', ip && dev.serial && compatible && !pending ? 'true' : 'false');
        cb.disabled = cb.getAttribute('data-lan-ready') !== 'true';
        label.appendChild(cb);
        const info = document.createElement('span');
        info.className = 'lan-sync-device-info';
        const nameEl = document.createElement('span');
        nameEl.className = 'lan-sync-device-name';
        nameEl.textContent = name;
        info.appendChild(nameEl);
        const ipEl = document.createElement('span');
        ipEl.className = 'lan-sync-device-detail';
        ipEl.textContent = `IP：${ip || '未提供'}`;
        info.appendChild(ipEl);
        const serialEl = document.createElement('span');
        serialEl.className = 'lan-sync-device-detail';
        serialEl.textContent = `序列号：${dev.serial || '未提供'}`;
        info.appendChild(serialEl);
        if (pending || !compatible || !dev.serial || !ip) {
            const hint = document.createElement('span');
            hint.className = 'lan-sync-device-detail';
            hint.textContent = pending ? '已校验，需重启系统应用' : (!compatible ? '请升级此设备播放器后同步' : '身份信息不完整，请重新扫描');
            info.appendChild(hint);
        }
        label.appendChild(info);
        container.appendChild(label);
    });

    container.querySelectorAll('input[type="checkbox"]').forEach(cb => {
        cb.addEventListener('change', () => {
            if (lanSyncMode === 'to-local' && cb.checked) {
                container.querySelectorAll('input[type="checkbox"]').forEach(other => {
                    if (other !== cb) other.checked = false;
                });
            }
            updateLanSyncDeviceFolderTitle();
        });
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

    if (pullBtn) {
        pullBtn.classList.toggle('active', lanSyncMode === 'to-local');
        pullBtn.classList.toggle('primary', lanSyncMode === 'to-local');
        pullBtn.setAttribute('aria-pressed', String(lanSyncMode === 'to-local'));
        pullBtn.disabled = lanSyncBusy;
    }
    if (pushBtn) {
        pushBtn.classList.toggle('active', lanSyncMode === 'to-device');
        pushBtn.classList.toggle('primary', lanSyncMode === 'to-device');
        pushBtn.setAttribute('aria-pressed', String(lanSyncMode === 'to-device'));
        pushBtn.disabled = lanSyncBusy;
    }

    if (devicePanel) {
        devicePanel.classList.toggle('lan-sync-folder-panel-selectable', lanSyncMode === 'to-device');
        devicePanel.classList.toggle('lan-sync-folder-panel-disabled', lanSyncMode === 'to-local');
        devicePanel.querySelectorAll('input[type="checkbox"]').forEach(cb => {
            cb.disabled = lanSyncBusy || lanSyncMode !== 'to-device';
        });
    }
    if (localPanel) {
        localPanel.classList.toggle('lan-sync-folder-panel-selectable', lanSyncMode === 'to-local');
        localPanel.classList.toggle('lan-sync-folder-panel-disabled', lanSyncMode === 'to-device');
        localPanel.querySelectorAll('input[type="checkbox"]').forEach(cb => {
            cb.disabled = lanSyncBusy || lanSyncMode !== 'to-local';
        });
    }
}

function setLanSyncBusy(busy, operation = '') {
    lanSyncBusy = busy;
    const scanBtn = document.getElementById('lan-sync-scan-btn');
    const execBtn = document.getElementById('lan-sync-exec-btn');
    if (scanBtn) {
        scanBtn.disabled = busy;
        scanBtn.textContent = busy && operation === 'scan' ? '扫描中…' : '扫描';
    }
    if (execBtn) {
        execBtn.disabled = busy;
        execBtn.textContent = busy && operation === 'sync' ? '同步中…' : '同步';
    }
    document.querySelectorAll('#lan-sync-device-list input[type="checkbox"]').forEach(cb => {
        cb.disabled = busy || cb.getAttribute('data-lan-ready') !== 'true';
    });
    applyLanSyncMode();
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
        const ip = checked[0].value;
        titleEl.textContent = ip && ip !== name ? `${name}（${ip}）` : name;
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
            if (lanSyncBusy) return;
            lanSyncMode = 'to-local';
            const selected = document.querySelectorAll('#lan-sync-device-list input[type="checkbox"]:checked');
            selected.forEach((checkbox, index) => { checkbox.checked = index === 0; });
            updateLanSyncDeviceFolderTitle();
            applyLanSyncMode();
        });
    }
    const pushModeBtn = document.getElementById('lan-sync-mode-push-btn');
    if (pushModeBtn) {
        pushModeBtn.addEventListener('click', () => {
            if (lanSyncBusy) return;
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
    if (lanSyncBusy) return;
    setLanSyncBusy(true, 'scan');
    addToCommandLog('配置同步', 'info', '正在扫描局域网设备…');
    try {
        const data = await apiGet('/system/lan-devices', { timeoutMs: 15000, retries: 0 });
        if (data == null) {
            const message = getLanSyncErrorMessage('扫描设备失败');
            renderLanSyncDeviceList([]);
            showError(message, 3000);
            addToCommandLog('配置同步', 'error', message);
            return;
        }

        const devices = Array.isArray(data.devices) ? data.devices : (Array.isArray(data) ? data : []);
        renderLanSyncDeviceList(devices);
        if (devices.length > 0) {
            addToCommandLog('配置同步', 'success', `发现 ${devices.length} 台设备`);
        } else {
            addToCommandLog('配置同步', 'info', '未发现可同步设备');
        }
    } catch (error) {
        const message = error?.message || '扫描失败，请检查网络后重试';
        showError(message, 5000);
        addToCommandLog('配置同步', 'error', message);
    } finally {
        setLanSyncBusy(false);
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

function getSelectedLanDeviceSerials() {
    const serials = {};
    document.querySelectorAll('.lan-sync-device-item input[type="checkbox"]:checked').forEach(cb => {
        serials[cb.value] = cb.getAttribute('data-lan-serial') || '';
    });
    return serials;
}

/** 根据当前同步方向更新箭头显示（默认不显示，同步中显示 → 或 ←） */
function updateLanSyncArrowCells() {
    const symbol = lanSyncArrowDirection === 'to-device' ? '←' : (lanSyncArrowDirection === 'to-local' ? '→' : '');
    document.querySelectorAll('.lan-sync-arrow-cell').forEach(el => { el.textContent = symbol; });
}

function buildLanSyncSuccessMessage(result, pullMode = false) {
    const files = Number(result?.files_count || 0);
    const devices = Number(result?.synced_count || (pullMode ? 1 : 0));
    const direction = pullMode ? '本机已完整接收并校验' : `${devices} 台设备已完整接收并校验`;
    return `${direction}，共 ${files} 个文件；请重启${pullMode ? '本机' : '接收设备'}系统后应用`;
}

function getLanSyncErrorMessage(fallback) {
    const message = getLastApiError()?.message;
    // Older devices may return question-mark placeholders instead of readable text.
    return typeof message === 'string' && message && !/\?{2,}|\uFFFD/.test(message) ? message : fallback;
}

async function startLanSync() {
    if (lanSyncBusy) return;
    const dirs = getSelectedLanDirs();
    const devices = getSelectedLanDevices();
    if (dirs.length === 0) {
        showError('请至少选择一个需要同步的文件夹', 2500);
        return;
    }
    if (devices.length === 0) {
        showError('请选择需要同步的目标设备', 2500);
        return;
    }

    setLanSyncBusy(true, 'sync');
    lanSyncArrowDirection = 'to-device';
    updateLanSyncArrowCells();
    addToCommandLog('配置同步', 'info', `正在推送配置到选中设备：${SYNC_BASE_LABEL} [${dirs.join(', ')}]`);
    try {
        const result = await apiAction('sync', 'sync_device', { sync_data: {
                    base_path: SYNC_BASE_PATH,
                    dirs: dirs,
                    target_ips: devices,
                    target_serials: getSelectedLanDeviceSerials()
                } }, { timeoutMs: SYNC_REQUEST_TIMEOUT_MS, retries: 0 });
        if (result != null) {
            const message = buildLanSyncSuccessMessage(result, false);
            showSuccess(message, 5000);
            addToCommandLog('配置同步', 'success', message);
        } else {
            const message = getLanSyncErrorMessage('同步到选中设备失败，请重试');
            showError(message, 5000);
            addToCommandLog('配置同步', 'error', message);
        }
    } catch (error) {
        const message = error?.message || '同步请求中断，请检查接收设备是否已等待重启';
        showError(message, 5000);
        addToCommandLog('配置同步', 'error', message);
    } finally {
        lanSyncArrowDirection = '';
        updateLanSyncArrowCells();
        setLanSyncBusy(false);
    }
}

/** 从一台选中的设备拉取配置到本机。 */
async function startLanSyncPull() {
    if (lanSyncBusy) return;
    const dirs = getSelectedLanDirs();
    const devices = getSelectedLanDevices();
    if (dirs.length === 0) {
        showError('请至少选择一个需要同步的文件夹', 2500);
        return;
    }
    if (devices.length !== 1) {
        showError('请选择一台设备作为同步来源', 2500);
        return;
    }
    const sourceIp = devices[0];

    setLanSyncBusy(true, 'sync');
    lanSyncArrowDirection = 'to-local';
    updateLanSyncArrowCells();
    addToCommandLog('配置同步', 'info', `正在从 ${sourceIp} 拉取配置到本机：${SYNC_BASE_LABEL} [${dirs.join(', ')}]`);
    try {
        const result = await apiAction('sync', 'sync_device', { sync_data: {
                    base_path: SYNC_BASE_PATH,
                    dirs: dirs,
                    source_ip: sourceIp,
                    source_serial: getSelectedLanDeviceSerials()[sourceIp]
                } }, { timeoutMs: SYNC_REQUEST_TIMEOUT_MS, retries: 0 });
        if (result != null) {
            const message = buildLanSyncSuccessMessage(result, true);
            showSuccess(message, 5000);
            addToCommandLog('配置同步', 'success', message);
        } else {
            const message = getLanSyncErrorMessage('同步到本机失败，请重试');
            showError(message, 5000);
            addToCommandLog('配置同步', 'error', message);
        }
    } catch (error) {
        const message = error?.message || '同步请求中断，请检查接收设备是否已等待重启';
        showError(message, 5000);
        addToCommandLog('配置同步', 'error', message);
    } finally {
        lanSyncArrowDirection = '';
        updateLanSyncArrowCells();
        setLanSyncBusy(false);
    }
}

// ==================== 工具函数 ====================

function setTextContent(id, text) {
    const el = document.getElementById(id);
    if (el) el.textContent = text || '--';
}
