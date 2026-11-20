import { apiGet, apiPost, getLastApiError } from '../core/api.js';
import { showConfirm, showError, showSuccess } from '../components/toast.js';

let state = null;
let busy = false;
const element = id => document.getElementById(id);
const label = mode => mode === 'Auto' ? '自动' : mode.replace(/([pi])(\d+)(?:\.0+)?$/, (_, scan, rate) => `${scan === 'i' ? 'i' : ''}@${rate} Hz`);

function parseState(data) {
    if (!data || typeof data.mode !== 'string' || !data.mode ||
        !Array.isArray(data.modes) || !data.modes.length ||
        data.modes.some(mode => typeof mode !== 'string' || !mode) ||
        typeof data.read_only !== 'boolean') {
        throw new Error('系统分辨率状态无效，请重新读取');
    }
    return data;
}

function render(message = '') {
    const select = element('common-display-resolution');
    if (!select) return;
    select.replaceChildren();
    if (state) {
        if (!state.modes.includes(state.mode)) {
            select.add(new Option(`${label(state.mode)}（当前）`, state.mode, false, true));
            select.options[0].disabled = true;
        }
        state.modes.forEach(mode => select.add(new Option(label(mode), mode)));
        select.value = state.mode;
    } else {
        select.add(new Option(busy ? '读取中…' : '读取失败', ''));
    }
    select.disabled = busy || !state || state.read_only;
    const status = element('display-resolution-status');
    if (status) {
        status.textContent = message || (state?.read_only ? '固定分辨率' : '');
        status.hidden = !status.textContent;
    }
    const retry = element('refresh-display-resolution-btn');
    if (retry) {
        retry.hidden = !!state || busy;
        retry.disabled = busy;
    }
}

async function readState() {
    const data = await apiGet('/system/display-resolution', { retries: 0 });
    if (!data) throw new Error(getLastApiError()?.message || '无法读取系统分辨率');
    return parseState(data);
}

export function initializeDisplayResolution() {
    element('common-display-resolution')?.addEventListener('change', saveResolution);
    element('refresh-display-resolution-btn')?.addEventListener('click', loadDisplayResolution);
    element('common-config-tab-display')?.addEventListener('click', loadDisplayResolution);
}

export async function loadDisplayResolution() {
    if (!element('common-display-resolution') || busy) return;
    busy = true;
    render();
    let message = '';
    try { state = await readState(); }
    catch (error) { state = null; message = error.message; }
    finally { busy = false; render(message); }
}

async function saveResolution() {
    if (busy || !state || state.read_only) return;
    const mode = element('common-display-resolution').value;
    if (mode === state.mode || !state.modes.includes(mode)) { render(); return; }
    busy = true;
    render();
    let message = '';
    try {
        if (!await showConfirm(`将系统分辨率切换为 ${label(mode)}，屏幕可能短暂黑屏。是否继续？`, '系统分辨率')) return;
        render('切换中…');
        const data = await apiPost('/system/display-resolution', { mode }, { retries: 0 });
        if (!data) throw new Error(getLastApiError()?.message || '系统分辨率保存未确认');
        state = parseState(data);
        if (state.mode !== mode) throw new Error('系统分辨率回读不一致');
        showSuccess(`系统分辨率已保存为 ${label(state.mode)}`);
    } catch (error) {
        showError(error.message);
        // A lost response may still have applied the mode; always re-read the system.
        try { state = await readState(); }
        catch { state = null; message = '保存结果未确认，请重新读取系统分辨率'; }
    } finally { busy = false; render(message); }
}
