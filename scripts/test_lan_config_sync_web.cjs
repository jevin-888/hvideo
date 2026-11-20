const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');

class Node {
    constructor(tag = 'div') {
        this.tag = tag; this.children = []; this.attrs = {}; this.listeners = {};
        this.checked = false; this.disabled = false; this.textContent = ''; this.classes = new Set();
        this.classList = { toggle: (key, enabled) => enabled ? this.classes.add(key) : this.classes.delete(key) };
    }
    set innerHTML(_) { this.children = []; }
    setAttribute(key, value) { this.attrs[key] = String(value); }
    getAttribute(key) { return this.attrs[key]; }
    appendChild(child) { child.parent = this; this.children.push(child); }
    addEventListener(event, listener) { this.listeners[event] = listener; }
    closest() { return this.parent; }
    querySelectorAll(selector) {
        const all = this.children.flatMap(child => [child, ...child.querySelectorAll(selector)]);
        return all.filter(item => item.tag === 'input' && (!selector.includes(':checked') || item.checked));
    }
}

async function exercise(file) {
    const whole = fs.readFileSync(file, 'utf8');
    const source = whole.slice(whole.indexOf('// ==================== 局域网配置同步'), whole.indexOf('// ==================== 工具函数'));
    assert(source.length > 100);
    const ids = new Map();
    for (const id of ['lan-sync-device-list', 'lan-sync-dirs-device-list', 'lan-sync-dirs-local-list',
        'lan-sync-device-folder-title', 'lan-sync-scan-btn', 'lan-sync-exec-btn',
        'lan-sync-mode-pull-btn', 'lan-sync-mode-push-btn']) ids.set(id, new Node());
    const arrow = new Node();
    const devicePanel = new Node(), localPanel = new Node();
    devicePanel.appendChild(ids.get('lan-sync-dirs-device-list'));
    localPanel.appendChild(ids.get('lan-sync-dirs-local-list'));
    let action = async () => ({ files_count: 2, synced_count: 1, restart_required: true });
    let scan = async () => ({ devices: [] });
    const calls = [], errors = [], successes = [];
    const document = {
        getElementById: id => ids.get(id), createElement: tag => new Node(tag),
        createTextNode: text => Object.assign(new Node('text'), { textContent: text }),
        querySelector: selector => selector.includes('folder-device') ? devicePanel : localPanel,
        querySelectorAll: selector => {
            if (selector.includes('arrow-cell')) return [arrow];
            const container = selector.includes('dirs-device') ? ids.get('lan-sync-dirs-device-list')
                : selector.includes('dirs-local') ? ids.get('lan-sync-dirs-local-list') : ids.get('lan-sync-device-list');
            return container.querySelectorAll(selector);
        }
    };
    const ctx = vm.createContext({ document, SYNC_CONFIG_DIRS: ['config', 'data'], SYNC_BASE_PATH: 'current',
        SYNC_BASE_LABEL: '当前数据根目录', SYNC_REQUEST_TIMEOUT_MS: 10800000,
        apiAction: (...args) => { calls.push(args); return action(...args); },
        apiGet: (...args) => scan(...args), getLastApiError: () => null,
        showError: message => errors.push(message), showSuccess: message => successes.push(message),
        addToCommandLog: () => {} });
    vm.runInContext(source, ctx);
    ctx.initializeLanSync();
    ctx.renderLanSyncDeviceList([
        { ip: '192.168.1.21', serial: 'A', name: 'Same name', config_sync: { protocol: 2 } },
        { ip: '192.168.1.22', serial: 'B', name: 'Same name', config_sync: { protocol: 2 } },
        { ip: '192.168.1.23', serial: '', name: 'No serial', config_sync: { protocol: 2 } },
        { ip: '192.168.1.24', serial: 'C', name: 'Old version' },
        { ip: '192.168.1.25', serial: 'D', name: 'Pending', config_sync: { protocol: 2, state: 'pending_restart' } }
    ]);
    const checkboxes = ids.get('lan-sync-device-list').querySelectorAll('input');
    assert.deepEqual(checkboxes.map(cb => cb.disabled), [false, false, true, true, true]);
    assert.equal(ids.get('lan-sync-mode-push-btn').classes.has('primary'), true);
    checkboxes[0].checked = checkboxes[1].checked = true;
    const pushing = ctx.startLanSync();
    assert.equal(arrow.textContent, '←');
    assert(checkboxes.every(cb => cb.disabled));
    await pushing;
    assert.equal(calls[0][0], 'sync'); assert.equal(calls[0][1], 'sync_device');
    assert.equal(JSON.stringify(calls[0][2].sync_data.target_serials), '{"192.168.1.21":"A","192.168.1.22":"B"}');
    assert(successes[0].includes('重启接收设备系统后应用'));
    ids.get('lan-sync-mode-pull-btn').listeners.click();
    assert.equal(checkboxes.filter(cb => cb.checked).length, 1);
    assert.equal(ids.get('lan-sync-mode-push-btn').classes.has('primary'), false);
    assert.equal(ids.get('lan-sync-mode-pull-btn').classes.has('primary'), true);
    checkboxes[1].checked = true; checkboxes[1].listeners.change();
    assert.equal(checkboxes[0].checked, false);
    const pulling = ctx.startLanSyncPull(); assert.equal(arrow.textContent, '→'); await pulling;
    assert.equal(calls[1][2].sync_data.source_ip, '192.168.1.22');
    assert.equal(calls[1][2].sync_data.source_serial, 'B');
    assert(successes[1].includes('重启本机系统后应用'));
    scan = async () => { throw new Error('scan failure'); };
    await ctx.scanLanDevices();
    assert.equal(ids.get('lan-sync-scan-btn').disabled, false);
    action = async () => { throw new Error('sync failure'); };
    await ctx.startLanSyncPull();
    assert.equal(arrow.textContent, '');
    assert.equal(ids.get('lan-sync-exec-btn').disabled, false);
    assert.equal(vm.runInContext('lanSyncBusy', ctx), false);
    assert.deepEqual(checkboxes.map(cb => cb.disabled), [false, false, true, true, true]);
    assert.deepEqual(errors, ['scan failure', 'sync failure']);
    console.log('PASS Web: mode, direction, identity, single-source selection, incompatible/pending, exception recovery:', path.relative(process.cwd(), file));
}
(async () => {
    await exercise(path.join(__dirname, '../src/network/web/modules/pages/systemSettings.js'));
})().catch(error => { console.error(error); process.exitCode = 1; });
