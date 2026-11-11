#pragma once
#include <pgmspace.h>
#include "site_nav.h"
static const char FIRMWARE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'none'">
<title>Firmware update · brain_zero</title><link rel="stylesheet" href="/brand/brand.css">
<link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg">
<style>
:root{color-scheme:light dark;--bg:#f4f7f7;--card:#fff;--ink:#193239;--muted:#50686d;--line:#dce6e5;--accent:#087b6b;--soft:#edf7f3;--bad:#a53630}
@media(prefers-color-scheme:dark){:root{--bg:#101a1e;--card:#17272d;--ink:#e2efed;--muted:#a4b7b9;--line:#2e444a;--accent:#5fd6b7;--soft:#203a35;--bad:#ff978d}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,sans-serif}main{max-width:740px;padding:22px;margin:auto}

a{color:var(--accent)}h1{font-size:30px;line-height:1.2;margin:8px 0}h2{font-size:18px;margin:0 0 10px}p{margin:0 0 12px}
.card{padding:18px;border:1px solid var(--line);border-radius:12px;background:var(--card);margin:16px 0}
.status{padding:12px 15px;border-radius:8px;background:var(--soft);overflow-wrap:anywhere}
.muted{color:var(--muted)}.error{color:var(--bad)}.row{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin-top:12px}
button{font:inherit;font-weight:650;padding:8px 12px;border:1px solid var(--line);border-radius:7px;color:var(--ink);background:var(--card);cursor:pointer}
button.primary{background:var(--soft);border-color:var(--accent);color:var(--accent)}button:disabled{opacity:.48;cursor:default}input,progress{width:100%;max-width:100%}progress{margin-top:10px;accent-color:var(--accent)}[hidden]{display:none!important}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(

<main>
<h1>Firmware update</h1>
<p class="muted">Check GitHub Releases for an update or upload an ESP32-S3 application build.</p>
<p id="current">Reading the current firmware…</p>
<p id="status" class="status" role="status" aria-live="polite">Connecting…</p>
<p id="error" class="error" role="alert" hidden></p>
<section id="release-card" class="card">
<h2>GitHub Releases</h2>
<p id="release-status" class="muted" role="status" aria-live="polite">Check GitHub for a newer firmware build.</p>
<p id="release-version" hidden></p>
<progress id="release-progress" aria-label="Firmware download progress" hidden></progress>
<div class="row"><button id="release-check" type="button" disabled>Check for updates</button><button id="release-stage" class="primary" type="button" hidden disabled>Download and verify</button></div>
</section>
<section id="upload-card" class="card">
<h2>Upload a build</h2>
<p>Choose the application <code>firmware.bin</code> from your ESP32-S3 build. Use the standalone app file. <span id="capacity"></span></p>
<label for="file">Application file</label><input type="file" id="file" accept=".bin,application/octet-stream" disabled>
<progress id="progress" max="100" value="0" hidden aria-label="Upload progress"></progress>
<div class="row"><button id="upload" class="primary" disabled>Upload and verify</button></div>
</section>
<section id="staged-card" class="card" hidden>
<h2>Ready to install</h2><p id="staged"></p>
<p>The controller will restart and ESS control will pause briefly. Leave the controller powered.</p>
<div class="row"><button id="install" class="primary" disabled>Install and restart</button><button id="discard" disabled>Discard</button></div>
</section>
<section id="trial-card" class="card" hidden>
<h2>Trial firmware</h2>
<p>Check the dashboard and inverter readings, then keep this build or return to the previous one.</p>
<div class="row"><button id="keep" class="primary" disabled>Keep this build</button><button id="rollback" disabled>Return to previous build</button></div>
</section>
</main>
<script>
'use strict';
const el=id=>document.getElementById(id);
const tr=text=>window.bzText?.(text)??text;
const format=(template,values)=>window.bzFormat?.(template,values)??template.replace(/\{(\w+)\}/g,(_,key)=>values[key]);
const bytes=value=>value.toLocaleString(document.documentElement.lang.replace('_','-')||'en');
let state=null,release=null,watchRelease=false,busy=false,restart=null,reading=false,actionGeneration=0,lastBootId;
function error(message=''){el('error').textContent=tr(message);el('error').hidden=!message}
function render(){
    const s=state, f=el('file').files[0];
    const releaseBusy=!!s?.release_busy||release?.status==='checking'||release?.status==='downloading';
    const ready=!!s?.token&&!busy&&!releaseBusy&&!restart&&!s.pending;
    const offer=release?.status==='available'&&release.tag&&release.size>0;
    const downloading=release?.status==='downloading';
    el('current').textContent=s?.slot ? format('Running {build} · {slot}',{build:s.build,slot:s.slot}) : tr('Reading the current firmware…');
    el('capacity').textContent=s?.max_bytes ? format('Slot capacity {bytes} bytes.',{bytes:bytes(s.max_bytes)}) : '';
    el('release-card').hidden=!!s&&(s.trial||s.staged);
    el('release-check').disabled=!ready||!s.max_bytes||s.trial||s.staged;
    el('release-stage').hidden=!offer;
    el('release-stage').disabled=el('release-check').disabled||!s.safe||!offer||release.can_stage===false||release.size>s.max_bytes;
    const releaseMessage=release?.status==='staged'&&!s?.staged?'':release?.message;
    el('release-status').textContent=tr(releaseMessage||'Check GitHub for a newer firmware build.');
    el('release-version').hidden=!offer;
    el('release-version').textContent=offer?format(release.preview?'Preview release {tag} · {bytes} bytes.':'Release {tag} · {bytes} bytes.',{tag:release.tag,bytes:bytes(release.size)}):'';
    const progress=el('release-progress');
    progress.hidden=!downloading;
    if(downloading&&release.size>0){progress.max=release.size;progress.value=release.received||0}
    else progress.removeAttribute('value');
    el('upload-card').hidden=!!s&&(s.trial||s.staged);
    el('trial-card').hidden=!s?.trial;
    el('staged-card').hidden=!s?.staged;
    el('staged').textContent=s?.staged?format('{bytes} bytes verified in {slot}.',{bytes:bytes(s.staged_bytes),slot:s.target}):'';
    if(!busy&&!restart)el('status').textContent=s ?
        (ready&&!s.safe ? format('{message} Waiting for mains and idle inverter operations.',{message:tr(s.message)}) : tr(s.message)) :
        tr('Controller unavailable. Reconnect to the same local network.');
    el('file').disabled=!ready||!s.max_bytes||s.trial||s.staged;
    el('upload').disabled=el('file').disabled||!s.safe||!f||f.size>s.max_bytes||!f.name.toLowerCase().endsWith('.bin');
    el('install').disabled=!ready||!s.staged||!s.safe;
    el('discard').disabled=!ready||!s.staged;
    el('keep').disabled=!ready||!s.trial;
    el('rollback').disabled=!ready||!s.trial||!s.safe;
}
async function refresh(){
    if(reading||busy||document.hidden)return;
    reading=true;
    const generation=actionGeneration;
    try{
        const r=await fetch('/api/firmware',{cache:'no-store',signal:AbortSignal.timeout(3500)});
        if(!r.ok)throw Error();
        const next=await r.json();
        if(generation!==actionGeneration)return;
        if(lastBootId!==undefined&&next.boot_id!==lastBootId){release=null;watchRelease=false}
        lastBootId=next.boot_id;
        state=next;
        if(restart&&(state.boot_id!==restart.boot||!state.pending&&Date.now()>restart.since+4000))restart=null;
        if(watchRelease||state.release_busy){
            watchRelease=true;
            try{
                const r=await fetch('/api/firmware/release',{cache:'no-store',signal:AbortSignal.timeout(3500)});
                const nextRelease=await r.json();
                if(!r.ok||!nextRelease.ok)throw Error();
                if(generation===actionGeneration){
                    release=nextRelease;
                    watchRelease=state.release_busy||release.status==='checking'||release.status==='downloading';
                }
            }catch{
                if(generation===actionGeneration)release={message:'Could not read the GitHub update status.'};
            }
        }
    }catch{
        if(generation!==actionGeneration)return;
        state=null;
        if(restart&&Date.now()-restart.since>60000){
            restart=null;
            error('Could not verify the new build from this page. Check the controller’s address or use USB recovery.');
        }
    }finally{reading=false;render()}
}
async function releaseAction(action){
    const s=state,button=el(action==='check'?'release-check':'release-stage');
    if(button.disabled||!s?.token)return;
    const body=action==='stage'?new URLSearchParams({tag:release.tag}):undefined;
    actionGeneration++;
    busy=true;error();render();
    el('release-status').textContent=tr(action==='check'?'Starting the GitHub release check…':'Starting the firmware download…');
    try{
        const r=await fetch('/api/firmware/release/'+action,{method:'POST',headers:{'X-Brain-Zero-OTA':s.token},body,signal:AbortSignal.timeout(5000)});
        const next=await r.json();
        if(!r.ok||!next.ok)throw Error(next.error||'Action refused.');
        release=next;
    }catch(e){error(e.message)}
    finally{watchRelease=true;busy=false;render();void refresh()}
}
el('release-check').addEventListener('click',()=>releaseAction('check'));
el('release-stage').addEventListener('click',()=>releaseAction('stage'));
el('file').addEventListener('change',()=>{error();render()});
el('upload').addEventListener('click',()=>{
    const f=el('file').files[0], s=state;
    if(!f||!s?.token||el('upload').disabled)return;
    actionGeneration++;
    busy=true;error();render();
    el('status').textContent='Uploading to the inactive slot…';
    el('progress').hidden=false;el('progress').value=0;
    const body=new FormData();body.append('firmware',f);
    const xhr=new XMLHttpRequest();xhr.open('POST','/api/firmware/upload');
    xhr.setRequestHeader('X-Brain-Zero-OTA',s.token);
    xhr.setRequestHeader('X-Firmware-Size',String(f.size));
    xhr.timeout=240000;
    xhr.upload.onprogress=e=>{
        if(e.lengthComputable)el('progress').value=100*e.loaded/e.total;
        if(e.lengthComputable&&e.loaded===e.total)el('status').textContent='Verifying the application image…';
    };
    xhr.onload=()=>{
        try{const s=JSON.parse(xhr.responseText);if(!s.ok)throw Error(s.error||'Upload rejected.');state=s;el('file').value=''}
        catch(e){error(e.message)}
    };
    xhr.onerror=xhr.ontimeout=()=>error('Upload interrupted. Choose the file and try again.');
    xhr.onloadend=()=>{busy=false;el('progress').hidden=true;render();void refresh()};
    xhr.send(body);
});
for(const id of ['install','discard','keep','rollback'])el(id).addEventListener('click',async()=>{
    if(el(id).disabled||!state?.token)return;
    const reboot=id==='install'||id==='rollback';
    const s=state;
    actionGeneration++;
    busy=true;error();render();
    try{
        const r=await fetch('/api/firmware/'+id,{method:'POST',headers:{'X-Brain-Zero-OTA':s.token},signal:AbortSignal.timeout(5000)});
        const next=await r.json();
        if(!r.ok||!next.ok)throw Error(next.error||'Action refused.');
        state=next;
        if(reboot){restart={boot:s.boot_id,since:Date.now()};el('status').textContent='Waiting for the controller to restart…'}
    }catch(e){error(e.message)}
    finally{busy=false;render();void refresh()}
});
document.addEventListener('visibilitychange',refresh);
setInterval(refresh,1500);void refresh();
</script></body></html>)HTML";
