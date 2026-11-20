const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../src/network/web');
const output = process.env.MATERIAL_BROWSER_QA_OUTPUT;
const requests = [], errors = [], streams = new Set();
let added = false;
const files = [
    { name: '本地演示.mp4', path: '/huoshan/video/local/本地演示.mp4', folder: 'local', source: 'local', storage: 'internal', type: 'video' },
    { name: '王杰 - 英雄泪.mp4', path: '/huoshan/video/%E7%8E%8B.mp4', folder: '', source: 'cloud', storage: 'internal', type: 'video' },
    { name: '云端活动.mp4', path: '/huoshan/video/cloud/云端活动.mp4', folder: 'cloud', source: 'cloud', storage: 'internal', type: 'video' },
    { name: '硬盘演示.mp4', path: '/storage/ssd/huoshan/video/硬盘演示.mp4', folder: '', source: 'local', storage: 'sata', type: 'video' },
    { name: '云端封面.png', path: '/huoshan/video/云端封面.png', folder: '', source: 'cloud', storage: 'internal', type: 'image' },
    { name: '云端音乐.mp3', path: '/huoshan/video/云端音乐.mp3', folder: '', source: 'cloud', storage: 'internal', type: 'audio' }
].map(file => ({ ...file, ...(file.source === 'cloud' ? {categoryId:file.folder === 'cloud' ? 'activity-id' : 'common-id',categoryName:file.folder === 'cloud' ? '活动素材' : '通用素材'} : {}), size: 123456, is_incompatible: false, is_transcoding: false }));
const deviceQueries = process.env.MATERIAL_DEVICE_QUERY_JSON
    ? JSON.parse(fs.readFileSync(process.env.MATERIAL_DEVICE_QUERY_JSON, 'utf8')) : null;
