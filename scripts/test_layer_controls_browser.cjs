const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

const root = path.resolve(__dirname, '../src/network/web');
const output = process.env.LAYER_CONTROLS_QA_OUTPUT;
const widths = [320, 390, 600, 768, 1024, 1280, 1920];
const panels = ['video', 'image', 'shape', 'text', 'capture'];
const videoFieldSizes = new Map();

const html = `<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="/style.css"><link rel="stylesheet" href="/shared/layer-properties.css"><link rel="stylesheet" href="/shared/capture-edid.css">
<style>body { padding: 12px; height: auto; min-height: 1600px; overflow: auto; } #layer-matrix-page { display:block; height:auto; min-height: 1600px; overflow: visible; } #layer-properties-form { max-width: 1100px; margin:auto; }</style>
<body><main id="layer-matrix-page"><div id="layer-properties-form"><section class="property-group"><header class="property-header"><h4>图层属性配置 · 高级参数超长标签</h4></header><div id="host" class="property-content property-content--compact-advanced tab-common"></div></section></div></main>
<script type="module">
import { generateVideoHtml } from '/modules/pages/layerMatrix/panels/VideoPanel.js';
import { generateImageHtml } from '/modules/pages/layerMatrix/panels/ImagePanel.js';
import { generateShapeParamHTML } from '/modules/pages/layerMatrix/panels/ShapePanel.js';
import { generateNormalTextHtml } from '/modules/pages/layerMatrix/panels/TextPanel.js';
import { generateCaptureHtml } from '/modules/pages/layerMatrix/panels/CapturePanel.js';
import { createNumInput, setupNumberInputButtons } from '/modules/pages/layerMatrix/panels/CommonControls.js';
const host = document.getElementById('host');
const sample = {
  video: generateVideoHtml({ volume: .5, alpha: .8, gaussian_blur: 2, fit_mode: 0, shape_type: 0 }),
  image: generateImageHtml({ filter_mode: 0, fade_in_time: .5, fade_out_time: .5, display_duration: 3, animated: false, shape_type: 0 }, 33),
  shape: generateShapeParamHTML(3, .25, 'layer'),
  text: generateNormalTextHtml('layer-', { text: '长文本预览', fontFile: '', textColorCss: '#ffffff', textColor: '1 1 1 1', bgColorCss: '#000000', bgColor: '0 0 0 1', isBgTransparent: false, fontSize: 36, alignment: 1, alphaVal: 255 }),
  capture: generateCaptureHtml({ captureType: 'MIPI', captureIndex: 0 })
};
host.innerHTML = sample[location.hash.slice(1)] || sample.video;
host.insertAdjacentHTML('beforeend', createNumInput('qa-decimal', '步进精度与上下限', .2, -.5, .5, .1) + createNumInput('qa-wide', '长标签与大数值配置项', 999999, 0, null, 1));
host.querySelectorAll('.setting-item').forEach((item) => item.dataset.paramTab = 'common');
let callbackCount = 0;
setupNumberInputButtons(host, () => callbackCount++);
window.qaReady = true;
window.qaState = () => ({ callbackCount, value: host.querySelector('input.number-input')?.value || '', inputCount: host.querySelectorAll('input.number-input').length });
window.qaRebind = () => { setupNumberInputButtons(host, () => callbackCount++); setupNumberInputButtons(host, () => callbackCount++); };
</script></main></body></html>`;

const server = http.createServer((req, res) => {
  const pathname = new URL(req.url, 'http://qa').pathname;
  if (pathname === '/controls-qa') {
    const mobile = req.url.includes('mobile');
    const source = mobile ? html.replace('href="/style.css"', 'href="/mobile/style.css"') : html;
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); res.end(source); return;
  }
  const staticPath = pathname.startsWith('/mobile/shared/') ? '/shared/' + pathname.slice('/mobile/shared/'.length) : pathname;
  const file = path.resolve(root, '.' + staticPath);
  if (!file.startsWith(root + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404).end(); return; }
  const type = { '.css': 'text/css', '.js': 'text/javascript', '.html': 'text/html' }[path.extname(file)] || 'application/octet-stream';
  res.writeHead(200, { 'Content-Type': type }); res.end(fs.readFileSync(file));
});

