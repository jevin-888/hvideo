const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');

const root = path.resolve(__dirname, '../src/network/web');
const desktop = fs.readFileSync(path.join(root, 'modules/pages/layerMatrix/panels/CapturePanel.js'), 'utf8');
const source = fs.readFileSync(path.join(root, 'modules/pages/layerMatrix/uiRenderer.js'), 'utf8');
assert.ok(source.includes('if (isCaptureLayer) setupCaptureEdidEvents(formContainer, isCurrentRender,'));
function state(custom = false, singleMode = false) {
    return { mode: custom ? 'custom' : 'default', singleMode: custom && singleMode, applied: true,
        requested: { width: 2160, height: 1920, refreshRate: 30 },
        preferred: { width: custom ? 2160 : 1920, height: custom ? 1920 : 1080, refreshRate: custom ? 29.9975 : 60 },
        input: { width: 1920, height: 1080, refreshRate: 60 } };
}
function setup() {
    const elements = new Map();
    for (const id of ['mode-status', 'width', 'height', 'refresh', 'custom', 'apply', 'refresh-status'])
        elements.set(id, { value: '', checked: false, disabled: true, hidden: true, textContent: '', listeners: {},
            addEventListener(event, handler) { this.listeners[event] = handler; } });
    elements.set('capture-resolution-locked', { checked: false, listeners: {},
        addEventListener(event, handler) { this.listeners[event] = handler; } });
    elements.set('capture-width', { value: '1920', addEventListener(event, handler) { this.listeners[event] = handler; }, listeners: {} });
    elements.set('capture-height', { value: '1080', addEventListener(event, handler) { this.listeners[event] = handler; }, listeners: {} });
    elements.set('capture-edid-mode', { value: 'default', addEventListener(event, handler) { this.listeners[event] = handler; }, listeners: {} });
    elements.get('width').value = '2160'; elements.get('height').value = '1920'; elements.get('refresh').value = '30';
    const container = { isConnected: true, querySelector: selector => {
        if (selector === '#capture-resolution-locked') return elements.get('capture-resolution-locked');
        if (selector === '#capture-edid-mode') return elements.get('capture-edid-mode');
        const id = selector.startsWith('#capture-edid-')
            ? selector.slice('#capture-edid-'.length)
            : selector.slice(1);
        return elements.get(id) || elements.get(selector.slice(1));
    }, querySelectorAll: () => [] };
    let server = state(), get = async () => server, post = async (_url, body) => (server = state(body.mode === 'custom', body.singleMode));
    let current = true;
    const calls = [], success = [], errors = [], captureLocks = [];
    const context = vm.createContext({
        apiGet: (...args) => { calls.push(['GET', ...args]); return get(...args); },
        apiPost: (...args) => { calls.push(['POST', ...args]); return post(...args); },
        getLastApiError: () => ({ message: 'device unavailable' }),
        showSuccess: text => success.push(text), showError: text => errors.push(text),
        createNumInput: () => '', fitModeBlockHtml: () => ''
    });
    vm.runInContext(desktop.replace(/^import .*;\r?\n/gm, '').replace(/^export /gm, ''), context);
    return { context, elements, calls, success, errors, captureLocks, container,
        load: () => context.setupCaptureEdidEvents(container, () => current,
            async capture => { captureLocks.push(capture); return true; }),
        click: () => elements.get('apply').listeners.click(),
        setGet: fn => { get = fn; }, setPost: fn => { post = fn; },
        stale: () => { current = false; } };
}
(async () => {
    const t = setup();
    await t.load();
    assert.equal(t.calls.length, 1); assert.equal(t.calls[0][0], 'GET');
    assert.equal(t.elements.get('mode-status').textContent, '自动：EDID 默认，采集适配信号源');
    assert.equal(t.elements.get('apply').disabled, false);
    t.elements.get('capture-edid-mode').value = 'custom';
    t.elements.get('capture-edid-mode').listeners.change();
    t.elements.get('capture-resolution-locked').checked = true;
    t.elements.get('capture-resolution-locked').listeners.change();
    assert.equal(t.calls.length, 1); // Selecting/editing never writes.
    t.elements.get('capture-width').value = '2161';
    await t.click(); assert.equal(t.calls.length, 1); assert.equal(t.errors.length, 1);
    t.elements.get('capture-width').value = '2160';
    t.elements.get('capture-height').value = '1920';
    await t.click();
    const first = t.calls.find(call => call[0] === 'POST');
    assert.equal(JSON.stringify(t.captureLocks), JSON.stringify([{ mode: 'custom', locked: true, width: 2160, height: 1920 }]));
    assert.equal(first[1], '/system/capture-edid');
    assert.equal(JSON.stringify(first[2]), JSON.stringify({ mode: 'custom', width: 2160, height: 1920, refreshRate: 30, singleMode: true }));
    assert.equal(first[3].retries, 0); assert.equal(t.success.length, 1);
    t.elements.get('capture-resolution-locked').checked = false;
    t.elements.get('capture-resolution-locked').listeners.change();
    await t.click();
    assert.equal(JSON.stringify(t.captureLocks), JSON.stringify([{ mode: 'custom', locked: true, width: 2160, height: 1920 }, { mode: 'custom', locked: false, width: 2160, height: 1920 }]));
    assert.equal(JSON.stringify(t.calls.filter(call => call[0] === 'POST').at(-1)[2]), '{"mode":"custom","width":2160,"height":1920,"refreshRate":30,"singleMode":false}');
    // Lost POST response still reads authoritative state, and never claims success.
    t.setPost(async () => null); t.setGet(async () => state(true));
    t.elements.get('capture-resolution-locked').checked = true;
    await t.click(); assert.equal(t.elements.get('mode-status').textContent, 'device unavailable'); assert.equal(t.success.length, 2);
    assert.equal(t.elements.get('capture-resolution-locked').checked, false); // Authoritative GET restores actual mode while the failure stays visible.
    assert.equal(t.errors.length, 2);
    t.setGet(async () => null);
    await t.elements.get('refresh-status').listeners.click();
    assert.equal(t.elements.get('apply').disabled, true);
    assert.equal(t.elements.get('refresh-status').disabled, false);
    const count = t.calls.length; await t.click(); assert.equal(t.calls.length, count);

    const stale = setup(); let resolve;
    stale.setGet(() => new Promise(done => { resolve = done; }));
    const loading = stale.load(); stale.stale(); resolve(state(true)); await loading;
    assert.equal(stale.elements.get('mode-status').textContent, '');
    const busy = setup(); await busy.load(); let finish;
    busy.setPost(() => new Promise(done => { finish = done; }));
    const pending = busy.click(); await busy.click();
    assert.equal(busy.calls.filter(call => call[0] === 'POST').length, 1);
    finish(state()); await pending;
    console.log('PASS: layer editor wiring, read-only initialization, explicit custom/default writes, validation, no retries, lost response refresh, unsupported device, stale render and duplicate-click protection');
})().catch(error => { console.error(error); process.exitCode = 1; });
