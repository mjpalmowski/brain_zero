// Cached diagnostics UI; only an explicit button queues bounded fault-report reads.
#pragma once
#include <pgmspace.h>
#include "site_nav.h"
static const char DIAGNOSTICS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'">
<title>Diagnostics · brain_zero</title><link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg"><link rel="stylesheet" href="/brand/brand.css">
<style>
:root{--bg:#f4f7f7;--card:#fff;--ink:#193239;--muted:#657a7e;--line:#dce6e5;--accent:#087b6b;--soft:#edf7f3;--warn:#95671d;--bad:#b54037}*{box-sizing:border-box}
@media(prefers-color-scheme:dark){:root{--bg:#101a1e;--card:#17272d;--ink:#e2efed;--muted:#9bb4b5;--line:#2e444a;--accent:#5fd6b7;--soft:#203a35;--warn:#f1c36c;--bad:#ff978d}}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}main,footer{max-width:1180px;margin:auto;padding:20px 24px}a{color:var(--accent)}main{padding-top:28px}h1{font-size:29px;letter-spacing:-.035em;line-height:1.2;margin:4px 0 9px}h2{font-size:17px;letter-spacing:-.02em;margin:0 0 15px}p{margin:0 0 14px}.eyebrow{font-size:10px;letter-spacing:.17em;color:var(--accent);font-weight:750}.muted,small{color:var(--muted)}small{font-size:11px;display:block}.heading{display:flex;align-items:center;justify-content:space-between;gap:20px;margin-bottom:20px}.button{padding:9px 14px;border:1px solid var(--line);border-radius:8px;color:var(--ink);background:var(--card);font:inherit;font-weight:600;cursor:pointer;white-space:nowrap}.button:disabled{opacity:.45;cursor:default}.button:focus-visible,a:focus-visible{outline:3px solid var(--accent);outline-offset:4px}.banner{border:1px solid var(--line);background:var(--soft);border-radius:11px;padding:15px 18px;margin-bottom:20px;display:flex;align-items:center;gap:12px}.dot{width:9px;height:9px;border-radius:50%;background:currentColor;flex:none}.good{color:var(--accent)}.warn{color:var(--warn)}.bad{color:var(--bad)}.banner strong{display:block}.banner small{margin-top:2px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:18px}.card{border:1px solid var(--line);border-radius:14px;background:var(--card);padding:22px;min-width:0}.card.wide{grid-column:1/-1}.facts{display:grid;grid-template-columns:1fr 1fr;gap:20px}.value{font-size:22px;font-weight:650;line-height:1.3;margin:4px 0;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}.label{font-size:11px;color:var(--muted)}.metric.stale .value{color:var(--muted);opacity:.65}.rows{margin:0}.row{display:flex;justify-content:space-between;gap:22px;padding:10px 0;border-bottom:1px solid var(--line);font-size:12px}.row:last-child{border:0}.row dt{color:var(--muted)}.row dd{margin:0;text-align:right;max-width:65%;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}.rows .stack{display:block}.rows .stack dd{max-width:100%;text-align:left;margin-top:5px;color:var(--ink)}.chips{display:flex;gap:7px;flex-wrap:wrap}.chip{padding:4px 9px;border:1px solid var(--line);border-radius:30px;font-size:11px;color:var(--muted)}.chip.on{color:var(--accent);background:var(--soft)}.chip.alarm{color:var(--bad);border-color:var(--bad)}.chip.blink{border-style:dashed}.subnote{border-top:1px solid var(--line);padding-top:12px;margin-top:16px;font-size:11px;color:var(--muted)}.table-wrap{overflow:auto}table{width:100%;border-collapse:collapse;font-size:12px}th,td{text-align:left;padding:11px 9px;border-bottom:1px solid var(--line);vertical-align:top}th{font-size:10px;text-transform:uppercase;letter-spacing:.05em;color:var(--muted)}td{overflow-wrap:anywhere}td:first-child{padding-left:0}tbody tr:last-child td{border-bottom:0}.event-list{list-style:none;padding:0;margin:0;max-height:280px;overflow:auto}.event-list li{display:grid;grid-template-columns:110px 1fr;gap:16px;border-bottom:1px solid var(--line);padding:10px 0;font-size:12px}.event-list time{color:var(--muted);font-variant-numeric:tabular-nums}.history-head{display:flex;justify-content:space-between;gap:15px}.history-head p{font-size:11px;color:var(--muted)}footer{font-size:11px;color:var(--muted);padding-bottom:30px}#last-update{font-size:11px;color:var(--muted);margin:12px 0 0}.empty{color:var(--muted);padding:12px 0}.build{overflow-wrap:anywhere}
@media(max-width:720px){main,footer{padding-left:17px;padding-right:17px}.grid{grid-template-columns:1fr}.heading{align-items:flex-start}.heading h1{font-size:26px}.card{padding:18px}.facts{gap:18px 12px}.value{font-size:20px}.table-wrap{margin:0 -3px}th,td{padding:10px 5px;font-size:11px}.event-list li{grid-template-columns:86px 1fr;gap:10px}.row{gap:12px}.heading .button{font-size:12px;padding:8px}.history-head{flex-wrap:wrap}}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(

<main><div class="heading"><div><div class="eyebrow">LIVE DIAGNOSTICS · READ ONLY</div><h1>Understand your MultiPlus</h1><p class="muted">Cached readings, control decisions and connection health.</p></div><button id="download" class="button" disabled>Download snapshot</button></div>
<div id="connection" class="banner" role="status"><span class="dot warn"></span><div><strong id="connection-title">Connecting…</strong><small id="connection-detail">Waiting for the first cached snapshot.</small></div></div>
<div class="grid">
<section class="card"><h2>MultiPlus now</h2><div id="multi" class="facts"></div><div class="subnote">Power is measured at the MultiPlus terminals. An external manual bypass switch is not detected.</div></section>
<section class="card"><h2>Why it is doing this</h2><dl id="policy" class="rows"></dl><div class="subnote">The ESS decision is the controller’s intent. The measured state and readbacks show what the inverter reports.</div></section>
<section class="card wide"><h2>Commands and independent feedback</h2><div class="table-wrap"><table><thead><tr><th>Control</th><th>Requested / sent</th><th>Feedback</th><th>Age</th></tr></thead><tbody id="commands"></tbody></table></div><div class="subnote">An acknowledgement confirms receipt. It does not prove that a current limit is being enforced or that ESS is active. Measured current and power are shown above.</div></section>
<section class="card"><h2>Battery and charging policy</h2><dl id="battery" class="rows"></dl></section>
<section class="card"><h2>Front-panel indicators</h2><div id="leds" class="chips"></div><p id="led-age" class="subnote">Waiting for LED feedback.</p><div id="current-fault" class="subnote">Waiting for fault indication.</div><div class="subnote">A blinking Temperature LED is a pre-alarm. A steady Temperature LED indicates an overtemperature shutdown. <a href="https://www.victronenergy.com/media/pg/MultiPlus-II_230V/en/operation.html" target="_blank" rel="noopener noreferrer">MultiPlus-II LED indications</a></div></section>
<section class="card wide"><h2>Extended inverter status · FF</h2><div id="ff-status" class="facts"></div><p id="ff-meta" class="subnote">Waiting for an extended-status snapshot.</p><div class="subnote">The S3 refreshes this snapshot while it controls a supported MultiPlus. “Read fault report” also fetches stored relay-test details. AC input availability and relay closure are separate indications. Renewable priority shows the operating mode. The relay-test OK flag shows whether a pass was indicated.</div></section>
<section class="card wide"><div class="history-head"><div><h2>VE.Bus fault report</h2><p>Read the inverter’s diagnostic report without changing settings or clearing history.</p></div><button id="read-fault" class="button" disabled>Read fault report</button></div><p id="fault-status" role="status" aria-live="polite" class="muted">No report requested.</p><div class="facts"><div><h2>Diagnostic snapshot</h2><dl id="fault-live" class="rows"></dl></div><div><h2>Stored relay-test report</h2><dl id="fault-history" class="rows"></dl></div></div><div class="subnote">The stored report may describe an old fault. Its report ID is not a lifetime fault count, and the inverter does not provide an occurrence date here. Current LED indications, the diagnostic snapshot and stored history are separate. <a href="https://www.victronenergy.com/live/ve.bus:ve.bus_error_codes" target="_blank" rel="noopener noreferrer">VE.Bus error reference</a></div></section>
<section class="card"><h2>VE.Bus health</h2><dl id="bus" class="rows"></dl><div class="subnote">Counters are since S3 startup. Counter gaps are observations, not a count of failed commands. Recent changes appear in the event list.</div></section>
<section class="card"><h2>brain_zero health</h2><dl id="system" class="rows"></dl></section>
<section class="card" id="wifi-health"><h2>Wi-Fi recovery</h2><dl id="wifi" class="rows"></dl></section>
<section class="card" id="mqtt-health"><h2>MQTT diagnostics</h2><dl id="mqtt" class="rows"></dl></section>
<details class="card wide"><summary>Raw VE.Bus readings and bench test</summary><dl id="raw-bus" class="rows"></dl></details>
<section class="card wide"><div class="history-head"><div><h2>Events observed here</h2><p>Last 60 changes sampled while this page is open. Kept in this browser’s memory.</p></div></div><ul id="events" class="event-list"><li class="empty">Waiting for a snapshot.</li></ul></section>
</div><p id="last-update"></p></main>
<footer>Automatic refreshes only read cached diagnostics, every two seconds while visible. “Read fault report” makes up to four read-only inverter queries once; it never resets or clears a report. Downloaded snapshots omit IP addresses, broker details, device identifiers and MQTT commands.</footer>
<script>
'use strict';
const $=id=>document.getElementById(id),esc=x=>String(x??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const tr=text=>globalThis.window?.bzText?.(text)??text;
const format=(text,values)=>globalThis.window?.bzFormat?.(text,values)??text.replace(/\{(\w+)\}/g,(_,key)=>values[key]);
const withStatus=(value,status)=>format('{value} · {status}',{value,status:tr(status)});
const STATES=['Down','Startup','Off','Slave','Inverting','Inverting (half)','AES','PowerAssist','Bypass','Charging'];
const MODES={4:'Off',5:'Charger only',6:'Inverter only',7:'On'},CHARGE=['Initializing','Bulk','Absorption','Float','Storage','Repeated absorption','Forced absorption','Equalization','Bulk stopped'];
const LEDS=['Mains','Absorption','Bulk','Float','Inverter','Overload','Low battery','Temperature'];
let sample=null,receivedAt=0,previous=null,connected=false,pollTimer=null,ageTimer=null,controller=null,generation=0,events=[],rates={};
let faultPosting=false,faultQueuedUntil=0,faultMessage='';
const hex=n=>'0x'+Number(n).toString(16).toUpperCase().padStart(6,'0');
// Verified against the native Venus decoder; capture #3 confirms the transitions.
const FF_FIELDS=[[4,"MainsPllLocked","Mains synchronization","Locked","Not locked"],[18,"AcIn1Available","AC input 1 availability","Available","Unavailable"],[24,"PreferRenewableEnergyActive","Renewable priority","Active","Inactive"],[28,"RelayTestOk","Relay test OK","Indicated","Not indicated"],[10,"HighDcVoltage","High DC voltage","Alarm","Not indicated"],[11,"HighDcCurrent","High DC current","Alarm","Not indicated"]];
function ffValue(f,bit){return f?.firmware===2623563&&Number.isInteger(f.valid_bits)&&f.valid_bits>bit&&f.valid_bits<=32&&Number.isInteger(f.extended_flags)&&f.extended_flags>=0&&f.extended_flags<=0xffffffff&&Number.isFinite(f.extended_age_s)&&f.extended_age_s>=0?Boolean(f.extended_flags&(2**bit)):null;}
const ffText=(field,value)=>value===null?'Unknown':field[value?3:4];
const FAULT_NAMES={1:'Another phase has switched off',2:'Mixed interface protocols',3:'Expected devices not found',4:'No other device found',5:'AC-out overvoltage',6:'DDC program error',7:'External device needs an assistant',8:'Ground relay test failed',10:'System time synchronization error',11:'Relay test fault',12:'Controller configuration mismatch',14:'Device cannot transmit',15:'VE.Bus combination error',16:'VE.Bus dongle missing',17:'Phase master missing',18:'Slave AC-out overvoltage while off',19:'Slave has no AC input',20:'Configuration mismatch',22:'Device cannot operate as a slave',24:'Switchover protection activated',25:'Incompatible VE.Bus configuration',26:'Internal error'};
const known=n=>typeof n==='number'&&Number.isFinite(n),bool=x=>x===true?'Yes':x===false?'No':'Unknown';
function number(n,unit='',digits=1){
 if(!known(n))return tr('Unknown');
 const value=n.toLocaleString(document.documentElement.lang,{maximumFractionDigits:digits});
 return unit?format('{number} {unit}',{number:value,unit:tr(unit)}):value;
}
const elapsed=()=>sample?Math.max(0,(performance.now()-receivedAt)/1000):0;
const age=a=>known(a)&&a>=0?a+elapsed():Infinity;
const reading=(key)=>Array.isArray(sample?.[key])&&known(sample[key][0])?sample[key][0]:null;
const readingAge=key=>age(sample?.[key]?.[1]);
function duration(s){
 if(!known(s)||s<0)return tr('Unknown');
 const hours=Math.floor(s/3600),minutes=Math.floor(s%3600/60),seconds=Math.floor(s%60);
 return hours?format('{hours} h {minutes} min',{hours,minutes}):minutes?format('{minutes} min {seconds} s',{minutes,seconds}):format('{seconds} s',{seconds});
}
const ageText=a=>Number.isFinite(a)?format('{age} ago',{age:duration(a)}):tr('No reading');
const stateName=(n,arr)=>arr[n]??'Unknown';
const limit=n=>known(n)&&n>=0?number(n,'W',0):'No configured limit';
function row(label,value,stack=false){return `<div class="row${stack?' stack':''}"><dt>${esc(label)}</dt><dd>${esc(value)}</dd></div>`;}
function metric(label,value,a=0,max=5){const timing=ageText(a);return `<div class="metric${a>=max?' stale':''}"><div class="label">${esc(label)}</div><div class="value">${esc(value)}</div><small>${esc(a>=max?format('{age} · stale',{age:timing}):timing)}</small></div>`;}
function event(text,tone=''){const s=sample;events.unshift({uptime_s:s?.now_s??null,at:s?.ess?.clock_valid?s.ess.clock:format('S3 {duration}',{duration:duration(s?.now_s)}),text:String(tr(text)).slice(0,220),tone});events=events.slice(0,60);}
function owner(s){if(s.multi_age_s==null||s.multi_age_s>=5)return'No fresh inverter traffic';if(s.master_age_s!=null&&s.master_age_s<10)return'External master / MK3 detected';return['polling','checking','writing'].includes(s.ctl?.state)?'brain_zero':s.ctl?.state==='paused'?'Paused for another master':format('Not controlling ({state})',{state:tr(s.ctl?.state||'unknown')});}
function observe(s){
 if(!previous){event(format('Connected. Current inverter state: {state}',{state:tr(stateName(s.device_state,STATES))}));previous=s;return;}
 const p=previous,restarted=p.diagnostics.boot_id!==s.diagnostics.boot_id;
 if(restarted){event(format('S3 restarted: {reason}',{reason:tr(s.diagnostics.reset_reason)}),'warn');rates={};}
 else{
  const dt=s.now_s-p.now_s;if(dt>0)rates={frames:Math.max(0,s.frames-p.frames)/dt,failed:Math.max(0,s.ctl.failed-p.ctl.failed),bad:Math.max(0,s.bad-p.bad),gaps:Math.max(0,s.gaps-p.gaps)};
  if(rates.bad||rates.gaps||rates.failed)event(format('Since previous sample: {bad} bad frames, {gaps} counter gaps, {failed} request failures',{bad:rates.bad||0,gaps:rates.gaps||0,failed:rates.failed||0}),'warn');
 }
 const fields=[['Control owner',owner(p),owner(s)],['Inverter state',stateName(p.device_state,STATES),stateName(s.device_state,STATES)],['Operating mode',MODES[p.actual_mode]||'Unknown',MODES[s.actual_mode]||'Unknown'],['Charging stage',stateName(p.charge_state,CHARGE),stateName(s.charge_state,CHARGE)],['ESS decision',p.ess.state_name,s.ess.state_name],['ESS schedule',p.ess.schedule,s.ess.schedule],['SOC source',p.ac_input?.soc_source,s.ac_input?.soc_source],['AC input',p.ac_input?.phase,s.ac_input?.phase],['PV operation',p.pv_disable?.phase,s.pv_disable?.phase],['MQTT',p.mqtt?.state,s.mqtt?.state],['Charger job',p.diagnostics.charger.state,s.diagnostics.charger.state]];
 if(!restarted)for(const [label,a,b]of fields)if(a!==b)event(format('{label}: {previous} → {current}',{label:tr(label),previous:tr(String(a??'Unknown')),current:tr(String(b??'Unknown'))}));
 if(!restarted&&(p.led_on!==s.led_on||p.led_blink!==s.led_blink))event('Front-panel LED indications changed.');
 if(!restarted)for(const field of FF_FIELDS){const a=ffValue(p.diagnostics.faults,field[0]),b=ffValue(s.diagnostics.faults,field[0]);if(a!==null&&b!==null&&a!==b)event(format('FF · {label}: {previous} → {current} (snapshot read {age} ago)',{label:tr(field[2]),previous:tr(ffText(field,a)),current:tr(ffText(field,b)),age:duration(s.diagnostics.faults.extended_age_s)}));}
 previous=s;
}
function render(){
 if(!sample)return;const s=sample,d=s.diagnostics,c=d.cached,e=s.ess,a=s.ac_input||{},pv=s.pv_disable||{},ct=s.ctl||{},busAge=age(s.multi_age_s),stale=busAge>=5,policyStale=age(c.policy_age_s)>=3||stale;
 $('connection-title').textContent=!connected?'Disconnected · last snapshot':document.hidden?'Paused while hidden':stale?'Inverter data is stale':'Receiving cached diagnostics';
 $('connection-detail').textContent=!connected?'Unable to reach brain_zero. Values below age from the last successful response.':stale?
  format('No fresh inverter frames. Last seen {age}.',{age:ageText(busAge)}):format('{owner} · {clock}',{owner:tr(owner(s)),clock:e.clock_valid?e.clock:tr('Controller clock not set')});
 $('connection').className='banner '+(!connected||stale?'warn':'good');$('connection').querySelector('.dot').className='dot';
 $('multi').innerHTML=metric('Operating mode',stateName(s.actual_mode,MODES),age(s.actual_mode_age_s),5)+metric('Inverter state',stateName(s.device_state,STATES),age(s.device_state_age_s),5)+metric('Charging stage',stateName(s.charge_state,CHARGE),age(s.charge_state_age_s),5)+metric('AC input power · + import',number(reading('acin_w')==null?null:-reading('acin_w'),'W',0),readingAge('acin_w'))+metric('AC-out net power',number(reading('acout_w'),'W',0),readingAge('acout_w'))+metric('Inverter / charger · + charging',number(reading('inverter_w'),'W',0),readingAge('inverter_w'))+metric('Battery voltage',number(reading('bat_v'),'V',2),readingAge('bat_v'))+metric('Reported battery current · + charging',number(reading('bat_a'),'A',1),readingAge('bat_a'))+metric('AC input voltage',number(reading('mains_v'),'V',1),readingAge('mains_v'))+metric('AC input frequency',number(reading('ac_hz'),'Hz',3),readingAge('ac_hz'));
 $('policy').innerHTML=row('ESS mode',e.mode_name)+row('Controller decision',policyStale?withStatus(tr(e.state_name),'stale'):e.state_name)+row('Reason',tr(e.why||'Unknown'),true)+row('Decision age',ageText(age(c.policy_age_s)))+row('ESS configured to run',bool(e.run))+row('Active schedule',e.schedule>=0?format('Slot {slot}',{slot:e.schedule+1}):'None')+row('Safety cap applied',bool(e.capped))+row('Ignore AC protection',tr(a.reason||'Unknown'),true)+row('PV restoration',tr(pv.reason||'Unknown'),true);
 let flags=reading('flags'),flagFresh=flags!=null&&readingAge('flags')<30&&!stale;
 const battery=c.battery,limits=c.limits;
 const external=withStatus(number(battery.external_soc,'%'),battery.external_valid?ageText(age(battery.external_age_s)):'unavailable');
 const sync=battery.sync_sent?format('{value} · {status} · {age}',{value:number(battery.sync_target,'%'),status:tr(battery.sync_verified?'matched by readback':'not verified'),age:ageText(age(battery.sync_age_s))}):'No update sent';
 $('battery').innerHTML=row('External battery SOC',external)+row('Inverter SOC',withStatus(number(reading('soc'),'%'),ageText(readingAge('soc'))))+row('Ignore AC SOC source',a.soc_source||'Unknown')+row('Inverter SOC synchronization',sync)+row('Minimum SOC setting',number(limits.minimum_soc,'%'))+row('Active ESS SOC floor',e.active_soc_limit>=0?number(e.active_soc_limit,'%'):'No active floor')+row('Charge power limit',limit(limits.max_charge_w))+row('Discharge power limit',limit(limits.max_discharge_w))+row('Import / export limits',format('{importLimit} / {exportLimit}',{importLimit:tr(limit(limits.max_import_w)),exportLimit:tr(limit(limits.max_feed_in_w))}))+row('Controller safety bound',limit(limits.safety_max_w))+row('Charging blocked by ESS flag',flagFresh?bool(Boolean(flags&1)):'Unknown / stale')+row('Discharging blocked by ESS flag',flagFresh?bool(Boolean(flags&2)):'Unknown / stale');
 const commandRow=(label,request,feedback,at)=>`<tr><td>${esc(label)}</td><td>${esc(request)}</td><td>${esc(feedback)}</td><td>${esc(ageText(at))}</td></tr>`;
 const ack=reading('accepted_setpoint_w'),ackAge=readingAge('accepted_setpoint_w');
 const accepted=ack==null?'No matched acknowledgement':withStatus(number(ack,'W',0),s.setpoint_fresh===1&&ackAge<13&&!stale?'acknowledged':'old acknowledgement');
 const modeAge=age(s.actual_mode_age_s),currentLimitAge=readingAge('limit_a');
 let commands=commandRow('ESS setpoint · + export / − import',number(reading('setpoint_w'),'W',0),accepted,ackAge)
  +commandRow('Operating mode',format('{mode} (last bus command)',{mode:tr(stateName(s.mode_cmd,MODES))}),withStatus(tr(stateName(s.actual_mode,MODES)),modeAge<5&&!stale?'readback':'stale'),modeAge)
  +commandRow('AC input current limit',number(reading('limit_cmd_a'),'A'),withStatus(number(reading('limit_a'),'A'),currentLimitAge<5&&!stale?'readback':'stale'),currentLimitAge);
 for(const p of d.charger.parameters){const unit=p.key==='charge_current'?'A':'V',labels={charge_current:'Stored DC charge ceiling',absorption_voltage:'Stored absorption voltage',float_voltage:'Stored float voltage'},at=age(p.age_s);commands+=commandRow(labels[p.key]||p.key,p.requested==null?'No pending request':number(p.requested,unit,2),p.value==null?'No recent cached read':withStatus(number(p.value,unit,2),at<120&&!stale?(p.verified?'verified readback':'cached setting'):'stale cached setting'),at);}
 const acAge=age(a.transport?.age_s),pvAge=age(pv.transport?.age_s);
 commands+=commandRow('Ignore AC input',bool(a.requested),a.ignored==null?'Unknown':withStatus(tr(a.ignored?'Ignoring':'Accepting'),a.transport?.verified&&acAge<3&&!stale?'verified readback':'not fresh/verified'),acAge)
  +commandRow('PV-disable',bool(pv.requested),pv.disabled==null?'Unknown':withStatus(tr(pv.disabled?'PV disabled':'PV enabled'),pv.transport?.verified&&pvAge<3&&!stale?'verified readback':'not fresh/verified'),pvAge);
 $('commands').innerHTML=commands;
 const ledFresh=age(s.led_age_s)<5&&!stale;
 $('leds').innerHTML=s.led_on==null?'<span class="muted">No LED feedback</span>':LEDS.map((name,i)=>{const blink=Boolean((s.led_blink||0)&(1<<i)),on=Boolean(s.led_on&(1<<i));return `<span class="chip ${ledFresh&&(on||blink)?i>=5?'alarm':'on':''}${blink?' blink':''}">${esc(format('{name}: {state}',{name:tr(name),state:tr(blink?'blinking':on?'on':'off')}))}</span>`;}).join('');
 $('led-age').textContent=withStatus(ageText(age(s.led_age_s)),ledFresh?'current indicators':'stale / unconfirmed');
 renderFaults(s,stale);
 const ff=d.faults,ffAge=age(ff?.extended_age_s),ffOld=ffAge>=60||stale||!connected;
 $('ff-status').innerHTML=FF_FIELDS.map(field=>{const value=ffValue(ff,field[0]);return `<div class="metric${ffOld||value===null?' stale':''}" data-ff-bit="${field[0]}"><div class="label">${esc(format('{label} · bit {bit}',{label:tr(field[2]),bit:field[0]}))}</div><div class="value">${esc(ffText(field,value))}</div><small>${esc(value===null?'No validated reading':withStatus(ageText(ffAge),ffOld?'stale snapshot':'cached snapshot'))}</small></div>`;}).join('');
 $('ff-meta').textContent=ffAge===Infinity?'FF not read · changes appear in the event list when a new snapshot is observed.':
  format('FF {word} · read {age} · changes appear in the event list when a new snapshot is observed.',{word:'0x'+Number(ff.extended_flags).toString(16).toUpperCase().padStart(8,'0'),age:ageText(ffAge)});
 const firmware=connected&&!stale&&Number.isInteger(s.inverter_firmware)&&s.inverter_firmware>0?String(s.inverter_firmware):tr('Unknown');
 $('bus').innerHTML=row(tr('VE.Bus firmware version'),firmware)+row('Control owner',stale?'Unknown / stale':owner(s))+row('Last inverter frame',ageText(busAge))+row('Last external master',ageText(age(s.master_age_s)))+row('Sync period',number(s.sync_ms,'ms',3))+row('Frame rate',stale||!connected?'No recent rate':number(rates.frames,'frames/s',1))+row('Frames since startup',number(s.frames,'',0))+row('Bad checksums / counter gaps',number(s.bad,'',0)+' / '+number(s.gaps,'',0))+row('Read requests / replies',number(ct.polls,'',0)+' / '+number(ct.answered,'',0))+row('Unanswered / delivery failures',number(ct.unanswered,'',0)+' / '+number(ct.failed,'',0))+row('ESS writes / acknowledgements',number(ct.writes,'',0)+' / '+number(ct.acked,'',0))+row('Flag writes / acknowledgements',number(ct.flag_writes,'',0)+' / '+number(ct.flag_acks,'',0))+row('Cycles yielded / backoffs',number(ct.yielded,'',0)+' / '+number(ct.backoffs,'',0))+row('UART framing / parity errors',number(c.uart.frame_errors,'',0)+' / '+number(c.uart.parity_errors,'',0))+row('UART FIFO / buffer overflows',number(c.uart.fifo_overflows,'',0)+' / '+number(c.uart.buffer_full,'',0));
 $('system').innerHTML=row('Wi-Fi signal',number(s.rssi,'dBm',0))+row('Free memory',number(s.heap/1024,'KiB',1))+row('Lowest free / largest block',number(d.min_heap/1024,'KiB',1)+' / '+number(d.max_alloc_heap/1024,'KiB',1))+row('Minimum free loop stack',number(d.loop_stack_min_free,'bytes',0))+row('Uptime at last response',duration(s.now_s))+row('Restart reason',format('{reason} ({code})',{reason:tr(d.reset_reason),code:d.reset_code}))+row('Restart pending',bool(d.restart_pending))+row('Controller clock',e.clock_valid?e.clock:'Not synchronized')+row('Firmware build',d.build);
 const wifi=s.wifi_recovery||{},mqtt=s.mqtt||{},tx=s.tx||{};
 $('wifi').innerHTML=row('Gateway reachable',bool(wifi.gateway_verified))+row('Gateway probe misses',number(wifi.gateway_misses,'',0))+row('Radio cycles / disconnects',number(wifi.radio_cycles,'',0)+' / '+number(wifi.disconnects,'',0))+row('Last disconnect code',wifi.disconnects?number(wifi.disconnect_reason,'',0):'None recorded')+row('Recovery restart used',bool(wifi.restart_used))+row('Channel / width',wifi.channel?number(wifi.channel,'',0)+' / '+number(wifi.width_mhz,'MHz',0):'Unknown')+row('Transmit power limit',number(wifi.tx_power_qdbm/4,'dBm',2));
 $('mqtt').innerHTML=row('Connection',mqtt.state||'Unknown')+row('Broker / client ID',[mqtt.broker,mqtt.node].filter(Boolean).join(' / ')||'Unknown')+row('Published / received / refused',[mqtt.published,mqtt.commands,mqtt.refused].map(n=>number(n,'',0)).join(' / '))+row('Last command',tr(mqtt.last_command||'None recorded'))+row('Connection error',tr(mqtt.error||'None reported'))+row('State topic',mqtt.state_topic||'Unknown')+row('Availability topic',mqtt.status_topic||'Unknown')+row('Command topic',mqtt.command_topic||'Unknown');
 $('raw-bus').innerHTML=row('MultiPlus / external master / controller frames',[s.multi_frames,s.master_frames,s.own_frames].map(n=>number(n,'',0)).join(' / '))+row('Temperature byte (scale unconfirmed)',number(s.temp_raw,'',0))+row('Raw flags',flags==null?'Unknown':hex(flags))+row('Bench test mode',tx.mode||'Unknown')+row('Time remaining',tx.mode&&tx.mode!=='off'?duration(tx.left_s):'–')+row('Requests / replies / failures',[tx.sent,tx.answered,tx.failed].map(n=>number(n,'',0)).join(' / '))+row('Writes / acknowledgements',[tx.writes,tx.acked].map(n=>number(n,'',0)).join(' / '))+row('Cycles yielded',number(tx.yielded,'',0))+row('Last reply delay',tx.last_reply_ms>=0?number(tx.last_reply_ms,'ms',1):'No reply')+row('Stop reason',tr(tx.why||'None recorded'));
 $('events').innerHTML=events.map(v=>`<li><time>${esc(v.at)}</time><span class="${v.tone}">${esc(v.text)}</span></li>`).join('');
 $('last-update').textContent=format('Snapshot received {age} ago · all values are cached · no additional VE.Bus requests',{age:duration(elapsed())});
}
function renderFaults(s,stale){
 const f=s.diagnostics.faults,button=$('read-fault');
 if(!f){button.disabled=true;$('fault-status').textContent='Fault report support is unavailable in this firmware.';return;}
 const code=f.led_code;
 $('current-fault').textContent=!ledFreshEnough(s)||code===-1?'Numbered fault indication: unknown / stale':code>0?
  format('VE.Bus error {code}: {fault}',{code,fault:tr(FAULT_NAMES[code]||'Unmapped fault code')}):'No qualifying numbered-fault LED pattern. This is not a complete alarm check.';
 const cooldown=Math.max(0,(f.cooldown_s||0)-elapsed()),queued=faultPosting||performance.now()<faultQueuedUntil;
 button.disabled=!connected||stale||owner(s)!=='brain_zero'||f.firmware!==2623563||f.active||cooldown>0||queued;
 button.textContent=f.active||queued?'Reading…':'Read fault report';
 const status={never:'No report requested.',reading:'Reading the inverter; ESS control keeps priority.',done:'Read complete. The readings below are cached.',partial:'Some fields could not be read. Earlier readings retain their original age.',cancelled:'Read cancelled or timed out. Earlier readings retain their original age.',unsupported:'This inverter returned an unrecognized diagnostic format.'};
 const message=tr(faultMessage||status[f.state]||'Unknown report state');
 $('fault-status').textContent=f.firmware!==2623563?format('{message} Detailed reads are not yet validated for this inverter firmware.',{message}):
  owner(s)!=='brain_zero'?format('{message} Available when brain_zero owns the bus.',{message}):
  cooldown>0&&!f.active?format('{message} Next read available in {seconds} s.',{message,seconds:Math.ceil(cooldown)}):message;
 const wa=age(f.warnings_age_s),ra=age(f.relay_age_s);
 const flag=(mask)=>{
  if(wa===Infinity)return 'Not read';
  const value=tr(f.warning_flags&mask?'Indicated':'Not indicated');
  return wa>=60||stale?withStatus(value,'stale snapshot'):value;
 };
 $('fault-live').innerHTML=row('Snapshot read',ageText(wa))+row('Recent relay-test failure',flag(1))+row('Repeated relay-test failure',flag(0x10000))+row('Relay test preventing connection',flag(0x20000))+row('Error / warning word',wa===Infinity?'Not read':hex(f.warning_flags))+(f.warning_unknown_bits?row('Unmapped warning bits',hex(f.warning_unknown_bits)):'');
 let history=row('Report read',ageText(ra));
 if(ra===Infinity)history+=row('Stored report','Not read');
 else if(!f.relay_present)history+=row('Stored report','No report available');
 else{
  history+=row('Report ID',String(f.relay_count))+row('Report code',hex(f.relay_code));
  const notes=[[1,'Backfeed safety test in progress'],[2,'Backfeed safety test passed'],[8,'AC0 / AC1 mismatch'],[16,'Communication error'],[32,'Ground relay error'],[0x1000,'Mains voltage mismatch'],[0x2000,'Period mismatch'],[0x4000,'Backfeed relay drive mismatch'],[0x10000,'PE2 unexpectedly open'],[0x20000,'PE2 unexpectedly closed']].filter(([bit])=>f.relay_code&bit).map(([,text])=>tr(text));
  history+=row('Recorded indications',notes.join('; ')||'No named indications in this report',true);
  const step=(f.relay_code>>8)&15;if(step)history+=row('Recorded failing step',String(step));
  if(f.relay_unknown_bits)history+=row('Unmapped report bits',hex(f.relay_unknown_bits));
 }
 $('fault-history').innerHTML=history;
}
function ledFreshEnough(s){return age(s.led_age_s)<5&&age(s.multi_age_s)<5;}
async function readFaultReport(){
 if($('read-fault').disabled)return;
 faultPosting=true;faultMessage='Requesting a read-only fault report…';render();
 const ac=new AbortController(),timeout=setTimeout(()=>ac.abort(),5000);
 try{
  const response=await fetch('/api/diagnostics/fault-report/read',{method:'POST',signal:ac.signal});
  const result=await response.json();if(!response.ok||!result.ok)throw Error(result.error||'Read request refused');
  faultMessage='';faultQueuedUntil=performance.now()+3000;event('Read-only fault report requested.');
 }catch(error){faultMessage=error.name==='AbortError'?'Read request timed out. Check the next cached snapshot before retrying.':String(error.message);}
 finally{clearTimeout(timeout);faultPosting=false;render();}
}
function exportSnapshot(){
 if(!sample)return;const s=sample,keys=['now_s','frames','bad','gaps','multi_frames','master_frames','own_frames','multi_age_s','master_age_s','sync_ms','inverter_w','acout_w','acin_w','bat_v','bat_a','dc_info_v','soc','mains_v','mains_a','inverter_v','inverter_a','ac_hz','ac_monitor','inverter_firmware','multiplus_ii','device_state','device_state_age_s','led_on','led_blink','led_age_s','actual_mode','actual_mode_age_s','mode_cmd','mode_age_s','charge_state','charge_state_age_s','limit_a','limit_cmd_a','temp_raw','flags','setpoint_w','accepted_setpoint_w','setpoint_fresh','setpoint_ack_age_s','tx','ctl','ess','ac_input','pv_disable','settings_read','write','rssi','heap','diagnostics'];
 const snapshot=Object.fromEntries(keys.filter(k=>k in s).map(k=>[k,s[k]]));snapshot.diagnostics={...s.diagnostics};delete snapshot.diagnostics.boot_id;
 snapshot.mqtt={state:s.mqtt?.state,published:s.mqtt?.published,refused:s.mqtt?.refused};
 const data={product:'brain_zero',format:1,exported_at:new Date().toISOString(),read_only:true,snapshot_age_s:Number(elapsed().toFixed(1)),connected,snapshot,events};
 const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'})),link=document.createElement('a');link.href=url;link.download='brain-zero-diagnostics-'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
async function poll(){
 if(document.hidden||controller)return;const token=generation;controller=new AbortController();const current=controller,timeout=setTimeout(()=>current.abort(),4500);
 try{const response=await fetch('/api/diagnostics',{cache:'no-store',signal:current.signal});if(!response.ok)throw Error('HTTP '+response.status);const s=await response.json();if(s.diagnostics?.schema!==1||!s.diagnostics.cached?.battery||!s.diagnostics.cached?.limits||!s.diagnostics.cached?.uart||!Array.isArray(s.diagnostics.charger?.parameters)||!known(s.diagnostics.boot_id)||!s.ess||!s.ctl||!known(s.now_s))throw Error('Invalid diagnostics snapshot');if(document.hidden||token!==generation)return;
  const restored=sample&&!connected;sample=s;receivedAt=performance.now();connected=true;if(restored)event('Connection restored.');observe(s);$('download').disabled=false;render();
 }catch(error){if(!document.hidden&&token===generation){if(connected)event('Connection lost. Showing the last cached snapshot.','warn');connected=false;if(sample)render();else{$('connection-title').textContent='Waiting for brain_zero';$('connection-detail').textContent='No valid diagnostic snapshot received. Retrying while this page is visible.';}}}
 finally{clearTimeout(timeout);if(controller===current)controller=null;if(!document.hidden&&token===generation)pollTimer=setTimeout(poll,2000);}
}
function visibility(){clearTimeout(pollTimer);clearInterval(ageTimer);generation++;if(document.hidden){controller?.abort();controller=null;return;}render();ageTimer=setInterval(render,1000);void poll();}
document.addEventListener('visibilitychange',visibility);window.addEventListener('pagehide',()=>{clearTimeout(pollTimer);clearInterval(ageTimer);generation++;controller?.abort();});$('download').addEventListener('click',exportSnapshot);$('read-fault').addEventListener('click',readFaultReport);visibility();
</script></body></html>)HTML";
