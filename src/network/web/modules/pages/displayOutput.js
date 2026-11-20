import { apiGet, apiPost, getLastApiError } from '../core/api.js';
import { showConfirm, showError, showSuccess } from '../components/toast.js';

const modes = ['hdmi', 'mipi'];
let state = null;
let busy = false;
let rebootScheduled = false;
const element = id => document.getElementById(id);

function parseState(data) {
    if (!data || !modes.includes(data.mode) ||
        !['', ...modes].includes(data.active_mode) ||
        typeof data.reboot_required !== 'boolean' ||
        data.reboot_required !== (data.mode !== data.active_mode)) {
        throw new Error('系统输出状态无效，请重新读取');
    }
    return data;
}

function render(message = '') {
    const select = element('common-display-output');
    if (!select) return;
    select.value = state?.mode || '';
    select.disabled = busy || !state || rebootScheduled;
    const refresh = element('refresh-display-output-btn');
    if (refresh) {
        refresh.hidden = !!state || busy || rebootScheduled;
        refresh.disabled = busy || rebootScheduled;
    }
    const reboot = element('reboot-display-output-btn');
    if (reboot) {
        reboot.hidden = !state?.reboot_required || rebootScheduled;
        reboot.disabled = busy || rebootScheduled;
    }
    const status = element('display-output-status');
    if (status) {
        status.title = message || (rebootScheduled ? '设备正在重启，请稍后刷新页面。' : !state
            ? '正在读取系统输出选择…'
            : state.reboot_required
                ? `已保存 ${state.mode.toUpperCase()}，重启设备后生效${state.active_mode ? `；当前输出为 ${state.active_mode.toUpperCase()}` : ''}。`
                : `当前输出：${state.mode.toUpperCase()}。`);
        status.textContent = rebootScheduled ? '重启中…' : busy && message
            ? (state ? '处理中…' : '读取中…') : !state ? '读取失败'
                : state.reboot_required ? '待重启' : '';
        status.hidden = !status.textContent;
    }
}

async function readState() {
    const data = await apiGet('/system/display-output', { retries: 0 });
    if (!data) throw new Error(getLastApiError()?.message || '无法读取系统输出选择');
    return parseState(data);
}

export function initializeDisplayOutput() {
    element('common-display-output')?.addEventListener('change', saveDisplayOutput);
    element('refresh-display-output-btn')?.addEventListener('click', loadDisplayOutput);
    element('reboot-display-output-btn')?.addEventListener('click', confirmOutputReboot);
    element('common-config-tab-display')?.addEventListener('click', loadDisplayOutput);
}

export async function loadDisplayOutput() {
    if (!element('common-display-output') || busy || rebootScheduled) return;
    busy = true;
    render('正在读取系统输出选择…');
    let message = '';
    try {
        state = await readState();
    } catch (error) {
        state = null;
        message = `${error.message}，请点击重新读取。`;
    } finally {
        busy = false;
        render(message);
    }
}

async function saveDisplayOutput() {
    if (busy || !state || rebootScheduled) return;
    const mode = element('common-display-output').value;
    if (!modes.includes(mode) || mode === state.mode) { render(); return; }
    busy = true;
    render('正在保存系统输出选择…');
    let saved = false;
    let message = '';
    try {
        const data = await apiPost('/system/display-output', { mode }, { retries: 0 });
        if (!data) throw new Error(getLastApiError()?.message || '系统输出选择保存未确认');
        state = parseState(data);
        if (state.mode !== mode) throw new Error('系统输出选择回读不一致');
        saved = true;
        showSuccess(`输出选择已保存为 ${mode.toUpperCase()}`);
    } catch (error) {
        showError(error.message);
        // A disconnected response may still have persisted. Re-read the system
        // selector before allowing another write; never guess or undo it.
        try { state = await readState(); }
        catch { state = null; }
        message = state ? '' : '保存结果未确认，请重新读取系统输出选择。';
    } finally {
        busy = false;
        render(message);
    }
    if (saved && state.reboot_required) await confirmOutputReboot();
}

async function confirmOutputReboot() {
    if (busy || !state?.reboot_required || rebootScheduled) return;
    busy = true;
    render();
    try {
        const confirmed = await showConfirm(
            `输出已保存为 ${state.mode.toUpperCase()}，重启设备后生效。是否立即重启？取消后可稍后重启。`,
            '重启使输出选择生效');
        if (!confirmed) return;
        const result = await apiPost('/system/reboot', {}, { retries: 0 });
        if (!result?.reboot) throw new Error(getLastApiError()?.message || '重启请求未确认，请检查设备状态');
        rebootScheduled = true;
        showSuccess('设备即将重启');
    } catch (error) {
        showError(error.message);
    } finally {
        busy = false;
        render();
    }
}
