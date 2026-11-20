import { apiAction, apiGet } from '../core/api.js';
import { showToast } from '../components/toast.js';
import { ApiModule } from '../../shared/moduleActions.js';

const DEFAULT_ACTIONS = [
    { kind: 'effect', layer_id: 1, effect_no: 0, effect_params: {} }
];

let initialized = false;
let config = createDefaultConfig();
let status = {};
let playlists = [];
let playlistItems = [];
let selectedPlaylistId = '';
let selectedItemIndex = -1;
let editingIndex = -1;
let itemLoadSerial = 0;

function createDefaultConfig() {
    return {
        version: 2,
        enabled: false,
        tolerance_ms: 80,
        bindings: []
    };
}

function clone(value) {
    return JSON.parse(JSON.stringify(value));
}

function formatTime(ms) {
    const value = Math.max(0, Number(ms) || 0);
    const hours = Math.floor(value / 3600000);
    const minutes = Math.floor((value % 3600000) / 60000);
    const seconds = Math.floor((value % 60000) / 1000);
    const millis = Math.floor(value % 1000);
    return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}.${String(millis).padStart(3, '0')}`;
}

function parseTime(value) {
    const text = String(value || '').trim();
    if (/^\d+$/.test(text)) return Number(text);
    const match = /^(\d+):([0-5]\d):([0-5]\d)(?:\.(\d{1,3}))?$/.exec(text);
    if (!match) throw new Error('时间格式应为 HH:MM:SS.mmm 或毫秒整数');
    const millis = Number((match[4] || '').padEnd(3, '0'));
    return Number(match[1]) * 3600000 + Number(match[2]) * 60000 + Number(match[3]) * 1000 + millis;
}

function notify(message, type = 'info') {
    showToast(message, type);
    const messageElement = document.getElementById('stage-control-message');
    if (messageElement) {
        messageElement.textContent = message;
        messageElement.dataset.type = type;
    }
}

function getSelectedItem() {
    return playlistItems.find((item) => Number(item.itemIndex) === Number(selectedItemIndex)) || null;
}

function getSelectedBinding(create = false) {
    if (!selectedPlaylistId || selectedItemIndex < 0) return null;
    const item = getSelectedItem();
    if (!item || !item.path) return null;
    const bindingIndex = config.bindings.findIndex((candidate) =>
        String(candidate.playlist_id) === String(selectedPlaylistId) &&
        Number(candidate.playlist_item_index) === Number(selectedItemIndex)
    );
    let binding = bindingIndex >= 0 ? config.bindings[bindingIndex] : null;
    if (binding && String(binding.media_uri) !== String(item.path)) {
        if (!create) return null;
        config.bindings.splice(bindingIndex, 1);
        binding = null;
    }
    if (!binding && create) {
        binding = {
            playlist_id: String(selectedPlaylistId),
            playlist_item_index: Number(selectedItemIndex),
            media_uri: item.path,
            media_title: item.title || item.path,
            cues: []
        };
        config.bindings.push(binding);
    }
    return binding || null;
}

function readTopLevelConfig() {
    return {
        ...config,
        version: 2,
        enabled: document.getElementById('stage-control-enabled')?.checked === true,
        tolerance_ms: Math.min(2000, Math.max(0, Number(document.getElementById('stage-control-tolerance')?.value) || 0)),
        bindings: (config.bindings || []).filter((binding) => Array.isArray(binding.cues) && binding.cues.length > 0)
    };
}

function setButtonState() {
    const armed = status?.armed === true;
    const enabled = config?.enabled === true;
    document.getElementById('stage-control-arm')?.toggleAttribute('disabled', armed || !enabled);
    document.getElementById('stage-control-disarm')?.toggleAttribute('disabled', !armed);
    document.getElementById('stage-control-add-cue')?.toggleAttribute('disabled', !getSelectedItem());
}

function renderStatus() {
    const armed = status?.armed === true;
    const media = status?.media_title || status?.media_path || '未检测到已绑定播放视频';
    const position = Number(status?.playback_position_ms);
    const statusElement = document.getElementById('stage-control-runtime-status');
    if (statusElement) {
        statusElement.innerHTML = '';
        const items = [
            ['运行状态', armed ? '已布防' : '未布防', armed ? 'active' : 'idle'],
            ['播放时间', Number.isFinite(position) && position >= 0 ? formatTime(position) : '--:--:--.---', ''],
            ['已触发', String(status?.fired_count || 0), ''],
            ['待执行', String(status?.pending_count || 0), ''],
            ['最后 Cue', status?.last_cue_id || '无', '']
        ];
        for (const [label, value, kind] of items) {
            const item = document.createElement('div');
            item.className = `stage-status-item ${kind}`.trim();
            const labelNode = document.createElement('span');
            labelNode.textContent = label;
            const valueNode = document.createElement('strong');
            valueNode.textContent = value;
            item.append(labelNode, valueNode);
            statusElement.appendChild(item);
        }
    }
    const mediaElement = document.getElementById('stage-control-media');
    if (mediaElement) mediaElement.textContent = media;
    const bindingElement = document.getElementById('stage-control-binding');
    if (bindingElement) {
        const playlist = playlists.find((candidate) => String(candidate.id) === String(status?.playlist_id));
        const playlistName = playlist?.name || status?.playlist_id || '未绑定播放列表';
        const itemIndex = Number(status?.playlist_item_index);
        bindingElement.textContent = itemIndex >= 0 ? `${playlistName} / 第 ${itemIndex + 1} 项` : playlistName;
    }
    const errorElement = document.getElementById('stage-control-error');
    if (errorElement) {
        errorElement.textContent = status?.last_error || '';
        errorElement.hidden = !status?.last_error;
    }
    setButtonState();
}

function renderSelectors() {
    const playlistSelect = document.getElementById('stage-control-playlist');
    const itemSelect = document.getElementById('stage-control-video');
    if (!playlistSelect || !itemSelect) return;

    playlistSelect.innerHTML = '';
    if (playlists.length === 0) {
        playlistSelect.add(new Option('暂无播放列表', ''));
    } else {
        for (const playlist of playlists) {
            playlistSelect.add(new Option(`${playlist.name || playlist.id} (${playlist.count || 0} 项)`, playlist.id));
        }
        playlistSelect.value = String(selectedPlaylistId);
    }

    itemSelect.innerHTML = '';
    if (playlistItems.length === 0) {
        itemSelect.add(new Option(selectedPlaylistId ? '该播放列表暂无视频' : '请先选择播放列表', ''));
    } else {
        for (const item of playlistItems) {
            const label = item.title || item.path || `视频 ${Number(item.itemIndex) + 1}`;
            itemSelect.add(new Option(`${Number(item.itemIndex) + 1}. ${label}`, item.itemIndex));
        }
        itemSelect.value = String(selectedItemIndex);
    }
    const selectedItem = getSelectedItem();
    const binding = getSelectedBinding(false);
    const info = document.getElementById('stage-control-selection-info');
    if (info) {
        info.textContent = selectedItem
            ? `${selectedItem.title || selectedItem.path} · ${binding ? `${binding.cues?.length || 0} 个 Cue 已预设` : '尚未建立时间轴绑定'}`
            : '选择播放列表和视频后，可为该视频独立预设 Cue 时间轴';
    }
}

function summarizeActions(actions) {
    return (actions || []).map((action) => {
        if (action.kind === 'effect') return `特效 L${action.layer_id} / #${action.effect_no}`;
        const protocol = String(action.protocol || '').toUpperCase();
        const target = action.serial_port || action.address || action.multicast_group || action.mode || '';
        return `${protocol} ${target}`.trim();
    }).join('；');
}

