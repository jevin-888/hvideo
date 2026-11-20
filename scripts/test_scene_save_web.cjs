const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');

async function main() {
    const file = path.resolve(__dirname, '../src/network/web/modules',
        'pages/layerMatrix/sceneManagement.js');
    const source = fs.readFileSync(file, 'utf8')
        .replace(/^import .*;\r?\n/gm, '').replace(/^export /gm, '');
    let result = null;
    let apiError = null;
    let licensed = true;
    let promptValue = '回归场景';
    let promptCount = 0;
    const errors = [], successes = [], calls = [];
    const context = vm.createContext({
        document: { getElementById: () => ({ value: '默认配置' }) },
        apiGet: async () => ({ current_scene: '默认配置' }),
        getLastApiError: () => apiError,
        sendSceneCommand: async action => { calls.push(action); return result; },
        addToCommandLog() {},
        showError: message => errors.push(message),
        showSuccess: message => successes.push(message),
        isModuleLicensed: () => licensed,
        showPrompt: async () => { promptCount++; return promptValue; },
        console
    });
    vm.runInContext(source, context);
    for (const message of ['scene module not licensed', '请求超时 (8000ms)',
        '保存配置失败: /huoshan/config/config.json', '登录已失效，请重新登录']) {
        apiError = { message };
        assert.equal(await context.saveDefaultScene(), false);
        assert.equal(errors.at(-1), `保存失败：${message}`);
        assert.equal(successes.length, 0);
    }
    apiError = null;
    assert.equal(await context.saveDefaultScene(), false);
    assert.equal(errors.at(-1), '保存失败：后端未返回保存结果');
    result = { config_path: '/huoshan/config/config.json', save_time_ms: 3 };
    assert.equal(await context.saveDefaultScene(), true);
    assert.deepEqual(successes, ['配置已保存']);
    assert.ok(calls.every(action => action === 'save_current'));
    licensed = false;
    const before = calls.length;
    assert.equal(await context.saveCurrentScene(), null);
    assert.equal(promptCount, 0, 'unlicensed save explains why without prompting');
    assert.equal(calls.length, before, 'unlicensed save sends no mutation');
    assert.match(errors.at(-1), /场景模板授权/);
    licensed = true;
    promptValue = null;
    assert.equal(await context.saveCurrentScene(), null);
    assert.equal(calls.length, before, 'cancel sends no mutation');
    promptValue = '回归场景';
    result = null;
    for (const message of ['scene module not licensed', '请求超时 (8000ms)', '磁盘写入失败']) {
        apiError = { message };
        assert.equal(await context.saveCurrentScene(), null);
        assert.match(errors.at(-1), message.includes('scene module') ? /场景模板授权/ : /保存场景失败/);
        assert.equal(context.getCurrentSceneName(), '默认配置');
    }
    result = { softBlocked: true, message: '授权受限' };
    assert.equal(await context.saveCurrentScene(), null);
    assert.match(errors.at(-1), /授权受限/);
    result = { config_path: '/huoshan/Scene/回归场景.json' };
    assert.equal(await context.saveCurrentScene(), '回归场景');
    assert.equal(context.getCurrentSceneName(), '回归场景');
    assert.equal(successes.at(-1), '已保存场景: 回归场景');
    assert.equal(calls.at(-1), 'save_as');
    console.log(`PASS: scene default save and actual API failure messages`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
