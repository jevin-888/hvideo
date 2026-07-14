/**
 * @file mobileControl.js（文件名）
 * @brief 移动端中控模块
 */

import { addToCommandLog } from '../core/commandLog.js';
import { apiPost, apiAction } from '../core/api.js';

const DEFAULT_CONTROL_CONFIG = {
    hvac: {
        enabled: true,
        title: '空调',
        default_temperature: 26,
        temperature_min: 16,
        temperature_max: 32,
        default_mode: 'cool',
        default_fan_speed: 3,
        items: [
            { id: 'temp_down', label: '降温', kind: 'temperature', enabled: true, functions: [] },
            { id: 'temp_up', label: '升温', kind: 'temperature', enabled: true, functions: [] },
            { id: 'power', label: '打开', kind: 'power', enabled: true, functions: [] },
            { id: 'cool', label: '制冷', kind: 'mode', mode: 'cool', enabled: true, functions: [] },
            { id: 'heat', label: '制热', kind: 'mode', mode: 'heat', enabled: true, functions: [] },
            { id: 'fan_low', label: '低风', kind: 'fan', speed: 1, enabled: true, functions: [] },
            { id: 'fan_medium', label: '中风', kind: 'fan', speed: 2, enabled: true, functions: [] },
            { id: 'fan_high', label: '高风', kind: 'fan', speed: 3, enabled: true, functions: [] }
        ]
    },
    lighting: {
        enabled: true,
        title: '灯光',
        dimmer: {
            enabled: true,
            default_value: 50
        },
        items: [
            { id: 'spot', label: '射灯', enabled: true, default_on: true, functions: [] },
            { id: 'strip', label: '灯带', enabled: true, default_on: false, functions: [] },
            { id: 'floor', label: '地台灯', enabled: true, default_on: false, functions: [] },
            { id: 'flood', label: '聚光灯', enabled: true, default_on: false, functions: [] }
        ]
    }
};

let controlConfig = cloneConfig(DEFAULT_CONTROL_CONFIG);
let controlEventsBound = false;
let controlConfigLoadPromise = null;

// 温度状态
let currentTemp = 26;
let hvacMode = 'cool';
let fanSpeed = 3;
let hvacOn = true;

// 灯光状态
let lightStates = {};
let dimmerValue = 50;
let currentLightType = 'spot';

/**
 * 初始化中控页面
 */
export function initializeControlPage() {
    switchTab('ac');
    bindControlEventsOnce();
    applyControlConfig(controlConfig);

    if (!controlConfigLoadPromise) {
        controlConfigLoadPromise = loadControlFunctionConfig()
            .finally(() => {
                controlConfigLoadPromise = null;
            });
    } else {
        controlConfigLoadPromise.then(() => applyControlConfig(controlConfig)).catch(() => {});
    }
}

function cloneConfig(config) {
    return JSON.parse(JSON.stringify(config || DEFAULT_CONTROL_CONFIG));
}

function clampNumber(value, min, max, fallback) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return fallback;
    return Math.max(min, Math.min(max, numeric));
}

