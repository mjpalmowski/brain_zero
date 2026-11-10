#pragma once
#include <pgmspace.h>
#include "site_nav.h"
#include "language_assets.h"

#define DEVICE_HEAD R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">)HTML" R"HTML(<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'; form-action 'self'">)HTML" R"HTML(<link rel="stylesheet" href="/brand/brand.css"><link rel="icon" href="/brand/brain-zero-icon.svg">)HTML" R"HTML(<link rel="stylesheet" href="/ui/settings.css">)HTML"

static const char DEVICE_CSS[] PROGMEM = R"CSS(:root{color-scheme:light dark;--bg:#f5f6f8;--card:#fff;--ink:#1d2430;--muted:#546173;--line:#dce2ea;--a:#2463eb}
@media(prefers-color-scheme:dark){:root{--bg:#101318;--card:#191e25;--ink:#e6e9ee;--muted:#a5afbe;--line:#333d4c;--a:#a0c5ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,sans-serif}
main{max-width:760px;margin:auto;padding:18px 16px 32px}h1{font-size:26px;margin:4px 0 12px}h2{font-size:19px;margin:0 0 12px}h3{font-size:16px;margin:12px 0 4px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:16px 0;padding:16px}
form{display:grid;gap:8px}form label{display:grid;gap:3px}input,select,button{font:inherit;color:inherit;border:1px solid var(--line);border-radius:6px;padding:8px;background:var(--bg)}
input:not([type=checkbox]),select{width:min(100%,360px)}button{cursor:pointer;width:max-content;max-width:100%}button:disabled{opacity:.55;cursor:default}
a{color:var(--a)}p{margin:6px 0 12px}.note,small{color:var(--muted)}.actions{display:flex;gap:8px;flex-wrap:wrap}.info{overflow-wrap:anywhere}
dl{display:grid;grid-template-columns:minmax(105px,auto) 1fr;gap:3px 10px;margin:8px 0 14px}dd{margin:0;overflow-wrap:anywhere}
.advanced{border-top:1px solid var(--line);margin-top:12px;padding-top:12px}.advanced summary{cursor:pointer;font-weight:600}
[hidden]{display:none!important}output{display:block;margin-top:6px;overflow-wrap:anywhere}#token-value{font:14px monospace;direction:ltr;user-select:all}
)CSS";

static const char DEVICE_HTML[] PROGMEM =
DEVICE_HEAD R"HTML(<title>Settings · brain_zero</title></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(
<main><h1>Settings</h1>
<section><h2>Connectivity</h2><h3>Wi-Fi</h3>
<dl><dt>Wi-Fi network</dt><dd id="wifi-current" dir="auto" data-no-translate>–</dd><dt>IP address</dt><dd id="ip" dir="ltr" data-no-translate>–</dd></dl>
<form id="wifi" autocomplete="off">
<label>Wi-Fi network (SSID)<input name="ssid" maxlength="32" required spellcheck="false" dir="auto"></label>
<label>Wi-Fi password<input name="password" type="password" maxlength="64" autocomplete="new-password"></label>
<small>Leave the password blank to keep it for the current network. Reopen <a href="http://brain-zero.local/settings">brain-zero.local</a> after switching Wi-Fi.</small>
<button>Test connection</button>
</form>
<div id="wifi-trial" hidden>
<p>Keep this network within 90 seconds, or the previous network will be restored.</p>
<p id="wifi-seconds" role="status"></p>
<div class="actions"><button id="wifi-keep">Keep network</button><button id="wifi-revert">Restore previous network</button></div>
</div>
<output id="wifi-message" role="status"></output>
<h3>MQTT</h3><p><a href="/settings/mqtt">MQTT and Home Assistant</a></p></section>

<section><h2>Remote Console</h2><p id="password-state" role="status"></p>
<form id="password" autocomplete="off">
<label id="current-label">Current password<input name="current" type="password" maxlength="64" autocomplete="current-password"></label>
<label>New password<input name="password" type="password" minlength="12" maxlength="64" autocomplete="new-password" required></label>
<label>Confirm password<input name="confirm" type="password" minlength="12" maxlength="64" autocomplete="new-password" required></label>
<small>Use 12–64 bytes. MQTT has its own broker credentials.</small>
<div class="actions"><button>Change password</button><button type="button" id="password-disable" hidden>Disable password</button><button type="button" id="logout" hidden>Sign out</button></div>
</form>
<output id="password-message" role="status"></output>
<details class="advanced"><summary>HTTP API token</summary>
<p class="note">For trusted clients of the HTTP API. It is shown once. A new token replaces the previous one.</p>
<form id="token" autocomplete="off">
<label id="token-current-label">Current password<input name="current" type="password" maxlength="64" autocomplete="current-password"></label>
<button>Generate API token</button></form>
<output id="token-value" aria-label="API token" hidden></output><output id="token-message" role="status"></output>
</details>
</section>

<section><h2>General</h2>
<form id="language"><label>Language<select name="code">)HTML"
BRAIN_ZERO_LANGUAGE_OPTIONS
R"HTML(</select></label><button>Save</button></form><output id="language-message" role="status"></output>
</section></main>
<script>
'use strict';
const $=id=>document.getElementById(id);
const tr=s=>window.bzText?.(s)||s;
const fields=id=>$(id).elements;
const write=(id,msg)=>{$(id+'-message').textContent=tr(msg)};
let loaded=false, reading=false;
async function api(path,data){
  const r=await fetch('/api/device'+path,data?{method:'POST',body:new URLSearchParams(data),signal:AbortSignal.timeout(10000)}:{cache:'no-store',signal:AbortSignal.timeout(4000)});
  if(r.status===401){location.replace('/login');throw Error('Sign in');}
  const j=await r.json();
  if(!r.ok||j.ok===false)throw Error(j.error||'Request failed');
  return j;
}
function paint(j){
  $('wifi-current').textContent=j.wifi.connected?j.wifi.ssid:tr('Not connected');
  $('ip').textContent=j.wifi.ip||'–';
  $('wifi').hidden=j.wifi.testing;
  $('wifi-trial').hidden=!j.wifi.testing;
  $('wifi-keep').disabled=!j.wifi.confirmable;
  $('wifi-seconds').textContent=window.bzFormat?.('{seconds} s',{seconds:j.wifi.seconds})??j.wifi.seconds+' s';
  if(j.wifi.notice)write('wifi',j.wifi.notice);
  $('current-label').hidden=$('token-current-label').hidden=$('password-disable').hidden=$('logout').hidden=!j.password_enabled;
  fields('password').current.required=fields('token').current.required=j.password_enabled;
  $('password-state').textContent=tr(j.password_enabled?'Password enabled':'Password disabled');
  if(!loaded){fields('language').code.value=j.language;fields('wifi').ssid.value=j.wifi.ssid;loaded=true;}
}
async function refresh(){if(reading||document.hidden)return;reading=true;
  try{paint(await api(''))}catch{if(!loaded)write('wifi','Controller unavailable.')}
  finally{reading=false}}
