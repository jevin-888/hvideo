'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class Element {
  constructor() { this.handlers = new Map(); this.children = []; this.value = ''; this.classList = { toggle() {} }; }
  addEventListener(name, handler) { this.handlers.set(name, handler); }
  replaceChildren() { this.children = []; }
  append(...nodes) { this.children.push(...nodes); }
  showModal() { this.open = true; }
  focus() {}
}

(async () => {
  const elements = new Map();
  const document = {
    getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
    createElement() { return new Element(); },
  };
  const get = id => document.getElementById(`app-update-${id}`);
  const states = ['launching', 'succeeded', 'launch_failed', 'failed'];
  const task = { fileName: 'selected.apk', status: 'running', allowDowngrade: false,
    devices: states.map((status, i) => ({ name: 'Player', terminalIp: `192.0.2.${i + 1}`, status, message: status })) };
  const window = { __TAURI__: { tauri: { async invoke(name) {
    assert.equal(name, 'get_app_update_state');
    return { apk: null, busy: task.status === 'running', task };
  } } }, addEventListener() {} };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../public/js/app-update.js'), 'utf8'), {
    document, window, setTimeout() { return 1; }, clearTimeout() {},
  });
  window.HvideoAppUpdate.open([], null);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(get('progress').max, 4);
  assert.equal(get('progress').value, 3);
  assert.match(get('results').children[0].children[0].textContent, /正在启动/);
  assert.match(get('results').children[2].children[0].textContent, /已安装，未启动/);
  assert.match(get('results').children[2].className, /warning/);
  assert.match(get('status').textContent, /成功 1 \/ 失败 1 \/ 已安装未启动 1/);
  assert.equal(get('start').disabled, true);
  task.status = 'completed';
  task.devices[0].status = 'succeeded';
  await get('refresh').handlers.get('click')();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(get('progress').value, 4);
  assert.match(get('status').textContent, /任务已结束.*成功 2 \/ 失败 1 \/ 已安装未启动 1/);
  console.log('APP UI: launch progress, installed-but-not-started warning, final counts and task locking passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