function renderCueList() {
    const body = document.getElementById('stage-control-cue-list');
    const empty = document.getElementById('stage-control-empty');
    if (!body) return;
    body.innerHTML = '';
    const binding = getSelectedBinding(false);
    const cues = binding?.cues || [];
    if (empty) {
        empty.hidden = cues.length !== 0;
        empty.textContent = getSelectedItem()
            ? '该视频暂无 Cue，点击“新增 Cue”创建。'
            : '请先选择播放列表和视频。';
    }

    cues.forEach((cue, index) => {
        const row = document.createElement('tr');
        if (cue.enabled === false) row.classList.add('is-disabled');
        const enabledCell = document.createElement('td');
        const toggle = document.createElement('input');
        toggle.type = 'checkbox';
        toggle.checked = cue.enabled !== false;
        toggle.title = '启用或停用此 Cue';
        toggle.addEventListener('change', () => {
            const current = getSelectedBinding(false);
            if (current) current.cues[index].enabled = toggle.checked;
            renderCueList();
        });
        enabledCell.appendChild(toggle);

        const timeCell = document.createElement('td');
        timeCell.className = 'stage-cue-time';
        timeCell.textContent = formatTime(cue.time_ms);
        const nameCell = document.createElement('td');
        const name = document.createElement('strong');
        name.setAttribute('translate', 'no');
        name.textContent = cue.name || cue.id;
        const id = document.createElement('small');
        id.textContent = cue.id;
        nameCell.append(name, id);
        const actionCell = document.createElement('td');
        actionCell.textContent = summarizeActions(cue.actions);
        actionCell.title = JSON.stringify(cue.actions || [], null, 2);
        const operationCell = document.createElement('td');
        operationCell.className = 'stage-cue-actions';
        operationCell.append(
            makeActionButton('编辑', () => openEditor(index)),
            makeActionButton('预览', () => previewCue(index), 'primary'),
            makeActionButton('删除', () => deleteCue(index), 'danger')
        );
        row.append(enabledCell, timeCell, nameCell, actionCell, operationCell);
        body.appendChild(row);
    });
}