function normalizeControlConfig(config = {}) {
    const normalized = cloneConfig(DEFAULT_CONTROL_CONFIG);
    const incoming = config && typeof config === 'object' ? config : {};
    const hvac = incoming.hvac && typeof incoming.hvac === 'object' ? incoming.hvac : {};
    const lighting = incoming.lighting && typeof incoming.lighting === 'object' ? incoming.lighting : {};

    normalized.hvac.enabled = hvac.enabled !== false;
    normalized.hvac.title = String(hvac.title || normalized.hvac.title);
    normalized.hvac.temperature_min = clampNumber(hvac.temperature_min, 0, 50, normalized.hvac.temperature_min);
    normalized.hvac.temperature_max = clampNumber(hvac.temperature_max, 1, 60, normalized.hvac.temperature_max);
    if (normalized.hvac.temperature_max < normalized.hvac.temperature_min) {
        normalized.hvac.temperature_max = normalized.hvac.temperature_min;
    }
    normalized.hvac.default_temperature = clampNumber(
        hvac.default_temperature,
        normalized.hvac.temperature_min,
        normalized.hvac.temperature_max,
        normalized.hvac.default_temperature
    );
    normalized.hvac.default_mode = String(hvac.default_mode || normalized.hvac.default_mode);
    normalized.hvac.default_fan_speed = clampNumber(hvac.default_fan_speed, 1, 3, normalized.hvac.default_fan_speed);

    const hvacItems = new Map((Array.isArray(hvac.items) ? hvac.items : [])
        .filter(item => item && typeof item === 'object')
        .map(item => [String(item.id || ''), item]));
    normalized.hvac.items = normalized.hvac.items.map(item => {
        const incomingItem = hvacItems.get(item.id) || {};
        return {
            ...item,
            label: String(incomingItem.label || item.label),
            enabled: incomingItem.enabled !== false,
            functions: Array.isArray(incomingItem.functions)
                ? incomingItem.functions
                : (Array.isArray(item.functions) ? item.functions : [])
        };
    });

    normalized.lighting.enabled = lighting.enabled !== false;
    normalized.lighting.title = String(lighting.title || normalized.lighting.title);
    const dimmer = lighting.dimmer && typeof lighting.dimmer === 'object' ? lighting.dimmer : {};
    normalized.lighting.dimmer.enabled = dimmer.enabled !== false;
    normalized.lighting.dimmer.default_value = clampNumber(dimmer.default_value, 0, 100, normalized.lighting.dimmer.default_value);

    const lightItems = new Map((Array.isArray(lighting.items) ? lighting.items : [])
        .filter(item => item && typeof item === 'object')
        .map(item => [String(item.id || ''), item]));
    normalized.lighting.items = normalized.lighting.items.map(item => {
        const incomingItem = lightItems.get(item.id) || {};
        return {
            ...item,
            label: String(incomingItem.label || item.label),
            enabled: incomingItem.enabled !== false,
            default_on: incomingItem.default_on === true,
            functions: Array.isArray(incomingItem.functions)
                ? incomingItem.functions
                : (Array.isArray(item.functions) ? item.functions : [])
        };
    });

    return normalized;
}

async function loadControlFunctionConfig() {
    try {
        const response = await apiAction('peripherals', 'get_config', {
                peripheral_type: 'function_config'
            });
        controlConfig = normalizeControlConfig(response || DEFAULT_CONTROL_CONFIG);
    } catch (error) {
        console.warn('Failed to load control function config:', error);
        controlConfig = normalizeControlConfig(DEFAULT_CONTROL_CONFIG);
    }
    applyControlConfig(controlConfig);
}

