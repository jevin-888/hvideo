'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../public/js/system-update.js'), 'utf8');
const html = fs.readFileSync(path.join(__dirname, '../public/index.html'), 'utf8');
assert.match(html, /id="system-update-downgrade" type="checkbox"/);
assert.doesNotMatch(html, /id="system-update-downgrade"[^>]*checked/);

class Element {
  constructor() {
    this.handlers = new Map(); this.children = [];
    this.disabled = false; this.checked = false; this.open = false; this.value = '';
    this.classes = new Set();
    this.classList = { toggle: (name, active) => { if (active) this.classes.add(name); else this.classes.delete(name); } };
  }
  addEventListener(name, handler) { this.handlers.set(name, handler); }
  replaceChildren() { this.children = []; }
  append(...nodes) { this.children.push(...nodes); }
  showModal() { this.open = true; }
  close() { this.open = false; this.handlers.get('close')?.(); }
  focus() {}
  async click() { await this.handlers.get('click')?.(); }
}

async function checkSubmission(allowDowngrade, hardware, override, failSigning = false) {
  const elements = new Map();
  const calls = [];
  let serverState = { rom: null, romReady: false, task: null, busy: false };
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new Element());
      return elements.get(id);
    },
    createElement() { return new Element(); },
  };
  const get = id => document.getElementById(`system-update-${id}`);
  const window = {
    __TAURI__: { tauri: { async invoke(command, args) {
      calls.push({ command, args });
      if (command === 'get_system_update_state') return serverState;
      if (command === 'choose_system_rom') {
        serverState = { rom: { sourcePath: 'D:\\ROM\\V50\\update.img', versionCode: 50,
          hardware, imageSize: 1024, releaseId: 'a'.repeat(64) }, romReady: true, task: null, busy: false };
        return serverState;
      }
      if (command === 'start_system_update') {
        if (failSigning) throw new Error('ROM 清单签名验证失败');
        serverState = { ...serverState, romReady: false, busy: true,
          task: { allowDowngrade: args.allowDowngrade, hardwareOverride: args.hardwareOverride, rom: { ...serverState.rom, hardware: args.hardwareOverride || hardware, releaseId: 'b'.repeat(64) }, devices: [{ name: 'Player',
            terminalIp: '192.0.2.10', status: 'queued', message: '等待检查' }] } };
        return serverState.task;
      }
      throw new Error(command);
    } } },
    addEventListener() {},
  };
  vm.runInNewContext(source, { document, window, setTimeout() { return 1; }, clearTimeout() {} });
  const selected = { ip: '192.0.2.10', device_name: 'Player' };
  window.HvideoSystemUpdate.open([selected], selected);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(get('start').disabled, true);
  assert.equal(get('downgrade').checked, false);
  assert.equal(get('hardware').value, 'hw82');
  await get('choose').click();
  assert.ok(get('file-info').children[1].textContent.includes(` · ${hardware} · `));
  assert.equal(get('start').disabled, false);
  get('downgrade').checked = allowDowngrade;
  get('hardware').value = override;
  await get('start').click();
  const submissions = calls.filter(call => call.command === 'start_system_update');
  assert.equal(submissions.length, 1);
  assert.deepEqual(JSON.parse(JSON.stringify(submissions[0].args)), {
    targets: [{ name: 'Player', terminalIp: '192.0.2.10' }], allowDowngrade, hardwareOverride: override || null,
  });
  if (failSigning) {
    assert.match(get('status').textContent, /签名验证失败/);
    await get('refresh').click();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(get('start').disabled, false);
    assert.equal(get('hardware').disabled, false);
    assert.equal(get('hardware').value, override);
    assert.ok(get('file-info').children[1].textContent.includes('a'.repeat(12)));
    return;
  }
  assert.equal(get('hardware').disabled, true);
  assert.ok(get('file-info').children[1].textContent.includes(` · ${override || hardware} · `));
  assert.ok(get('file-info').children[1].textContent.includes('b'.repeat(12)));
  assert.equal(get('fields').disabled, true);
  assert.equal(get('start').disabled, true);
  await get('start').click();
  assert.equal(calls.filter(call => call.command === 'start_system_update').length, 1);
  await get('close').click();
  get('downgrade').checked = !allowDowngrade; get('hardware').value = 'changed';
  window.HvideoSystemUpdate.open([selected], selected);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(get('downgrade').checked, allowDowngrade);
  assert.equal(get('hardware').value, override);
  assert.equal(get('fields').disabled, true);
}

