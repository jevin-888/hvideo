// Run with PLAYWRIGHT_MODULE pointing to an installed Playwright package if needed.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const acorn = require('acorn');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = process.env.HVIDEO_WEB_ROOT || path.resolve(__dirname, '../src/network/web');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const pages = ['index.html', 'login.html', 'mobile/index.html', 'fusion-mobile/index.html', 'factory-serial.html'];
const normalize = value => value.trim().replace(/\s+/g, ' ');
const catalogContext = { window: {} };
vm.createContext(catalogContext);
vm.runInContext(read('shared/i18n-catalog.js'), catalogContext);
for (const [key, translations] of Object.entries(catalogContext.window.PlayerTranslations)) {
    assert.deepEqual(Object.keys(translations).sort(), ['en', 'id', 'th', 'vi'], key);
    const slots = value => [...new Set(value.match(/\{\d+\}/g) || [])].sort();
    for (const value of Object.values(translations)) {
        assert.ok(value && !/[\u4e00-\u9fff]/.test(value), key);
        assert.deepEqual(slots(value), slots(key), key);
    }
}
const server = http.createServer((req, res) => {
    const file = path.resolve(root, '.' + new URL(req.url, 'http://test').pathname);
    if (!file.startsWith(root + path.sep) || !fs.existsSync(file)) { res.writeHead(404).end(); return; }
    let body = fs.readFileSync(file);
    const ext = path.extname(file);
    if (ext === '.html') body = body.toString().replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, tag =>
        /src="[^"]*shared\/i18n(?:-catalog)?\.js/.test(tag) ? tag : '');
    res.setHeader('Content-Type', { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css' }[ext] || 'application/octet-stream');
    res.end(body);
});
async function main() {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const url = `http://127.0.0.1:${server.address().port}/`;
    const browser = await chromium.launch({ headless: true, channel: 'msedge' });
    try {
        const context = await browser.newContext();
        const page = await context.newPage();
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        for (const file of pages) {
            await page.goto(url + file);
            await page.waitForFunction(() => !!window.PlayerI18n);
            await page.evaluate(() => PlayerI18n.setLanguage('zh'));
            const sources = await page.evaluate(() => {
                const result = [];
                const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
                while (walker.nextNode()) {
                    const node = walker.currentNode;
                    if (!node.parentElement.closest('script,style,textarea,[translate="no"],[data-i18n-ignore]') && /[\u4e00-\u9fff]/.test(node.nodeValue)) result.push(node.nodeValue.trim().replace(/\s+/g, ' '));
                }
                for (const el of document.querySelectorAll('*')) for (const attr of ['title', 'placeholder', 'aria-label', 'alt', 'label']) {
                    const text = el.getAttribute(attr);
                    if (text && /[\u4e00-\u9fff]/.test(text)) result.push(text.trim().replace(/\s+/g, ' '));
                }
                return [...new Set(result)];
            });
            for (const code of ['en', 'id', 'vi', 'th']) {
                await page.locator('[data-player-language]').selectOption(code);
                const missing = await page.evaluate(sources => sources.filter(source => PlayerI18n.t(source) === source), sources);
                assert.deepEqual(missing, [], file + ':' + code);
                const visibleChinese = await page.evaluate(() => {
                    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT), result = [];
                    while (walker.nextNode()) {
                        const node = walker.currentNode;
                        if (!node.parentElement.closest('script,style,textarea,code,pre,[translate="no"],[data-i18n-ignore]') && /[\u4e00-\u9fff]/.test(node.nodeValue)) result.push(node.nodeValue.trim());
                    }
                    return result;
                });
                assert.deepEqual(visibleChinese, [], file + ':' + code + ' rendered text');
                assert.equal(await page.locator('html').getAttribute('lang'), { en: 'en-US', id: 'id-ID', vi: 'vi-VN', th: 'th-TH' }[code]);
            }
            await page.reload();
            assert.equal(await page.locator('[data-player-language]').inputValue(), 'th', file + ' persistence');
            await page.evaluate(() => PlayerI18n.setLanguage('zh'));
            assert.equal(await page.title(), normalize(sources.find(s => /HVIDEO|调试端/.test(s)) || await page.title()));
            console.log('PASS five languages, static vocabulary, attributes and reload:', file);
        }
        await page.goto(url + 'index.html');
        await page.evaluate(() => {
            const fixture = document.createElement('section'); fixture.id = 'i18n-fixture';
            fixture.innerHTML = '<button id="test-action">保存</button><span id="dynamic-message">加载中...</span><span class="material-name" title="保存">保存</span><span class="playlist-item-name">系统设置</span><span class="template-name">重启系统</span><span data-i18n-ignore>保存</span><input id="business-input" value="D:/歌曲/保存.mp4" placeholder="请输入名称"><select id="implicit"><option>保存</option></select><select id="explicit"><option value="保存">保存</option></select><pre>保存</pre>';
            document.body.append(fixture);
            window.clickCount = 0;
            document.getElementById('test-action').addEventListener('click', () => clickCount++);
        });
        await page.locator('[data-player-language]').selectOption('en');
        assert.equal(await page.locator('#test-action').textContent(), 'Save');
        assert.equal(await page.locator('#implicit').inputValue(), '保存');
        assert.equal(await page.locator('#explicit').inputValue(), '保存');
        assert.equal(await page.locator('#business-input').inputValue(), 'D:/歌曲/保存.mp4');
        assert.equal(await page.locator('.material-name').textContent(), '保存');
        assert.equal(await page.locator('.material-name').getAttribute('title'), '保存');
        assert.equal(await page.locator('.playlist-item-name').textContent(), '系统设置');
        assert.equal(await page.locator('.template-name').textContent(), '重启系统');
        await page.locator('#test-action').evaluate(el => el.click());
        assert.equal(await page.evaluate(() => clickCount), 1);
        await page.evaluate(() => {
            document.getElementById('dynamic-message').textContent = '正在播放：系统设置.mp4';
            document.getElementById('business-input').setAttribute('placeholder', '请输入用户名');
            document.title = 'HVIDEO控制软件';
        });
        await page.waitForFunction(() => document.getElementById('dynamic-message').textContent === 'Now playing: 系统设置.mp4');
        assert.equal(await page.title(), 'HVIDEO Controller');
        assert.equal(await page.locator('#business-input').getAttribute('placeholder'), 'Enter username');
        const second = await context.newPage(); await second.goto(url + 'login.html');
        assert.equal(await second.locator('[data-player-language]').inputValue(), 'en');
        await page.evaluate(() => PlayerI18n.setLanguage('vi'));
        await second.waitForFunction(() => PlayerI18n.language === 'vi');
        await page.evaluate(() => PlayerI18n.setLanguage('zh'));
        assert.equal(await page.locator('#test-action').textContent(), '保存');
        assert.equal(await page.locator('#dynamic-message').textContent(), '正在播放：系统设置.mp4');
        assert.equal(await page.locator('#implicit option').textContent(), '保存');
        assert.equal(await page.locator('#business-input').getAttribute('placeholder'), '请输入用户名');
        assert.equal(await page.evaluate(() => {
            const el = document.getElementById('test-action'); el.textContent = '暂停';
            return PlayerI18n.sourceText(el);
        }), '暂停', 'Immediate source reads must see external writes before the observer runs');
        // Run the actual serialization function after translating its visible label.
        const peripheral = read('modules/pages/peripheralManagement.js');
        const ast = acorn.parse(peripheral, { ecmaVersion: 'latest', sourceType: 'module' });
        const forwardId = ast.body.find(node => node.type === 'VariableDeclaration' && node.declarations.some(item => item.id.name === 'FORWARD_FUNCTION_ID'));
        await page.addScriptTag({ content: peripheral.slice(forwardId.start, forwardId.end) });
        const functions = ['chipToFunctionData', 'isForwardChip'];
        for (const name of functions) {
            const fn = ast.body.find(node => node.type === 'FunctionDeclaration' && node.id.name === name);
            await page.addScriptTag({ content: peripheral.slice(fn.start, fn.end) });
        }
        await page.evaluate(() => {
            const chip = document.createElement('div'); chip.id = 'test-chip';
            chip.dataset.functionId = 'play'; chip.dataset.action = 'play'; chip.dataset.functionCode = '2';
            chip.innerHTML = '<span>保存</span>'; document.body.append(chip);
        });
        await page.locator('[data-player-language]').selectOption('en');
        const serialized = await page.evaluate(() => chipToFunctionData(document.getElementById('test-chip')));
        assert.equal(serialized.function_name, '保存');
        assert.equal(serialized.action, 'play');
        assert.equal(serialized.function_code, 2);
        assert.equal(await page.evaluate(() => PlayerI18n.t('确认删除 Cue “保存”？')), 'Delete cue “保存”?');
        assert.equal(await page.evaluate(() => PlayerI18n.t('设备类型:')), 'Device type:');
        assert.equal(await page.evaluate(() => PlayerI18n.t('设备返回的新业务数据')), '设备返回的新业务数据');
        await page.evaluate(() => PlayerI18n.setLanguage('unsupported'));
        assert.equal(await page.evaluate(() => PlayerI18n.language), 'en');
        console.log('PASS dynamic messages/title/attributes, exact Chinese roundtrip, names/paths/values/events, cross-page/tab persistence and actual peripheral serialization');
        // Exercise the actual importer: textContent assignments are not HTML templates.
        for (const file of ['index.html', 'mobile/index.html']) {
            await page.goto(url + file);
            await page.waitForFunction(() => !!window.PlayerI18n);
            await page.evaluate(async () => {
                const host = document.createElement('div');
                host.className = 'template-actions-bar';
                host.innerHTML = '<button id="serial-add-template-btn" type="button">Add</button>';
                document.body.append(host);
                const { initSerialTemplateImport } = await import('/shared/serialTemplateImport.js');
                initSerialTemplateImport({ getType: () => 'rs485',
                    confirm: async (message, title) => {
                        let preview = document.getElementById('import-preview');
                        if (!preview) { preview = document.createElement('div'); preview.id = 'import-preview'; document.body.append(preview); }
                        preview.replaceChildren();
                        for (const text of [title, message]) {
                            const part = document.createElement('p'); part.textContent = text; preview.append(part);
                        }
                        return false; // No configuration writes during this UI regression.
                    }, submit: () => { throw new Error('Unexpected import submission'); },
                    refresh: () => {}, log: () => {} });
            });
            for (const [code, expected] of Object.entries({
                zh: ['导入表格', '下载命令表'], en: ['Import spreadsheet', 'Download command sheet'],
                id: ['Impor lembar kerja', 'Unduh lembar perintah'],
                vi: ['Nhập bảng tính', 'Tải bảng lệnh'], th: ['นำเข้าตาราง', 'ดาวน์โหลดตารางคำสั่ง']
            })) {
                await page.locator('[data-player-language]').selectOption(code);
                assert.equal(await page.locator('#serial-import-templates-btn').textContent(), expected[0]);
                assert.equal(await page.locator('a[download="485墙板命令.xlsx"]').textContent(), expected[1]);
            }
            await page.locator('[data-player-language]').selectOption('en');
            for (const duplicate of [false, true]) {
                await page.evaluate(() => document.getElementById('serial-import-templates-btn').click());
                await page.locator('.template-actions-bar input[type=file]').setInputFiles({
                    name: 'commands.csv', mimeType: 'text/csv',
                    buffer: Buffer.from('墙板编码,墙板名称\nAA,风景\n' + (duplicate ? 'AA,风景\n' : ''))
                });
                await page.waitForFunction(duplicate => {
                    const text = document.getElementById('import-preview')?.textContent || '';
                    return text.includes('Import wall panel commands') && text.includes('Import 1 commands into RS485') &&
                        text.includes('merge 1 duplicate rows') === duplicate;
                }, duplicate);
                assert.ok((await page.locator('#import-preview').textContent()).includes('AA 风景'));
            }
            for (const source of ['图层模板', '屏幕控制', '息屏开', '息屏关']) {
                const translated = await page.evaluate(source => PlayerI18n.t(source), source);
                assert.ok(!/[\u4e00-\u9fff]/.test(translated), source);
            }
        }
        console.log('PASS actual desktop/mobile import buttons in five languages, confirmation with duplicates and preserved command names');
        // Every JS-generated HTML panel must have a translated vocabulary.
        const html = [];
        function visit(node) {
            if (!node || typeof node !== 'object') return;
            let text;
            if (node.type === 'TemplateLiteral') text = node.quasis.map((q, i) => q.value.raw + (i < node.expressions.length ? `{${i}}` : '')).join('');
            if (node.type === 'Literal' && typeof node.value === 'string') text = node.value;
            if (text && /<[a-z][^>]*>/i.test(text)) html.push(text);
            for (const value of Object.values(node)) if (Array.isArray(value)) value.forEach(visit); else if (value && typeof value === 'object') visit(value);
        }
        function scan(dir) {
            for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
                const file = path.join(dir, entry.name);
                if (entry.isDirectory() && entry.name !== 'libs') scan(file);
                else if (entry.name.endsWith('.js') && !['qrcode.min.js', 'i18n.js', 'i18n-catalog.js'].includes(entry.name)) visit(acorn.parse(fs.readFileSync(file, 'utf8'), { ecmaVersion: 'latest', sourceType: 'module' }));
            }
        }
        scan(root);
        const missing = await page.evaluate(html => {
            const keys = new Set();
            for (const source of html) {
                const div = document.createElement('div'); div.innerHTML = source;
                const walker = document.createTreeWalker(div, NodeFilter.SHOW_TEXT);
                while (walker.nextNode()) if (!walker.currentNode.parentElement.closest('style,script')) keys.add(walker.currentNode.nodeValue.trim().replace(/\s+/g, ' '));
                for (const el of div.querySelectorAll('*')) for (const attr of ['placeholder', 'title', 'aria-label', 'alt', 'label']) if (el.hasAttribute(attr)) keys.add(el.getAttribute(attr).trim().replace(/\s+/g, ' '));
            }
            return [...keys].filter(key => /[\u4e00-\u9fff]/.test(key) && PlayerI18n.t(key, 'en') === key);
        }, html);
        assert.deepEqual(missing, [], 'Untranslated dynamic panel text');
        assert.deepEqual(errors, []);
        if (process.env.I18N_SCREENSHOT_DIR) {
            fs.mkdirSync(process.env.I18N_SCREENSHOT_DIR, { recursive: true });
            await page.goto(url + 'index.html');
            await page.locator('[data-player-language]').selectOption('en');
            await page.screenshot({ path: path.join(process.env.I18N_SCREENSHOT_DIR, 'desktop-en.png') });
            await page.goto(url + 'login.html');
            await page.locator('[data-player-language]').selectOption('en');
            await page.screenshot({ path: path.join(process.env.I18N_SCREENSHOT_DIR, 'login-en.png'), fullPage: true });
            await page.setViewportSize({ width: 390, height: 844 });
            await page.goto(url + 'mobile/index.html');
            await page.locator('[data-player-language]').selectOption('th');
            await page.screenshot({ path: path.join(process.env.I18N_SCREENSHOT_DIR, 'mobile-th.png'), fullPage: true });
        }
        console.log('PASS all generated HTML panels and catalog language/placeholder integrity');
        await context.close();
    } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => server.close());
