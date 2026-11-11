// Inverter settings file workflow (/multi). Downloads settings or a compatibility profile,
// previews uploaded settings, and applies them only on confirmation with inverter readback.
// Setting labels in N derive from kylehart/mk2vsc, docs/FIELDS.md (MIT License),
// Copyright (c) 2026 Kyle Hart; see third_party/mk2vsc_LICENSE.txt.
#pragma once

#include <pgmspace.h>
#include "site_nav.h"

static const char MULTI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Inverter settings · brain_zero</title><link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg"><link rel="stylesheet" href="/brand/brand.css">
<style>
:root{--bg:#f5f6f8;--card:#fff;--ink:#1d2430;--muted:#586574;--line:#e2e5ea;--good:#167044;--warn:#965b00;--bad:#b42318;--a:#2463eb;--soft:#eef4ff}
@media(prefers-color-scheme:dark){:root{--bg:#101318;--card:#191e25;--ink:#e6e9ee;--muted:#a1adbb;--line:#303945;--good:#65d69a;--warn:#edbd65;--bad:#ff9b92;--a:#8bb8ff;--soft:#202c3e}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
a{color:var(--a);text-underline-offset:3px}main{max-width:800px;margin:auto;padding:24px 24px 40px}
h1{font-size:30px;line-height:1.2;letter-spacing:-.025em;margin:0 0 10px}h2{font-size:20px;line-height:1.3;margin:0 0 8px}h3{font-size:15px;margin:0 0 8px}
p{margin:0 0 16px}.intro{color:var(--muted);margin-bottom:8px}.device{font-size:13px;color:var(--muted);margin-bottom:24px;min-height:20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:24px;margin-bottom:20px;min-width:0}
.hint{color:var(--muted);font-size:13px}.bar{display:flex;flex-wrap:wrap;gap:12px;align-items:center}.instructions[open] ol{margin-top:12px}
ol{padding-left:22px;margin:0 0 12px}li{padding-left:3px;margin:6px 0}.notice{margin:12px 0 0;overflow-wrap:anywhere}.notice:empty{display:none}
.good{color:var(--good)}.warn{color:var(--warn)}.bad{color:var(--bad)}.file{margin:16px 0 0;padding:12px 14px;background:var(--bg);border-radius:8px;overflow-wrap:anywhere}.filename{font-weight:600}
button,.button{font:inherit;font-weight:600;color:var(--ink);padding:10px 18px;border:1px solid var(--line);border-radius:8px;background:var(--card);cursor:pointer;min-height:44px}
button.primary{background:#2463eb;border-color:#2463eb;color:white}button:disabled{opacity:.45;cursor:default}button:not(:disabled):hover,.button:hover{filter:brightness(.94)}
.button{text-decoration:none}.profile-submit-note{margin:12px 0 0}
:focus-visible{outline:3px solid var(--a);outline-offset:3px}input[type=file]{display:block;width:100%;max-width:100%;font:inherit;color:var(--muted)}
input::file-selector-button{font:inherit;color:var(--ink);padding:10px 14px;border:1px solid var(--line);border-radius:8px;background:var(--bg);margin-right:12px;cursor:pointer;min-height:44px}
progress{display:block;width:100%;height:8px;accent-color:#2463eb;margin-top:16px}details{margin:12px 0 16px}summary{cursor:pointer;color:var(--a);font-size:14px}
.wrap{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:13px;margin-top:10px}th,td{text-align:left;padding:9px 6px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:600}td.value{white-space:nowrap;font-variant-numeric:tabular-nums}.apply{margin-top:18px}footer{color:var(--muted);font-size:13px;padding:0 4px}code{font:inherit;font-weight:600}
[hidden]{display:none!important}@media(max-width:520px){header{padding:16px 20px;gap:14px}main{padding:18px 16px 28px}.card{padding:20px}h1{font-size:27px}nav{gap:8px 16px}.bar button.primary{width:100%}}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(

<main>
<h1>Inverter settings</h1><p class="intro">Read your MultiPlus-II setup or back up its settings.</p>
<div class="device" id="device" role="status">Connecting to brain_zero…</div>
<section class="card" aria-labelledby="setup-title">
<h2 id="setup-title">Inverter setup</h2>
<p class="hint">Read the connected MultiPlus-II to check its model, charge settings and ESS Assistant.</p>
<div class="bar"><button class="primary" id="inspect" disabled>Read inverter setup</button></div>
<progress id="inspect-progress" max="100" value="0" aria-label="Reading settings" hidden></progress>
<p class="notice hint" id="setup-status" role="status" aria-live="polite"></p>
<p class="notice bad" id="inspect-msg" role="alert"></p>
<div class="wrap" id="setup-values" hidden><table><tbody id="setup-rows"></tbody></table></div>
<details id="direct-edit" class="instructions" open><summary>Change settings in VEConfigure</summary>
<ol><li>Disconnect brain_zero from VE.Bus and connect the MultiPlus-II to a Windows PC with an MK3-USB.</li>
<li>Make your changes in <a href="https://www.victronenergy.com/media/pg/VEConfigure_Manual/en/introduction-and-requirements.html" target="_blank" rel="noopener noreferrer">VEConfigure</a> and select <strong>Send settings</strong>.</li>
<li>Reconnect brain_zero to VE.Bus and select <strong>Read inverter setup</strong> again.</li></ol>
</details></section>
<section class="card" aria-labelledby="download-title">
<h2 id="download-title">Download settings</h2>
<p class="hint">Get a fresh <code>.rvsc</code> file from your MultiPlus. Downloading does not change its settings.</p>
<div class="bar"><button class="primary" id="download" disabled>Download settings</button></div>
<progress id="download-progress" max="100" value="0" aria-label="Reading settings" hidden></progress>
<div class="notice hint" id="download-msg" role="status" aria-live="polite"></div>
<details id="offline-edit" class="instructions" hidden><summary>Edit a copy in VEConfigure</summary>
<ol><li>Keep an untouched copy of the download as your backup.</li><li>Open a copy in <a href="https://www.victronenergy.com/support-and-downloads/software" target="_blank" rel="noopener noreferrer">VEConfigure</a> on your Windows PC and make your changes.</li><li>Close VEConfigure and confirm saving changes. Upload the edited <code>.rvsc</code> file below.</li></ol>
</details></section>
<section id="upload-section" class="card" aria-labelledby="upload-title" hidden>
<h2 id="upload-title">2. Upload settings</h2>
<p class="hint">Choose the edited <code>.rvsc</code> for this MultiPlus. You can review the changes before applying them.</p>
<input type="file" id="upfile" accept=".rvsc" aria-label="Choose edited RVSC settings file" disabled>
<div class="file" id="upinfo" hidden><div class="filename" id="filename"></div><div class="hint" id="filedetail"></div></div>
<div class="notice bad" id="upload-msg" role="alert"></div>
<div id="updiff"></div>
<div class="bar apply"><button class="primary" id="upwrite" disabled>Apply settings</button><button id="upcheck" hidden>Check file</button><button id="upclear" hidden>Remove file</button><button id="upcancel" hidden>Cancel before writing</button></div>
<div class="notice hint" id="upstate" role="status" aria-live="polite"></div>
<p class="hint" style="margin:16px 0 0">brain_zero checks compatibility, applies the changes and reads them back from the MultiPlus to confirm. Protected changes use the authorization saved by VEConfigure in your file.</p>
</section>
<section class="card" aria-labelledby="profile-title">
<h2 id="profile-title">Compatibility profile</h2>
<p class="hint">Share your inverter’s firmware and settings limits to help us verify compatibility.</p>
<div class="bar"><button class="primary" id="profile" disabled>Download compatibility profile</button>
<button id="submit" disabled>Submit to developer</button></div>
<p class="hint profile-submit-note">Submit your compatibility profile to brain-zero.com. No GitHub account is needed.</p>
<p class="hint"><a href="https://brain-zero.com/compatibility/" target="_blank" rel="noopener noreferrer">Open the submission page</a></p>
<progress id="profile-progress" max="100" value="0" aria-label="Reading settings" hidden></progress>
<div class="notice hint" id="profile-msg" role="status" aria-live="polite"></div>
<progress id="submit-progress" max="100" value="0" aria-label="Reading settings" hidden></progress>
<div class="notice hint" id="submit-msg" role="status" aria-live="polite"></div>
</section>
<footer>Use <a href="/inverter">Inverter controls</a> or <a href="/ess">ESS settings</a> for everyday adjustments. See <a href="/diagnostics">Diagnostics</a> for connection and VE.Bus health.</footer>
</main>
<script>
const N=[["Flags0","bitmask"],["Flags1","bitmask"],["Charger \u203a Charge curve \u203a Absorption voltage","V"],["Charger \u203a Charge curve \u203a Float voltage","V"],["Charger \u203a Charge curve \u203a Charge current","A"],["Inverter \u203a General \u203a Inverter output voltage","V"],["General \u203a Shore limit \u203a AC input current limit","A"],["Charger \u203a Absorption timing \u203a Repeated absorption time (probable)","min"],["Charger \u203a Absorption timing \u203a Repeated absorption interval (probable)","min"],["Charger \u203a Absorption timing \u203a Maximum absorption time","min"],["Charger \u203a Charge curve \u203a Charge curve",""],["Inverter \u203a General \u203a DC input low shut-down","V"],["UBatLow hysteresis for Inverter","V"],["Number of slaves connected",""],["Special three phase setting",""],["Virtual switch \u203a Usage \u203a Specify virtual switch usage",""],["Virtual switch \u203a A: Set VS ON \u203a when load higher than (probable)","A"],["Virtual switch \u203a A: Set VS ON \u203a when Udc higher than (probable)","V"],["Virtual switch \u203a A: Set VS ON \u203a when Udc lower than (probable)","V"],["vstonIInvHigh","s"],["vstonUBatHigh","s"],["vstonUBatLow","s"],["Virtual switch \u203a A: Set VS ON \u203a when not charging for (probable)","s"],["Virtual switch \u203a A: Set VS ON \u203a when fan on for (probable)","s"],["vstonTemperatureAlarm","s"],["vstonLowBatteryAlarm","s"],["vstonOverloadAlarm","s"],["vstonUBatRippleAlarm","s"],["Virtual switch \u203a B: Set VS OFF \u203a when load lower than (probable)","A"],["Virtual switch \u203a B: Set VS OFF \u203a when Udc higher than (probable)","V"],["Virtual switch \u203a B: Set VS OFF \u203a when Udc lower than (probable)","V"],["vstoffIInvLow","s"],["vstoffUBatHigh","s"],["vstoffUBatLow","s"],["Virtual switch \u203a B: Set VS OFF \u203a when charging for (probable)","s"],["Virtual switch \u203a B: Set VS OFF \u203a when fan off for (probable)","s"],["Virtual switch \u203a B: Set VS OFF \u203a when bulk charge finished for","min"],["Virtual switch \u203a B: Set VS OFF \u203a when no VS ON condition for","min"],["Virtual switch \u203a B: Set VS OFF \u203a when no AC input for (probable)","s"],["vstoffTemperatureAlarm","s"],["vstoffLowBatteryAlarm","s"],["vstoffOverloadAlarm","s"],["vstoffUBatRippleAlarm","s"],["vsMinimumOnTime","min"],["Grid \u203a Transfer switch \u203a AC low disconnect (probable)","V"],["Hysteresis for parameter 44","V"],["Grid \u203a Transfer switch \u203a AC high disconnect (probable)","V"],["Hysteresis for parameter 46","V"],["Inverter \u203a PowerAssist \u203a Assist current boost factor",""],["IMainsLimit (AC2)","A"],["Inverter \u203a enable AES \u203a Start AES when load lower than (probable)","A"],["Hysteresis on AES current limit","A"],["vs2onILoadHigh","A"],["vs2onILoadHigh (time)","s"],["vs2onUBatLow","V"],["vs2onUBatLow (time)","s"],["vs2offILoadLow","A"],["vs2offILoadLow (time)","min"],["vs2offUBatHigh","V"],["vs2offUBatHigh (time)","min"],["Flags2","bitmask"],["Flags3","bitmask"],["Virtual switch \u203a VS options \u203a Make period time","Hz"],["UBat low pre-alarm offset","V"],["General \u203a Enable battery monitor \u203a Battery capacity","Ah"],["General \u203a Enable battery monitor \u203a State of charge when Bulk finished","%"],["fsUBatStart","V"],["fsUBatStartDelay","s"],["fsUBatStop","V"],["fsUBatStopDelay","s"],["vs2StartOnSOC","%"],["vllTempCompensation",""],["General \u203a Enable battery monitor \u203a Charge efficiency",""],["IInvLimitDuringAssist","A"],["AbsToFloatSOCReset","%"],["IBatForStopAbsorption",""],["InvertRedundancy",""],["ExpectedPresenceFlags0","bitmask"],["ExpectedPresenceFlags2","bitmask"],["RelativePhaseInfo",""],["MyShortID",""],["Grid \u203a Grid code selection \u203a Country / grid code standard (probable)",""],["PermanentFlags0","bitmask"],["Inverter \u203a shut-down on SOC \u203a SOC low shut-down (probable)","%"],["Inverter \u203a shut-down on SOC \u203a SOC low restart (probable)","%"],["InfoID0",""],["Charger \u203a Temperature / Lithium \u203a Stop charger below",""],["Charger \u203a Temperature / Lithium \u203a Temperature compensation",""],["UBatDontCharge","V"],["CurrentSensorFactor",""]];
const $=id=>document.getElementById(id);
const esc=t=>String(t).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"})[c]);
const t=text=>typeof text==="string"?(window.bzText?.(text)??text):text;
const fmt=(template,values)=>window.bzFormat?.(template,values)??template.replace(/\{(\w+)\}/g,(match,key)=>values[key]??match);
let data=null,up=null,connected=false,action=false,pendingDownload=null,pendingInspection=false,timer=null,refreshing=null;
const downloads={
    download:{label:"Download settings",url:"/api/settings.rvsc",
        filename:()=>"multi-settings-"+data.serial+"-"+stamp()+".rvsc",
        incomplete:"Could not read a complete settings file. {reason}",success:"Settings downloaded. Keep a copy as your backup."},
    profile:{label:"Download compatibility profile",url:"/api/compatibility.json",
        filename:()=>"compatibility.json",
        incomplete:"Could not read a complete compatibility profile. {reason}",success:"Compatibility profile downloaded."},
    submit:{label:"Submit to developer",url:"/api/compatibility.json",
        incomplete:"Could not read a complete compatibility profile. {reason}"},
};
const submissionOrigin="https://brain-zero.com";
let submission=null;
function finishSubmission(text,kind="hint"){
    if(submission)clearTimeout(submission.timeout);
    submission=null;
    message("submit-msg",text,kind);
    render();
}
function sendSubmission(){
    if(!submission?.ready||!submission.profile)return;
    if(submission.window.closed){
        finishSubmission("Open the submission page and upload compatibility.json.","warn");
        return;
    }
    submission.window.postMessage({type:"brain-zero-profile",nonce:submission.nonce,profile:submission.profile},submissionOrigin);
}
window.addEventListener("message",event=>{
    if(!submission||event.origin!==submissionOrigin||event.source!==submission.window||
        event.data?.nonce!==submission.nonce)return;
    if(event.data.type==="brain-zero-profile-ready"){
        submission.ready=true;
        sendSubmission();
    }else if(event.data.type==="brain-zero-profile-received"&&submission.profile){
        finishSubmission("Compatibility profile ready. Review it on brain-zero.com.","good");
    }
});
function message(id,text,kind="hint"){const el=$(id);el.textContent=t(text);el.className="notice "+kind}
async function json(path,options){const r=await fetch(path,{cache:"no-store",...options});const j=await r.json();if(!r.ok||j.ok===false)throw Error(j.error||"The request was refused.");return j}
function stamp(){const d=new Date(),p=n=>String(n).padStart(2,"0");return d.getFullYear()+"-"+p(d.getMonth()+1)+"-"+p(d.getDate())+"-"+p(d.getHours())+p(d.getMinutes())+p(d.getSeconds())}
function save(blob,name){const a=document.createElement("a"),url=URL.createObjectURL(blob);a.href=url;a.download=name;document.body.appendChild(a);a.click();setTimeout(()=>{URL.revokeObjectURL(url);a.remove()},1000)}
function renderDownloads(busy){
    for(const [id,download] of Object.entries(downloads)){
        const reading=pendingDownload===id;
        $(id).disabled=!connected||busy||(id==="submit"&&!!submission);
        $(id).textContent=t(reading?"Reading settings…":download.label);
        $(id+"-progress").hidden=!reading;
        $(id+"-progress").value=reading&&data?.active?data.progress:0;
        if(reading&&data?.active){
            message(id+"-msg",fmt("Reading settings from the MultiPlus… {progress}%",{progress:data.progress}));
        }
    }
}
async function finishDownload(id){
    const download=downloads[id],channel=id+"-msg";
    if(data.inspection||!data.file){
        const text=fmt(download.incomplete,{reason:t(data.file_problem)});
        if(id==="submit")finishSubmission(text,"bad");
        else message(channel,text,"bad");
        return;
    }
    try{
        const response=await fetch(download.url,{cache:"no-store"});
        if(!response.ok){
            const result=await response.json().catch(()=>({}));
            throw Error(result.error||"Download failed.");
        }
        if(id==="submit"){
            const profile=await response.json();
            if(!submission)throw Error("Open the submission page and upload compatibility.json.");
            if(!profile?.profile||typeof profile.grid!=="string"||
                Object.keys(profile).sort().join(",")!=="grid,profile")throw Error("The request was refused.");
            submission.profile=profile;
            message(channel,"Preparing the submission…");
            sendSubmission();
        }else{
            save(await response.blob(),download.filename());
            message(channel,download.success,"good");
        }
    }catch(error){
        if(id==="submit")finishSubmission(error.message||"Download failed. Please try again.","bad");
        else message(channel,error.message||"Download failed. Please try again.","bad");
    }
}
function value(id,raw){const s=data&&data.settings.find(s=>s[0]===id),n=N[id];if(!s||s[1]!==2||!n)return fmt("{value} (raw)",{value:raw});
if(n[1]==="bitmask")return "0x"+raw.toString(16).toUpperCase().padStart(4,"0");const v=s[3]<0?(raw+s[4])/-s[3]:s[3]>0?(raw+s[4])*s[3]:raw;const value=String(+v.toFixed(4));return n[1]?fmt("{value} {unit}",{value,unit:n[1]}):value}
function renderSetup(busy,canUpload){
    const setup=connected?data?.setup:null;
    const reading=pendingInspection||setup?.status==="reading";
    const ready=!reading&&setup?.status==="ready";
    $("inspect").disabled=!connected||busy;
    $("inspect").textContent=t(reading?"Reading settings…":"Read inverter setup");
    $("inspect-progress").hidden=!reading;
    $("inspect-progress").value=reading&&data?.inspection?data.progress:0;
    $("direct-edit").hidden=canUpload;
    $("setup-values").hidden=!ready;
    const status={ready:"Inverter setup read.",incomplete:setup?.problem||"Some setup settings could not be read. Try reading the inverter again.",
        stale:"Read the inverter setup again to refresh these settings.",unsupported:"Inverter setup is available for supported standalone MultiPlus-II units."};
    const summary=reading?"Reading settings from the MultiPlus…":status[setup?.status]??"";
    if($("setup-status").textContent!==t(summary))message("setup-status",summary);
    if(!ready){$("setup-rows").replaceChildren();return}

    const rows=[["Inverter",setup.model],["Nominal DC voltage",fmt("{value} {unit}",{value:setup.nominal_voltage,unit:"V"})],
        ["ESS Assistant",t(setup.ess_setpoint_ram?"Detected":"Unavailable")]];
    for(const [id,label] of [[2,"Absorption voltage"],[3,"Float voltage"],[4,"Charge current"],
        [6,"AC input current limit"],[64,"Battery capacity"]]){
        const setting=data.settings.find(s=>s[0]===id);
        rows.push([label,setting?.[1]===2?value(id,setting[2]):t("Unavailable")]);
    }
    const body=rows.map(([label,result])=>'<tr><th scope="row">'+esc(t(label))+'</th><td class="value">'+esc(result)+'</td></tr>').join("");
    if($("setup-rows").innerHTML!==body)$("setup-rows").innerHTML=body;
}
function render(){const busy=action||pendingDownload||pendingInspection||!!(data&&data.active)||!!(up&&up.busy);
const canUpload=connected&&data?.setup?.can_upload===true;
const currentSerial=data?.serial&&data.setup?.status!=="stale"&&(!data.inspection||data.setup?.status==="ready");
$("device").textContent=connected?(currentSerial?fmt("Serial {serial} · Firmware {firmware}",data):"Connected to brain_zero"):"Connecting to brain_zero…";
renderSetup(busy,canUpload);
$("download-title").textContent=t(canUpload?"1. Download settings":"Download settings");
$("offline-edit").hidden=!canUpload;
$("upload-section").hidden=!canUpload&&!(up&&up.state!=="empty");
renderDownloads(busy);
$("upfile").disabled=!canUpload||busy;
if(!up)return;
const c=up.check,loaded=up.state!=="empty",changes=c.settings.length||c.assistants;
$("upinfo").hidden=!loaded;$("filename").textContent=loaded?up.file.name:"";
$("filedetail").textContent=loaded?fmt(up.file.credentials?"Serial {serial} · Firmware {firmware} · Embedded authorization":"Serial {serial} · Firmware {firmware}",up.file):"";
$("upclear").hidden=!loaded;$("upclear").disabled=!connected||busy;
$("upcheck").hidden=!(canUpload&&loaded&&!c.done&&!up.read.active&&!up.busy);$("upcheck").disabled=!canUpload||busy;
$("upcancel").hidden=up.state!=="reading";$("upcancel").disabled=!connected||action;
$("upwrite").disabled=!(canUpload&&loaded&&c.done&&c.ok&&changes&&up.bus.master&&!busy);
let h="",st="",kind="hint";const expanded=$("updiff").querySelector("details")?.open;
if(loaded&&!up.busy){
if(!c.done){st=up.read.active?"Checking the file against your MultiPlus…":"Read the MultiPlus to check this file."}
else if(!c.ok){st=fmt("Cannot apply this file. {reason}",{reason:t(c.why)});kind="bad"}
else if(!changes){st="Already matches your MultiPlus. No changes needed.";kind="good"}
else{const count=c.settings.length;
h='<p class="notice">'+esc(count?fmt(count===1?"{count} setting will change.":"{count} settings will change.",{count}):t("The assistant configuration will change."))+"</p>";
if(c.protected)h+='<p class="notice hint">Protected settings will use the file’s embedded VEConfigure authorization.</p>';
if(c.restart||c.assistants)h+='<p class="notice warn">'+esc([count&&c.assistants?t("The assistant configuration will also change."):"",t("Applying this file restarts the MultiPlus and interrupts AC-out. Keep brain_zero powered during the restart.")].filter(Boolean).join(" "))+"</p>";
if(c.removes_assistants)h+='<p class="notice bad"><strong>This file removes all assistants, including ESS.</strong></p>';
if(count)h+='<details'+(expanded?' open':'')+'><summary>Review settings changes</summary><div class="wrap"><table><thead><tr><th>Setting</th><th>Current setting</th><th>In the file</th></tr></thead><tbody>'+c.settings.map(s=>'<tr><td>'+esc(N[s[0]]?t(N[s[0]][0]):fmt("Setting {id}",{id:s[0]}))+'</td><td class="value">'+esc(value(s[0],s[1]))+'</td><td class="value">'+esc(value(s[0],s[2]))+'</td></tr>').join("")+"</tbody></table></div></details>";
if(!up.bus.master){st="Unplug the MK3 from VE.Bus before applying settings. brain_zero must have control of the inverter.";kind="warn"}
}}
if(loaded&&!up.busy&&!canUpload){h="";st="Use VEConfigure to change settings on this inverter.";kind="warn"}
if(up.state==="reading")st="Reading the current settings before applying changes…";
else if(up.state==="writing")st=up.write.step==="authorizing"?"Checking the file’s authorization with the MultiPlus…":fmt("Applying settings… {progress}%",{progress:up.write.progress});
else if(up.state==="verifying")st="Reading settings back from the MultiPlus to verify the changes…";
else if(up.state==="done"){st=up.message;kind="good"}
else if(up.state==="failed"){st=up.message;kind="bad"}
$("updiff").innerHTML=h;message("upstate",st,kind);
}
// One timer; cached status only. Physical reads start exclusively from a user action.
function refresh(){if(refreshing||action)return refreshing;clearTimeout(timer);
refreshing=(async()=>{try{[data,up]=await Promise.all([json("/api/settings"),json("/api/upload")]);connected=true;
if(pendingInspection&&!data.active)pendingInspection=false;
if(pendingDownload&&!data.active){
    await finishDownload(pendingDownload);
    pendingDownload=null;
}
}catch(e){connected=false;$("device").textContent="Connection lost. Retrying…";
if(pendingDownload)message(pendingDownload+"-msg","Connection lost. Waiting for brain_zero…","warn");
}finally{refreshing=null;render();if(!connected)$("device").textContent="Connection lost. Retrying…";
if(!connected||pendingDownload||pendingInspection||data&&data.active||up&&up.busy)timer=setTimeout(refresh,connected?1500:3000);
else if(!document.hidden&&data?.setup?.status==="ready")timer=setTimeout(refresh,5000)}})();return refreshing}
async function run(task,channel="upload-msg"){if(action)return;action=true;message(channel,"");render();try{await refreshing;await task()}catch(e){message(channel,e.message||"Could not reach brain_zero.","bad")}finally{action=false;await refresh()}}
$("inspect").addEventListener("click",()=>run(async()=>{await json("/api/settings/read?inspect=1",{method:"POST"});pendingInspection=true},"inspect-msg"));
for(const id of Object.keys(downloads).filter(id=>id!=="submit")){
    $(id).addEventListener("click",()=>run(async()=>{
        await json("/api/settings/read",{method:"POST"});
        pendingDownload=id;
        message(id+"-msg","Reading settings from the MultiPlus…");
    },id+"-msg"));
}
$("submit").addEventListener("click",()=>{
    if($("submit").disabled)return;
    const nonce=Array.from(crypto.getRandomValues(new Uint8Array(16)),byte=>byte.toString(16).padStart(2,"0")).join("");
    const target=window.open(submissionOrigin+"/compatibility/#from-controller="+nonce,"_blank");
    if(!target){message("submit-msg","Open the submission page and upload compatibility.json.","warn");return}
    submission={window:target,nonce,ready:false,profile:null,timeout:setTimeout(()=>{
        if(pendingDownload==="submit")pendingDownload=null;
        finishSubmission("The submission page did not respond. Download compatibility.json and upload it on brain-zero.com.","warn");
    },300000)};
    run(async()=>{
        await json("/api/settings/read",{method:"POST"});
        pendingDownload="submit";
        message("submit-msg","Reading settings from the MultiPlus…");
    },"submit-msg").then(()=>{
        if(submission&&!pendingDownload&&!submission.profile){
            clearTimeout(submission.timeout);
            submission=null;
            render();
        }
    });
});
$("upfile").addEventListener("change",()=>{const f=$("upfile").files[0];if(!f)return;
if(!/\.rvsc$/i.test(f.name)){message("upload-msg","Choose the .rvsc file saved by VEConfigure.","bad");$("upfile").value="";return}
run(async()=>{const fd=new FormData();fd.append("file",f,f.name);await json("/api/upload",{method:"POST",body:fd})})});
$("upcheck").addEventListener("click",()=>run(()=>json("/api/settings/read",{method:"POST"})));
$("upclear").addEventListener("click",()=>run(async()=>{await json("/api/upload/clear",{method:"POST"});$("upfile").value=""}));
$("upcancel").addEventListener("click",()=>run(()=>json("/api/upload/cancel",{method:"POST"})));
$("upwrite").addEventListener("click",()=>{if($("upwrite").disabled)return;const c=up.check,n=c.settings.length;
const question=c.assistants?(n?fmt(n===1?"Apply {count} setting and the assistant configuration to your MultiPlus?":"Apply {count} settings and the assistant configuration to your MultiPlus?",{count:n}):t("Apply the assistant configuration to your MultiPlus?")):
fmt(n===1?"Apply {count} setting to your MultiPlus?":"Apply {count} settings to your MultiPlus?",{count:n});
const notes=[question,(c.restart||c.assistants)?t("The MultiPlus will restart and AC-out will be interrupted. Keep brain_zero powered."):"",c.removes_assistants?t("This file removes all assistants, including ESS."):"",t("Settings will be read back to verify the changes.")];
if(!confirm(notes.filter(Boolean).join("\n\n")))return;
run(()=>json("/api/upload/write",{method:"POST"}))});
window.addEventListener("focus",()=>refresh());
document.addEventListener("visibilitychange",()=>{if(!document.hidden)refresh()});
document.addEventListener("DOMContentLoaded",refresh,{once:true});
</script></body></html>
)HTML";
