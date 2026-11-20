const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
    '../src/network/web/mobile/modules/pages/mobileControl.js'), 'utf8');
const start = source.indexOf('async function executeConfiguredButton(');
const end = source.indexOf('\n}\n', start) + 2;
const execute = source.slice(start, end);

(async () => {
    for (const functionCount of [1, 8]) {
        const calls = [], logs = [];
        const failure = '目标 192.0.2.1:4352，目标端口拒绝连接；已额外重试 3 轮，共 4 轮，仍未成功';
        const context = vm.createContext({
            getConfiguredButton: () => ({ label: '关', functions: Array(functionCount).fill({}) }),
            apiAction: async (...args) => { calls.push(args); return null; },
            getLastApiError: () => ({ message: failure }),
            addToCommandLog: (...args) => logs.push(args),
        });
        vm.runInContext(execute, context);
        await vm.runInContext("executeConfiguredButton('projector', 'power_off')", context);
        assert.equal(calls.length, 1, 'Do not repeat the outer HTTP command');
        assert.equal(calls[0][0], 'peripherals');
        assert.equal(calls[0][1], 'execute_function_button');
        assert.equal(JSON.stringify(calls[0][2]), '{"region":"projector","button_id":"power_off"}');
        assert.equal(calls[0][3], Math.max(120000, functionCount * 30000 + 8000));
        assert.equal(logs.at(-1)[1], 'error');
        assert.equal(logs.at(-1)[2], failure, 'Preserve the semantic server error');
    }
    console.log('PASS control UI: retry budget, unchanged request contract, no HTTP replay, semantic error');
})().catch(error => { console.error(error); process.exitCode = 1; });
