// Tests the same pure validation functions embedded in the firmware's web UI.
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=readFileSync(new URL('../web/index.html',import.meta.url),'utf8');
const logic=html.split('// PROFILE_FILES_BEGIN')[1].split('// PROFILE_FILES_END')[0].split('\n').slice(1).join('\n');
const ctx=vm.createContext({TextEncoder,IN:Array(23)});
vm.runInContext(logic+';globalThis.files={validateProfileFile,makeProfileFile};',ctx);
const {validateProfileFile,makeProfileFile}=ctx.files;
export function fixture(){
  const p={name:'Test profile',mode:3,deadzone:7,deadzone_right:19,gyro_side:0,outer:95,swap:0,threshold:120,rumble:1,strength:100,
    rumble_style:1,rumble_frequency:5,impulse_flags:0,impulse_strength:100,impulse_frequency:10,mouse:0,mouse_speed:100,mouse_flags:0,map:Array(23).fill(0)};
  const settings={profile_max:8,modes:Array.from({length:6},(_,i)=>({family:i===0?'switch':'gp',outputs:['None','A','B',null]})),
    types:Array.from({length:5},(_,i)=>({name:['Pro','GameCube','Joy-Con pair','Joy-Con left','Joy-Con right'][i],numbered:i>=3,active:0,
      defaults:Array.from({length:6},()=>Array(23).fill(0)),profiles:[structuredClone(p),...Array(7).fill(null)]})),
    joycon_single:1,profile_mouse:true,profile_controls:true,last_ctrl:0,ctrl_type:0};
  settings.types[2].profiles[0].gyro_side=1;
  return settings;
}
const s=fixture(),clone=x=>JSON.parse(JSON.stringify(x));
const library=makeProfileFile(s,true,0,0);
const restored=validateProfileFile(clone(library),s);
assert.equal(restored.controllers[2].profiles[0].gyro_side,1);
assert.equal(restored.controllers[0].profiles[0].deadzone_right,19);
const single=makeProfileFile(s,false,2,0);
assert.equal(validateProfileFile(clone(single),s).type,2);
assert(!JSON.stringify(library).includes('wifi_pass'));
for(const mutate of [
 d=>d.version=99,
 d=>d.controllers[1].type=0,
 d=>d.controllers[4].profiles[0].map[0]=3,
 d=>d.controllers[4].profiles[0].deadzone_right=41,
 d=>d.controllers[4].profiles[0].gyro_side=1,
 d=>d.controllers[4].active=7,
 d=>d.controllers[4].profiles[0].map.pop(),
 d=>d.controllers[4].profiles[0].deadzone='7',
 d=>d.controllers[4].profiles[0].name='é'.repeat(10),
 d=>d.controllers[4].profiles.push(null),
]){
 const invalid=clone(library);mutate(invalid);
 const before=JSON.stringify(s);assert.throws(()=>validateProfileFile(invalid,s));assert.equal(JSON.stringify(s),before);
}
const holes=clone(library);holes.controllers[4].profiles[7]=clone(holes.controllers[4].profiles[0]);
assert(validateProfileFile(holes,s).controllers[4].profiles[7]);
const old=clone(single);delete old.profile.deadzone_right;delete old.profile.gyro_side;
assert.equal(validateProfileFile(old,s).profile.deadzone_right,7);
const zero=clone(single);zero.profile.deadzone_right=0;assert.equal(validateProfileFile(zero,s).profile.deadzone_right,0);
console.log('Profile files: single/library round trips, old-field defaults, independent options and atomic rejection passed');
