// Local-only browser test. All device requests are mocked, no hardware involved.
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'web/index.html'), 'utf8');
const frame = '<svg xmlns="http://www.w3.org/2000/svg" width="800" height="600"><rect width="800" height="600" fill="#29312b"/><rect x="70" y="65" width="320" height="280" fill="#6f8172"/><path d="M230 65v280M70 205h320" stroke="#344137" stroke-width="12"/><rect x="460" y="250" width="260" height="180" rx="8" fill="#434f42"/><path d="M0 460h800v140H0z" fill="#343b31"/><text x="40" y="550" font-size="19" fill="#b9dca4" font-family="sans-serif">TEST FRAME / simulated camera</text></svg>';
const motion = {enabled:true,events:3,last_uptime_ms:12000,last_score:28,photo_bytes:400,sent:3,failed:0};
let online = true, frameRequests = 0, settingsChanges = 0;
const resolutionRequests = [];
const settings = {role:'stream',size:'400x296',quality:75,compress_ms:108,fps_x10:80};
const cameras = () => [
 {id:'stream',name:'客厅 · 实时画面',role:'stream',configured:true,online,settings:{...settings}},
 {id:'detector',name:'入口 · 移动检测',role:'detector',configured:true,online,settings:{...settings,role:'detector',fps_x10:0,size:'320x240'},motion}
];
const event = {id:'a'.repeat(32),score:28,received_at:'2026-11-10T17:30:00Z'};
(async () => {
 const browser = await chromium.launch({headless:true,channel:process.env.BROWSER_CHANNEL || 'chrome'});
 try {
  const page = await browser.newPage({viewport:{width:1440,height:1024}}), errors=[];
  page.on('pageerror',error=>errors.push(error.message));
  await page.route('http://camera.test/**', async route => {
   const url = new URL(route.request().url());
   if(url.pathname === '/') return route.fulfill({contentType:'text/html',body:html});
   if(url.pathname === '/api/state') return route.fulfill({json:{cameras:cameras(),events:[event]}});
   if(url.pathname.includes('/snap')) { frameRequests++; return route.fulfill({contentType:'image/svg+xml',headers:{'X-Frame-Id':String(frameRequests)},body:frame}); }
   if(url.pathname.includes('/settings')) {
    settingsChanges++;
    resolutionRequests.push({camera:url.pathname.split('/')[3],size:url.searchParams.get('size')});
    const isDetector=url.pathname.includes('/detector/');
    return route.fulfill({json:{...settings,role:isDetector?'detector':'stream',size:{qvga:'320x240',cif:'400x296',hvga:'480x320',vga:'640x480',svga:'800x600'}[url.searchParams.get('size')]}});
   }
   if(url.pathname.includes('/api/events/')) return route.fulfill({contentType:'image/svg+xml',body:frame});
   return route.fulfill({status:404});
  });
  await page.goto('http://camera.test/');
  await page.waitForFunction(()=>!document.getElementById('liveImage').classList.contains('hidden'));
  await page.waitForFunction(()=>!document.getElementById('eventImage').classList.contains('hidden'));
  assert.equal(await page.locator('#connection').textContent(),'2 / 2 台在线');
  assert.equal(await page.locator('#eventCount').textContent(),'3');
  assert.equal(await page.locator('#eventList li').count(),1);
  await page.locator('#pauseButton').click();
  // Let a request already in flight settle, then assert no further frames.
  await page.waitForTimeout(250);
  const pausedCount=frameRequests; await page.waitForTimeout(350);
  assert.equal(frameRequests,pausedCount);
  await page.locator('#pauseButton').click();
  await page.waitForFunction(()=>document.getElementById('pauseButton').textContent==='暂停画面');
  await page.locator('#resolution').selectOption('vga');
  await page.waitForFunction(()=>document.getElementById('toast').textContent==='分辨率已更新');
  assert.equal(settingsChanges,1);
  assert.deepEqual(resolutionRequests[0],{camera:'stream',size:'vga'});
  await page.locator('.device').nth(1).click();
  assert.equal(await page.locator('#resolution').isDisabled(),false);
  assert.equal(await page.locator('#pauseButton').isDisabled(),true);
  assert.match(await page.locator('#viewMessage').textContent(),/检测设备/);
  await page.locator('#resolution').selectOption('svga');
  await page.waitForFunction(()=>document.getElementById('toast').textContent==='分辨率已更新');
  assert.equal(settingsChanges,2);
  assert.deepEqual(resolutionRequests[1],{camera:'detector',size:'svga'});
  await page.locator('.device').nth(0).click();
  await page.waitForFunction(()=>!document.getElementById('liveImage').classList.contains('hidden'));
  await fs.promises.mkdir(path.join(root,'.tools/screenshots'),{recursive:true});
  await page.screenshot({path:path.join(root,'.tools/screenshots/dashboard-desktop.png'),fullPage:true});
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
  assert.equal(await page.evaluate(()=>{const a=document.querySelectorAll('.device');return Math.abs(a[0].getBoundingClientRect().top-a[1].getBoundingClientRect().top)<2}),true);
  await page.screenshot({path:path.join(root,'.tools/screenshots/dashboard-mobile.png'),fullPage:true});
  online=false; await page.reload();
  await page.waitForFunction(()=>document.getElementById('viewMessage').textContent==='设备暂时离线');
  assert.equal(await page.locator('#connection').textContent(),'0 / 2 台在线');
  assert.equal(await page.locator('#resolution').isDisabled(),true);
  online=true;
  const devicePage = await browser.newPage({viewport:{width:390,height:844}});
  devicePage.on('pageerror',error=>errors.push(error.message));
  await devicePage.route('http://device.test/**', async route=>{
   const url=new URL(route.request().url());
   if(url.pathname==='/') return route.fulfill({contentType:'text/html',body:html.replace('data-backend="hub"','data-backend="device"')});
   if(url.pathname==='/set') return route.fulfill({json:{...settings,role:'detector'}});
   if(url.pathname==='/motion') return route.fulfill({json:motion});
   if(url.pathname==='/motion/photo') return route.fulfill({contentType:'image/svg+xml',body:frame});
   throw new Error('unexpected device request: '+url.pathname);
  });
  await devicePage.goto('http://device.test/');
  await devicePage.waitForFunction(()=>!document.getElementById('eventImage').classList.contains('hidden'));
  assert.equal(await devicePage.locator('#connection').textContent(),'1 / 1 台在线');
  assert.equal(await devicePage.locator('#pauseButton').isDisabled(),true);
  assert.equal(await devicePage.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
  assert.deepEqual(errors,[]);
  console.log('dashboard: live/event rendering, pause/resume, resolution, device roles, offline state, desktop/mobile overflow passed');
 } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