function bindControlEventsOnce() {
    if (controlEventsBound) return;
    controlEventsBound = true;

    document.querySelectorAll('.control-tab').forEach(tab => {
        tab.addEventListener('click', function() {
            const tabName = this.dataset.tab;
            switchTab(tabName);
        });
    });

    document.getElementById('temp-up')?.addEventListener('click', () => {
        if (getConfiguredButton('hvac', 'temp_up')?.enabled === false) return;
        const maxTemp = controlConfig.hvac.temperature_max;
        if (currentTemp < maxTemp) {
            currentTemp++;
            updateTemperature();
            executeConfiguredButton('hvac', 'temp_up');
        }
    });

    document.getElementById('temp-down')?.addEventListener('click', () => {
        if (getConfiguredButton('hvac', 'temp_down')?.enabled === false) return;
        const minTemp = controlConfig.hvac.temperature_min;
        if (currentTemp > minTemp) {
            currentTemp--;
            updateTemperature();
            executeConfiguredButton('hvac', 'temp_down');
        }
    });

    document.getElementById('hvac-control-buttons')?.addEventListener('click', event => {
        const button = event.target.closest('.ac-btn');
        if (!button) return;
        const item = getHvacItem(button.dataset.functionId);
        if (item) handleHvacAction(item);
    });

    document.getElementById('lighting-switches')?.addEventListener('change', event => {
        const checkbox = event.target.closest('input[type="checkbox"][data-light-id]');
        if (!checkbox) return;
        const lightId = checkbox.dataset.lightId;
        lightStates[lightId] = checkbox.checked;
        updateLightLabel(lightId);
        if (lightId === currentLightType) {
            updateDimmerLabel();
        }
        addToCommandLog('灯光控制', 'info', `${getLightName(lightId)} ${checkbox.checked ? '已打开' : '已关闭'}`);
        executeConfiguredButton('lighting', lightId);
    });

    document.getElementById('lighting-switches')?.addEventListener('click', event => {
        const label = event.target.closest('.light-label');
        if (!label) return;
        const lightId = label.dataset.lightId;
        if (lightId && lightStates[lightId]) {
            currentLightType = lightId;
            updateLightSelection();
            updateDimmerLabel();
        }
    });

    const dimmerSlider = document.getElementById('dimmer-slider');
    if (dimmerSlider) {
        dimmerSlider.addEventListener('input', function() {
            dimmerValue = parseInt(this.value, 10);
            updateDimmer();
        });
    }
}

function applyControlConfig(config) {
    controlConfig = normalizeControlConfig(config);
    currentTemp = controlConfig.hvac.default_temperature;
    hvacMode = controlConfig.hvac.default_mode;
    fanSpeed = controlConfig.hvac.default_fan_speed;
    dimmerValue = controlConfig.lighting.dimmer.default_value;

    syncControlTabTitles();
    updateTemperatureControlsAvailability();
    renderHvacButtons();
    renderLightSwitches();
    updateTemperature();
    updateHvacMode();
    updateLightSelection();
    updateDimmerVisibility();
    updateDimmer();
    updateDimmerLabel();
}

function updateTemperatureControlsAvailability() {
    const tempUp = document.getElementById('temp-up');
    const tempDown = document.getElementById('temp-down');
    if (tempUp) {
        tempUp.disabled = getConfiguredButton('hvac', 'temp_up')?.enabled === false;
    }
    if (tempDown) {
        tempDown.disabled = getConfiguredButton('hvac', 'temp_down')?.enabled === false;
    }
}

function syncControlTabTitles() {
    const acTab = document.querySelector('.control-tab[data-tab="ac"]');
    const lightingTab = document.querySelector('.control-tab[data-tab="lighting"]');
    if (acTab) acTab.textContent = controlConfig.hvac.title || '空调';
    if (lightingTab) lightingTab.textContent = controlConfig.lighting.title || '灯光';
}

function renderHvacButtons() {
    const container = document.getElementById('hvac-control-buttons');
    if (!container) return;

    if (!controlConfig.hvac.enabled) {
        container.innerHTML = '<div class="control-empty">空调功能未启用</div>';
        return;
    }

    const items = controlConfig.hvac.items.filter(item =>
        item.enabled !== false && item.kind !== 'temperature'
    );
    if (!items.length) {
        container.innerHTML = '<div class="control-empty">暂无空调功能</div>';
        return;
    }

    container.innerHTML = items.map(item => `
        <button class="ac-btn" type="button" data-function-id="${escapeHtml(item.id)}" data-kind="${escapeHtml(item.kind || '')}">
            ${getHvacIcon(item)}
            <span>${escapeHtml(item.label || item.id)}</span>
        </button>
    `).join('');
}

