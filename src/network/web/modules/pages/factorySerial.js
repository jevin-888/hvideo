import { getAuthToken, clearAuthToken, validateSession } from '../../shared/authSession.js';
import { parseApiResponse } from '../../shared/apiResponseParser.js';

const $ = id => document.getElementById(id);
let storedSerial = null;
let busy = false;
let awaitingReboot = false;
let heartbeat;
const loginUrl = '/login.html?terminalAutoLogin=1&next=factory-serial';
$('target-ip').textContent = window.location.hostname;

function status(text, error = false) {
    $('serial-status').textContent = text;
    $('serial-status').classList.toggle('error', error);
}

function render() {
    const ready = storedSerial !== null && !busy && !awaitingReboot;
    $('new-serial').disabled = !ready;
    $('reload-serial').disabled = busy;
    $('write-serial').disabled = !ready ||
        !/^[A-Za-z][A-Za-z0-9]{0,13}$/.test($('new-serial').value) ||
        $('new-serial').value === storedSerial;
}

async function readResponse(response, token, write = false) {
    if (response.status === 401) {
        if (getAuthToken() === token) clearAuthToken();
        window.location.replace(loginUrl);
        throw new Error('登录已过期，请重新登录');
    }
    if (response.status === 404 || response.status === 405) {
        throw new Error('设备尚不支持局域网写号，请先安装新版播放器');
    }
    const parsed = parseApiResponse(await response.json());
    if (!response.ok || !parsed.ok) throw new Error(parsed.error?.message || `HTTP ${response.status}`);
    const data = parsed.data;
    if (!data || typeof data.serial !== 'string' || typeof data.stored_serial !== 'string' ||
        typeof data.reboot_required !== 'boolean') throw new Error('写号接口响应格式不正确');
    if (write && (typeof data.reboot_scheduled !== 'boolean' ||
        data.reboot_delay_ms !== (data.reboot_scheduled ? 2000 : 0))) {
        throw new Error('写号接口缺少有效的自动重启结果，请更新播放器');
    }
    return data;
}

function showRecord(data) {
    storedSerial = data.stored_serial;
    $('current-serial').textContent = data.serial || '未获取';
    $('stored-serial').textContent = storedSerial || '尚未写入';
    $('stored-row').hidden = !data.reboot_required;
}

async function reload() {
    if (busy) return;
    busy = true;
    storedSerial = null;
    render();
    status('正在读取出厂序列号…');
    try {
        if (!await validateSession()) {
            window.location.replace(loginUrl);
            return;
        }
        const token = getAuthToken();
        const response = await fetch('/api/v1/system/factory-serial', {
            headers: { Authorization: `Bearer ${token}` },
            cache: 'no-store', signal: AbortSignal.timeout(10000)
        });
        const data = await readResponse(response, token);
        showRecord(data);
        if (!data.reboot_required) awaitingReboot = false;
        status(data.reboot_required ? '新号待生效，设备重启后请重新读取。' : '');
    } catch (error) {
        status(`读取失败：${error.message || error}`, true);
    } finally {
        busy = false;
        render();
    }
}

$('serial-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (busy || $('write-serial').disabled) return;
    const serial = $('new-serial').value;
    const expectedSerial = storedSerial;
    busy = true;
    render();
    status('正在写入并回读校验，请稍候…');
    try {
        const token = getAuthToken();
        if (!token) {
            window.location.replace(loginUrl);
            return;
        }
        const response = await fetch('/api/v1/system/factory-serial', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
            body: JSON.stringify({ serial, expected_serial: expectedSerial }),
            signal: AbortSignal.timeout(15000)
        });
        const data = await readResponse(response, token, true);
        if (data.stored_serial !== serial) throw new Error('设备回读的序列号与请求不一致');
        showRecord(data);
        awaitingReboot = data.reboot_scheduled;
        status(data.reboot_scheduled
            ? '写号成功，已安排 2 秒后重启。恢复后请重新读取。'
            : '写号成功，自动重启安排失败，请手动重启，无需重写。', !data.reboot_scheduled);
    } catch (error) {
        storedSerial = null;
        status(`结果未确认：${error.message || error}。可能已写入，请恢复连接后重新读取，勿重复写号。`, true);
    } finally {
        busy = false;
        render();
    }
});
$('new-serial').addEventListener('input', render);
$('reload-serial').addEventListener('click', () => void reload());
window.addEventListener('beforeunload', event => {
    if (busy) { event.preventDefault(); event.returnValue = ''; }
});
heartbeat = setInterval(async () => {
    if (busy || awaitingReboot) return;
    try {
        if (!await validateSession()) window.location.replace(loginUrl);
    } catch (_) { /* 网络恢复后继续检查，读取/写入操作会显示具体错误。 */ }
}, 30000);
window.addEventListener('pagehide', () => clearInterval(heartbeat), { once: true });
void reload();