function form(id, fn){$(id).addEventListener('submit',async e=>{e.preventDefault();const button=$(id).querySelector('button');button.disabled=true;
  try{await fn(fields(id))}catch(e){write(id,e.message)}finally{button.disabled=false}})}
form('wifi',async f=>{
  await api('/wifi',{ssid:f.ssid.value,password:f.password.value});
  f.password.value='';
  write('wifi','Connecting to the new network. Reopen Settings from the new network to keep it.');
});
async function wifiAction(path){try{await api('/wifi/'+path,{});await refresh()}catch(e){write('wifi',e.message)}}
$('wifi-keep').onclick=()=>wifiAction('keep');
$('wifi-revert').onclick=()=>wifiAction('revert');
form('password',async f=>{
  if(f.password.value!==f.confirm.value)throw Error('Passwords do not match');
  await api('/password',{current:f.current.value,password:f.password.value});
  $('password').reset();write('password','Password saved');await refresh();
});
$('password-disable').onclick=async()=>{
  if(!confirm(tr('Disable password protection?')))return;
  try{await api('/password',{current:fields('password').current.value,disable:1});$('password').reset();
    $('token-value').hidden=true;$('token-value').textContent='';write('password','Password disabled');await refresh()}
  catch(e){write('password',e.message)}
};
$('logout').onclick=async()=>{await fetch('/logout',{method:'POST',body:new URLSearchParams()});location.replace('/login')};
form('token',async f=>{
  $('token-value').hidden=true;$('token-value').textContent='';
  const j=await api('/token',{current:f.current.value});
  f.current.value='';$('token-value').textContent=j.token;$('token-value').hidden=false;
  write('token','Copy this token now');
});
form('language',async f=>{await api('/language',{code:f.code.value});location.reload()});
refresh();setInterval(refresh,2000);
</script></body></html>)HTML";

static const char LOGIN_HTML[] PROGMEM =
DEVICE_HEAD R"HTML(<title>Remote Console · brain_zero</title></head><body>)HTML"
BRAIN_ZERO_LOCALE_SCRIPT
R"HTML(<main><h1>brain_zero</h1><section><h2>Remote Console</h2>
<form id="login" method="post" action="/login">
<label>Password<input type="password" name="password" maxlength="64" autocomplete="current-password" required autofocus></label>
<button>Sign in</button><output id="login-message" role="alert"></output></form>
</section></main><script>
document.getElementById('login').addEventListener('submit',async e=>{
 e.preventDefault();const f=e.target,b=f.querySelector('button'),o=document.getElementById('login-message');b.disabled=true;
 try{const r=await fetch('/login',{method:'POST',body:new URLSearchParams(new FormData(f))});
 const j=await r.json();if(!j.ok)throw Error(j.error);location.replace('/');}
 catch(e){o.textContent=window.bzText?.(e.message)||e.message;f.elements.password.value='';f.elements.password.focus()}
 finally{b.disabled=false}
});
</script></body></html>)HTML";