if (deviceQueries) files.splice(0, files.length, ...deviceQueries.all.map(file=>({...file,type:'video'})));
const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://qa');
    if (url.pathname === '/qa') {
        let html = fs.readFileSync(path.join(root, 'index.html'), 'utf8').replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '');
        html = html.replace('</body>', `<style>.page{display:none!important}#video-control-page{display:block!important}.system-header,.sidebar{display:none!important}.main-content{margin-left:0!important}#video-control-page{padding:12px}</style>
          <script src="/shared/i18n-catalog.js"></script><script src="/shared/i18n.js"></script>
          <script type="module">localStorage.setItem('hvideo_auth_token','qa-token');
          import {initializeMaterials} from '/modules/features/materials.js';initializeMaterials();window.qaReady=true;</script></body>`);
        res.setHeader('Content-Type', 'text/html; charset=utf-8');res.end(html);return;
    }
    if (url.pathname === '/api/v1/events') {
        res.writeHead(200, {'Content-Type':'text/event-stream','Cache-Control':'no-cache','Connection':'keep-alive'});
        res.write(': connected\n\n');streams.add(res);req.on('close',()=>streams.delete(res));return;
    }
    if (url.pathname === '/api/v1/materials/thumbnail') {
        res.writeHead(200, {'Content-Type':'image/gif'});res.end(Buffer.from('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7','base64'));return;
    }
    if (url.pathname.startsWith('/api/v1/')) {
        requests.push(url.pathname + url.search);
        let data = [];
        if (url.pathname === '/api/v1/system/storage-space') data={available_bytes:22*1024**3};
        if (url.pathname === '/api/v1/video/transcode_status') data={active:false,states:[]};
        if (url.pathname === '/api/v1/materials' || url.pathname === '/api/v1/materials/folders') {
            const selection = files.filter(file => file.type === (url.searchParams.get('type') || 'video') &&
                file.storage === url.searchParams.get('storage') &&
                (url.searchParams.get('source') === 'all' || file.source === url.searchParams.get('source')) &&
                (!url.searchParams.get('folder') || (file.source === 'local' && file.folder === url.searchParams.get('folder'))) &&
                (!url.searchParams.get('categoryId') || (file.source === 'cloud' && file.categoryId === url.searchParams.get('categoryId'))));
            data = url.pathname.endsWith('/folders')
                ? [...new Map(selection.map(file=>file.source === 'cloud' ? [file.categoryId,{name:file.categoryName,key:'cloud-category:'+file.categoryId,categoryId:file.categoryId,kind:'cloud-category',path:''}] : [file.folder,{name:file.folder,key:file.folder,kind:'folder',path:'/huoshan/video/'+file.folder}])).values()].filter(folder=>folder.name).map(folder=>({...folder,count:1}))
                : selection;
        }
        const reply = () => {res.setHeader('Content-Type','application/json');res.end(JSON.stringify({ok:true,data,error:null}));};
        // A late local response must never replace a newer cloud selection.
        if (url.searchParams.get('source') === 'local') setTimeout(reply,150); else reply();
        return;
    }
    const file = path.resolve(root, '.'+url.pathname);
    if (!file.startsWith(root+path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {res.writeHead(404).end();return;}
    res.setHeader('Content-Type', {'.js':'text/javascript; charset=utf-8','.css':'text/css','.html':'text/html; charset=utf-8'}[path.extname(file)] || 'application/octet-stream');
    res.end(fs.readFileSync(file));
});
(async()=>{
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    const browser = await chromium.launch({headless:true,channel:'msedge'});
    try {
        const page = await browser.newPage({viewport:{width:2048,height:1212}});
        page.on('pageerror',e=>errors.push(e.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/qa`);
        await page.waitForFunction(()=>window.qaReady);
        const items=page.locator('#material-list .material-item');
        if (deviceQueries) {
            assert.equal(deviceQueries.local.length,0);
            assert.equal(deviceQueries.cloud.length,1);
            assert.equal(deviceQueries.folders[0].name,'通用素材');
            await page.locator('[data-source=cloud]').click();
            await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===1);
            const categoryKey='cloud-category:'+deviceQueries.folders[0].categoryId;
            const category=page.locator('#material-folder-list [data-folder="'+categoryKey+'"]');
            await category.click();
            await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===1);
            assert.equal(await category.textContent(),'通用素材');
            assert.match(await items.innerText(),/英雄泪/);
            assert.match(await items.innerText(),/通用素材/);
            assert.equal(await page.locator('#folder-actions-toolbar button').count(),0);
            assert.equal(await page.locator('#material-folder-list [data-folder="fdfeb7da-3067-4ccf-947a-425066eb73c0"]').count(),0);
            assert.ok(requests.some(url=>url.includes('categoryId='+deviceQueries.folders[0].categoryId)));
            if (output) await page.screenshot({path:path.join(output,'device-material-library.png'),fullPage:true});
            await page.locator('[data-source=local]').click();
            await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===0);
            assert.deepEqual(errors,[]);
            console.log('PASS device snapshot browser: one cloud card, 通用素材 category and filter, no ID directory or category disk operations, local video 0');
            return;
        }
        await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===3);
        assert.equal(await page.locator('.material-source-btn.active').getAttribute('data-source'),'all');
        await page.locator('[data-source=local]').click();await page.locator('[data-source=cloud]').click();
        await page.waitForTimeout(300);assert.equal(await items.count(),2);
        assert.deepEqual(await page.locator('.material-source').allTextContents(),['云端','云端']);
        assert.equal(await page.locator('#material-folder-list [data-folder=local]').count(),0);
        await page.locator('#material-folder-list [data-folder="cloud-category:activity-id"]').click();
        await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===1);
        assert.match(await items.innerText(),/云端活动/);
        assert.equal(await page.locator('#folder-actions-toolbar button').count(),0);
        assert.match(await items.innerText(),/活动素材/);
        assert.doesNotMatch(await items.innerText(),/internal-uuid/);
        await page.locator('#material-search').fill('英雄泪');
        await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===0);
        await page.locator('#material-refresh-btn').click();
        await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===1);
        assert.match(await items.innerText(),/英雄泪/);
        assert.equal(await page.locator('.material-source-btn.active').getAttribute('data-source'),'cloud');
        assert.ok(requests.some(u=>u.startsWith('/api/v1/materials?')&&u.includes('source=cloud')&&u.includes('categoryId=activity-id')));
        await page.locator('#material-search').fill('');
        await page.locator('.type-btn[data-type=image]').click();
        await page.waitForFunction(()=>document.querySelector('.material-name')?.textContent==='云端封面.png');
        assert.equal(await page.locator('#material-storage').isVisible(),false);
        await page.locator('.type-btn[data-type=audio]').click();
        await page.waitForFunction(()=>document.querySelector('.material-name')?.textContent==='云端音乐.mp3');
        await page.locator('.type-btn[data-type=video]').click();
        await page.locator('[data-source=local]').click();
        await page.locator('#material-storage').selectOption('sata');
        await page.waitForFunction(()=>document.querySelector('.material-name')?.textContent==='硬盘演示.mp4');
        await page.locator('#material-storage').selectOption('internal');
        await page.locator('[data-source=all]').click();
        await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===3);
        const fresh={...files[1],name:'新下载.mp4',path:'/huoshan/video/新下载.mp4'};files.push(fresh);added=true;
        for(const stream of streams)stream.write('event: materials\ndata: '+JSON.stringify({path:fresh.path})+'\n\n');
        await page.waitForFunction(()=>document.querySelectorAll('#material-list .material-item').length===4);
        await page.locator('[data-player-language]').selectOption('en', {force:true});
        await page.waitForFunction(()=>document.querySelector('[data-source=cloud]').textContent==='Cloud');
        assert.deepEqual(await page.locator('.material-source-btn').allTextContents(),['All sources','Local','Cloud']);
        assert.ok((await page.locator('.material-name').allTextContents()).includes('王杰 - 英雄泪.mp4'));
        await page.locator('[data-player-language]').selectOption('zh', {force:true});
        await page.waitForFunction(()=>document.querySelector('[data-source=cloud]').textContent==='云端');
        if (output) await page.screenshot({path:path.join(output,'material-library-desktop.png'),fullPage:true});
        assert.deepEqual(errors,[]);assert.ok(added);
        console.log('PASS browser: all/local/cloud, cloud category names and filters, no disk actions for categories, folders, search, refresh, types, SATA, late-response protection, download SSE refresh; no page errors');
    } finally {await browser.close();for(const stream of streams)stream.end();server.close();}
})().catch(error=>{console.error(error);process.exitCode=1;server.close();});
