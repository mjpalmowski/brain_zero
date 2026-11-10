#pragma once
#include <pgmspace.h>
#include "site_nav.h"
static const char INVERTER_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Inverter controls · brain_zero</title><link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg"><link rel="stylesheet" href="/brand/brand.css"><style>
:root{color-scheme:light dark;--bg:#f5f6f8;--card:#fff;--ink:#1d2430;--line:#e2e5ea;--muted:#687180;--a:#2463eb}
@media(prefers-color-scheme:dark){:root{--bg:#101318;--card:#191e25;--ink:#e6e9ee;--line:#29303a;--muted:#98a2ae;--a:#6ea8fe}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,sans-serif}main,header{max-width:850px;margin:auto;padding:16px}header{display:flex;flex-wrap:wrap;align-items:baseline;gap:8px 18px}h1{font-size:20px;margin:0}h2{font-size:16px;margin:0 0 12px}a{color:var(--a)}section{border:1px solid var(--line);border-radius:10px;background:var(--card);padding:18px;margin-bottom:16px}.row{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:14px 0}.row label{flex:1 1 220px}select,input,button{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:7px 10px}input{width:100px}button{cursor:pointer}button:disabled{opacity:.45;cursor:default}.muted{font-size:13px;color:var(--muted)}#feedback{min-height:1.5em;overflow-wrap:anywhere}output{font-weight:600}.error{color:#d34836}
section summary{cursor:pointer;font-size:16px;font-weight:600}section details[open]>summary{margin-bottom:12px}footer{color:var(--muted);font-size:13px}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(
<header><h1>Inverter controls</h1></header>
<main><p id="availability" class="muted">Connecting…</p>
<section><h2>Operating controls</h2>
<form id="mode-form"><div class="row"><label for="mode">Operating mode<br><output id="reported-mode">Unknown</output></label><select id="mode"><option value="On">On</option><option value="Charger only">Charger only</option><option value="Inverter only">Inverter only</option><option value="Off">Off</option></select><button id="mode-send">Apply mode</button></div></form>
<form id="current-form"><div class="row"><label for="current">AC input current limit<br><output id="reported-current">Unknown</output></label><input id="current" type="number" min="0" max="50" step="0.1" required><span>A</span><button id="current-send">Apply limit</button></div><p id="range" class="muted"></p></form>
<p id="feedback" role="status" aria-live="polite" hidden></p></section>
<section><h2>Ignore AC input</h2><p class="muted">Timed battery operation with ESS active. Charging windows, low battery and stale data reconnect automatically. A higher ESS reserve takes precedence.</p><p id="ac-status" role="status">Waiting for readback…</p><div class="row"><button id="ac-ignore" type="button" disabled>Ignore AC input</button><button id="ac-reconnect" type="button" disabled>Reconnect grid</button></div>
<form id="ac-settings"><div class="row"><label>Reconnect SOC (%)<input id="ac-soc" type="number" min="0" max="95" step="1" required></label><label>Backup voltage (V)<input id="ac-voltage" type="number" min="48" max="56" step="0.1" required></label><label>Maximum duration (min)<input id="ac-minutes" type="number" min="1" max="240" step="1" required></label><button id="ac-save">Save protection</button></div></form><p id="ac-feedback" role="status"></p></section>
<section><h2>Charger</h2><p>Reported charge stage: <output id="charge">Unknown</output></p>
<h3>Stored charger settings <output id="charger-profile" class="muted" hidden></output></h3>
<p class="muted">These values are saved in the inverter. Actual charging can be limited by ESS, a BMS, temperature or the AC supply. Readback confirms the stored settings, not the instantaneous battery voltage or current.</p>
<form id="charger-form">
<div class="row"><label for="charge_current">Charge current<br><output id="read-charge_current">Unknown</output></label><input id="charge_current" type="number" step="1" disabled required><span>A DC</span><span id="range-charge_current" class="muted"></span></div>
<div class="row"><label for="absorption_voltage">Absorption voltage<br><output id="read-absorption_voltage">Unknown</output></label><input id="absorption_voltage" type="number" step="0.01" disabled required><span>V</span><span id="range-absorption_voltage" class="muted"></span></div>
<div class="row"><label for="float_voltage">Float voltage<br><output id="read-float_voltage">Unknown</output></label><input id="float_voltage" type="number" step="0.01" disabled required><span>V</span><span id="range-float_voltage" class="muted"></span></div>
<div class="row"><button id="charger-save" disabled>Save charger settings</button><button id="charger-read" type="button">Read from inverter</button></div>
</form><p id="charger-feedback" role="status" aria-live="polite">Reading charger settings…</p><p id="charger-availability" class="muted"></p>
<hr style="border:0;border-top:1px solid var(--line);margin:24px 0"><details><summary>Equalization</summary>
<div class="row"><button id="equalize" type="button">Start equalization</button><button id="stop_equalize_absorption" type="button">Interrupt → absorption</button><button id="stop_equalize_float" type="button">Interrupt → float</button></div>
<p class="muted">Equalization is available when the charger is ready and outside bulk. Use it only when the battery manufacturer permits equalization. In an ESS system, solar chargers can add excessive current at the raised voltage.</p><p id="equalize-feedback" role="status" aria-live="polite" hidden></p></details></section>
<section><h2>PowerAssist settings</h2><p class="muted">Stored inverter settings. Reads are explicit; saving changes only the supported PowerAssist flag or boost factor and checks inverter readback.</p>
<form id="assist-form"><div class="row"><label><input id="power-assist" type="checkbox" style="width:18px" disabled>PowerAssist enabled<br><output id="assist-enabled-read">Unknown</output></label></div>
<div class="row"><label for="assist-boost">Assist current boost factor<br><output id="assist-boost-read">Unknown</output></label><input id="assist-boost" type="number" min="0.25" max="3.5" step="0.0625" disabled required></div>
<div class="row"><button id="assist-save" disabled>Save PowerAssist settings</button><button id="assist-read" type="button">Read from inverter</button></div></form><p id="assist-feedback" role="status" aria-live="polite">Read the settings to edit them.</p></section>
<section><h2>Temporarily disable AC PV</h2><p class="muted">Confirmed Ignore AC operation only. Raises AC-out to about 53 Hz, affecting every connected appliance. Restores automatically on timeout, reconnection, charging or a protection event. Readback confirms the inverter flag; solar restart may take longer.</p><p id="pv-status" role="status">Waiting for readback…</p><div class="row"><button id="pv-disable" disabled>Disable AC PV</button><button id="pv-restore" disabled>Restore PV now</button></div><form id="pv-settings"><div class="row"><label>Restore after (seconds)<input id="pv-seconds" type="number" min="10" max="300" step="1" required></label><button id="pv-save" disabled>Save duration</button></div></form><p id="pv-feedback" role="status"></p></section>
<section><details><summary>VE.Bus maintenance</summary><div class="row"><button id="redetect_vebus" type="button">Redetect VE.Bus system</button><button id="restart_vebus" type="button">Restart VE.Bus system</button><button id="restart-controller" type="button">Restart brain_zero controller</button></div><p class="muted">Restarting VE.Bus interrupts the inverter's AC output. Restarting brain_zero briefly pauses local control. Redetection runs device discovery again.</p><p id="maintenance-feedback" role="status" aria-live="polite" hidden></p><p id="restart-feedback" role="status" aria-live="polite" hidden></p></details></section>
<footer>For VE.Bus health, fault reports and detailed readback, open <a href="/diagnostics">Diagnostics</a>.</footer>
</main><script>
const $=id=>document.getElementById(id);let state, busy=false, refreshing=false, first=true, feedbackUntil=0, feedbackId='feedback';
const t=text=>typeof text==='string'?(window.bzText?.(text)??text):text;
const fmt=(template,values)=>window.bzFormat?.(template,values)??template.replace(/\{(\w+)\}/g,(match,key)=>values[key]??match);
function feedback(id,message,failed=false){const el=$(id);el.hidden=!message;el.textContent=t(message);el.className=failed?'error':''}
const chargerKeys=['charge_current','absorption_voltage','float_voltage'];let chargerState,chargerTarget='',chargerBase='',chargerDirty=false,chargerSending=false,chargerAwaiting=false,chargerPolling=false,chargerFeedbackUntil=0;
const chargerNames={charge_current:'Charge current',absorption_voltage:'Absorption voltage',float_voltage:'Float voltage'};
function chargerDisplay(j){chargerState=j;const active=['reading','writing','verifying'].includes(j.state);
if(chargerAwaiting&&!active&&(j.state==='done'||j.state==='failed')){chargerAwaiting=false;if(j.state==='done')chargerDirty=false}
const target=j.nominal_voltage?`${j.product_id}/${j.firmware}/${j.nominal_voltage}`:'';
const valid=!!target&&j.parameters.length===chargerKeys.length&&j.parameters.every(p=>p.value!==null&&p.supported);
if(target!==chargerTarget||!valid){chargerTarget=target;chargerBase='';chargerDirty=false}
const profile=$('charger-profile');profile.hidden=!target;profile.textContent=target?'· '+fmt('{value} {unit}',{value:j.nominal_voltage,unit:'V'}):'';
if(valid&&!chargerDirty&&!active){for(const p of j.parameters)$(p.key).value=p.value.toFixed(p.key==='charge_current'?0:2);chargerBase=j.parameters.map(p=>p.raw).join(',')}
for(const p of j.parameters){const current=p.key==='charge_current';
$('read-'+p.key).textContent=p.value===null?'Unknown':fmt(p.verified?'{value} {unit} · readback verified':'{value} {unit}',{value:p.value.toFixed(current?0:2),unit:current?t('A DC'):'V'});
if(p.supported){$(p.key).min=p.minimum;$(p.key).max=p.maximum;$(p.key).step=p.step;$('range-'+p.key).textContent=fmt('{minimum}–{maximum} {unit}',{minimum:p.minimum,maximum:p.maximum,unit:current?'A':'V'})}else $('range-'+p.key).textContent='Range unavailable';$(p.key).disabled=chargerSending||active||!j.available||!valid}
$('charger-save').disabled=busy||chargerSending||active||!j.available||!valid||!chargerBase;$('charger-read').disabled=busy||chargerSending||active;
$('charger-availability').textContent=t(j.reason);if(Date.now()>chargerFeedbackUntil){$('charger-feedback').textContent=j.saving&&j.changes?fmt('{message} ({confirmed}/{total} changes read back)',{message:t(j.message),confirmed:j.confirmed,total:j.changes}):t(j.message);$('charger-feedback').className=j.state==='failed'?'error':''}}
async function pollCharger(){if(chargerPolling)return;chargerPolling=true;try{const r=await fetch('/api/charger',{cache:'no-store'});if(!r.ok)throw Error('Charger settings unavailable');chargerDisplay(await r.json())}catch(e){$('charger-availability').textContent=e.message;$('charger-save').disabled=true;$('charger-read').disabled=true;for(const k of chargerKeys)$(k).disabled=true}finally{chargerPolling=false}}
async function postCharger(path,body){chargerSending=true;$('charger-save').disabled=$('charger-read').disabled=true;try{const r=await fetch(path,{method:'POST',body});const j=await r.json();$('charger-feedback').textContent=j.message||j.error;$('charger-feedback').className=r.ok?'':'error';chargerFeedbackUntil=Date.now()+1500;if(r.ok)chargerAwaiting=true}catch(e){$('charger-feedback').textContent='Connection lost; read from inverter before retrying';chargerFeedbackUntil=Date.now()+5000}finally{chargerSending=false;await pollCharger()}}
for(const k of chargerKeys)$(k).addEventListener('input',()=>chargerDirty=true);
$('charger-form').onsubmit=e=>{e.preventDefault();if(!chargerState?.available||!chargerBase)return;const a=Number($('absorption_voltage').value),f=Number($('float_voltage').value);if(f>a){$('charger-feedback').textContent='Float voltage must not exceed absorption voltage';chargerFeedbackUntil=Date.now()+4000;return}
const changes=chargerState.parameters.filter(p=>Number($(p.key).value)!==p.value).map(p=>fmt('{setting} · {before} → {after} {unit}',{setting:t(chargerNames[p.key]||p.key),before:p.value,after:$(p.key).value,unit:p.key==='charge_current'?'A':'V'}));
if(changes.length&&!confirm(fmt('Save these charger settings in the inverter?\n\n{changes}\n\nUse values approved for your battery. Each change will be read back.',{changes:changes.join('\n')})))return;
const body=new URLSearchParams({base:chargerBase});for(const k of chargerKeys)body.set(k,$(k).value);postCharger('/api/charger',body)};
$('charger-read').onclick=()=>{chargerDirty=false;postCharger('/api/charger/read')};
async function refresh(){if(refreshing)return;refreshing=true;try{const r=await fetch('/api/inverter',{cache:'no-store'});if(!r.ok)throw Error('Inverter status unavailable');state=await r.json();acDisplay(state.ac_input);pvDisplay(state.pv_disable);$('availability').textContent=t(state.reason);$('reported-mode').textContent=t(state.mode_name);$('reported-current').textContent=state.current_a<0?'Unknown':fmt('{value} A',{value:state.current_a.toFixed(1)});$('charge').textContent=t(state.charge_name);
if(first){if(state.mode>=4)$('mode').value=state.mode_name;if(state.current_a>=0){$('current').value=state.current_a;first=false}}
$('range').textContent=state.minimum_a<0?'Waiting for the inverter’s current-limit range.':fmt(state.minimum_a>0?
'Reported range {minimum}–{maximum} A or 0 A. Zero changes grid/PowerAssist behaviour.':
'Reported range {minimum}–{maximum} A. Zero changes grid/PowerAssist behaviour.',{minimum:state.minimum_a.toFixed(1),maximum:state.maximum_a.toFixed(1)});
$('current').max=state.maximum_a>=0?state.maximum_a:50;
$('mode-send').disabled=busy||!state.mode_adjustable;$('current-send').disabled=busy||!state.current_adjustable;$('equalize').disabled=busy||!state.can_equalize;
for(const k of ['stop_equalize_absorption','stop_equalize_float'])$(k).disabled=busy||!state.can_interrupt;
for(const k of ['restart_vebus','redetect_vebus'])$(k).disabled=busy||!state.available;
$('restart-controller').disabled=busy;
if(Date.now()>feedbackUntil)feedback(feedbackId,state.command.state==='idle'?'':state.command.message,state.command.state==='failed');
}catch(e){$('availability').textContent=fmt('Connection lost. {reason}',{reason:t(e.message)});document.querySelectorAll('button').forEach(b=>b.disabled=true)}finally{refreshing=false}}
async function command(key,value,target='feedback'){busy=true;feedbackId=target;document.querySelectorAll('button').forEach(b=>b.disabled=true);try{const r=await fetch('/api/inverter',{method:'POST',body:new URLSearchParams({[key]:value})});const j=await r.json();feedback(target,j.message||j.error,!r.ok);feedbackUntil=Date.now()+2000}catch(e){feedback(target,e.message,true);feedbackUntil=Date.now()+5000}finally{busy=false;await refresh()}}
$('mode-form').onsubmit=e=>{e.preventDefault();const m=$('mode').value;if(m==='Off'&&!confirm(t('Turn off the inverter/charger? AC-out loads may lose power.')))return;if(m==='Inverter only'&&!confirm(t('Switch to inverter only? The battery will supply AC-out loads.')))return;command('inverter_mode',m)};
$('current-form').onsubmit=e=>{e.preventDefault();command('ac_input_limit',$('current').value)};
const confirmations={equalize:'Start equalization? This raises battery charging voltage. Continue only if the battery permits it. Solar chargers in an ESS system can add excessive current.',restart_vebus:'Restart the VE.Bus system? AC output will be interrupted.',redetect_vebus:'Redetect the VE.Bus system? Bus control will pause during discovery.'};
for(const k of ['equalize','stop_equalize_absorption','stop_equalize_float','restart_vebus','redetect_vebus'])$(k).onclick=()=>{if(confirmations[k]&&!confirm(t(confirmations[k])))return;command(k,'PRESS',k.endsWith('_vebus')?'maintenance-feedback':'equalize-feedback')};
let assistData,assistBusy=false,assistDirty=false;
function assistDisplay(j){assistData=j;const p=j.parameters||[],known=p.length===2&&p.every(x=>x.supported&&x.value!==null);
if(known&&!assistDirty){$('power-assist').checked=!!p[0].value;$('assist-boost').value=p[1].value}
$('assist-enabled-read').textContent=known?(p[0].value?'Enabled':'Disabled'):'Unknown';$('assist-boost-read').textContent=known?p[1].value:'Unknown';
for(const k of ['power-assist','assist-boost','assist-save'])$(k).disabled=assistBusy||!known||!j.available;
$('assist-read').disabled=assistBusy;if(!assistBusy)$('assist-feedback').textContent=j.message;}
async function assistCached(){const r=await fetch('/api/assist',{cache:'no-store'});if(!r.ok)throw Error('PowerAssist settings unavailable');return r.json()}
async function assistAction(path,body){if(assistBusy)return;let assistError='';assistBusy=true;$('assist-feedback').textContent='Waiting for inverter readback…';if(assistData)assistDisplay(assistData);
try{const r=await fetch(path,{method:'POST',body});const j=await r.json();if(!r.ok)throw Error(j.error||'Request refused');
let done=false;for(let i=0;i<20;i++){const status=await assistCached();assistDisplay(status);if(!['reading','writing','verifying'].includes(status.state)){if(status.state!=='done')throw Error(status.message);assistDirty=false;done=true;break}await new Promise(resolve=>setTimeout(resolve,1000))}if(!done)throw Error('Readback timed out; request was not repeated')}
catch(e){assistError=e.message}finally{assistBusy=false;if(assistData)assistDisplay(assistData);if(assistError)$('assist-feedback').textContent=assistError}}
for(const k of ['power-assist','assist-boost'])$(k).oninput=()=>assistDirty=true;
$('assist-read').onclick=()=>{if(!assistDirty||confirm(t('Discard PowerAssist edits and read stored settings?'))){assistDirty=false;assistAction('/api/assist/read')}};
$('assist-form').onsubmit=e=>{e.preventDefault();if(!assistData||assistBusy)return;const p=assistData.parameters;if(!confirm(t('Save PowerAssist settings and verify inverter readback?')))return;assistAction('/api/assist',new URLSearchParams({power_assist:$('power-assist').checked?'1':'0',boost_factor:$('assist-boost').value,base:p.map(x=>x.raw).join(',')}))};
$('restart-controller').onclick=async()=>{if(!confirm(t('Restart brain_zero? Local control will pause briefly.')))return;try{const r=await fetch('/api/system',{method:'POST',body:new URLSearchParams({reboot:'PRESS'})});const j=await r.json();feedback('restart-feedback',j.message||j.error,!r.ok)}catch(e){feedback('restart-feedback','Restart outcome unknown. Refresh before retrying.',true)}};
let acData,acDirty=false,acBusy=false,acBase;
function acDisplay(j){if(!j)return;acData=j;
const phase={connected:'Grid connected',islanded:'Running from battery',disconnecting:'Switching to battery',reconnecting:'Reconnecting to grid',unavailable:'AC input control unavailable',unsupported:'AC input control unsupported',initializing:'Waiting for grid readback'};
const battery=j.soc==null?t('Battery SOC unavailable'):fmt('Battery {soc}% · Reconnect at {floor}%',{soc:j.soc,floor:j.effective_soc_floor});
$('ac-status').textContent=[t(phase[j.phase]),t(j.reason),battery,j.requested?fmt('{seconds}s remaining',{seconds:j.remaining_s}):''].filter(Boolean).join(' · ');
if(!acDirty){$('ac-soc').value=j.settings.reconnect_soc;$('ac-voltage').value=j.settings.reconnect_v;$('ac-minutes').value=j.settings.minutes;acBase=j.revision}
$('ac-ignore').disabled=acBusy||acDirty||!j.can_enable;$('ac-reconnect').disabled=acBusy;$('ac-save').disabled=acBusy;}
for(const k of ['ac-soc','ac-voltage','ac-minutes'])$(k).oninput=()=>acDirty=true;
async function acPost(settings,fields){if(acBusy||!acData)return;acBusy=true;acDisplay(acData);try{const r=await fetch('/api/ac-input'+(settings?'/settings':''),{method:'POST',body:new URLSearchParams({...fields,base:settings?acBase:acData.revision})});const j=await r.json();$('ac-feedback').textContent=j.message||j.error;if(r.ok&&settings)acDirty=false;}catch(e){$('ac-feedback').textContent='Outcome unknown; check readback before retrying'}finally{acBusy=false;await refresh();acDisplay(state.ac_input)}}
$('ac-ignore').onclick=()=>{if(confirm(t('Use the battery for AC-out loads until the protection floor, a charging window or the duration limit?')))acPost(false,{ignore:1})};
$('ac-reconnect').onclick=()=>acPost(false,{ignore:0});
$('ac-settings').onsubmit=e=>{e.preventDefault();acPost(true,{reconnect_soc:$('ac-soc').value,reconnect_v:$('ac-voltage').value,minutes:$('ac-minutes').value})};
let pvData,pvBusy=false,pvDirty=false,pvBase;
function pvDisplay(j){if(!j)return;pvData=j;const phase={disabled:'PV restriction confirmed',enabled:'PV restriction cleared',disabling:'Disabling PV',restoring:'Restoring PV',unavailable:'PV control unavailable',initializing:'Waiting for PV readback'};$('pv-status').textContent=[t(phase[j.phase]),t(j.reason),j.requested?fmt('{seconds}s remaining',{seconds:j.remaining_s}):!j.can_enable?t(j.blocked):''].filter(Boolean).join(' · ');if(!pvDirty){$('pv-seconds').value=j.settings.seconds;pvBase=j.revision}$('pv-disable').disabled=pvBusy||pvDirty||!j.can_enable;$('pv-restore').disabled=pvBusy;$('pv-save').disabled=pvBusy||!pvDirty;}
$('pv-seconds').oninput=()=>{pvDirty=true;if(pvData)pvDisplay(pvData)};
async function pvPost(settings,fields){if(pvBusy||!pvData)return;pvBusy=true;pvDisplay(pvData);try{const r=await fetch('/api/pv-disable'+(settings?'/settings':''),{method:'POST',body:new URLSearchParams({...fields,base:settings?pvBase:pvData.revision})});const j=await r.json();$('pv-feedback').textContent=j.message||j.error;if(r.ok&&settings)pvDirty=false}catch(e){$('pv-feedback').textContent='Outcome unknown; check readback before retrying'}finally{pvBusy=false;await refresh();if(state)pvDisplay(state.pv_disable)}}
$('pv-disable').onclick=()=>{if(confirm(fmt('Raise AC-out to about 53 Hz for up to {seconds} seconds? This stops compatible solar and affects connected appliances.',{seconds:pvData.settings.seconds})))pvPost(false,{disable:1})};
$('pv-restore').onclick=()=>pvPost(false,{disable:0});$('pv-settings').onsubmit=e=>{e.preventDefault();pvPost(true,{seconds:$('pv-seconds').value})};
document.addEventListener('DOMContentLoaded',()=>{
assistCached().then(assistDisplay).catch(e=>$('assist-feedback').textContent=t(e.message));
refresh();pollCharger();setInterval(()=>{if(!busy)refresh();pollCharger()},1000);
},{once:true});
</script></body></html>)HTML";