function assertNoOverflow(page, width) {
  return page.evaluate((viewportWidth) => {
    const root = document.documentElement;
    const overflow = root.scrollWidth > viewportWidth + 1;
    const offenders = [...document.querySelectorAll('#layer-properties-form *')].filter((el) => {
      const r = el.getBoundingClientRect();
      return r.width > 0 && (r.right > viewportWidth + 1 || r.left < -1);
    }).slice(0, 5).map((el) => ({ tag: el.tagName, id: el.id, className: el.className, right: el.getBoundingClientRect().right }));
    const narrowInputs = [...document.querySelectorAll('input.number-input')].filter(el => el.getBoundingClientRect().width > 0 && el.getBoundingClientRect().width < 48).map(el => ({ id: el.id, width: el.getBoundingClientRect().width, wrapper: el.parentElement.getBoundingClientRect().width, item: el.closest('.setting-item').getBoundingClientRect().width, container: el.closest('.property-content').getBoundingClientRect().width, grid: getComputedStyle(el.closest('.property-content')).gridTemplateColumns }));
    const overlaps = [...document.querySelectorAll('.number-input-wrapper')].flatMap(el => {
      const boxes = [...el.children].filter(child => child.getBoundingClientRect().width > 0).map(child => ({ id: child.id || child.className, rect: child.getBoundingClientRect() }));
      return boxes.slice(1).filter((box, index) => box.rect.left < boxes[index].rect.right - 1).map(box => box.id);
    });
    const firstInput = document.querySelector('input.number-input');
    return { overflow, offenders, pageWidth: root.scrollWidth, narrowInputs, overlaps,
      inputBackground: firstInput ? getComputedStyle(firstInput).backgroundColor : null,
      inputColor: firstInput ? getComputedStyle(firstInput).color : null,
      controlHeight: firstInput?.getBoundingClientRect().height || 0 };
  }, width);
}

