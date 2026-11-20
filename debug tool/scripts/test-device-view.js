'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const vm = require('node:vm');
const navigation = fs.readFileSync(path.join(__dirname, '../src-tauri/src/debug_navigation.js'), 'utf8');
const html = fs.readFileSync(path.join(__dirname, '../public/index.html'), 'utf8');
const css = fs.readFileSync(path.join(__dirname, '../public/css/main.css'), 'utf8');
const app = fs.readFileSync(path.join(__dirname, '../public/js/app.js'), 'utf8');
const rust = fs.readFileSync(path.join(__dirname, '../src-tauri/src/lib.rs'), 'utf8');

assert.match(css, /\.device-view-toolbar \{ position: absolute; z-index: 2; inset: 0;/);
assert.match(app, /invoke\('set_debug_window_chrome', \{ hidden: true \}\)/);
assert.match(app, /invoke\('set_debug_window_chrome', \{ hidden: false \}\)/);
assert.match(rust, /fn set_debug_window_chrome\(/);
assert.match(rust, /debug_active: AtomicBool/);
assert.match(rust, /debug_active.*load\(Ordering::SeqCst\)/s);

function harness({ topLevel = false, protocol = 'http:' } = {}) {
  const elements = new Map();
  const messages = [];
  const listeners = new Map();
  let mutations;
  let disconnected = false;
  const container = {
    children: [], classes: new Set(),
    classList: { add(value) { container.classes.add(value); } },
    insertBefore(button, title) {
      assert.equal(title, elements.get('app-title'));
      container.children.unshift(button);
      elements.set(button.id, button);
    },
  };
  const document = {
    getElementById(id) { return elements.get(id); },
    head: { children: [], append(node) { this.children.push(node); } },
    createElement(tag) {
      return { tag, handlers: new Map(), attributes: new Map(),
        setAttribute(key, value) { this.attributes.set(key, value); },
        addEventListener(name, callback) { this.handlers.set(name, callback); },
      };
    },
  };
  const parent = { postMessage(message) { messages.push(message.type); } };
  const window = { parent, addEventListener(name, callback) { listeners.set(name, callback); } };
  if (topLevel) window.parent = window;
  vm.runInNewContext(navigation, { window, document, location: { protocol },
    MutationObserver: class {
      constructor(callback) { mutations = callback; }
      observe(target) { assert.equal(target, document); }
      disconnect() { disconnected = true; }
    },
  });
  return {
    elements, messages, container, document,
    addTitle() { elements.set('app-title', { closest(selector) {
      assert.equal(selector, '.header-left'); return container;
    } }); },
    attach(source = parent) { listeners.get('message')?.({ source, data: { type: 'hvideo-debug-navigation-attach' } }); },
    mutate() { mutations?.(); },
    disconnected() { return disconnected; },
  };
}

const page = harness();
assert.deepEqual(page.messages, ['hvideo-debug-navigation-request']);
page.addTitle();
page.attach({}); // Other frames cannot activate the bridge.
assert.equal(page.container.children.length, 0);
page.attach();
assert.equal(page.container.children.length, 1);
assert.ok(page.container.classes.has('hvideo-debug-title-row'));
assert.equal(page.container.children[0].attributes.get('aria-label'), '返回设备列表');
assert.equal(page.document.head.children.length, 1);
assert.equal(page.messages.at(-1), 'hvideo-debug-home-ready');
page.attach(); // A frame load requests readiness again without duplicating UI.
assert.equal(page.container.children.length, 1);
assert.equal(page.document.head.children.length, 1);
page.container.children[0].handlers.get('click')();
assert.equal(page.messages.at(-1), 'hvideo-debug-home');

const delayed = harness();
delayed.attach();
delayed.addTitle();
delayed.mutate();
assert.equal(delayed.container.children.length, 1);
assert.equal(delayed.disconnected(), true);

for (const options of [{ topLevel:true }, { protocol:'https:' }]) {
  const standalone = harness(options);
  standalone.addTitle(); standalone.attach();
  assert.equal(standalone.messages.length, 0);
  assert.equal(standalone.container.children.length, 0);
}
console.log('Device navigation: trusted-parent activation, title-row insertion, SVG button return, delayed DOM, load handshake and standalone isolation passed.');
