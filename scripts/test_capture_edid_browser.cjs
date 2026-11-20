const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../src/network/web');
const output = process.env.EDID_QA_OUTPUT;
const server = http.createServer((req, res) => {
    const pathname = new URL(req.url, 'http://qa').pathname;
    if (pathname === '/edid-qa') {
        const mobile = req.url.includes('mobile');
        res.setHeader('Content-Type', 'text/html; charset=utf-8');
        res.end(`<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
          <link rel="stylesheet" href="/${mobile ? 'mobile/' : ''}style.css">
          <link rel="stylesheet" href="/shared/layer-properties.css"><link rel="stylesheet" href="/shared/capture-edid.css">
          <body style="padding:16px;background:#0f0f23;color:white"><main id="layer-matrix-page"><div id="layer-properties-form" style="width:100%;max-width:1000px;margin:auto;position:static;display:block">
          <div class="property-group"><h3>采集 · 高级参数</h3><div id="host" class="property-content tab-advanced"></div></div></div>
          <script type="module">
          import {generateCaptureHtml,setupCaptureEdidEvents} from '/modules/pages/layerMatrix/panels/CapturePanel.js';
          const host=document.getElementById('host');host.innerHTML=generateCaptureHtml({captureType:'MIPI',captureIndex:0});
          host.querySelectorAll('.setting-item').forEach(el=>el.dataset.paramTab='advanced');
          await setupCaptureEdidEvents(host, () => true, capture => {const config={captureResolutionLocked:capture.locked};if(capture.mode==='custom'){config.captureWidth=capture.width;config.captureHeight=capture.height;}return fetch('/api/v1/config/layers/10', {method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify(config)}).then(response => response.ok ? fetch('/api/v1/config/save', {method:'POST'}).then(saved => saved.ok) : false);});window.qaReady=true;
          </script></main></body></html>`);
        return;
    }
    const staticPath = pathname.startsWith('/mobile/shared/')
        ? '/shared/' + pathname.slice('/mobile/shared/'.length)
        : pathname;
    const file = path.resolve(root, '.' + staticPath);
    if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404).end(); return; }
    res.setHeader('Content-Type', { '.css': 'text/css', '.js': 'text/javascript', '.html': 'text/html' }[path.extname(file)] || 'application/octet-stream');
    res.end(fs.readFileSync(file));
});
(async () => {
    if (output) fs.mkdirSync(output, { recursive: true });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const browser = await chromium.launch({ headless: true, channel: 'msedge' });
    try {
        for (const mobile of [false, true]) {
            const page = await browser.newPage({ viewport: mobile ? { width: 390, height: 844 } : { width: 1100, height: 900 } });
            const errors = [], posts = [], configPuts = [], configSaves = [];
            const missing = [];
            page.on('pageerror', error => errors.push(error.message));
            page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
            page.on('response', response => { if (response.status() === 404) missing.push(response.url()); });
            let custom = false;
            await page.route('**/api/v1/system/capture-edid', async route => {
                if (route.request().method() === 'POST') { posts.push(route.request().postDataJSON()); custom = posts.at(-1).mode === 'custom'; }
                await route.fulfill({ json: { ok: true, error: null, data: { mode: custom ? 'custom' : 'default', singleMode: posts.at(-1)?.singleMode || false, applied: true,
                    requested: { width: 2160, height: 1920, refreshRate: 30 },
                    preferred: { width: custom ? 2160 : 1920, height: custom ? 1920 : 1080, refreshRate: custom ? 29.9975 : 60 },
                    input: { width: 1920, height: 1080, refreshRate: 60 } } } });
            });
            await page.route('**/api/v1/config/layers/**', async route => {
                if (route.request().method() === 'PUT') configPuts.push(route.request().postDataJSON());
                await route.fulfill({ json: { ok: true, error: null, data: {} } });
            });
            await page.route('**/api/v1/config/save', async route => {
                configSaves.push(true);
                await route.fulfill({ json: { ok: true, error: null, data: { saved: true } } });
            });
            await page.goto(`http://127.0.0.1:${server.address().port}/edid-qa?${mobile ? 'mobile' : 'desktop'}`);
            try {
                await page.waitForFunction(() => window.qaReady, null, { timeout: 8000 });
            } catch (error) {
                console.error('capture EDID page init failed', mobile ? 'mobile' : 'desktop', errors, missing);
                throw error;
            }
            assert.equal(posts.length, 0);
            assert.equal(await page.locator('#capture-edid-mode-status').textContent(), '自动：EDID 默认，采集适配信号源');
            assert.equal(await page.locator('.capture-custom-only').first().isVisible(), false, 'automatic EDID mode hides custom resolution fields');
            await page.locator('#capture-edid-apply').click();
            await page.waitForFunction(() => !document.querySelector('#capture-edid-apply').disabled);
            assert.deepEqual(posts, [{ mode: 'default' }]);
            assert.deepEqual(configPuts, [{ captureResolutionLocked: false }]);
            await page.locator('#capture-edid-mode').selectOption('custom');
            assert.equal(await page.locator('.capture-custom-only').first().isVisible(), true, 'custom EDID mode shows custom resolution fields');
            assert.equal(await page.locator('.capture-lock-label').textContent(), '锁定分辨率', 'custom EDID mode uses lock-resolution label');
            await page.locator('#capture-resolution-locked').check();
            await page.locator('#capture-width').fill('2160');
            await page.locator('#capture-height').fill('1920');
            await page.locator('#capture-edid-refresh').fill('30');
            assert.equal(posts.length, 1);
            await page.locator('#capture-edid-apply').click();
            await page.waitForFunction(() => !document.querySelector('#capture-edid-apply').disabled);
            assert.deepEqual(posts, [{ mode: 'default' }, { mode: 'custom', width: 2160, height: 1920, refreshRate: 30, singleMode: true }]);
            assert.deepEqual(configPuts.at(-1), { captureWidth: 2160, captureHeight: 1920, captureResolutionLocked: true });
            assert.equal(configSaves.length, 2);
            const rowLayout = await page.evaluate(() => {
                const ids = ['mode', 'apply', 'refresh-status'];
                const boxes = ids.map(id => (id === 'mode' ? document.getElementById('capture-edid-mode') : document.getElementById('capture-edid-' + id)).getBoundingClientRect());
                const row = document.querySelector('.capture-resolution-row');
                return { tops: boxes.map(box => box.top), heights: boxes.map(box => box.height),
                    widths: boxes.map(box => box.width), rowHeight: row.getBoundingClientRect().height,
                    scrollWidth: row.scrollWidth, clientWidth: row.clientWidth };
            });
            assert.ok(rowLayout.rowHeight <= (mobile ? 180 : 52), 'compact control row stays within viewport');
            if (!mobile) assert.ok(rowLayout.scrollWidth <= rowLayout.clientWidth, 'desktop controls fit without scrolling');
            // The advanced-only row must remain hidden on the common tab.
            await page.locator('#host').evaluate(el => { el.classList.remove('tab-advanced'); el.classList.add('tab-common'); });
            assert.equal(await page.locator('.capture-resolution-card').isVisible(), false);
            await page.locator('#host').evaluate(el => { el.classList.remove('tab-common'); el.classList.add('tab-advanced'); });
            const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
            assert.equal(overflow, false, 'page must fit viewport');
            await page.evaluate(() => document.getElementById('toast-container')?.remove());
            if (output) await page.screenshot({ path: path.join(output, mobile ? 'mobile-edid.png' : 'desktop-edid.png'), fullPage: true });
            await page.locator('#capture-resolution-locked').uncheck();
            await page.locator('#capture-edid-apply').click();
            await page.waitForFunction(() => !document.querySelector('#capture-edid-apply').disabled);
            assert.deepEqual(posts.at(-1), { mode: 'custom', width: 2160, height: 1920, refreshRate: 30, singleMode: false });
            assert.deepEqual(configPuts.at(-1), { captureWidth: 2160, captureHeight: 1920, captureResolutionLocked: false });
            await page.locator('#capture-edid-mode').selectOption('default');
            assert.equal(await page.locator('.capture-custom-only').first().isVisible(), false, 'returning to automatic EDID mode hides custom resolution fields');
            await page.locator('#capture-edid-apply').click();
            await page.waitForFunction(() => !document.querySelector('#capture-edid-apply').disabled);
            assert.deepEqual(posts.at(-1), { mode: 'default' });
            assert.deepEqual(configPuts.at(-1), { captureResolutionLocked: false });
            assert.deepEqual(errors, []);
            console.log('PASS browser default/custom/reset, actual module imports, no autosave and viewport:', mobile ? '390px mobile' : '1100px desktop');
            await page.close();
        }
    } finally { await browser.close(); server.close(); }
})().catch(error => { server.close(); console.error(error); process.exitCode = 1; });