(async () => {
  if (output) fs.mkdirSync(output, { recursive: true });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch({ headless: true, channel: 'msedge' });
  try {
    for (const mobile of [false, true]) {
      for (const panel of panels) {
        const page = await browser.newPage({ viewport: { width: mobile ? 390 : 1280, height: mobile ? 844 : 900 }, isMobile: mobile, hasTouch: mobile });
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        page.on('console', (message) => { if (message.type() === 'error') errors.push(message.text()); });
        await page.goto(`http://127.0.0.1:${server.address().port}/controls-qa?${mobile ? 'mobile' : 'desktop'}#${panel}`);
        await page.waitForFunction(() => window.qaReady);
        assert.deepEqual(errors, [], `${panel} ${mobile ? 'mobile' : 'desktop'} module errors`);
        for (const width of widths) {
          await page.setViewportSize({ width, height: mobile ? 844 : 900 });
          const result = await assertNoOverflow(page, width);
          assert.equal(result.overflow, false, `${panel} ${mobile ? 'mobile' : 'desktop'} ${width}px overflow ${JSON.stringify(result.offenders)}`);
          assert.deepEqual(result.narrowInputs, [], `${panel} ${width}px numeric editors too narrow`);
          assert.deepEqual(result.overlaps, [], `${panel} ${width}px overlapping numeric children`);
          assert.notEqual(result.inputBackground, 'rgb(255, 255, 255)', `${panel} ${width}px numeric input retains dark control surface`);
          assert.ok(result.controlHeight >= 32, `${panel} ${width}px numeric control has stable height`);
          if (panel === 'video' || panel === 'capture') {
            const fieldSizes = await page.evaluate(() => ['video-gaussian-blur', 'video-fit-mode', 'layer-shape-type', 'layer-invert', 'layer-black-to-transparent'].map(id => {
              const control = document.getElementById(id);
              const field = control.closest('.setting-item').getBoundingClientRect();
              const box = control.getBoundingClientRect();
              return { id, fieldWidth: Math.round(field.width), width: Math.round(box.width), height: Math.round(box.height) };
            }));
            const key = `${mobile}-${width}`;
            if (panel === 'video') videoFieldSizes.set(key, fieldSizes);
            else assert.deepEqual(fieldSizes, videoFieldSizes.get(key), `capture and video field sizes match at ${width}px`);
          }
          if (panel === 'capture') {
            assert.equal(await page.locator('#layer-shape-param-item').isVisible(), false, 'default capture shape hides its unused parameter');
          }
          if (output && (width === 320 || width === 390)) await page.screenshot({ path: path.join(output, `${mobile ? 'mobile' : 'desktop'}-${panel}-${width}.png`), fullPage: true });
        }
        const input = page.locator('input.number-input').first();
        if (await input.count()) {
          const before = await page.evaluate(() => window.qaState());
          const plus = page.locator(`button.number-btn.plus[data-target="${await input.getAttribute('id')}"]`).first();
          await plus.click();
          const afterPlus = await page.evaluate(() => window.qaState());
          assert.equal(afterPlus.callbackCount, before.callbackCount + 1, `${panel} plus callback`);
          await page.locator(`button.number-btn.number-reset[data-target="${await input.getAttribute('id')}"]`).first().click();
          assert.equal((await page.evaluate(() => window.qaState())).callbackCount, afterPlus.callbackCount + 1, `${panel} reset callback`);
          await input.fill('999999');
          await input.press('ArrowDown');
          assert.ok(Number(await input.inputValue()) <= Number(await input.getAttribute('max') || 999999), `${panel} numeric bounds`);
        }
        await page.evaluate(() => window.qaRebind());
        const decimal = page.locator('#qa-decimal');
        await decimal.fill('0.2');
        const calls = (await page.evaluate(() => window.qaState())).callbackCount;
        await page.locator('[data-target="qa-decimal"].plus').click();
        assert.equal(await decimal.inputValue(), '0.3', 'decimal precision');
        assert.equal((await page.evaluate(() => window.qaState())).callbackCount, calls + 1, 'reinitialization must not duplicate callbacks');
        await decimal.fill('0.5');
        await page.locator('[data-target="qa-decimal"].plus').click();
        assert.equal(await decimal.inputValue(), '0.5', 'maximum');
        await decimal.fill('-0.5');
        await page.locator('[data-target="qa-decimal"].minus').click();
        assert.equal(await decimal.inputValue(), '-0.5', 'minimum');
        await decimal.evaluate(el => el.disabled = true);
        const disabledCalls = (await page.evaluate(() => window.qaState())).callbackCount;
        await page.locator('[data-target="qa-decimal"].plus').click();
        assert.equal(await decimal.inputValue(), '-0.5', 'disabled editor cannot be changed by buttons');
        assert.equal((await page.evaluate(() => window.qaState())).callbackCount, disabledCalls, 'disabled editor callback');
        await decimal.evaluate(el => el.disabled = false);
        await decimal.fill('0.2');
        await decimal.click();
        assert.equal(await decimal.evaluate(el => document.activeElement === el), true, 'click to edit');
        await decimal.press('ArrowUp');
        assert.equal(await decimal.inputValue(), '0.3', 'keyboard step');
        const box = await decimal.boundingBox();
        const dragValue = Number(await decimal.inputValue());
        await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
        await page.mouse.down();
        await page.mouse.move(box.x + box.width / 2 + 40, box.y + box.height / 2, { steps: 4 });
        await page.mouse.up();
        assert.ok(Number(await decimal.inputValue()) > dragValue, 'horizontal drag increments the value');
        assert.equal(await decimal.evaluate(el => document.activeElement === el), false, 'drag does not leave the numeric field focused');
        if (mobile) {
          await page.evaluate(() => window.scrollTo(0, 0));
          const start = await decimal.boundingBox();
          assert.ok(start.y < 780, 'mobile numeric control remains reachable for touch scroll');
          const session = await page.context().newCDPSession(page);
          await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: start.x + start.width / 2, y: start.y + start.height / 2, id: 1 }] });
          for (let offset = 10; offset <= 90; offset += 10) {
            await session.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: start.x + start.width / 2 + 1, y: start.y + start.height / 2 - offset, id: 1 }] });
            await page.waitForTimeout(16);
          }
          await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
          const scrollState = await page.evaluate(() => ({ y: window.scrollY, rootY: document.documentElement.scrollTop, bodyY: document.body.scrollTop, height: document.documentElement.scrollHeight, bodyHeight: document.body.scrollHeight, client: document.documentElement.clientHeight, overflow: getComputedStyle(document.body).overflow }));
          assert.ok(scrollState.height > scrollState.client, `mobile page has vertical scroll room: ${JSON.stringify(scrollState)}`);
          // Headless Chromium does not advance native scrolling for synthetic CDP touch moves;
          // the scroll room and pan-y contract are the stable assertions here.
          assert.ok(await page.evaluate(() => getComputedStyle(document.querySelector('.numeric-drag-field')).touchAction.includes('pan-y')), 'touch drag preserves vertical pan');
          await session.detach();
        }
        await page.close();
      }
    }
    console.log(`PASS layer controls browser: ${panels.length} panel generators x desktop/mobile x ${widths.length} widths; overflow, imports, numeric plus/reset/bounds verified`);
  } finally { await browser.close(); server.close(); }
})().catch((error) => { server.close(); console.error(error.stack || error); process.exitCode = 1; });
