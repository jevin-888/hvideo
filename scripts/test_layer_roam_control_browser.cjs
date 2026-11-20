const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

const root = path.resolve(__dirname, '../src/network/web');
const html = `<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<body>
  <section id="layer-roam-config-section">
    <select id="layer-roam-mode"><option value="0">静止</option><option value="1" selected>水平</option></select>
    <div class="number-input-wrapper"><button class="number-btn minus" data-target="layer-roam-speed">-</button><input id="layer-roam-speed" type="number" min="1" max="1000" step="1" value="100" data-default="100"><button class="number-btn plus" data-target="layer-roam-speed">+</button></div>
    <input id="layer-roam-loop" type="checkbox" checked>
  </section>
  <section id="slice-roam-config-section">
    <select id="slice-roam-mode"><option value="0">静止</option><option value="1" selected>水平</option></select>
    <div class="number-input-wrapper"><button class="number-btn minus" data-target="slice-roam-speed">-</button><input id="slice-roam-speed" type="number" min="1" max="1000" step="1" value="100" data-default="100"><button class="number-btn plus" data-target="slice-roam-speed">+</button></div>
    <input id="slice-roam-loop" type="checkbox" checked>
  </section>
  <script type="module">
    const base = '/modules/pages/layerMatrix/';
    const common = await import(base + 'panels/CommonControls.js');
    window.qaSetupNumberInputButtons = common.setupNumberInputButtons;
    window.qaPosts = [];
    window.qaSliceSaves = 0;
    window.qaApiPost = async (url, body) => { window.qaPosts.push({ url, body }); return { ok: true }; };
    window.qaCommandLog = () => {};
    let source = await (await fetch(base + 'uiRenderer.js')).text();
    source = source.replace(/^import .*;\\r?\\n/gm, '').replace(/^export /gm, '');
    source = 'const setupNumberInputButtons = window.qaSetupNumberInputButtons;\\nconst apiPost = window.qaApiPost;\\nconst addToCommandLog = window.qaCommandLog;\\n' + source + '\\nwindow.qaSetupRoamConfigListeners = setupRoamConfigListeners;';
    const blob = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
    await import(blob);
    window.qaSetupRoamConfigListeners(10, 'layer');
    window.qaSetupRoamConfigListeners(10, 'slice', { sliceKey: 'slice-1', applySliceProperties: async () => { window.qaSliceSaves += 1; } });
    window.qaReady = true;
  </script>
</body></html>`;

const server = http.createServer((req, res) => {
    const pathname = new URL(req.url, 'http://qa').pathname;
    if (pathname === '/roam-qa') {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(html);
        return;
    }
    const staticPath = pathname.startsWith('/mobile/shared/')
        ? '/shared/' + pathname.slice('/mobile/shared/'.length)
        : pathname;
    const file = path.resolve(root, '.' + staticPath);
    if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
        res.writeHead(404).end();
        return;
    }
    res.writeHead(200, { 'Content-Type': { '.css': 'text/css', '.js': 'text/javascript' }[path.extname(file)] || 'application/octet-stream' });
    res.end(fs.readFileSync(file));
});

(async () => {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const browser = await chromium.launch({ headless: true, channel: 'msedge' });
    try {
        for (const variant of ['desktop', 'mobile']) {
            const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
            await page.goto(`http://127.0.0.1:${server.address().port}/roam-qa?${variant}`);
            await page.waitForFunction(() => window.qaReady);
            assert.deepEqual(errors, [], `${variant} module errors`);

            await page.locator('[data-target="layer-roam-speed"].plus').click();
            await page.waitForTimeout(40);
            const layerPosts = await page.evaluate(() => window.qaPosts.slice());
            assert.equal(layerPosts.length, 1, `${variant} layer plus saves exactly once`);
            assert.equal(layerPosts[0].url, '/layers/10/roam');

            await page.locator('[data-target="slice-roam-speed"].plus').click();
            await page.waitForTimeout(40);
            assert.equal(await page.evaluate(() => window.qaSliceSaves), 1, `${variant} slice plus saves exactly once`);
            await page.close();
        }
        console.log('PASS layer roam browser: desktop/mobile layer and slice plus each trigger one save');
    } finally {
        await browser.close();
        server.close();
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
