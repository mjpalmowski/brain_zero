import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source=fs.readFileSync(new URL('../src/firmware_page.h',import.meta.url),'utf8');
const script=source.match(/<script>([\s\S]*?)<\/script>/)[1];
const nodes=new Map(),calls=[];
function element(id){
    if(!nodes.has(id))nodes.set(id,{id,files:[],disabled:false,hidden:false,textContent:'',listeners:{},
        addEventListener(k,f){this.listeners[k]=f},removeAttribute(k){delete this[k]}});
    return nodes.get(id);
}
let fw,release,post;
const normal={phase:'normal',pending:false,active:false,remaining_s:0,message:''};
const maintenance=(phase,pending=false)=>({phase,pending,active:!pending,remaining_s:300,message:pending?'Preparing firmware update.':'Bluetooth battery monitoring is paused for firmware update.'});
function state(boot,m=normal,busy=false){return {ok:true,boot_id:boot,build:'test',slot:'app0',max_bytes:3342336,staged:false,trial:false,pending:m.pending,release_busy:busy,token:'local-owner-nonce',safe:true,bluetooth_allocated:!m.active,maintenance:m};}
function job(status,m=normal){return {ok:true,status,maintenance:m,message:'test',size:0,tag:'',can_stage:false};}
fw=state(1);release=job('idle');
const context=vm.createContext({console,window:{},document:{hidden:false,documentElement:{lang:'en'},getElementById:element,addEventListener(){}},
    AbortSignal,URLSearchParams,Date,setInterval(){},
    fetch:async(url,options={})=>{
        calls.push([url,options.method||'GET']);
        const value=options.method==='POST'?post:url==='/api/firmware'?fw:release;
        if(value instanceof Error)throw value;
        return {ok:true,status:options.method==='POST'?202:200,json:async()=>structuredClone(value)};
    }});
vm.runInContext(script,context);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
await flush();
assert.equal(element('release-check').disabled,false);
assert.equal(element('battery-update-note').hidden,false);

// An accepted check restarts the controller. Keep polling the release job even
// after its old boot identity and ordinary busy flag disappear.
post=job('idle',maintenance('restart_pending',true));
fw=state(1,maintenance('restart_pending',true));
release=post;
await element('release-check').listeners.click();await flush();
assert.equal(vm.runInContext('watchRelease',context),true);
assert.equal(element('release-check').disabled,true);
fw=new Error('restarting');await vm.runInContext('refresh()',context);
assert.equal(vm.runInContext('watchRelease',context),true);
fw=state(2,maintenance('waiting'));release=job('idle',maintenance('waiting'));
await vm.runInContext('refresh()',context);
assert.equal(vm.runInContext('restart',context),null);
assert.equal(element('release-status').textContent,release.maintenance.message);
assert.equal(element('release-cancel').hidden,false);

fw=state(2,maintenance('checking'),true);release=job('checking',maintenance('checking'));
await vm.runInContext('refresh()',context);
assert.equal(element('release-check').disabled,true);
assert.equal(element('release-cancel').disabled,false);
const count=calls.filter(([p])=>p==='/api/firmware/release').length;
assert.ok(count>=3,'Release polling survives the maintenance reboot');

// Current/error completion resumes BLE and ends polling without retaining an
// obsolete offer from the previous boot.
fw=state(2);release=job('current');await vm.runInContext('refresh()',context);
assert.equal(vm.runInContext('watchRelease',context),false);
assert.equal(element('release-cancel').hidden,true);
assert.equal(element('release-check').disabled,false);

// Cancellation remains available during TLS. It follows cleanup until the
// firmware reports normal operation again.
fw=state(2,maintenance('checking'),true);release=job('checking',maintenance('checking'));
await vm.runInContext('refresh()',context);
post=job('checking',maintenance('resuming'));
await element('release-cancel').listeners.click();await flush();
assert.ok(calls.some(([p,m])=>p==='/api/firmware/release/cancel'&&m==='POST'));
fw=state(2);release=job('error');await vm.runInContext('refresh()',context);
assert.equal(element('release-check').disabled,false);
assert.equal(element('release-cancel').hidden,true);
console.log('Firmware page: maintenance reboot, job polling, completion and cancellation passed.');
