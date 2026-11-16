#pragma once
#include <pgmspace.h>
#include "site_nav.h"
static const char BATTERY_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; form-action 'self'"><title>Batteries · brain_zero</title><link rel="stylesheet" href="/brand/brand.css"><link rel="stylesheet" href="/ui/settings.css"><style>
body{font:15px/1.5 system-ui,sans-serif;margin:0}main{max-width:900px;margin:auto;padding:20px}section{border:1px solid #8994a455;border-radius:12px;padding:16px;margin:16px 0}label{display:block;margin:10px 0}input,select,button{font:inherit;padding:7px;max-width:100%;box-sizing:border-box}input:not([type=checkbox]){display:block;width:280px}table{border-collapse:collapse;width:100%}th,td{text-align:right;padding:7px;border-bottom:1px solid #8994a433}th:first-child,td:first-child{text-align:left}small{display:block;opacity:.75}output{display:block;margin:12px 0}details{margin:12px 0}pre{white-space:pre-wrap;overflow-wrap:anywhere}
</style></head><body>)HTML" BRAIN_ZERO_NAV R"HTML(
<main><h1>Batteries</h1><section><h2>Battery readings</h2>
<table><thead><tr><th>Measurement</th><th>JK</th><th>PACE</th><th>Combined bank</th></tr></thead><tbody id="readings"></tbody></table>
<p id="bank-note"></p><small>Positive current charges the battery. The combined bank needs both packs and their capacities.</small></section>
<form id="battery"><section><h2>Bluetooth connections</h2>
<label><input type="checkbox" name="enabled"> Enable Bluetooth battery monitoring</label>
<label>JK Bluetooth address<input name="jk_address" maxlength="17" placeholder="AA:BB:CC:DD:EE:FF" autocomplete="off" spellcheck="false"></label>
<small>JK02_24S: qualified for JK hardware 10.XG, firmware 10.07. Disconnect its existing Bluetooth bridge before connecting here.</small>
<label>PACE bridge Bluetooth address<input name="pace_address" maxlength="17" placeholder="AA:BB:CC:DD:EE:FF" autocomplete="off" spellcheck="false"></label>
<small>Use the brain_zero CAN-to-BLE bridge, with its existing CAN wiring.</small>
<label>JK capacity for combined SOC (Ah)<input name="jk_capacity_ah" type="number" min="0" max="10000" step="0.1"></label>
<small>Zero uses the capacity reported by the JK BMS.</small>
<label>PACE capacity for combined SOC (Ah)<input name="pace_capacity_ah" type="number" min="0" max="10000" step="0.1"></label>
<small>Enter the pack's usable capacity. CAN does not supply it in this profile.</small>
</section><section><h2>Battery control</h2>
<label>ESS SOC source<select name="soc_source"><option value="0">Existing source</option><option value="1">JK</option><option value="2">PACE over BLE</option><option value="3">Combined bank</option></select></label>
<label><input type="checkbox" name="charge_guard"> Enable battery charge limits</label>
<p>When enabled, these limits temporarily cap the MultiPlus charge request. They do not reconfigure the inverter or control other chargers.</p>
<label>Maximum SOC of either pack (%)<input name="max_soc" type="number" min="0" max="100" step="1"></label>
<label>Maximum cell voltage (mV)<input name="max_cell_mv" type="number" min="0" max="4500" step="1"></label>
<label>Maximum cell voltage difference (mV)<input name="max_delta_mv" type="number" min="0" max="1000" step="1"></label>
<label>Charge power limit when reached (W)<input name="limited_charge_w" type="number" min="0" max="5000" step="1"></label>
<small>Zero pauses requested charging. Fresh SOC, cells, charging permission and protection status from both packs are required; missing data pauses charging while this feature is enabled. Set all three thresholds before enabling it.</small>
<p id="guard-status" role="status"></p></section><button>Save</button><output id="message" role="status"></output></form>
<p><a href="/diagnostics">Diagnostics</a></p></main><script>
'use strict';const $=id=>document.getElementById(id),form=$('battery');let loaded=false,busy=false,edited=false;
const tr=s=>window.bzText?.(s)||s;
const rows=[['State of charge','soc','%',1],['Battery voltage','voltage','V',2],['Battery current','current','A',2],['Minimum cell voltage','min_cell_v','V',3],['Maximum cell voltage','max_cell_v','V',3],['Cell voltage difference','delta_cell_v','mV',0,1000],['Maximum battery temperature','temp_max_c','°C',1]];
for(const row of rows){const r=document.createElement('tr'),label=document.createElement('th');label.textContent=tr(row[0]);r.append(label);for(const p of ['jk','pace','bank']){const td=document.createElement('td');td.id=p+'-'+row[1];td.dataset.noTranslate='';r.append(td);} $('readings').append(r);}
form.addEventListener('input',()=>edited=true);
async function api(data){const r=await fetch('/api/bms',data?{method:'POST',body:new URLSearchParams(data),signal:AbortSignal.timeout(12000)}:{cache:'no-store',signal:AbortSignal.timeout(5000)});if(r.status===401){location.replace('/login');throw Error('Sign in');}const j=await r.json();if(!r.ok)throw Error(j.error||'Request failed');return j;}
function paint(j){if(!loaded||!edited){for(const [k,v] of Object.entries(j.settings)){const e=form.elements[k];if(e)e.type==='checkbox'?e.checked=!!v:e.value=v;}loaded=true;}
for(const [label,key,unit,dp,mult=1] of rows)for(const p of ['jk','pace','bank']){const v=j[p][key];$(p+'-'+key).textContent=typeof v==='number'?(v*mult).toFixed(dp)+' '+unit:'–';}
$('bank-note').textContent=tr(j.bank.complete?'Both packs are reporting':'Combined bank incomplete');$('guard-status').textContent=tr(j.guard.reason);}
async function read(){if(busy||document.hidden)return;busy=true;try{paint(await api());}catch(e){$('message').textContent=tr(e.message);}finally{busy=false;}}
form.addEventListener('submit',async e=>{e.preventDefault();if(busy)return;busy=true;form.querySelector('button').disabled=true;const data=Object.fromEntries(new FormData(form));for(const k of ['enabled','charge_guard'])data[k]=form.elements[k].checked?'1':'0';try{const j=await api(data);edited=false;paint(j);$('message').textContent=tr('Saved');}catch(e){$('message').textContent=tr(e.message);}finally{busy=false;form.querySelector('button').disabled=false;}});
read();setInterval(read,3000);
</script></body></html>)HTML";
