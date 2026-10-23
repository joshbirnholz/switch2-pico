// Run with PLAYWRIGHT_MODULE pointing to a locally installed playwright module.
import {mkdir} from 'node:fs/promises';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const shots=process.env.MIC_TEST_SCREENSHOTS || '/tmp/s2-mic-test-ui';
await mkdir(shots,{recursive:true});
import assert from 'node:assert/strict';
const browser=await chromium.launch({headless:true});
const page=await browser.newPage({viewport:{width:1280,height:800},colorScheme:'dark'});
const errors=[];page.on('pageerror',e=>errors.push(e.message));
await page.goto(new URL('../web/index.html',import.meta.url).href);
await page.evaluate(()=>{
 window.fixture={phase:'ready',session:1,cancels:[],delayOpen:0,fail:false};
 api=async(path,opts)=>{
  if(!path.startsWith('/api/mic-test'))throw Error('Unexpected route');
  const q=new URL(path,'http://test').searchParams,do_=q.get('do');
  if(do_==='open') {if(fixture.delayOpen)await new Promise(r=>setTimeout(r,fixture.delayOpen));fixture.phase='ready';fixture.session++;}
  if(do_==='record')fixture.phase='recording';
  if(do_==='cancel'){fixture.cancels.push(Number(q.get('session')));fixture.phase='idle';}
  const state={session:fixture.session,phase:fixture.phase,peak:9000,elapsed_ms:2300,remaining_ms:2700,error:''};
  return {ok:!fixture.fail,text:()=> 'Headset disconnected',json:()=>state};
 };
});
await page.evaluate(()=>openMicTest());
await page.getByRole('button',{name:'Begin Recording',exact:true}).waitFor({state:'visible'});
await page.screenshot({path:`${shots}/ready.png`});
await page.getByRole('button',{name:'Begin Recording',exact:true}).click();
await page.waitForFunction(()=>document.querySelector('#mic-test-title').textContent==='Recording...');
assert.equal(await page.locator('#mic-test-level').getAttribute('aria-label'),'Microphone audio level');
assert.equal(await page.locator('#mic-test-input-symbol').getAttribute('hidden'),null);
assert(await page.locator('.mic-level-symbol').evaluate(el=>el.getBoundingClientRect().right<document.querySelector('#mic-test-level').getBoundingClientRect().left));
await page.screenshot({path:`${shots}/recording.png`});
await page.evaluate(()=>fixture.phase='playback');
await page.waitForFunction(()=>document.querySelector('#mic-test-title').textContent==='Now playing back...');
assert.equal(await page.locator('#mic-test-level').getAttribute('aria-label'),'Playback audio level');
assert.equal(await page.locator('#mic-test-output-symbol').getAttribute('hidden'),null);
assert.notEqual(await page.locator('#mic-test-input-symbol').getAttribute('hidden'),null);
await page.screenshot({path:`${shots}/playback.png`});
assert.equal(await page.locator('#mic-test-level-label, #mic-test-time').count(),0);
await page.evaluate(()=>fixture.phase='done');
await page.waitForFunction(()=>!document.querySelector('#mic-test-dialog').open);
assert.equal(await page.evaluate(()=>micTestView),null);
// Escape still cancels an open preview.
await page.evaluate(()=>openMicTest());
await page.getByRole('button',{name:'Begin Recording',exact:true}).waitFor({state:'visible'});
await page.keyboard.press('Escape');
await page.waitForFunction(()=>!document.querySelector('#mic-test-dialog').open);
assert((await page.evaluate(()=>fixture.cancels.length))>0);
// Close while open is in flight: cancel the late session as soon as it arrives.
await page.evaluate(()=>{fixture.delayOpen=200;void openMicTest();closeMicTest();});
await page.waitForTimeout(400);
assert.equal(await page.evaluate(()=>micTestView),null);
assert.equal(await page.evaluate(()=>fixture.phase),'idle');
// A disconnected device gets an actionable error, with Close still available.
await page.evaluate(()=>{fixture.delayOpen=0;fixture.fail=true;return openMicTest();});
await page.getByText('Headset disconnected',{exact:true}).waitFor();
await page.getByRole('button',{name:'Close',exact:true}).click();
assert.deepEqual(errors,[]);
await page.setViewportSize({width:390,height:844});
await page.evaluate(()=>{fixture.fail=false;return openMicTest()});
await page.screenshot({path:`${shots}/mobile.png`});
assert(await page.locator('#mic-test-dialog').evaluate(el=>el.getBoundingClientRect().width<=window.innerWidth));
await page.evaluate(()=>closeMicTest());
await browser.close();
console.log('Web popup: record/playback meters, automatic dismissal, Escape/cancel, late-open cleanup, error, and mobile layout passed');
