import { apiGet, apiPost, getLastApiError } from '../core/api.js';
import { showError, showSuccess } from '../components/toast.js';
import { handleUnauthorized } from '/shared/authSession.js';

let busy = false;
let rebootPending = false;
const element = id => document.getElementById(id);

function setBusy(value, operation = '') {
    busy = value;
    element('config-backup-export').disabled = value;
    element('config-backup-import').disabled = value;
    element('config-backup-export').textContent = operation === 'export' ? '备份中…' : '备份配置';
    element('config-backup-import').textContent = operation === 'import' ? '还原中…' : '还原配置';
}

function reportFailure(operation, error) {
    let reason = typeof error?.message === 'string' ? error.message.trim() : '';
    if (/failed to fetch|networkerror|load failed/i.test(reason)) reason = '网络连接失败，请检查设备连接';
    if (!reason || /\?{2,}|\uFFFD/.test(reason)) reason = '未收到有效的错误原因，请检查设备日志';
    showError(`${operation}失败：${reason}`, 10000);
}

async function refreshPendingRestore(reportResult = false) {
    const data = await apiGet('/system/config-backup/status', { retries: 0 });
    if (typeof data?.restore_pending === 'boolean') rebootPending = data.restore_pending;
    if (reportResult && !data?.restore_pending && data?.message?.startsWith('完整还原失败'))
        showError(data.message, 10000);
}

async function rebootDevice() {
    const result = await apiPost('/system/reboot', {}, { retries: 0 });
    if (result?.reboot !== true) throw new Error(`备份已校验，但设备重启失败：${getLastApiError()?.message || '设备未确认重启请求'}；请再次点击还原配置重试`);
    showSuccess('还原成功，设备即将重启');
}

async function checkResponse(response, token) {
    if (handleUnauthorized(response, token)) throw new Error('登录已失效，请重新登录');
    if (!response.ok) {
        let message = `操作失败（HTTP ${response.status}）`;
        try { message = (await response.json()).error?.message || message; } catch { /* Plain transport error. */ }
        throw new Error(message);
    }
}

export async function initializeConfigBackup() {
    if (!element('config-backup-export')) return;
    element('config-backup-export').addEventListener('click', exportConfig);
    element('config-backup-import').addEventListener('click', async () => {
        if (busy) return;
        if (!rebootPending) { element('config-backup-file').click(); return; }
        // A failed/lost reboot response must remain retryable, including after refresh.
        setBusy(true, 'import');
        try { await rebootDevice(); }
        catch (error) { reportFailure('还原', error); setBusy(false); }
    });
    element('config-backup-file').addEventListener('change', importConfig);
    setBusy(true);
    try { await refreshPendingRestore(true); }
    catch { /* A later failed upload rechecks pending state before allowing retry. */ }
    finally { setBusy(false); }
}

export async function exportConfig() {
    if (busy) return;
    setBusy(true, 'export');
    let writable;
    let choosingFile = false;
    try {
        const filename = `hsvj-config-${new Date().toISOString().replace(/[:.]/g, '-')}.zip`;
        // Supported browsers write each chunk to disk, avoiding a multi-GB Blob.
        if (typeof window.showSaveFilePicker === 'function') {
            choosingFile = true;
            const handle = await window.showSaveFilePicker({ suggestedName: filename,
                types: [{ description: '配置备份 ZIP', accept: { 'application/zip': ['.zip'] } }] });
            choosingFile = false;
            writable = await handle.createWritable();
        }
        const token = localStorage.getItem('hvideo_auth_token');
        const response = await fetch('/api/v1/system/config-backup', { method: 'GET', headers: { Authorization: `Bearer ${token}` }, cache: 'no-store' });
        await checkResponse(response, token);
        const total = Number(response.headers.get('Content-Length'));
        if (!response.headers.get('Content-Type')?.includes('application/zip')) throw new Error('服务器未返回 ZIP 压缩包');
        const chunks = [];
        const reader = response.body.getReader();
        let received = 0;
        try {
            while (true) {
                const { value, done } = await reader.read();
                if (done) break;
                received += value.byteLength;
                if (writable) await writable.write(value);
                else chunks.push(value);
            }
        } catch (error) {
            await reader.cancel().catch(() => {});
            throw error;
        }
        if (total && received !== total) throw new Error('下载不完整，请重新导出');
        if (writable) { await writable.close(); writable = null; }
        else {
            const url = URL.createObjectURL(new Blob(chunks, { type: 'application/zip' }));
            const link = document.createElement('a');
            link.href = url;
            link.download = filename;
            document.body.appendChild(link);
            link.click();
            link.remove();
            setTimeout(() => URL.revokeObjectURL(url), 60000);
        }
        showSuccess('备份成功');
    } catch (error) {
        if (writable) await writable.abort().catch(() => {});
        if (!(choosingFile && error.name === 'AbortError')) reportFailure('备份', error);
    } finally { setBusy(false); }
}

function upload(file) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        const token = localStorage.getItem('hvideo_auth_token');
        xhr.open('POST', '/api/v1/system/config-backup');
        xhr.setRequestHeader('Authorization', `Bearer ${token}`);
        xhr.setRequestHeader('Content-Type', 'application/zip');
        xhr.responseType = 'json';
        xhr.timeout = 30 * 60 * 1000;
        xhr.onload = () => {
            if (handleUnauthorized(xhr, token)) { reject(new Error('登录已失效，请重新登录')); return; }
            if (xhr.status !== 200 || xhr.response?.ok !== true) {
                reject(new Error(xhr.response?.error?.message || `导入失败（HTTP ${xhr.status}）`));
                return;
            }
            resolve(xhr.response.data);
        };
        xhr.onerror = () => reject(new Error('连接中断，导入结果未确认，请检查设备后再操作'));
        xhr.ontimeout = () => reject(new Error('等待导入结果超时，请检查设备后再操作'));
        xhr.onabort = () => reject(new Error('导入已中断'));
        xhr.send(file);
    });
}

export async function importConfig() {
    const input = element('config-backup-file');
    const file = input.files?.[0];
    if (!file || busy) return;
    setBusy(true, 'import');
    let rebootScheduled = false;
    try {
        if (!file.name.toLowerCase().endsWith('.zip')) throw new Error('请选择 ZIP 格式的配置备份');
        if (!file.size) throw new Error('备份文件为空，请重新选择');
        if (file.size > 8 * 1024 ** 3) throw new Error('备份文件超过 8 GB，无法还原');
        const result = await upload(file);
        if (!Number.isInteger(result?.files) || result.files < 0 || result.restart_required !== true || result.restore_pending !== true)
            throw new Error('还原结果无效');
        rebootPending = true;
        await rebootDevice();
        rebootScheduled = true;
    } catch (error) {
        reportFailure('还原', error);
        try { await refreshPendingRestore(); } catch { /* Keep known pending state. */ }
    } finally {
        input.value = '';
        // Prevent another transfer while the device is shutting down.
        if (!rebootScheduled) setBusy(false);
    }
}
