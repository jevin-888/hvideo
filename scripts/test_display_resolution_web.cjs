const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');

const source = fs.readFileSync(path.join(__dirname, '../src/network/web/modules/pages/displayResolution.js'), 'utf8')
    .replace(/^import .*;\r?\n/gm, '').replace(/^export /gm, '');
const modes = ['3840x2160p60.00', '3840x2160p30.00', '1920x1080p60.00'];
const status = mode => ({ mode, modes, read_only: false });
function setup() {
    const elements = new Map();
    for (const id of ['common-display-resolution', 'display-resolution-status', 'refresh-display-resolution-btn', 'common-config-tab-display']) {
        elements.set(id, {
            value: '', options: [], listeners: {}, hidden: false, disabled: false,
            addEventListener(event, fn) { this.listeners[event] = fn; },
            replaceChildren() { this.options = []; },
            add(option) { this.options.push(option); }
        });
    }
    const calls = [], successes = [], errors = [];
    let get = async () => status(modes[2]), post = async (_url, body) => status(body.mode);
    let confirm = async () => true;
    const context = vm.createContext({
        document: { getElementById: id => elements.get(id) },
        Option: function (text, value) { this.text = text; this.value = value; },
        apiGet: (...args) => { calls.push(['GET', ...args]); return get(...args); },
        apiPost: (...args) => { calls.push(['POST', ...args]); return post(...args); },
        getLastApiError: () => ({ message: 'test service failure' }),
        showConfirm: (...args) => confirm(...args),
        showSuccess: message => successes.push(message),
        showError: message => errors.push(message)
    });
    vm.runInContext(source, context);
    context.initializeDisplayResolution();
    return { elements, calls, successes, errors, context,
        select: elements.get('common-display-resolution'),
        setGet(fn) { get = fn; }, setPost(fn) { post = fn; }, setConfirm(fn) { confirm = fn; } };
}

(async () => {
    const t = setup();
    await t.context.loadDisplayResolution();
    assert.equal(t.select.value, modes[2]);
    assert.equal(t.select.disabled, false);
    assert.equal(t.select.options[0].text, '3840x2160@60 Hz');
    t.select.value = modes[1];
    await t.select.listeners.change();
    assert.equal(t.calls.filter(call => call[0] === 'POST').length, 1);
    const write = t.calls.find(call => call[0] === 'POST');
    assert.equal(write[1], '/system/display-resolution');
    assert.equal(JSON.stringify(write[2]), JSON.stringify({ mode: modes[1] }));
    assert.equal(write[3].retries, 0);
    assert.equal(t.select.value, modes[1]);
    assert.equal(t.successes.length, 1);
    t.setConfirm(async () => false);
    t.select.value = modes[0];
    await t.select.listeners.change();
    assert.equal(t.calls.filter(call => call[0] === 'POST').length, 1);
    assert.equal(t.select.value, modes[1]);

    t.setConfirm(async () => true);
    t.setPost(async () => null);
    t.setGet(async () => status(modes[0]));
    t.select.value = modes[0];
    await t.select.listeners.change();
    assert.equal(t.select.value, modes[0]); // A lost write response must re-read, not roll back UI blindly.
    assert.equal(t.errors.length, 1);
    assert.equal(t.successes.length, 1);

    t.setGet(async () => ({ mode: modes[2], modes: [modes[2]], read_only: true }));
    await t.context.loadDisplayResolution();
    assert.equal(t.select.disabled, true);
    const writeCount = t.calls.filter(call => call[0] === 'POST').length;
    await t.select.listeners.change();
    assert.equal(t.calls.filter(call => call[0] === 'POST').length, writeCount);

    t.setGet(async () => status('Auto'));
    await t.context.loadDisplayResolution();
    assert.equal(t.select.value, 'Auto');
    assert.equal(t.select.options[0].disabled, true);
    t.setGet(async () => ({ mode: modes[0], modes: [], read_only: false }));
    await t.context.loadDisplayResolution();
    assert.equal(t.select.disabled, true);
    assert.equal(t.elements.get('refresh-display-resolution-btn').hidden, false);
    t.setGet(async () => status(modes[2]));
    await t.elements.get('refresh-display-resolution-btn').listeners.click();
    assert.equal(t.select.disabled, false);

    let release;
    t.setPost(() => new Promise(resolve => { release = resolve; }));
    t.select.value = modes[1];
    const pending = t.select.listeners.change();
    await new Promise(setImmediate);
    const count = t.calls.filter(call => call[0] === 'POST').length;
    await t.select.listeners.change();
    await t.context.loadDisplayResolution();
    assert.equal(t.calls.filter(call => call[0] === 'POST').length, count);
    assert.equal(t.select.disabled, true);
    release(status(modes[1]));
    await pending;
    assert.equal(t.select.disabled, false);
    console.log('PASS: Web load, canonical write, cancel, lost response, MIPI, Auto, invalid response/retry, duplicate prevention');
})().catch(error => { console.error(error); process.exitCode = 1; });
