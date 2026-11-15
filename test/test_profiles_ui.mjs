// PLAYWRIGHT_MODULE and PLAYWRIGHT_BROWSERS_PATH can point to temporary installs.
import {readFile} from 'node:fs/promises';
import assert from 'node:assert/strict';
import {fixture} from './test_profile_files.mjs';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE||'playwright');
const browser=await chromium.launch({headless:true});
const page=await browser.newPage({viewport:{width:1400,height:1000}});
const errors=[];page.on('pageerror',e=>errors.push(e.message));
page.on('dialog',d=>d.accept());
await page.goto(new URL('../web/index.html',import.meta.url).href);
await page.evaluate(s=>{
  S=s;S.boot_mode=3;S.boot_mouse=0;
  S.types.forEach((T,t)=>CUR[t]=T.active);
  MAP_CTRL=0;setOnline(true);$('jc-single').checked=true;
  showSection('ctrl');showSub('sticks');showTab(0);renderProfile();setDirty(false);
},fixture());
assert.equal(await page.locator('#pf-deadzone').inputValue(),'7');
assert.equal(await page.locator('#pf-deadzone-right').inputValue(),'19');
assert(await page.locator('#pf-gyro-side-row').isHidden());
await page.locator('#pf-deadzone-right').fill('23');
await page.locator('#pf-deadzone-right').dispatchEvent('input');
const downloadPromise=page.waitForEvent('download');
await page.getByRole('button',{name:'Export profile',exact:true}).click();
const download=await downloadPromise;
const exported=JSON.parse(await readFile(await download.path(),'utf8'));
assert.equal(exported.profile.deadzone_right,23);
assert.equal(exported.profile.deadzone,7);
exported.profile.name='Imported';exported.profile.deadzone_right=0;
await page.locator('#profile-file').setInputFiles({name:'profile.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(exported))});
await page.waitForFunction(()=>document.querySelector('#pf-name').value==='Imported');
assert.equal(await page.locator('#pf-deadzone-right').inputValue(),'0');
assert(await page.evaluate(()=>DIRTY));
// Invalid documents never mutate profiles, even when the last entry is invalid.
const before=await page.evaluate(()=>JSON.stringify(S.types));
await page.locator('#profile-file').setInputFiles({name:'invalid.json',mimeType:'application/json',buffer:Buffer.from('{broken')});
await page.waitForFunction(()=>document.querySelector('#toast').textContent.startsWith('Import failed:'));
assert.equal(await page.evaluate(()=>JSON.stringify(S.types)),before);
// Pair-specific gyro choice is stashed alongside both deadzones.
await page.evaluate(()=>{stashProfile();MAP_CTRL=2;showTab(2);renderProfile();});
assert(await page.locator('#pf-gyro-side-row').isVisible());
assert.equal(await page.locator('#pf-gyro-side').inputValue(),'1');
await page.locator('#pf-gyro-side').selectOption('0');
await page.evaluate(()=>stashProfile());
assert.equal(await page.evaluate(()=>S.types[2].profiles[0].gyro_side),0);
const libraryPromise=page.waitForEvent('download');
await page.getByRole('button',{name:'Export all profiles',exact:true}).click();
const libraryDownload=await libraryPromise;
const library=JSON.parse(await readFile(await libraryDownload.path(),'utf8'));
assert.equal(library.controllers.length,5);
library.controllers[2].profiles[0].gyro_side=1;
await page.locator('#profile-file').setInputFiles({name:'library.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify(library))});
await page.waitForFunction(()=>document.querySelector('#pf-gyro-side').value==='1');
// Existing Save serializes the extended fields without committing on import.
await page.evaluate(()=>{
  api=async(path,opts)=>{window.savedBody=opts.body;return {ok:false}};
  return save();
});
const saved=await page.evaluate(()=>savedBody);
assert(saved.includes(',d0,0,Imported'));
assert(saved.includes(',d19,1,Test%20profile'));
// Gyro mouse (firmware that has it): the pair's options and combinations,
// stashed and saved as the g-group (Joy-Con types only).
await page.evaluate(()=>{S.profile_gyro_mouse=true;stashProfile();MAP_CTRL=2;showTab(2);showSub('mouse');renderProfile();});
assert(await page.locator('#gmouse-update').isHidden());
assert(await page.locator('#pf-gmouse').isEnabled());
assert(await page.locator('#pf-gmouse-speed').isDisabled());
await page.locator('#pf-gmouse').check();
assert(await page.locator('#pf-gmouse-speed').isEnabled());
assert(await page.locator('#pf-gmouse-toggle input[data-in="20"]').isChecked());   // C
assert(await page.locator('#pf-gmouse-toggle input[data-in="13"]').isChecked());   // Capture
assert.equal(await page.locator('#pf-gmouse-toggle input').count(),23);
assert((await page.locator('#gmouse-hint').textContent()).includes('left Joy-Con'));
await page.locator('#pf-gmouse-toggle input[data-in="20"]').uncheck();
await page.locator('#pf-gmouse-recenter input[data-in="6"]').check();   // ZL
await page.locator('#pf-gmouse-inv-y').check();
await page.locator('#pf-gmouse-clicks').uncheck();
await page.evaluate(()=>stashProfile());
const gp=await page.evaluate(()=>S.types[2].profiles[0]);
assert.equal(gp.gyro_mouse,1);assert.equal(gp.gyro_mouse_toggle,1<<13);assert.equal(gp.gyro_mouse_recenter,1<<6);
assert.equal(gp.gyro_mouse_flags,2|4);
await page.evaluate(()=>save());
const gyroBody=await page.evaluate(()=>savedBody);
assert(gyroBody.includes(`,g1,100,6,${1<<13},${1<<6},Test%20profile`));
assert(!/prof_0_0=[^&]*,g\d/.test(gyroBody));   // not on the Pro Controller
// A single (R): only its own buttons.
await page.evaluate(()=>{stashProfile();MAP_CTRL=4;showTab(4);renderProfile();});
assert.equal(await page.locator('#pf-gmouse-toggle input').count(),12);
assert(await page.locator('#pf-gmouse-toggle input[data-in="9"]').isChecked());   // Plus
await page.evaluate(()=>{S.profile_gyro_mouse=false;renderProfile();});
assert(await page.locator('#gmouse-update').isVisible());
assert(await page.locator('#pf-gmouse').isDisabled());
await page.evaluate(()=>{save();S.profile_gyro_mouse=true;showSub('sticks');});
assert(!/,g\d+,\d+,/.test(await page.evaluate(()=>savedBody)));
// Correct controls on single Joy-Con profiles, including a narrow viewport.
await page.evaluate(()=>{stashProfile();MAP_CTRL=3;showTab(3);renderProfile();});
assert(await page.locator('#pf-right-deadzone-row').isHidden());
assert(await page.locator('#pf-gyro-side-row').isHidden());
await page.setViewportSize({width:390,height:844});
await page.screenshot({path:'/tmp/switch2-profiles-mobile.png',fullPage:true});
assert(await page.evaluate(()=>document.documentElement.scrollWidth<=window.innerWidth));
// Old firmware does not understand the d-group: never put it in a save.
await page.evaluate(()=>{S.profile_controls=false;renderProfile();return save()});
assert(!await page.evaluate(()=>savedBody.includes(',d')));
assert(await page.locator('#pf-deadzone-right').isDisabled());
// The complete 40-profile library, including percent-encoded names, fits the
// transport limit. This catches accidental regressions to the old 8 KiB cap.
await page.evaluate(()=>{
  S.profile_controls=true;
  S.modes.forEach(m=>m.outputs=Array.from({length:32},(_,i)=>'Output '+i));
  S.profile_gyro_mouse=true;
  S.types.forEach((T,t)=>{const p=T.profiles[0];T.profiles=Array.from({length:8},()=>({...p,name:'%'.repeat(19),deadzone:40,outer:100,strength:200,rumble_frequency:10,impulse_strength:200,map:Array(23).fill(28),
    ...(t>=2?{gyro_mouse:1,gyro_mouse_speed:250,gyro_mouse_flags:7,gyro_mouse_toggle:(1<<23)-1,gyro_mouse_recenter:(1<<23)-1}:{})}))});
  $('wifi_pass').value='%'.repeat(63);
  const ssid=document.querySelector('[data-k=wifi_ssid]');if(ssid)ssid.value='%'.repeat(32);
  renderProfile();return save();
});
const fullBody=await page.evaluate(()=>savedBody);
assert(fullBody.length+1024<12288, `Full profile body exceeds the transport limit (${fullBody.length} bytes)`);
assert.deepEqual(errors,[]);
await browser.close();
console.log('Profiles web UI: independent controls, downloads, imports, validation, Save serialization and mobile layout passed');
