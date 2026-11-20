const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../src/network/web');
const XLSX = require(path.join(root,'libs/xlsx.full.min.js'));
const fixture = path.join(root,'templates/rs485-wall-commands.xlsx');
const source = fs.readFileSync(path.join(root,'shared/serialTemplateImport.js'),'utf8');
const sandbox = { TextEncoder, Map, Error };
vm.createContext(sandbox);
vm.runInContext(source.replace(/^export /gm,'').replaceAll('import.meta.url', '"http://test/shared/serialTemplateImport.js"'),sandbox);
const parse = text => JSON.parse(JSON.stringify(sandbox.normalizeTemplateRows(sandbox.parseTemplateCsv(text))));
assert.equal(parse('\uFEFF墙板编码,墙板名称\r\naa:44-11 20,风景\r\nAA441120,风景\r\n').duplicateCount,1);
assert.equal(parse('名称,命令码\n"名称,带逗号",00FF').templates[0].name,'名称,带逗号');
for (const text of ['墙板编码,墙板名称\nGG,错','墙板编码,墙板名称\nABC,错','墙板编码,墙板名称\nAA,',
    '墙板编码,墙板名称\nAA,甲\nAA,乙','墙板编码,墙板编码,墙板名称\nAA,BB,甲','墙板编码,墙板名称\nAA,"未闭合']) {
    assert.throws(()=>parse(text));
}
const server=http.createServer((req,res)=>{
 const file=path.resolve(root,'.'+new URL(req.url,'http://test').pathname);
 if(!file.startsWith(root+path.sep)||!fs.existsSync(file)){res.writeHead(404).end();return;}
 const ext=path.extname(file);let body=fs.readFileSync(file);
 if(ext==='.html')body=body.toString().replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi,'');
 res.setHeader('Content-Type',{'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css'}[ext]||'application/octet-stream');res.end(body);
});
async function main(){
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({headless:true,channel:'msedge'});
 try{
  const page=await browser.newPage();const errors=[];page.on('pageerror',error=>errors.push(error.message));
  const url=`http://127.0.0.1:${server.address().port}`;
  await page.goto(url+'/index.html');
  await page.evaluate(async()=>{
   const mod=await import('/shared/serialTemplateImport.js');window.importer=mod;
   window.calls=[];window.logs=[];window.confirmResult=true;window.currentType='rs485';window.result={created_count:18,updated_count:0};
   const options={getType:()=>window.currentType,confirm:async message=>{window.preview=message;return window.confirmResult;},submit:async(type,templates)=>{window.calls.push({type,templates});return window.result;},refresh:async()=>{window.refreshed=(window.refreshed||0)+1;},log:(...args)=>window.logs.push(args)};
   mod.initSerialTemplateImport(options);mod.initSerialTemplateImport(options);
  });
  assert.equal(await page.locator('#serial-import-templates-btn').count(),1);
  await page.evaluate(()=>document.getElementById('serial-import-templates-btn').click());
  const input=page.locator('.template-actions-bar input[type=file]');
  await input.setInputFiles(fixture);
  await page.waitForFunction(()=>window.calls.length===1);
  assert.equal(await page.evaluate(()=>calls[0].templates.length),18);
  assert.equal(await page.evaluate(()=>calls[0].templates[14].data),'4630300A');
  assert.equal(await page.evaluate(()=>calls[0].type),'rs485');
  await page.waitForFunction(()=>!document.getElementById('serial-import-templates-btn').disabled);
  assert.equal(await page.evaluate(()=>refreshed),1);
  await page.evaluate(()=>{document.getElementById('serial-import-templates-btn').click();window.confirmResult=false;});
  await input.setInputFiles(fixture);await page.waitForFunction(()=>!document.getElementById('serial-import-templates-btn').disabled);
  assert.equal(await page.evaluate(()=>calls.length),1);
  await page.evaluate(()=>{window.confirmResult=true;document.getElementById('serial-import-templates-btn').click();});
  await input.setInputFiles({name:'bad.csv',mimeType:'text/csv',buffer:Buffer.from('墙板编码,墙板名称\nAA,有效\nGG,无效')});
  await page.waitForFunction(()=>logs.some(row=>row[0]==='导入墙板命令失败'));
  assert.equal(await page.evaluate(()=>calls.length),1);
  const workbook=XLSX.read(fs.readFileSync(fixture));
  for(const format of ['xls','csv']){
   const buffer=Buffer.from(XLSX.write(workbook,{type:'buffer',bookType:format}));
   await page.evaluate(()=>{window.currentType='rs232';document.getElementById('serial-import-templates-btn').click();});
   await input.setInputFiles({name:'commands.'+format,mimeType:'application/octet-stream',buffer});
   await page.waitForFunction(()=>!document.getElementById('serial-import-templates-btn').disabled);
  }
  assert.equal(await page.evaluate(()=>calls.length),3);
  assert.equal(await page.evaluate(()=>calls[1].type),'rs232');
  assert.equal(await page.evaluate(()=>calls[1].templates[0].name),'风景');
  // Formula files must never be submitted, including formulas with cached text.
  workbook.Sheets[workbook.SheetNames[0]].B2.f='"风景"';
  await page.evaluate(()=>document.getElementById('serial-import-templates-btn').click());
  await input.setInputFiles({name:'formula.xlsx',mimeType:'application/octet-stream',buffer:Buffer.from(XLSX.write(workbook,{type:'buffer',bookType:'xlsx'}))});
  await page.waitForFunction(()=>!document.getElementById('serial-import-templates-btn').disabled);
  assert.equal(await page.evaluate(()=>calls.length),3);
  assert.ok(await page.evaluate(()=>logs.at(-1)[2].includes('公式')));
  await page.evaluate(()=>{window.result=null;document.getElementById('serial-import-templates-btn').click();});
  await input.setInputFiles(fixture);await page.waitForFunction(()=>!document.getElementById('serial-import-templates-btn').disabled);
  assert.ok(await page.evaluate(()=>logs.at(-1)[0].includes('失败')));
  assert.equal(await page.evaluate(()=>refreshed),3);
  const download=await page.locator('a[download="485墙板命令.xlsx"]').getAttribute('href');
  assert.equal((await page.request.get(download)).status(),200);
  await page.evaluate(()=>{
   const config=document.getElementById('config-serial');
   document.body.replaceChildren(config);
   Object.assign(config.style,{display:'block',position:'relative',width:'1100px',height:'600px',padding:'20px'});
   config.classList.remove('hidden');
   Object.assign(document.body.style,{display:'block',height:'auto',overflow:'auto'});
  });
  if (process.env.SERIAL_TEMPLATE_QA_OUTPUT) {
    await page.locator('#config-serial .template-actions-bar').screenshot({path:path.join(process.env.SERIAL_TEMPLATE_QA_OUTPUT,'import-toolbar.png')});
  }
  assert.deepEqual(errors,[]);
  console.log('PASS: Excel/XLS/CSV, 18 source commands, preview/cancel, invalid row atomicity, formula rejection, type routing, failure reporting and template download');
 } finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
}
main().catch(error=>{console.error(error);server.close();process.exitCode=1;});