async function checkResultMonitoring() {
  const elements = new Map();
  const timers = new Map();
  let nextTimer = 0;
  let rejectNext = true;
  let polls = 0;
  const task = { status: 'running', allowDowngrade: false, hardwareOverride: 'hw81',
    devices: [{ name: 'Player', terminalIp: '192.0.2.10', status: 'staging', message: '正在推送镜像' }] };
  const state = { rom: null, romReady: false, busy: true, task };
  const document = {
    getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
    createElement() { return new Element(); },
  };
  const get = id => document.getElementById(`system-update-${id}`);
  const window = { __TAURI__: { tauri: { async invoke(command) {
    assert.equal(command, 'get_system_update_state'); polls += 1;
    if (rejectNext) { rejectNext = false; throw new Error('暂时断开'); }
    return JSON.parse(JSON.stringify(state));
  } } }, addEventListener() {} };
  vm.runInNewContext(source, { document, window,
    setTimeout(callback) { const id = ++nextTimer; timers.set(id, callback); return id; },
    clearTimeout(id) { timers.delete(id); },
  });
  const flush = () => new Promise(resolve => setImmediate(resolve));
  async function tick() {
    assert.equal(timers.size, 1, 'one automatic refresh remains scheduled');
    const [id, callback] = timers.entries().next().value;
    timers.delete(id); callback(); await flush();
  }
  const selected = { ip: '192.0.2.10', device_name: 'Player' };
  window.HvideoSystemUpdate.open([selected], selected); await flush();
  assert.match(get('status').textContent, /自动重试/);
  assert.equal(get('start').disabled, true);
  await tick();
  assert.equal(get('start').disabled, true);
  assert.match(get('results').children[0].children[0].textContent, /推送 ROM/);
  rejectNext = true; await tick();
  assert.match(get('status').textContent, /自动重试/);
  assert.equal(get('start').disabled, true);
  task.devices[0].status = 'rebooting'; await tick();
  assert.match(get('results').children[0].children[0].textContent, /等待设备恢复/);
  assert.doesNotMatch(get('results').children[0].className, /succeeded/);
  await get('close').click(); assert.equal(timers.size, 0);
  window.HvideoSystemUpdate.open([selected], selected); await flush();
  task.devices[0].status = 'verifying'; await tick();
  assert.match(get('results').children[0].children[0].textContent, /确认升级结果/);
  task.devices = [
    { name: 'Player', terminalIp: '192.0.2.10', status: 'succeeded', message: 'ROM v73 已核对' },
    { name: 'Failed', terminalIp: '192.0.2.11', status: 'failed', message: 'partition write failed' },
    { name: 'Offline', terminalIp: '192.0.2.12', status: 'unconfirmed', message: '等待结果超时' },
  ];
  task.status = 'completed'; state.busy = false; await tick();
  assert.match(get('status').textContent, /任务已结束.*成功 1 \/ 失败 1 \/ 待确认 1/);
  assert.match(get('results').children[0].className, /succeeded/);
  assert.match(get('results').children[1].children[1].textContent, /partition write failed/);
  assert.match(get('results').children[2].className, /warning/);
  assert.equal(timers.size, 0);
  assert.ok(polls >= 6);
}

(async () => {
  for (const hardware of ['universal', 'hw81', 'hw82']) {
    for (const override of ['hw82', 'hw81']) {
      for (const allowDowngrade of [false, true]) await checkSubmission(allowDowngrade, hardware, override);
    }
  }
  await checkSubmission(false, 'universal', 'hw81', true);
  await checkResultMonitoring();
  console.log('ROM UI: hardware/downgrade submission, result monitoring, automatic retry, reopen, final success/failure/unconfirmed and locking passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