function renderLightSwitches() {
    const container = document.getElementById('lighting-switches');
    if (!container) return;

    if (!controlConfig.lighting.enabled) {
        lightStates = {};
        currentLightType = '';
        container.innerHTML = '<div class="control-empty">灯光功能未启用</div>';
        return;
    }

    const items = controlConfig.lighting.items.filter(item => item.enabled !== false);
    lightStates = {};
    items.forEach(item => {
        lightStates[item.id] = item.default_on === true;
    });
    currentLightType = items.find(item => lightStates[item.id])?.id || items[0]?.id || '';

    if (!items.length) {
        container.innerHTML = '<div class="control-empty">暂无灯光功能</div>';
        return;
    }

    container.innerHTML = items.map(item => `
        <div class="light-switch-item">
            <label class="light-toggle">
                <input type="checkbox" id="light-${escapeHtml(item.id)}" data-light-id="${escapeHtml(item.id)}" ${lightStates[item.id] ? 'checked' : ''}>
                <span class="toggle-slider"></span>
            </label>
            <span class="light-label" data-light-id="${escapeHtml(item.id)}">${escapeHtml(item.label || item.id)}</span>
        </div>
    `).join('');
}

function getHvacItem(id) {
    return controlConfig.hvac.items.find(item => item.id === id);
}

function getConfiguredButton(region, buttonId) {
    const area = controlConfig[region];
    if (!area || !Array.isArray(area.items)) return null;
    return area.items.find(item => item.id === buttonId) || null;
}

async function executeConfiguredButton(region, buttonId) {
    const button = getConfiguredButton(region, buttonId);
    if (!button || !Array.isArray(button.functions) || button.functions.length === 0) {
        return;
    }
    try {
        const response = await apiAction('peripherals', 'execute_function_button', {
                region,
                button_id: buttonId
            });
        if (response == null) {
            addToCommandLog('功能执行', 'error', `${button.label || buttonId} 执行失败`);
        }
    } catch (error) {
        addToCommandLog('功能执行', 'error', error.message || `${button.label || buttonId} 执行失败`);
    }
}

function handleHvacAction(item) {
    if (item.kind === 'mode') {
        setHvacMode(item.mode || item.id);
    } else if (item.kind === 'fan') {
        setFanSpeed(item.speed || 1);
    } else {
        setHvacState(true);
    }
    executeConfiguredButton('hvac', item.id);
}

function getHvacIcon(item) {
    if (item.kind === 'mode' && item.mode === 'heat') {
        return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="5" /><path d="M12 2v6M12 16v6M2 12h6M16 12h6" /></svg>';
    }
    if (item.kind === 'mode') {
        return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83" /></svg>';
    }
    if (item.kind === 'fan') {
        return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v20M6 6h12M6 18h12" /><circle cx="12" cy="12" r="4" /></svg>';
    }
    return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10" /><path d="M12 8v8M8 12h8" /></svg>';
}

