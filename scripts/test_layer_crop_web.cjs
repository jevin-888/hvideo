const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '../src/network/web');
const sides = ['left', 'top', 'right', 'bottom'];
const camel = side => 'crop' + side[0].toUpperCase() + side.slice(1);
const plain = value => JSON.parse(JSON.stringify(value));

function setup() {
    const values = { 'layer-x': 0, 'layer-y': 0, 'layer-width': 1920,
        'layer-height': 1080, 'layer-priority': 1, 'video-fit-mode': 0 };
    sides.forEach(side => values['layer-crop-' + side] = 0);
    const elements = new Map(Object.entries(values).map(([id, value]) => [id, { value: String(value) }]));
    const config = Object.fromEntries(sides.map(side => [camel(side), 0]));
    const runtime = { ...config };
    const calls = [];
    const context = vm.createContext({
        document: { getElementById: id => elements.get(id) }, setTimeout,
        parsePosition: value => value, parseSize: value => value,
        addToCommandLog() {},
        apiGet: async url => plain(url.startsWith('/runtime/') ? runtime : config),
        apiPut: async (url, patch) => {
            calls.push([url, plain(patch)]);
            if (url.startsWith('/runtime/')) Object.assign(runtime, plain(patch));
            else {
                Object.assign(config, plain(patch));
                // The real config endpoint reapplies the complete config to runtime.
                Object.assign(runtime, config);
            }
            return plain(url.startsWith('/runtime/') ? runtime : config);
        },
        handleApiOperation: async (promise, _name, _ok, _error, success) => success(await promise)
    });
    for (const relative of ['panels/InputReader.js', 'layerFieldModel.js', 'layerManagement.js']) {
        const source = fs.readFileSync(path.join(root, 'modules/pages/layerMatrix', relative), 'utf8');
        vm.runInContext(source.replace(/^import .*;\r?\n/gm, '').replace(/^export /gm, ''), context, { filename: relative });
    }
    return { context, elements, calls, config, runtime };
}

(async () => {
    const t = setup();
    for (const values of [[0, 10, 0, 0], [12.5, 20, 7.5, 15], [80, 90, 80, 90], [-5, '', 150, 'bad'], [0, 0, 0, 0]]) {
        sides.forEach((side, i) => t.elements.get('layer-crop-' + side).value = String(values[i]));
        const normalized = values.map(value => Math.max(0, Math.min(100, Number(value) || 0)) / 100);
        normalized[2] = Math.min(normalized[2], 1 - normalized[0]);
        normalized[3] = Math.min(normalized[3], 1 - normalized[1]);
        const expected = Object.fromEntries(sides.map((side, i) => [camel(side), normalized[i]]));
        const inputs = t.context.readLayerCommonInputs(1, {});
        const runtimePatch = plain(t.context.buildRuntimeLayerPatch(inputs));
        const configPatch = plain(t.context.buildConfigLayerPatch(inputs, 1));
        for (const [key, value] of Object.entries(expected)) {
            assert.equal(runtimePatch[key], value, 'desktop runtime ' + key);
            assert.equal(configPatch[key], value, 'desktop config ' + key);
        }
        await t.context.applyLayerProperties(1, [{ id: 1, type: 'video' }], {}, () => ({}), () => {}, () => {});
        for (const [key, value] of Object.entries(expected)) {
            assert.equal(t.runtime[key], value, 'config must not reset runtime ' + key);
            assert.equal(t.config[key], value, 'save must retain ' + key);
        }
        const reloaded = t.context.configLayerToUiLayer(t.config);
        sides.forEach((side, i) => assert.equal(reloaded['crop_' + side], normalized[i]));
    }
    sides.forEach(side => t.elements.delete('layer-crop-' + side));
    assert.ok(!Object.keys(t.context.readLayerCommonInputs(1, {})).some(key => key.startsWith('crop')));
    assert.equal(t.calls.length, 10);
    console.log('PASS ' + 'desktop: input -> runtime/config PUT -> config reapply -> reload; four sides, decimals, limits, invalid/reset and absent controls');
})().catch(error => { console.error(error); process.exitCode = 1; });
