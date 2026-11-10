// The ESS settings page (/ess) shows settings and a short controller status.
// Reads and saves settings through /api/ess (web.h).
#pragma once

#include <pgmspace.h>
#include "site_nav.h"

static const char ESS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESS settings · brain_zero</title><link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg"><link rel="stylesheet" href="/brand/brand.css">
<style>
:root{--bg:#f5f6f8;--card:#fff;--ink:#1d2430;--muted:#687180;--line:#e2e5ea;--bad:#c62828;--a:#2463eb;--hi:#eef3ff}
@media (prefers-color-scheme:dark){:root{--bg:#101318;--card:#191e25;--ink:#e6e9ee;--muted:#98a2ae;--line:#29303a;--bad:#f85149;--a:#6ea8fe;--hi:#1c2738}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
a{color:var(--a)}
header{display:flex;flex-wrap:wrap;gap:4px 16px;align-items:baseline;justify-content:space-between;padding:14px 16px 0;max-width:900px;margin:auto}
h1{font-size:18px;margin:0}header a,#status{font-size:13px}#status{color:var(--muted);margin:4px 0 0}
main{padding:4px 16px 24px;max-width:900px;margin:auto}
h2{font-size:12px;margin:18px 0 8px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.05em}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:6px 12px;min-width:0}
.s{font-size:12px;color:var(--muted)}
.row{display:flex;flex-wrap:wrap;align-items:center;gap:6px 10px;padding:9px 0;border-top:1px solid var(--line)}
.row:first-child{border-top:0}.row[hidden]{display:none}
.name{flex:1 1 230px;display:flex;gap:8px;align-items:center}
.hint{flex-basis:100%;font-size:12px;color:var(--muted)}
input,select,button{font:inherit;color:var(--ink)}
input[type=number],input[type=text],input[type=time],select{padding:5px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg)}
input[type=number]{width:96px}input:disabled{opacity:.4}input[type=checkbox]{width:18px;height:18px;margin:0}
input:invalid{border-color:var(--bad)}
.wrap{overflow-x:auto}table{border-collapse:collapse;width:100%;min-width:620px}
th,td{text-align:left;padding:7px 6px;border-top:1px solid var(--line);white-space:nowrap}
th{font-size:12px;color:var(--muted);font-weight:600;border-top:0}
tr.active td{background:var(--hi)}td input[type=number]{width:70px}td input[type=text]{width:72px}
.bar{position:sticky;bottom:0;background:var(--bg);padding:12px 0;display:flex;flex-wrap:wrap;gap:10px;align-items:center}
button{padding:7px 16px;border:1px solid var(--line);border-radius:6px;background:var(--card);cursor:pointer}
button[type=submit]{background:var(--a);border-color:var(--a);color:#fff}
footer{max-width:900px;margin:auto;padding:0 16px 20px;color:var(--muted);font-size:12px}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(
<header><h1>ESS settings</h1><a href="/diagnostics">Diagnostics</a></header>
<main>
<p id="status" role="status">Loading ESS status…</p>
<form id="f" autocomplete="off" novalidate>
<h2>This ESP32</h2>
<div class="card">
<div class="row"><label class="name" for="run">Run ESS from this ESP32</label><input type="checkbox" id="run" name="run">
<span class="hint">The controller yields to Venus when it detects an MK3 master.</span></div>
<div class="row"><label class="name" for="safety_max">Safety cap</label><input type="number" id="safety_max" name="safety_max" min="0" max="5000" step="1" required> W
<span class="hint">Limits the grid setpoint in both directions, including during scheduled charging. The controller can stop battery discharge when the cap reduces a requested import.</span></div>
</div>
<h2>ESS mode</h2>
<div class="card">
<div class="row"><label class="name" for="mode">Mode</label><select id="mode" name="mode">
<option value="0">Optimized (with BatteryLife)</option><option value="1">Optimized (without BatteryLife)</option>
<option value="2">Keep batteries charged</option><option value="3">External control</option></select>
<span class="hint" id="mode-hint"></span></div>
<div class="row"><label class="name" for="min_soc">Minimum SOC (unless grid fails)</label><input type="number" id="min_soc" name="min_soc" min="0" max="100" step="1" required> %</div>
<div class="row"><label class="name" for="grid_setpoint">Grid setpoint</label><input type="number" id="grid_setpoint" name="grid_setpoint" min="-5000" max="5000" step="1" required> W
<span class="hint">+ imports from the grid, − exports, as in Venus.</span></div>
<div class="row ext"><label class="name" for="ext_no_charge">Disable charge</label><input type="checkbox" id="ext_no_charge" name="ext_no_charge"></div>
<div class="row ext"><label class="name" for="ext_no_feed_in">Disable feed-in and discharge</label><input type="checkbox" id="ext_no_feed_in" name="ext_no_feed_in"></div>
</div>
<h2>Battery voltage protection</h2>
<div class="card">
<div class="row"><label class="name" for="voltage_hold_enabled">Use grid at low battery voltage</label><input type="checkbox" id="voltage_hold_enabled" name="voltage_hold_enabled">
<span class="hint">The MultiPlus charges only in scheduled windows. If its battery voltage stays at or below the threshold for 5 seconds, the grid supplies the loads until the next scheduled MultiPlus charge window.</span></div>
<div class="row"><label class="name" for="voltage_hold_v">Switch to grid at</label><input type="number" id="voltage_hold_v" name="voltage_hold_v" min="8" max="80" step="0.01" placeholder="V" required disabled> V
<span class="hint" id="voltage-hold-status">Waiting for controller status</span></div>
</div>
<h2>Limits</h2>
<div class="card">
<div class="row"><label class="name"><input type="checkbox" data-limit="max_charge">Limit charge power</label><input type="number" id="max_charge" name="max_charge" min="0" max="5000" step="1"> W
<span class="hint">Battery charging, from the grid or solar; surplus solar is exported instead.</span></div>
<div class="row"><label class="name"><input type="checkbox" data-limit="max_discharge">Limit inverter power</label><input type="number" id="max_discharge" name="max_discharge" min="0" max="5000" step="1"> W
<span class="hint">Battery discharge; the grid supplies the rest. 0 W stops discharge altogether.</span></div>
<div class="row"><label class="name"><input type="checkbox" data-limit="max_feed_in">Limit system feed-in</label><input type="number" id="max_feed_in" name="max_feed_in" min="0" max="5000" step="1"> W
<span class="hint">Limits exports to the grid, including the grid setpoint.</span></div>
<div class="row"><label class="name"><input type="checkbox" data-limit="max_import">Limit grid import</label><input type="number" id="max_import" name="max_import" min="0" max="5000" step="1"> W
<select name="always_peak_shave" aria-label="peak shaving"><option value="0">above the minimum SOC only</option><option value="1">always</option></select>
<span class="hint">Peak shaving: the battery covers what the house draws over the limit, and charging from the grid stays under it.</span></div>
<div class="row"><label class="name" for="dc_pv_feed_in">Feed in DC-coupled PV overvoltage</label><input type="checkbox" id="dc_pv_feed_in" name="dc_pv_feed_in"></div>
</div>
<h2>Scheduled charge levels</h2>
<div class="card"><div class="wrap"><table id="sched"><thead><tr><th>#</th><th>On</th><th>Day</th><th>Start</th><th>Duration</th><th>Charge to</th><th>Discharge above it</th><th>Grid after target</th></tr></thead><tbody></tbody></table></div>
<p class="s"><strong>Grid after target</strong> supplies loads from the grid after reaching the target, until the window ends. It takes priority over allowing discharge.</p>
<p class="s">Schedules apply in the optimized ESS modes. Windows can run past midnight, and the first active row takes priority. A 100% target charges for the full window.</p></div>
<h2>Clock</h2>
<div class="card">
<div class="row"><label class="name" for="tz">Time zone (POSIX TZ)</label><input type="text" id="tz" name="tz" maxlength="47" spellcheck="false" required>
<span class="hint" id="clock"></span></div>
</div>
<div class="bar"><button type="submit">Save</button><button type="button" id="reload">Discard changes</button><span id="msg" class="s"></span></div>
</form>
</main>
<footer>Web access is controlled by the Remote Console password in Settings.</footer>
<script>
const $=id=>document.getElementById(id),F=$("f"),E=k=>F.elements[k];
const t=text=>typeof text==="string"?(window.bzText?.(text)??text):text;
const fmt=(template,values)=>window.bzFormat?.(template,values)??template.replace(/\{(\w+)\}/g,(match,key)=>values[key]??match);
const DAYS=["Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Every day","Weekdays","Weekends","Monthly, the 1st"];
const LIMITS=["max_charge","max_discharge","max_feed_in","max_import"];
const MODE_HINT=["BatteryLife adjusts the discharge threshold based on recent charging and keeps it at or above the minimum SOC.",
"Discharge stops at the minimum SOC. Slow charging from the grid starts five percentage points below it.",
"Charges up to the configured limits and prevents discharge while the grid is available.",
"Uses the grid setpoint and the controls below. The external controller manages SOC and schedules."];
const hm=s=>String(Math.floor(s/3600)).padStart(2,"0")+":"+String(Math.floor(s%3600/60)).padStart(2,"0");
const tb=document.querySelector("#sched tbody");
for(let i=1;i<=5;i++){const tr=document.createElement("tr");tr.id="r"+i;tr.innerHTML=
'<td>'+i+'</td><td><input type="checkbox" name="s'+i+'_on"></td>'+
'<td><select name="s'+i+'_day" aria-label="day">'+DAYS.map((d,k)=>'<option value="'+k+'">'+d+"</option>").join("")+"</select></td>"+
'<td><input type="time" name="s'+i+'_start" aria-label="start" required></td>'+
'<td><input type="text" name="s'+i+'_duration" aria-label="duration" pattern="([01]?[0-9]|2[0-3]):[0-5][0-9]|24:00" placeholder="h:mm" required></td>'+
'<td><input type="number" name="s'+i+'_soc" aria-label="target SOC" min="0" max="100" step="1" required> %</td>'+
'<td><input type="checkbox" name="s'+i+'_discharge" aria-label="discharge above the target"></td>'+
'<td><input type="checkbox" name="s'+i+'_grid_after_target"></td>';tb.appendChild(tr)}
document.addEventListener("DOMContentLoaded",()=>{for(let i=1;i<=5;i++){
E("s"+i+"_on").setAttribute("aria-label",fmt("window {slot} on",{slot:i}));
E("s"+i+"_grid_after_target").setAttribute("aria-label",fmt("Schedule {slot}. After reaching the target, supply loads from grid until End",{slot:i}));
}},{once:true});
function showMode(){const m=+E("mode").value;document.querySelectorAll(".ext").forEach(e=>e.hidden=m!=3);$("mode-hint").textContent=MODE_HINT[m]}
function limitBox(k){return document.querySelector('[data-limit="'+k+'"]')}
function showVoltageHold(){E("voltage_hold_v").disabled=!E("voltage_hold_enabled").checked}
function fill(s){
E("run").checked=!!s.run;E("safety_max").value=s.safety_max;E("mode").value=s.mode;E("min_soc").value=s.min_soc;
E("grid_setpoint").value=s.grid_setpoint;E("ext_no_charge").checked=!!s.ext_no_charge;E("ext_no_feed_in").checked=!!s.ext_no_feed_in;
E("voltage_hold_enabled").checked=!!s.voltage_hold_cv;
E("voltage_hold_v").value=s.voltage_hold_cv?(s.voltage_hold_cv/100).toFixed(2):"";
showVoltageHold();
LIMITS.forEach(k=>{const on=s[k]>=0;limitBox(k).checked=on;if(on||E(k).value==="")E(k).value=on?s[k]:0;E(k).disabled=!on});
E("always_peak_shave").value=s.always_peak_shave?1:0;E("dc_pv_feed_in").checked=!!s.dc_pv_feed_in;E("tz").value=s.tz;
s.schedules.forEach((w,j)=>{const p="s"+(j+1)+"_";E(p+"on").checked=!!w.on;E(p+"day").value=w.day;E(p+"start").value=hm(w.start);
E(p+"duration").value=hm(w.duration);E(p+"soc").value=w.soc;E(p+"discharge").checked=!!w.discharge;E(p+"grid_after_target").checked=!!s.schedule_grid_after_target?.[j]});
showMode()}
function showStatus(message){const status=$("status");if(status.textContent!==message)status.textContent=message}
function highlightSchedule(slot){
for(let i=1;i<=5;i++){const row=$("r"+i),active=slot==i-1;row.classList.toggle("active",active);row.setAttribute("aria-current",String(active))}}
function live(st,s){
const controlling=st.run&&st.ctl==="writing",slot=controlling?st.schedule:-1;
const status=[t(!st.run?"ESS is off":controlling?"ESS active":st.ctl==="paused"?"Venus (MK3) is in control":"Waiting for inverter control")];
if(slot>=0)status.push(fmt("Charge window {slot}",{slot:slot+1}));
if(controlling&&st.grid_hold)status.push(t("Grid hold requested"));
else if(controlling&&st.charging)status.push(t("Charging requested"));
showStatus(status.join(" · "));
highlightSchedule(slot);
$("clock").textContent=st.clock_valid?fmt("Controller time {clock}. Europe/London is GMT0BST,M3.5.0/1,M10.5.0.",{clock:st.clock}):"Schedules wait for the controller clock to sync over Wi-Fi.";
$("voltage-hold-status").textContent=!s.voltage_hold_cv?"Off":!controlling?"Waiting for ESS control":
st.voltage_hold_confirmed?"Grid supply confirmed":st.voltage_hold?"Waiting for inverter confirmation":
st.charge_inhibited?"Charging paused until the next window":"Scheduled charge window";
}
async function load(full){const r=await fetch("/api/ess",{cache:"no-store"});if(!r.ok)throw Error("ESS request failed");const j=await r.json();if(full)fill(j.settings);live(j.status,j.settings);return j.settings}
function offline(){showStatus("Controller unavailable");highlightSchedule(-1);$("clock").textContent="Controller time unavailable";$("voltage-hold-status").textContent="Status unavailable"}
E("mode").addEventListener("change",showMode);
E("voltage_hold_enabled").addEventListener("change",showVoltageHold);
LIMITS.forEach(k=>limitBox(k).addEventListener("change",()=>{E(k).disabled=!limitBox(k).checked}));
F.addEventListener("submit",async e=>{e.preventDefault();
const bad=[...F.elements].find(x=>!x.disabled&&x.willValidate&&!x.checkValidity());
if(bad){bad.focus();$("msg").textContent="check the highlighted field";return}
const b=new URLSearchParams();b.set("run",E("run").checked?1:0);
b.set("voltage_hold_cv",E("voltage_hold_enabled").checked?Math.round(E("voltage_hold_v").valueAsNumber*100):0);
["safety_max","mode","min_soc","grid_setpoint","tz","always_peak_shave"].forEach(k=>b.set(k,E(k).value.trim()));
["ext_no_charge","ext_no_feed_in","dc_pv_feed_in"].forEach(k=>b.set(k,E(k).checked?1:0));
LIMITS.forEach(k=>b.set(k,limitBox(k).checked?E(k).value.trim():-1));
for(let i=1;i<=5;i++){const p="s"+i+"_";["on","discharge","grid_after_target"].forEach(k=>b.set(p+k,E(p+k).checked?1:0));["day","start","duration","soc"].forEach(k=>b.set(p+k,E(p+k).value.trim()))}
$("msg").textContent="saving…";
try{const r=await fetch("/api/ess",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b});
const j=await r.json();if(!j.ok){$("msg").textContent=fmt("Refused. {reason}",{reason:t(j.error)});return}
const saved=await load(true),grid=saved.schedule_grid_after_target;
const verified=grid?.length===5&&grid.every((on,i)=>Number(on)===Number(b.get("s"+(i+1)+"_grid_after_target")));
$("msg").textContent=verified?fmt(saved.run?"Settings saved. ESS is on. {mode}":"Settings saved. ESS is off. {mode}",
{mode:t(E("mode").selectedOptions[0]?.textContent||"")}):"Grid after target did not match the saved settings.";
}catch(err){$("msg").textContent="Could not verify the save. Reload to check the settings."}});
$("reload").addEventListener("click",()=>load(true).then(()=>$("msg").textContent="reloaded").catch(offline));
document.addEventListener("DOMContentLoaded",()=>{
load(true).catch(offline);setInterval(()=>{if(!document.hidden)load(false).catch(offline)},5000);
},{once:true});
</script></body></html>
)HTML";
