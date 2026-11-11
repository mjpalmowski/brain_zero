// The dashboard polls /api/state once a second. Its ESS card starts and stops
// ESS from this ESP32 (POST /api/ess, run=1 or 0) and links to the ESS settings page (web.h).
#pragma once

#include <pgmspace.h>
#include "site_nav.h"

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>brain_zero</title><link rel="icon" type="image/svg+xml" href="/brand/brain-zero-icon.svg"><link rel="stylesheet" href="/brand/brand.css">
<style>
:root{--bg:#f5f6f8;--card:#fff;--ink:#1d2430;--muted:#687180;--line:#e2e5ea;--good:#1a7f37;--warn:#a65f00;--bad:#c62828;--a:#2463eb;--b:#d9480f;--c:#0e9f6e}
@media (prefers-color-scheme:dark){:root{--bg:#101318;--card:#191e25;--ink:#e6e9ee;--muted:#98a2ae;--line:#29303a;--good:#3fb950;--warn:#d29922;--bad:#f85149;--a:#6ea8fe;--b:#ff922b;--c:#38d9a9}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:4px 16px;align-items:baseline;padding:14px 16px 0;max-width:1100px;margin:auto}
h1{font-size:18px;margin:0}#status{color:var(--muted);font-size:13px}header a{color:var(--a);font-size:14px}
main{padding:4px 16px 24px;max-width:1100px;margin:auto}
h2{font-size:12px;margin:18px 0 8px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.05em}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px;min-width:0}
.k{color:var(--muted);font-size:12px}.v{font-size:24px;font-weight:600;font-variant-numeric:tabular-nums;white-space:nowrap}
.u{font-size:13px;color:var(--muted);margin-left:3px;font-weight:400}.s{font-size:12px;color:var(--muted)}
.stale .v{opacity:.35}.good{color:var(--good)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.gauge{margin:0;text-align:center;padding:12px}
.gauge figcaption{font-weight:600;color:var(--ink);font-size:13px}
.gauge-face{position:relative;width:min(100%,170px);margin:6px auto 0}
.gauge-face svg{display:block;width:100%;height:auto}
.gauge-face path{fill:none;stroke-width:8;stroke-linecap:round}
.gauge-track{stroke:var(--line)}.gauge-fill{stroke:var(--a)}
.gauge.outside .gauge-fill{stroke:var(--warn)}
.gauge.stale .gauge-fill,.gauge-fill[stroke-dasharray="0 100"]{visibility:hidden}
.gauge-reading{position:absolute;inset-inline:0;bottom:17px}
.gauge-scale{display:flex;justify-content:space-between;direction:ltr;gap:8px;padding-inline:7px}
.gauge-note{min-height:1.4em;margin:3px 0 0}.gauge.outside .gauge-note{color:var(--warn)}
footer{max-width:1100px;margin:auto;padding:0 16px 20px;color:var(--muted);font-size:12px}
.ctl{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px}
.ctl input{width:96px;font:inherit;padding:5px 8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--ink)}
.ctl button{font:inherit;padding:5px 12px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--ink);cursor:pointer}
.ctl button:hover{border-color:var(--a)}.ctl a{color:var(--a)}
.warning-head{display:flex;align-items:baseline;justify-content:space-between;gap:10px}
.warning-head a{font-size:12px;color:var(--a)}
.warning-items{list-style:none;margin:0;padding:0;display:grid;gap:6px}
.warning-items li{display:flex;align-items:baseline;gap:8px;flex-wrap:wrap}
.warning-level{font-size:11px;font-weight:650;border:1px solid currentColor;border-radius:999px;padding:1px 7px}
#warning-sustain:not([hidden]){display:block;margin-top:6px}
.warning-history{margin-top:10px}.warning-history summary{cursor:pointer;font-weight:600}
.warning-history summary span{font-weight:400;margin-left:6px}
.warning-history p{margin:7px 0}.warning-history ol{list-style:none;margin:0;padding:0;max-height:320px;overflow:auto}
.warning-history li{display:grid;grid-template-columns:1.2fr 1fr 1fr;gap:8px;align-items:baseline;padding:8px 0;border-top:1px solid var(--line);font-size:13px}
.history-condition{display:flex;align-items:baseline;gap:6px;flex-wrap:wrap}.warning-history time{display:block;font-variant-numeric:tabular-nums}
@media(max-width:600px){.warning-history li{grid-template-columns:1fr 1fr}.history-condition{grid-column:1/-1}}
</style></head><body>)HTML"
BRAIN_ZERO_NAV
R"HTML(
<header><h1 id="inverter-name">Inverter</h1><span id="status">connecting…</span></header>
<main>
<section aria-labelledby="warning-heading">
<div class="warning-head"><h2 id="warning-heading">Inverter warnings</h2><a href="/diagnostics">View diagnostics</a></div>
<div class="card" id="warning-panel">
<div id="warning-live" role="status" aria-live="polite"><span class="s">Waiting for inverter status…</span></div>
<div class="s" id="warning-sustain" hidden></div>
</div>
<details class="card warning-history" id="warning-history">
<summary>Warning and alarm history <span class="s" id="warning-history-count"></span></summary>
<p class="s">Latest 32 episodes observed by the S3. Times use your browser’s time zone. After a telemetry gap, the clear time is when the S3 first sees the condition clear.</p>
<p class="s" id="warning-history-status">Loading history…</p>
<ol id="warning-history-entries" aria-label="Warning and alarm episodes, newest first"></ol>
</details>
</section>
<h2>Power</h2>
<div class="grid">
<div class="card"><div class="k">Grid (AC-in)</div><div class="v" id="acin">–</div><div class="s" id="acin-s"></div></div>
<div class="card"><div class="k">AC-out (loads − AC-coupled solar)</div><div class="v" id="acout">–</div><div class="s" id="acout-s"></div></div>
<div class="card"><div class="k">Inverter / charger</div><div class="v" id="inv">–</div><div class="s" id="inv-s"></div></div>
</div>
<h2>ESS control</h2>
<div class="card"><div class="k">Grid setpoint</div><div class="v" id="ctl">–</div><div class="s" id="ctl-s"></div><div class="s" id="ctl-why"></div>
<div class="ctl"><button type="button" id="ess-run">Start ESS</button><button type="button" id="ess-stop">Stop ESS</button><a href="/ess">ESS settings</a></div>
<div class="s" id="ctl-msg">ESS pauses while another VE.Bus controller is active.</div></div>
<section aria-labelledby="battery-heading"><h2 id="battery-heading">Battery</h2><div class="grid" id="battery-gauges"></div></section>
<section aria-labelledby="ac-heading"><h2 id="ac-heading">AC readings</h2><div class="grid" id="ac-gauges"></div></section>
<h2>Inverter</h2>
<div class="grid">
<div class="card"><div class="k">State</div><div class="v" id="state">–</div><div class="s" id="mode-s"></div></div>
<div class="card"><div class="k">Input current limit</div><div class="v" id="limit">–</div><div class="s" id="limit-s"></div></div>
</div>
<p class="s"><span>Inverter/firmware not supported?</span> <a href="/multi#profile-title">Submit to developer</a></p>
</main>
<footer>Grey values are stale.</footer>
<script>
const STATES={0:"Down",1:"Startup",2:"Off",3:"Slave",4:"Invert full",5:"Invert half",6:"Invert AES",7:"Power assist",8:"Bypass",9:"Charge"};
const MODES={4:"Off",5:"Charger only",6:"Inverter only",7:"On"};
const $=id=>document.getElementById(id);
const tr=text=>globalThis.window?.bzText?.(text)??text;
const format=(text,values)=>globalThis.window?.bzFormat?.(text,values)??text.replace(/\{(\w+)\}/g,(_,key)=>values[key]);
const esc=text=>String(text??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const val=r=>r?r[0]:null, age=r=>r?r[1]:Infinity;
const decimal=(x,d)=>x.toLocaleString(document.documentElement.lang,{minimumFractionDigits:d,maximumFractionDigits:d});
const num=(x,d,u)=>x==null?"–":decimal(x,d)+'<span class="u">'+u+"</span>";
const watts=x=>num(x,0,"W");
function put(id,html,stale){const e=$(id);e.innerHTML=html;e.parentElement.classList.toggle("stale",!!stale)}
function dur(t){
    t=Math.floor(t);
    const hours=Math.floor(t/3600),minutes=Math.floor(t%3600/60),seconds=t%60;
    return hours?format("{hours} h {minutes} min",{hours,minutes}):minutes?format("{minutes} min {seconds} s",{minutes,seconds}):format("{seconds} s",{seconds});
}
const recent=(seconds,limit=5)=>Number.isFinite(seconds)&&seconds>=0&&seconds<limit;
const readingFresh=reading=>Number.isFinite(val(reading))&&recent(age(reading));
const gaugeArc="M 12 76 A 68 68 0 0 1 148 76";
function makeGauges(group,specs){
    return specs.map(spec=>{
        const {field,label,min,max,unit}=spec;
        const card=document.createElement("figure");
        card.className="card gauge";
        card.dataset.gauge=field;
        card.innerHTML=`<figcaption>${label}</figcaption><div class="gauge-face">
          <svg viewBox="0 0 160 86" aria-hidden="true">
            <path class="gauge-track" d="${gaugeArc}"/>
            <path class="gauge-fill" d="${gaugeArc}" pathLength="100" stroke-dasharray="0 100"/>
          </svg><div class="gauge-reading v">–</div>
          <div class="gauge-scale s" aria-hidden="true"><span>${min} ${unit}</span><span>${max} ${unit}</span></div>
        </div><p class="gauge-note s">Waiting for reading</p>`;
        $(group).append(card);
        return {...spec,card,fill:card.querySelector(".gauge-fill"),value:card.querySelector(".gauge-reading"),note:card.querySelector(".gauge-note")};
    });
}
// Victron's 230 V inverter input ranges are display scales only. Retain the site scale for 48 V.
const dcVoltageScales={12:[9.5,17],24:[19,33],48:[47,58.5]};
const [dcMin,dcMax]=dcVoltageScales[48];
const gauges=[
    ...makeGauges("battery-gauges",[
        {field:"soc",label:"Battery SOC",min:0,max:100,unit:"%",digits:1,maxAge:30},
        {field:"bat_v",label:"DC voltage",min:dcMin,max:dcMax,unit:"V",digits:2},
        {field:"bat_a",label:"MultiPlus DC current",min:0,max:110,unit:"A",digits:1,direction:true}
    ]),
    ...makeGauges("ac-gauges",[
        {field:"mains_v",label:"AC input voltage",min:230,max:255,unit:"V",digits:1},
        {field:"inverter_v",label:"AC output voltage",min:230,max:255,unit:"V",digits:1},
        {field:"ac_hz",label:"AC output frequency",min:49,max:51,unit:"Hz",digits:2}
    ])
];
function renderGauge(gauge,reading){
    const raw=val(reading), seconds=age(reading);
    const known=Number.isFinite(raw)&&Number.isFinite(seconds)&&seconds>=0;
    const fresh=known&&recent(seconds,gauge.maxAge);
    const value=gauge.direction?Math.abs(raw):raw;
    const scale=!fresh?"":value<gauge.min?"Below scale":value>gauge.max?"Above scale":"";
    const direction=fresh&&gauge.direction?(raw>0?"Charging":raw<0?"Discharging":"Idle"):"";
    const fill=fresh?Math.min(100,Math.max(0,(value-gauge.min)*100/(gauge.max-gauge.min))):0;
    gauge.fill.setAttribute("stroke-dasharray",fill+" 100");
    gauge.card.classList.toggle("stale",known&&!fresh);
    gauge.card.classList.toggle("outside",!!scale);
    gauge.value.innerHTML=num(known?value:null,gauge.digits,gauge.unit);
    gauge.note.textContent=!known?"Unavailable":!fresh?"Stale reading":direction&&scale?
        format("{direction} · {scale}",{direction:tr(direction),scale:tr(scale)}):tr(direction||scale);
}
function renderInverterName(model){
    const heading=$("inverter-name"),name=model?.name||tr("Inverter");
    if(heading.textContent!==name)heading.textContent=name;
}
function renderGauges(s,model){
    const nominal=model?.nominal_voltage;
    const [min,max]=dcVoltageScales[nominal===12||nominal===24?nominal:48];
    for(const gauge of gauges){
        let reading=s[gauge.field];
        if(gauge.field==="bat_v"){
            if(gauge.min!==min||gauge.max!==max){
                gauge.min=min;
                gauge.max=max;
                const [minLabel,maxLabel]=gauge.card.querySelectorAll(".gauge-scale span");
                minLabel.textContent=decimal(min,Number.isInteger(min)?0:1)+" V";
                maxLabel.textContent=decimal(max,Number.isInteger(max)?0:1)+" V";
            }
            if(model&&!readingFresh(reading)&&readingFresh(s.dc_info_v))reading=s.dc_info_v;
        }
        renderGauge(gauge,reading);
    }
}
const warningNames=["Low battery voltage","Inverter overload","High inverter temperature","High DC ripple"];
const busErrorKind=warningNames.length;
const mainsLostKind=busErrorKind+1;
const extraWarningNames=["Voltage sense error","Temperature sense error","High DC voltage","High DC current","AC input not accepted"];
const validCode=code=>Number.isInteger(code)&&code>=1&&code<=26;
function warningName(kind,code){
    if(kind===busErrorKind)return format("VE.Bus error {code}",{code});
    if(kind===mainsLostKind)return tr("Mains input lost");
    return tr(warningNames[kind]??extraWarningNames[kind-mainsLostKind-1]);
}
const warningLevel=level=>level===2?"Alarm":"Warning";
const warningColor=level=>level===2?"bad":"warn";
const levelBadge=level=>'<span class="warning-level">'+esc(tr(warningLevel(level)))+'</span>';
function inverterWarnings(status){
    const rows=[];
    if(status?.mains_lost===true)rows.push({label:warningName(mainsLostKind),level:1});
    if(status?.leds_known===true){
        if(validCode(status.led_code))rows.push({label:warningName(busErrorKind,status.led_code),level:2});
        warningNames.forEach((label,kind)=>{
            const level=status.levels?.[kind];
            if(level===1||level===2)rows.push({label,level});
        });
    }
    extraWarningNames.forEach((label,index)=>{
        const level=status?.extra_levels?.[index];
        if(level===1||level===2)rows.push({label,level});
    });
    return rows.sort((a,b)=>b.level-a.level);
}
let warningMarkup,latestWarningStatus,historyRevision,historyPending=false;
let dateFormat, dateLanguage;
const validStamp=s=>Array.isArray(s)&&s.length===2&&s.every(x=>Number.isInteger(x)&&x>=0&&x<=0xffffffff);
function stampText(stamp){
    if(!stamp[0])return esc(format("Uptime {duration} · clock unavailable",{duration:dur(stamp[1])}));
    const date=new Date(stamp[0]*1000);
    const lang=document.documentElement.lang;
    if(lang!==dateLanguage){
        dateLanguage=lang;
        dateFormat=new Intl.DateTimeFormat(lang,{dateStyle:"medium",timeStyle:"long"});
    }
    return '<time datetime="'+date.toISOString()+'">'+esc(dateFormat.format(date))+'</time>';
}
function updateOpenEpisodes(){
    const status=latestWarningStatus;
    for(const el of $("warning-history-entries").querySelectorAll("[data-kind]")){
        const kind=Number(el.dataset.kind);
        let active;
        if(kind===busErrorKind)active=status?.led_code===Number(el.dataset.code);
        else if(kind===mainsLostKind)active=status?.mains_lost===true;
        else if(kind>mainsLostKind)active=status?.extra_levels?.[kind-mainsLostKind-1]===Number(el.dataset.level);
        else active=status?.leds_known===true&&status.levels?.[kind]===Number(el.dataset.level);
        el.textContent=active?"Still active":"Awaiting confirmation";
    }
}
async function refreshWarningHistory(revision){
    if(!Number.isInteger(revision)||revision===historyRevision||historyPending)return;
    historyPending=true;
    const note=$("warning-history-status");
    try{
        const r=await fetch("/api/warning-history",{cache:"no-store",signal:AbortSignal.timeout(4000)});
        if(!r.ok)throw Error("History HTTP "+r.status);
        const data=await r.json();
        if(!Number.isInteger(data.revision)||!Array.isArray(data.entries)||data.entries.length>32)throw Error("Invalid history");
        const rows=data.entries.map(e=>{
            if(!Number.isInteger(e.kind)||e.kind<0||e.kind>mainsLostKind+extraWarningNames.length||
                (e.kind===busErrorKind&&!validCode(e.code))||![1,2].includes(e.level)||
                !validStamp(e.appeared)||(e.cleared!==null&&!validStamp(e.cleared)))throw Error("Invalid episode");
            const cleared=e.cleared?stampText(e.cleared):'<span data-kind="'+e.kind+'" data-level="'+e.level+'" data-code="'+(e.kind===busErrorKind?e.code:0)+'"></span>';
            return '<li><div class="history-condition '+warningColor(e.level)+'">'+levelBadge(e.level)+
                '<strong>'+esc(warningName(e.kind,e.code))+'</strong></div><div><div class="k">Appeared</div>'+stampText(e.appeared)+
                '</div><div><div class="k">Cleared</div>'+cleared+'</div></li>';
        });
        $("warning-history-entries").innerHTML=rows.join("");
        $("warning-history-count").textContent=rows.length?format("{count} recorded",{count:rows.length}):"";
        note.textContent=rows.length?"":"No warning or alarm episodes recorded yet.";
        note.hidden=rows.length>0;
        historyRevision=data.revision;
        updateOpenEpisodes();
    }catch(e){
        note.hidden=false;
        note.textContent=historyRevision===undefined?"Warning history unavailable.":"History refresh unavailable. Showing the last received history.";
    }finally{historyPending=false;}
}
function renderWarnings(s){
const status=s?.warning_status;
const fresh=status?.leds_known===true;
const mainsKnown=typeof status?.mains_lost==="boolean";
const warnings=inverterWarnings(status);
let markup=warnings.length?'<ul class="warning-items">'+warnings.map(({label,level})=>
'<li class="'+warningColor(level)+'">'+levelBadge(level)+'<strong>'+esc(label)+'</strong></li>').join('')+'</ul>':"";
if(!fresh)markup+='<div class="s warn">Inverter warning status unavailable</div>';
else if(!mainsKnown)markup+='<div class="s warn">Mains input status unavailable</div>';
else if(!warnings.length)markup='<span class="s">No active inverter warnings indicated</span>';
if(status?.extra_levels?.slice(2,4).includes(-1))markup+='<div class="s">High DC alarm status unavailable</div>';
if(markup!==warningMarkup){$("warning-live").innerHTML=markup;warningMarkup=markup;}
const sustain=$("warning-sustain");
sustain.hidden=!(fresh&&status?.sustain===true&&recent(status.sustain_age_s,60));
if(!sustain.hidden)sustain.textContent=format("Last diagnostic read reported Sustain mode · {age} ago",{age:dur(status.sustain_age_s)});
latestWarningStatus=status;
updateOpenEpisodes();
void refreshWarningHistory(status?.history_revision);
}
function render(s){
renderWarnings(s);
const connected=recent(s.multi_age_s),candidate=s.multiplus_ii;
const model=connected&&typeof candidate?.name==="string"&&recent(candidate.age_s,Infinity)?candidate:null;
renderInverterName(model);
const acin=val(s.acin_w),acout=val(s.acout_w),inv=val(s.inverter_w);
put("acin",watts(acin==null?null:Math.abs(acin)),age(s.acin_w)>5);
$("acin-s").textContent=acin==null?"":acin>0?"exporting":acin<0?"importing":"balanced";
put("acout",watts(acout),age(s.acout_w)>5);
$("acout-s").textContent=acout==null?"":acout<0?"solar exceeds the loads":"drawn by the loads";
put("inv",watts(inv==null?null:Math.abs(inv)),age(s.inverter_w)>5);
$("inv-s").textContent=inv==null?"":inv>0?"charging the battery":inv<0?"discharging the battery":"idle";
renderGauges(s,model);
put("state",STATES[s.device_state]||"–",!recent(s.device_state_age_s));
const mode=recent(s.actual_mode_age_s)?MODES[s.actual_mode]:null;
$("mode-s").textContent=mode?format("Operating mode · {mode}",{mode:tr(mode)}):"";
put("limit",num(val(s.limit_a),1,"A"),age(s.limit_a)>5);
$("limit-s").textContent=val(s.limit_max_a)==null?"":format("maximum {amps} A",{amps:decimal(val(s.limit_max_a),1)});
renderCtl(s);
$("status").textContent=connected?"Connected":"Inverter data unavailable";
}
const CTL={testing:"Waiting for a test to finish",paused:"Another controller is active",
"backing off":"Waiting to reconnect",polling:"Reading inverter data",checking:"Checking ESS configuration"};
const grid=w=>w===0?"0 W":format(w>0?"Export {watts} W":"Import {watts} W",{watts:Math.abs(w)});
function renderCtl(s){
const c=s.ctl,e=s.ess;
if(!c||!e)return;
const writing=e.run&&c.state==="writing",confirmed=writing&&s.setpoint_fresh===1;
put("ctl",!e.run?"Off":writing&&Number.isFinite(e.setpoint_w)?esc(grid(e.setpoint_w)):"Waiting",false);
$("ctl").className="v"+(confirmed?" good":e.run?" warn":"");
const mode=tr(e.mode_name||""),state=e.run?tr(e.state_name||""):"";
$("ctl-s").textContent=mode&&state?format("{mode} · {state}",{mode,state}):mode||state;
const reason=writing?(confirmed?(e.why?.startsWith("grid setpoint,")?"":e.why):"Waiting for inverter confirmation"):CTL[c.state]||"Waiting for inverter control";
const detail=tr(reason||""),slot=e.schedule>=0?e.schedule+1:0;
$("ctl-why").textContent=!e.run?"":!slot?detail:detail?
    format("{reason} · Charge window {slot}",{reason:detail,slot}):format("Charge window {slot}",{slot});
}
async function post(body){$("ctl-msg").textContent="sending…";
try{const r=await fetch("/api/ess",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body});
 const j=await r.json();$("ctl-msg").textContent=j.ok?tr(j.message):format("refused: {error}",{error:tr(j.error)})}
catch(e){$("ctl-msg").textContent="no answer from the listener"}}
$("ess-run").addEventListener("click",()=>post("run=1"));
$("ess-stop").addEventListener("click",()=>post("run=0"));
async function tick(){
try{
const r=await fetch("/api/state",{cache:"no-store",signal:AbortSignal.timeout(4000)});
if(!r.ok)throw Error("State HTTP "+r.status);
render(await r.json());
}catch(e){
renderWarnings(null);
renderInverterName(null);
$("status").textContent="Controller unreachable";
for(const value of document.querySelectorAll("main .v:not(.gauge-reading)"))value.parentElement.classList.add("stale");
for(const {card,note} of gauges){card.classList.add("stale");note.textContent="Controller unavailable";}
}finally{setTimeout(tick,1000)}
}
tick();
</script></body></html>
)HTML";
