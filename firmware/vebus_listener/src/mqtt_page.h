// The MQTT page (/mqtt): the broker, the topics and Home Assistant discovery (mqtt_link.h), with the
// connection's state. Self-contained like the dashboard; reads GET /api/mqtt every 2 s for the status
// and saves with POST /api/mqtt (web.h).
#pragma once

#include <pgmspace.h>
#include "site_nav.h"

static const char MQTT_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MQTT and Home Assistant · brain_zero</title><link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg"><link rel="stylesheet" href="/brand/brand.css">
<style>
:root{--bg:#f5f6f8;--card:#fff;--ink:#1d2430;--muted:#687180;--line:#e2e5ea;--good:#1a7f37;--warn:#a65f00;--bad:#c62828;--a:#2463eb}
@media (prefers-color-scheme:dark){:root{--bg:#101318;--card:#191e25;--ink:#e6e9ee;--muted:#98a2ae;--line:#29303a;--good:#3fb950;--warn:#d29922;--bad:#f85149;--a:#6ea8fe}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
a{color:var(--a)}
header{display:flex;flex-wrap:wrap;gap:4px 16px;align-items:baseline;padding:18px 16px 0;max-width:900px;margin:auto}
h1{font-size:24px;margin:0}
main{padding:4px 16px 24px;max-width:900px;margin:auto}
h2{font-size:12px;margin:18px 0 8px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.05em}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:6px 12px;min-width:0}
.v{font-size:22px;font-weight:600}.s{font-size:12px;color:var(--muted)}.good{color:var(--good)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.row{display:flex;flex-wrap:wrap;align-items:center;gap:6px 10px;padding:9px 0;border-top:1px solid var(--line)}
.row:first-child{border-top:0}
.name{flex:1 1 230px;display:flex;gap:8px;align-items:center}
.hint{flex-basis:100%;font-size:12px;color:var(--muted)}
input,select,button{font:inherit;color:var(--ink)}
input[type=number],input[type=text],input[type=password]{padding:5px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg)}
input[type=number]{width:96px}input[type=text],input[type=password]{width:220px;max-width:100%}
input[type=checkbox]{width:18px;height:18px;margin:0}input:invalid{border-color:var(--bad)}
code{font-size:13px;overflow-wrap:anywhere}
.topics{margin-top:18px}.topics summary{cursor:pointer;font-weight:600}.topics .card{margin-top:8px}
.bar{position:sticky;bottom:0;background:var(--bg);padding:12px 0;display:flex;flex-wrap:wrap;gap:10px;align-items:center}
button{padding:7px 16px;border:1px solid var(--line);border-radius:6px;background:var(--card);cursor:pointer}
button[type=submit]{background:var(--a);border-color:var(--a);color:#fff}
footer{max-width:900px;margin:auto;padding:0 16px 20px;color:var(--muted);font-size:12px}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(
<header><h1>MQTT and Home Assistant</h1></header>
<main>
<h2>Connection</h2>
<div class="card"><div class="v" id="now" role="status">Connecting…</div>
<div class="s">Controller ID <code id="controller-id" dir="ltr">–</code></div><div class="s" id="connection-error"></div></div>
<form id="f" autocomplete="off" novalidate>
<h2>Broker</h2>
<div class="card">
<div class="row"><label class="name" for="enabled">Use MQTT</label><input type="checkbox" id="enabled" name="enabled"></div>
<div class="row"><label class="name" for="host">Broker</label><input type="text" id="host" name="host" maxlength="63" spellcheck="false" placeholder="mqtt-broker.local">
<label for="port" class="s">port</label><input type="number" id="port" name="port" min="1" max="65535" step="1" required>
<span class="hint">Use the same broker as Home Assistant.</span></div>
<div class="row"><label class="name" for="user">User name</label><input type="text" id="user" name="user" maxlength="47" spellcheck="false"></div>
<div class="row"><label class="name" for="password">Password</label><input type="password" id="password" name="password" maxlength="63" autocomplete="new-password">
<label class="s"><input type="checkbox" id="password_clear"> clear it</label>
<span class="hint" id="pw-hint">Leave the password blank to keep the one stored on the ESP32.</span></div>
</div>
<h2>External battery</h2>
<div class="card">
<div class="row"><label class="name" for="external_input">Use external battery data</label><input type="checkbox" id="external_input" name="external_input">
<span class="hint">Receive fresh battery SOC and external charging current over MQTT.</span></div>
<div class="row"><span class="name">External battery input topic</span><code id="battery-input-topic" dir="ltr">–</code></div>
</div>
<details class="topics" id="topics"><summary>Topics and discovery</summary>
<div class="card">
<div class="row"><label class="name" for="base">Base topic</label><input type="text" id="base" name="base" maxlength="47" spellcheck="false" required pattern="[A-Za-z0-9_\-]+(/[A-Za-z0-9_\-]+)*"></div>
<div class="row"><label class="name" for="discovery">Home Assistant discovery</label><input type="checkbox" id="discovery" name="discovery">
<label for="prefix" class="s">prefix</label><input type="text" id="prefix" name="prefix" maxlength="31" spellcheck="false" required pattern="[A-Za-z0-9_\-]+(/[A-Za-z0-9_\-]+)*">
<span class="hint">Announce the inverter and its readings to Home Assistant.</span></div>
<div class="row"><label class="name" for="interval_s">Publish every</label><input type="number" id="interval_s" name="interval_s" min="1" max="3600" step="1" required> s</div>
</div>
</details>
<div class="bar"><button type="submit">Save</button><button type="button" id="reload">Discard changes</button><span id="msg" class="s" role="status"></span></div>
</form>
</main>
<footer>Broker permissions control who can send commands. <a href="/diagnostics#mqtt-health">MQTT diagnostics</a></footer>
<script>
const $=id=>document.getElementById(id),F=$("f"),E=k=>F.elements[k];
const tr=text=>window.bzText?.(text)??text;
const format=(template,values)=>window.bzFormat?.(template,values)??template.replace(/\{(\w+)\}/g,(_,key)=>values[key]);
const fieldNames={enabled:"Use MQTT",host:"Broker",port:"port",user:"User name",password:"Password",base:"Base topic",discovery:"Home Assistant discovery",prefix:"prefix",interval_s:"Publish every",external_input:"Use external battery data"};
const refusalReasons={"user name or password not accepted":"User name or password not accepted","client id rejected":"Client ID rejected",refused:"Connection refused"};
function connectionError(error){
    const refused=/^the broker refused the connection: (.+) \(code (\d+)\)$/.exec(error);
    if(refused)return format("The broker refused the connection ({reason}, code {code}).",{reason:tr(refusalReasons[refused[1]]??refused[1]),code:refused[2]});
    const socket=/^no connection to (.+):(\d+) \(socket error (-?\d+)\)$/.exec(error);
    if(socket)return format("No connection to {host}:{port} (socket error {code}).",{host:socket[1],port:socket[2],code:socket[3]});
    return tr(error);
}
function saveError(error){
    const invalid=/^bad value for (\w+)$/.exec(error);
    return invalid ? format("Check the value for {field}.",{field:tr(fieldNames[invalid[1]]??invalid[1])}) :
        format("MQTT settings refused. {error}",{error:tr(error)});
}
function fill(j){const s=j.settings;
E("enabled").checked=!!s.enabled;E("host").value=s.host;E("port").value=s.port;E("user").value=s.user;E("password").value="";
$("password_clear").checked=false;E("password").placeholder=tr(s.password_set?"unchanged":"none");
E("base").value=s.base;E("discovery").checked=!!s.discovery;E("prefix").value=s.prefix;E("interval_s").value=s.interval_s;
E("external_input").checked=!!j.battery_input?.enabled}
function live(st){
 $("controller-id").textContent=st.victron_gx?.installation_id||"–";
$("now").textContent=tr({connected:"Connected",connecting:"Connecting…","not connected":"Not connected",off:"MQTT is off","no broker set":"No broker set"}[st.state]||st.state);
$("now").className="v"+(st.state=="connected"?" good":st.state=="off"?"":" warn");
$("connection-error").textContent=st.state=="off"?"":connectionError(st.error||"")}
async function load(full){const r=await fetch("/api/mqtt",{cache:"no-store"});if(!r.ok)throw Error("MQTT settings unavailable");
const j=await r.json();if(full)fill(j);live(j.status);$("battery-input-topic").textContent=j.battery_input?.topic||"–"}
function offline(){$("now").textContent="Controller unavailable";$("now").className="v warn";$("connection-error").textContent=""}
F.addEventListener("submit",async e=>{e.preventDefault();
const bad=[...F.elements].find(x=>x.willValidate&&!x.checkValidity());
if(bad){const details=bad.closest("details");if(details)details.open=true;bad.focus();$("msg").textContent="Check the highlighted field";return}
const b=new URLSearchParams();b.set("enabled",E("enabled").checked?1:0);b.set("discovery",E("discovery").checked?1:0);
b.set("external_input",E("external_input").checked?1:0);
["host","port","user","base","prefix","interval_s"].forEach(k=>b.set(k,E(k).value.trim()));
if(E("password").value)b.set("password",E("password").value);if($("password_clear").checked)b.set("password_clear",1);
$("msg").textContent="saving…";
try{const r=await fetch("/api/mqtt",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b});
const j=await r.json();
if(j.ok){$("msg").textContent=tr(j.message==="saved; MQTT is off"?"Settings saved. MQTT is off.":"Settings saved. Connecting to the broker.");load(true)}
else $("msg").textContent=saveError(j.error||"Request failed");
}catch(err){$("msg").textContent="no answer from the listener"}});
$("reload").addEventListener("click",()=>load(true).then(()=>$("msg").textContent="reloaded").catch(offline));
load(true).catch(offline);setInterval(()=>load(false).catch(offline),2000);
</script></body></html>
)HTML";
