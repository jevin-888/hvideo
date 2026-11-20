const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');

class Element {
    constructor(target) {
        this.dataset = { commonConfigTarget: target, commonConfigPanel: target };
        this.style = {}; this.hidden = false; this.disabled = false;
        this.classes = new Set(); this.listeners = {}; this.controls = [];
        this.classList = {
            contains: key => this.classes.has(key),
            remove: key => this.classes.delete(key),
            toggle: (key, yes) => yes ? this.classes.add(key) : this.classes.delete(key)
        };
    }
    setAttribute(key, value) { this[key] = value; }
    addEventListener(key, fn) { this.listeners[key] = fn; }
    click() { this.listeners.click?.(); }
    focus() { this.focused = true; }
    querySelectorAll() { return this.controls; }
}

async function main() {
    const targets = ['schedule', 'ip', 'sync', 'vod', 'display', 'options'];
    const tabs = targets.map(target => new Element(target));
    const panels = targets.map(target => new Element(target));
    const lyrics = new Element('vod'); panels.push(lyrics);
    panels.forEach(panel => panel.controls.push({ disabled: false }));
    const pageNames = ['scene-template', 'effect-control', 'fusion', 'projection-correction', 'stage-control'];
    const menus = new Map(pageNames.map(name => [name, new Element()]));
    const pages = new Map(pageNames.map(name => [name + '-page', new Element()]));
    const ids = new Map([...pages, ['common-config-tab-vod', tabs[3]]]);
    const document = {
        getElementById: id => ids.get(id),
        querySelector: () => tabs[0],
        querySelectorAll: selector => {
            if (selector === '[data-common-config-target]') return tabs;
            if (selector === '[data-common-config-panel]') return panels;
            if (selector === '[data-common-config-panel="vod"]') return [panels[3], lyrics];
            const page = /data-page="([^"]+)"/.exec(selector)?.[1];
            return page ? [menus.get(page)] : [];
        }
    };
    let currentLicense = { status: 'valid', modules: ['effects', 'fusion'] };
    const context = vm.createContext({ document,
        window: { dispatchEvent() {} }, CustomEvent: class {},
        apiGet: async () => currentLicense
    });
    const load = file => fs.readFileSync(path.join(root, file), 'utf8')
        .replace(/^import .*;\r?\n/gm, '').replace(/^export /gm, '');
    vm.runInContext(load('src/network/web/modules/core/moduleLicense.js'), context);
    const settings = load('src/network/web/modules/pages/systemSettings.js');
    vm.runInContext(settings, context);
    context.applyModuleLicense(currentLicense);
    context.initializeCommonConfigSwitcher();
    assert.equal(tabs[3].hidden, true);
    for (const i of [0, 1, 2, 4, 5]) {
        assert.equal(tabs[i].hidden, false);
        tabs[i].click();
        assert.equal(panels[i].hidden, false, `other tab ${targets[i]} still opens`);
    }
    tabs[2].listeners.keydown({ key: 'ArrowRight', preventDefault() {} });
    assert.equal(tabs[4].focused, true, 'keyboard skips unauthorized VOD');
    assert.equal(context.isPageLicensed('scene-template'), false);
    assert.equal(context.isPageLicensed('system-settings'), true);
    assert.equal(menus.get('fusion').hidden, false);
    for (const [module, page] of [['scene', 'scene-template'], ['cave', 'projection-correction'], ['stage', 'stage-control']]) {
        context.applyModuleLicense({ status: 'valid', modules: [module] });
        assert.equal(context.isPageLicensed(page), true);
        assert.equal(menus.get(page).hidden, false);
    }
    context.applyModuleLicense({ status: 'time_calibrating', modules: ['vod'] });
    assert.equal(tabs[3].hidden, false);
    tabs[3].click();
    assert.equal(panels[3].hidden, false);
    assert.equal(lyrics.hidden, false);
    context.applyModuleLicense(currentLicense);
    assert.equal(tabs[0].classList.contains('active'), true, 'revocation selects available tab');
    assert.equal(panels[3].hidden, true);
    assert.equal(lyrics.hidden, true);
    for (const status of ['unlicensed', 'expired']) {
        context.applyModuleLicense({ status, modules: ['vod', 'cave'] });
        assert.equal(context.isModuleLicensed('vod'), false);
    }
    currentLicense = null;
    await context.refreshModuleLicense();
    assert.equal(context.isModuleLicensed('cave'), false);

    // Save other tabs while VOD is absent: protected values are retained exactly.
    const original = { vodMode: 2, enableVod: true, onlineVodHost: 'server',
        onlineVodRoomId: 'room', vodVolumeGainDb: 3, audioOutputLayerId: 4, lyricFont: 'font.ttf',
        lyricTitleFontSize: 123, lyricTextSpacing: 3, networkIpMode: 'dynamic' };
    ids.set('common-network-ip-mode', { value: 'static' });
    ids.set('common-vod-mode', { value: '0' });
    ids.set('common-vod-volume-gain-db', { value: '5' });
    ids.set('common-lyric-font', { value: '' });
    let posted;
    let persistedConfig = { ...original };
    context.apiGet = async () => ({ ...persistedConfig });
    context.apiPost = async (_, config) => {
        posted = config;
        persistedConfig = { ...config };
        return {};
    };
    context.roundNumbersInObject = data => data;
    context.addToCommandLog = context.showSuccess = () => {};
    context.showError = message => { throw new Error(message); };
    context.normalizeLyricFontValue = value => value;
    await context.saveCommonConfig();
    assert.equal(posted.networkIpMode, 'static');
    for (const [key, value] of Object.entries(original)) {
        if (key !== 'networkIpMode') assert.equal(posted[key], value, key);
    }
    assert.equal(Object.hasOwn(posted, 'lyricTextFontSize'), false, 'no hidden defaults injected');

    context.applyModuleLicense({ status: 'valid', modules: ['vod'] });
    for (let db = 0; db <= 10; db++) {
        ids.get('common-vod-volume-gain-db').value = String(db);
        await context.saveCommonConfig();
        assert.equal(posted.vodVolumeGainDb, db, `VOD gain ${db} saved as integer`);
    }
    for (const value of ['-1', '11', '2.5', 'invalid']) {
        ids.get('common-vod-volume-gain-db').value = value;
        posted = undefined;
        await assert.rejects(() => context.saveCommonConfig());
        assert.equal(posted, undefined, 'invalid gain never posted');
    }
    ids.get('common-vod-volume-gain-db').value = '4';
    context.apiPost = async (_, config) => { posted = config; return {}; };
    await assert.rejects(() => context.saveCommonConfig(), /回读不一致/,
        'gain not persisted by backend cannot report success');
    for (const db of [undefined, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) {
        persistedConfig = db === undefined ? {} : { vodVolumeGainDb: db };
        ids.get('common-vod-volume-gain-db').value = 'stale';
        await context.loadConfigForSettings();
        assert.equal(ids.get('common-vod-volume-gain-db').value, String(db ?? 0),
            'load saved gain; legacy configs default to off');
    }

    const tool = path.resolve(root, '../license-tool');
    const html = fs.readFileSync(path.join(tool, 'public/index.html'), 'utf8');
    const inputs = [...html.matchAll(/<input\b[^>]*name="module"[^>]*>/g)].map(match => match[0]);
    assert.equal(inputs.length, 9);
    for (const id of ['scene', 'cave', 'stage', 'vod']) {
        const input = inputs.find(input => input.includes(`value="${id}"`));
        assert(input && !/\bchecked\b/.test(input), `${id} starts unchecked`);
    }
    const idsFromIssuer = inputs.map(input => /value="([^"]+)"/.exec(input)[1]).sort();
    const actualIds = vm.runInContext('Object.keys(MODULE_LABELS).sort()', context);
    assert.deepEqual(Array.from(actualIds), idsFromIssuer);

    // Every verifier accepts exactly the IDs exposed by the issuer.
    const contracts = [
        ['src/core/LicenseManager.cpp', /supportedModules = \{([^}]+)\}/],
        ['../Hvideo_server/src/player_license.rs', /const MODULES:.*?= &\[([^\]]+)\]/],
        ['../license-server/src/server_player_license.rs', /const MODULES:.*?= &\[([^\]]+)\]/],
        ['../server_license-tool/src-tauri/src/player_license.rs', /const MODULES:.*?= &\[([^\]]+)\]/]
    ];
    for (const [file, pattern] of contracts) {
        const text = fs.readFileSync(path.resolve(root, file), 'utf8');
        const list = [...pattern.exec(text)[1].matchAll(/"([^"]+)"/g)].map(match => match[1]).sort();
        assert.deepEqual(list, idsFromIssuer, `module IDs match ${file}`);
    }

    // Global last-device options never grant optional modules to a new device.
    const app = fs.readFileSync(path.join(tool, 'public/js/app.js'), 'utf8');
    const start = app.indexOf('  function applyLicenseOptions(');
    const end = app.indexOf('  function loadGlobalLastLicenseOptions(', start);
    let checkedValues;
    const formCtx = vm.createContext({ $: () => null,
        setCheckedValues: (_, values) => { checkedValues = Array.from(values); }
    });
    vm.runInContext(app.slice(start, end), formCtx);
    const saved = { modules: ['TV', 'cave', 'scene', 'stage', 'vod'] };
    formCtx.applyLicenseOptions(saved, { applyOptionalModules: false });
    assert.deepEqual(checkedValues, ['TV']);
    formCtx.applyLicenseOptions(saved, { applyOptionalModules: true });
    assert.deepEqual(checkedValues, saved.modules);
    console.log('PASS: 9 module IDs, 4 optional defaults, independent permissions, VOD tab/keyboard/revoke, other tabs and saves, failed/expired licenses');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