function makeActionButton(label, handler, kind = '') {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `stage-mini-button ${kind}`.trim();
    button.textContent = label;
    button.addEventListener('click', handler);
    return button;
}

function renderConfig() {
    const enabled = document.getElementById('stage-control-enabled');
    const tolerance = document.getElementById('stage-control-tolerance');
    if (enabled) enabled.checked = config.enabled === true;
    if (tolerance) tolerance.value = String(config.tolerance_ms ?? 80);
    renderSelectors();
    renderCueList();
    renderStatus();
}

function openEditor(index = -1) {
    const binding = getSelectedBinding(false);
    if (!getSelectedItem()) {
        notify('请先选择播放列表中的具体视频', 'warning');
        return;
    }
    editingIndex = index;
    const cue = index >= 0 && binding ? clone(binding.cues[index]) : {
        id: `cue-${Date.now()}`,
        name: '新 Cue',
        time_ms: 0,
        enabled: true,
        actions: clone(DEFAULT_ACTIONS)
    };
    document.getElementById('stage-cue-editor-title').textContent = index >= 0 ? '编辑 Cue' : '新增 Cue';
    document.getElementById('stage-cue-id').value = cue.id;
    document.getElementById('stage-cue-name').value = cue.name || cue.id;
    document.getElementById('stage-cue-time').value = formatTime(cue.time_ms);
    document.getElementById('stage-cue-enabled').checked = cue.enabled !== false;
    document.getElementById('stage-cue-actions-json').value = JSON.stringify(cue.actions || DEFAULT_ACTIONS, null, 2);
    document.getElementById('stage-cue-editor').hidden = false;
    document.getElementById('stage-cue-editor').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

function closeEditor() {
    editingIndex = -1;
    const editor = document.getElementById('stage-cue-editor');
    if (editor) editor.hidden = true;
}

function applyEditor() {
    try {
        const binding = getSelectedBinding(true);
        if (!binding) throw new Error('请先选择有效的播放列表视频');
        const id = document.getElementById('stage-cue-id').value.trim();
        const name = document.getElementById('stage-cue-name').value.trim() || id;
        if (!/^[A-Za-z0-9_.-]{1,128}$/.test(id)) throw new Error('Cue ID 只能包含字母、数字、点、下划线和短横线');
        const actions = JSON.parse(document.getElementById('stage-cue-actions-json').value);
        if (!Array.isArray(actions) || actions.length === 0) throw new Error('actions 必须是非空 JSON 数组');
        const duplicate = binding.cues.some((cue, index) => cue.id === id && index !== editingIndex);
        if (duplicate) throw new Error(`当前视频的 Cue ID 已存在: ${id}`);
        const cue = {
            id,
            name,
            time_ms: parseTime(document.getElementById('stage-cue-time').value),
            enabled: document.getElementById('stage-cue-enabled').checked,
            actions
        };
        if (editingIndex >= 0) binding.cues[editingIndex] = cue;
        else binding.cues.push(cue);
        binding.cues.sort((a, b) => Number(a.time_ms) - Number(b.time_ms));
        closeEditor();
        renderSelectors();
        renderCueList();
        notify('当前视频 Cue 已更新，点击“保存配置”写入设备', 'success');
    } catch (error) {
        notify(error.message, 'error');
    }
}

function deleteCue(index) {
    const binding = getSelectedBinding(false);
    const cue = binding?.cues?.[index];
    if (!cue || !window.confirm(globalThis.PlayerI18n?.t(`确认删除 Cue “${cue.name || cue.id}”？`) ?? `确认删除 Cue “${cue.name || cue.id}”？`)) return;
    binding.cues.splice(index, 1);
    closeEditor();
    renderSelectors();
    renderCueList();
    notify('当前视频 Cue 已删除，点击“保存配置”写入设备', 'warning');
}

async function previewCue(index) {
    const binding = getSelectedBinding(false);
    const cue = binding?.cues?.[index];
    if (!cue) return;
    const result = await apiAction(ApiModule.STAGE_CONTROL, 'preview_cue', { cue });
    if (!result) {
        notify('Cue 预览失败', 'error');
        return;
    }
    status = result.status || status;
    renderStatus();
    notify(`已提交预览: ${cue.name || cue.id}`, 'success');
}

async function loadPlaylists() {
    const response = await apiGet('/playlists');
    if (!Array.isArray(response)) {
        playlists = [];
        playlistItems = [];
        selectedPlaylistId = '';
        selectedItemIndex = -1;
        return false;
    }
    playlists = response;
    if (!playlists.some((playlist) => String(playlist.id) === String(selectedPlaylistId))) {
        const configuredPlaylistId = config.bindings.find((binding) =>
            playlists.some((playlist) => String(playlist.id) === String(binding.playlist_id))
        )?.playlist_id;
        selectedPlaylistId = configuredPlaylistId
            ? String(configuredPlaylistId)
            : (playlists[0]?.id ? String(playlists[0].id) : '');
    }
    await loadPlaylistItems(selectedPlaylistId);
    return true;
}

async function loadPlaylistItems(playlistId) {
    const serial = ++itemLoadSerial;
    playlistItems = [];
    selectedItemIndex = -1;
    if (!playlistId) {
        renderConfig();
        return false;
    }
    const playlist = playlists.find((candidate) => String(candidate.id) === String(playlistId));
    const layerId = Number(playlist?.targetLayerId) > 0 ? Number(playlist.targetLayerId) : 1;
    const response = await apiGet(`/playlists/${encodeURIComponent(playlistId)}/items?layerId=${layerId}`);
    if (serial !== itemLoadSerial) return false;
    playlistItems = Array.isArray(response) ? response : [];
    const configuredIndexes = config.bindings
        .filter((binding) => String(binding.playlist_id) === String(playlistId))
        .map((binding) => Number(binding.playlist_item_index));
    selectedItemIndex = configuredIndexes.find((index) => playlistItems.some((item) => Number(item.itemIndex) === index))
        ?? Number(playlistItems[0]?.itemIndex ?? -1);
    renderConfig();
    return true;
}

async function selectPlaylist(playlistId) {
    selectedPlaylistId = String(playlistId || '');
    closeEditor();
    await loadPlaylistItems(selectedPlaylistId);
}

function selectVideo(itemIndex) {
    selectedItemIndex = Number(itemIndex);
    closeEditor();
    renderConfig();
}

async function loadConfig(showMessage = false) {
    const result = await apiAction(ApiModule.STAGE_CONTROL, 'get_config', {});
    if (!result?.config) {
        notify('无法读取舞台控制配置', 'error');
        return false;
    }
    config = { ...createDefaultConfig(), ...clone(result.config) };
    config.bindings = Array.isArray(config.bindings) ? config.bindings : [];
    status = result.status || {};
    closeEditor();
    await loadPlaylists();
    renderConfig();
    if (showMessage) notify('舞台控制配置已刷新', 'success');
    return true;
}

async function saveConfig() {
    const nextConfig = readTopLevelConfig();
    const result = await apiAction(ApiModule.STAGE_CONTROL, 'replace_config', { config: nextConfig }, 15000);
    if (!result?.config) {
        notify('保存舞台控制配置失败', 'error');
        return;
    }
    config = clone(result.config);
    status = result.status || status;
    renderConfig();
    notify('舞台控制配置已保存', 'success');
}

async function setEnabled(enabled) {
    const result = await apiAction(ApiModule.STAGE_CONTROL, 'set_enabled', { enabled });
    if (!result?.config) {
        document.getElementById('stage-control-enabled').checked = !enabled;
        notify('切换舞台控制启用状态失败', 'error');
        return;
    }
    config = clone(result.config);
    status = result.status || status;
    renderConfig();
    notify(enabled ? '舞台控制已启用' : '舞台控制已停用', 'success');
}

async function runAction(action, successMessage) {
    const result = await apiAction(ApiModule.STAGE_CONTROL, action, {});
    if (!result) {
        notify(`${successMessage}失败`, 'error');
        return;
    }
    config = result.config ? clone(result.config) : config;
    status = result.status || status;
    renderConfig();
    notify(successMessage, 'success');
}

function bindEvents() {
    document.getElementById('stage-control-refresh')?.addEventListener('click', () => loadConfig(true));
    document.getElementById('stage-control-save')?.addEventListener('click', saveConfig);
    document.getElementById('stage-control-enabled')?.addEventListener('change', (event) => setEnabled(event.target.checked));
    document.getElementById('stage-control-arm')?.addEventListener('click', () => runAction('arm', '舞台控制已布防'));
    document.getElementById('stage-control-disarm')?.addEventListener('click', () => runAction('disarm', '舞台控制已撤防'));
    document.getElementById('stage-control-reset')?.addEventListener('click', () => runAction('reset_timeline', '时间轴触发状态已重置'));
    document.getElementById('stage-control-playlist')?.addEventListener('change', (event) => selectPlaylist(event.target.value));
    document.getElementById('stage-control-video')?.addEventListener('change', (event) => selectVideo(event.target.value));
    document.getElementById('stage-control-add-cue')?.addEventListener('click', () => openEditor(-1));
    document.getElementById('stage-cue-apply')?.addEventListener('click', applyEditor);
    document.getElementById('stage-cue-cancel')?.addEventListener('click', closeEditor);
}

export async function initializeStageControl() {
    if (!initialized) {
        initialized = true;
        bindEvents();
    }
    await loadConfig(false);
}

export async function refreshStageControl() {
    await loadConfig(false);
}