function escapeHtml(value) {
    return String(value ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

/**
 * 切换标签页
 */
function switchTab(tabName) {
    document.querySelectorAll('.control-tab').forEach(tab => {
        tab.classList.toggle('active', tab.dataset.tab === tabName);
    });

    document.querySelectorAll('.control-panel').forEach(panel => {
        panel.classList.toggle('active', panel.id === `${tabName}-panel`);
    });
}

/**
 * 更新灯光选择状态
 */
function updateLightSelection() {
    document.querySelectorAll('.light-label').forEach(label => {
        label.classList.toggle('active', label.dataset.lightId === currentLightType);
    });
}

/**
 * 更新灯光标签状态
 */
function updateLightLabel(lightId) {
    const checkbox = document.getElementById(`light-${lightId}`);
    const label = checkbox?.closest('.light-switch-item')?.querySelector('.light-label');
    if (!label) return;

    if (lightStates[lightId]) {
        label.classList.add('active');
        return;
    }

    label.classList.remove('active');
    if (lightId === currentLightType) {
        const firstOn = Object.keys(lightStates).find(id => lightStates[id]);
        currentLightType = firstOn || Object.keys(lightStates)[0] || '';
        updateLightSelection();
        updateDimmerLabel();
    }
}

/**
 * 更新温度显示
 */
function updateTemperature() {
    const tempDisplay = document.getElementById('temp-display');
    if (tempDisplay) {
        tempDisplay.textContent = currentTemp;
    }
}

/**
 * 设置风速
 */
function setFanSpeed(speed) {
    fanSpeed = speed;
    updateFanSpeedButtons();
    const speedName = getFanSpeedName(speed);
    addToCommandLog('风速控制', 'info', `风速设置为 ${speedName}`);
}

/**
 * 更新风速按钮状态
 */
function updateFanSpeedButtons() {
    document.querySelectorAll('.ac-btn[data-kind="fan"]').forEach(btn => {
        const item = getHvacItem(btn.dataset.functionId);
        btn.classList.toggle('active', Number(item?.speed) === Number(fanSpeed));
    });
}

/**
 * 设置HVAC状态
 */
function setHvacState(on) {
    hvacOn = on;
    updateHvacPowerButtons();
    addToCommandLog('HVAC控制', 'info', `HVAC ${on ? '已打开' : '已关闭'}`);
}

/**
 * 设置HVAC模式
 */
function setHvacMode(mode) {
    hvacMode = mode;
    updateHvacMode();
    addToCommandLog('HVAC控制', 'info', `模式切换为: ${getHvacModeName(mode)}`);
}

/**
 * 更新HVAC模式显示
 */
function updateHvacMode() {
    document.querySelectorAll('.ac-btn[data-kind="mode"]').forEach(btn => {
        const item = getHvacItem(btn.dataset.functionId);
        btn.classList.toggle('active', (item?.mode || item?.id) === hvacMode);
    });
    updateFanSpeedButtons();
    updateHvacPowerButtons();
}

function updateHvacPowerButtons() {
    document.querySelectorAll('.ac-btn[data-kind="power"]').forEach(btn => {
        btn.classList.toggle('active', hvacOn);
    });
}

function getHvacModeName(mode) {
    const configured = controlConfig.hvac.items.find(item =>
        item.kind === 'mode' && (item.mode === mode || item.id === mode)
    );
    if (configured?.label) return configured.label;
    const names = {
        cool: '制冷',
        heat: '制热',
        fan: '送风',
        dry: '除湿',
        auto: '自动'
    };
    return names[mode] || mode;
}

function getFanSpeedName(speed) {
    const configured = controlConfig.hvac.items.find(item =>
        item.kind === 'fan' && Number(item.speed) === Number(speed)
    );
    if (configured?.label) return configured.label;
    return speed === 1 ? '低风' : speed === 2 ? '中风' : '高风';
}

function updateDimmerVisibility() {
    const dimmerControl = document.querySelector('.dimmer-control');
    if (dimmerControl) {
        dimmerControl.classList.toggle('hidden', controlConfig.lighting.dimmer.enabled === false);
    }
}

/**
 * 更新调光显示
 */
function updateDimmer() {
    const dimmerValueEl = document.getElementById('dimmer-value');
    const dimmerSlider = document.getElementById('dimmer-slider');
    const dimmerCircle = document.getElementById('dimmer-circle');

    if (dimmerValueEl) {
        dimmerValueEl.textContent = `${dimmerValue}%`;
    }
    if (dimmerSlider) {
        dimmerSlider.value = dimmerValue;
    }
    if (dimmerCircle) {
        const percentage = Math.max(0, Math.min(100, dimmerValue));
        dimmerCircle.style.setProperty('--dimmer-angle', `${percentage * 3.6}deg`);
    }

    updateDimmerLabel();
}

/**
 * 更新调光标签
 */
function updateDimmerLabel() {
    const dimmerLabel = document.getElementById('dimmer-label');
    if (dimmerLabel) {
        dimmerLabel.textContent = currentLightType ? getLightName(currentLightType) : '灯光';
    }
}

/**
 * 获取灯光名称
 */
function getLightName(lightId) {
    const configured = controlConfig.lighting.items.find(item => item.id === lightId);
    return configured?.label || lightId;
}
