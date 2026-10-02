#pragma once
#include <pgmspace.h>

/* ===========================================================================
 * KUTSU Sense — master dashboard SPA (served at GET / by the master node)
 * ---------------------------------------------------------------------------
 * Self-contained: no external CDN/fonts (the LAN may be offline). Vanilla JS.
 * Data source: GET /api/nodes (latest record per node), polled every 2 s.
 *
 * The master stores only the LATEST record per node, so all derived views live
 * in the browser and accumulate over the session:
 *   - Notifications  : ALERT/WARNING latched on per-metric severity rise (like
 *                      the device display) + INFO on node join/leave.
 *   - Meaningful Stats: every metric currently at warn/alert level, with
 *                      Highest/Median/Lowest from the client-side history buffer.
 *   - System         : browser clock + active/inactive/total nodes + per-node
 *                      runtime (record `up`) and first-seen (now - up).
 *   - Real-Time      : nodes grouped by room, each with value + sparkline.
 *   - Node dialog    : all metrics as severity-coloured charts + live values.
 * Colours come from the firmware severity (`sev`/`s_*`) — never re-derived.
 * =========================================================================== */

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kutsu Dashboard</title>
<style>
:root{--bg:#0a0d12;--pan:#12161d;--pan2:#161b23;--edge:#232a35;--tx:#e8eef6;--mut:#8b97a7;
--ok:#22c55e;--warn:#eab308;--alert:#ef4444;--acc:#5b9dff}
*{box-sizing:border-box}body{margin:0;font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--tx)}
h2{font-size:20px;font-weight:600;margin:0 0 12px}
header{display:flex;align-items:center;gap:16px;padding:18px 26px}
header .brand{font-size:20px;font-weight:700}
header nav{margin-left:auto;display:flex;gap:26px}
header nav a{color:var(--mut);text-decoration:none;cursor:pointer;font-size:15px}
header nav a.on,header nav a:hover{color:var(--tx)}
main{padding:6px 26px 40px;max-width:1500px;margin:0 auto}
.row1{display:grid;grid-template-columns:repeat(auto-fit,minmax(330px,1fr));gap:26px;margin-bottom:30px}
.panel{background:var(--pan);border:1px solid var(--edge);border-radius:14px;padding:16px}
.mut{color:var(--mut)}
/* Notifications */
.alegend{display:flex;gap:8px;align-items:center;margin-bottom:10px}
.badge{font-size:10.5px;font-weight:700;letter-spacing:.4px;padding:2px 9px;border-radius:20px}
.b0{background:#123a22;color:var(--ok)}.b1{background:#3a3410;color:var(--warn)}.b2{background:#3a1414;color:var(--alert)}
.notes{display:flex;flex-direction:column;gap:7px;max-height:260px;overflow:auto}
.note{display:flex;gap:8px;font-size:13.5px}.note time{margin-left:auto;color:var(--mut);font-size:11px;flex:none}
.note.warn{color:var(--warn)}.note.alert{color:var(--alert)}.note.info{color:var(--tx)}
.empty{color:var(--mut);font-size:13px;padding:6px 2px}
/* Meaningful stats */
.statgrid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.stat{background:var(--pan2);border:1px solid var(--edge);border-radius:11px;padding:12px}
.stat .st{font-size:15px;font-weight:600}.stat .sr{color:var(--mut);font-size:12px;margin-bottom:8px}
.srow{display:flex;justify-content:space-between;font-variant-numeric:tabular-nums;margin-top:3px}
.srow span{color:var(--mut)}.hi1{color:var(--warn)}.hi2{color:var(--alert)}
/* System */
.sysgrid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.tile{background:var(--pan2);border:1px solid var(--edge);border-radius:11px;padding:14px}
.tile.wide{grid-column:1/-1}
.clock{display:flex;align-items:baseline;gap:14px}
.clock .t{font-size:34px;font-weight:700;font-variant-numeric:tabular-nums}
.clock .d{font-size:14px}.clock .d b{display:block;font-weight:600}
.tk{color:var(--mut);font-size:12px;margin-bottom:6px}
.narow{display:flex;justify-content:space-between;margin-top:4px}
.narow b{font-variant-numeric:tabular-nums}
.rtt{width:100%;border-collapse:collapse;font-size:12.5px}
.rtt th{text-align:left;color:var(--mut);font-weight:500;padding:2px 0}
.rtt td{padding:3px 0;font-variant-numeric:tabular-nums}.rtt td:last-child,.rtt th:last-child{text-align:right}
.dot{width:8px;height:8px;border-radius:50%;background:var(--ok);display:inline-block;margin-right:6px}
/* Real-time monitoring */
.rooms{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:22px}
.room .rh{font-size:18px;font-weight:600;margin:0 2px 10px}
.ncards{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}
.ncard{background:var(--pan2);border:1px solid var(--edge);border-radius:12px;padding:13px;cursor:pointer;transition:.12s}
.ncard:hover{border-color:var(--acc)}
.ncard.s1{box-shadow:inset 3px 0 0 var(--warn)}.ncard.s2{box-shadow:inset 3px 0 0 var(--alert)}
.nh{display:flex;align-items:center;gap:8px;margin-bottom:9px}.nh .nn{font-weight:600;font-size:13px}
.nh .badge{margin-left:auto}
.mrow{display:flex;justify-content:space-between;font-size:12px;margin-top:8px}
.mrow .mv{font-variant-numeric:tabular-nums;font-weight:600}.mrow .mv.s1{color:var(--warn)}.mrow .mv.s2{color:var(--alert)}
canvas.spark{width:100%;height:30px;display:block;margin-top:3px}
.off{opacity:.5}
/* Dialog */
#ov{position:fixed;inset:0;background:rgba(5,7,11,.74);display:none;align-items:center;justify-content:center;padding:18px;z-index:10}
#ov .box{background:var(--pan);border:1px solid var(--edge);border-radius:16px;max-width:760px;width:100%;padding:22px}
#ov h3{margin:0;display:flex;align-items:center;gap:10px;font-size:20px}
#ov .x{margin-left:auto;cursor:pointer;color:var(--mut);font-size:24px;line-height:1}
#ov .did{color:var(--mut);font-size:12px;margin:2px 0 16px}
.dgrid{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.dm .dh{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:5px}
.dm .dh .dl{color:var(--mut)}.dm .dh .dv{font-size:18px;font-weight:700;font-variant-numeric:tabular-nums}
.dm .dh .dv.s1{color:var(--warn)}.dm .dh .dv.s2{color:var(--alert)}
canvas.big{width:100%;height:66px;display:block;background:var(--bg);border:1px solid var(--edge);border-radius:9px}
.dfoot{display:flex;justify-content:flex-end;margin-top:18px}
.btn{background:transparent;color:var(--tx);border:1px solid var(--edge);border-radius:20px;padding:7px 18px;cursor:pointer;font-size:13px}
.btn:hover{border-color:var(--acc)}
@media(max-width:640px){.statgrid,.sysgrid,.dgrid{grid-template-columns:1fr}}
</style></head><body>
<header>
  <div class="brand">Kutsu Dashboard</div>
  <nav>
    <a class="on" onclick="scrollTo(0,0)">Dashboard</a>
    <a onclick="document.getElementById('statsSec').scrollIntoView({behavior:'smooth'})">Statistics</a>
    <a onclick="exportAll()">Export</a>
    <a class="mut">Settings</a>
  </nav>
</header>
<main>
  <div class="row1">
    <section><h2>Notifications</h2>
      <div class="panel">
        <div class="alegend"><b>Alerts</b><span class="badge b1">WARNING</span><span class="badge b2">ALERT</span></div>
        <div class="notes" id="notes"></div>
      </div>
    </section>
    <section id="statsSec"><h2>Meaningful Statistics</h2>
      <div class="panel"><div class="statgrid" id="stats"></div></div>
    </section>
    <section><h2>System</h2>
      <div class="panel">
        <div class="sysgrid">
          <div class="tile clock"><div class="t" id="ckT">--:--</div><div class="d"><b id="ckD">--</b><span id="ckDt">--.--.--</span></div></div>
          <div class="tile"><div class="tk">Active nodes</div>
            <div class="narow"><span><span class="dot"></span>Active</span><b id="naA" style="color:var(--ok)">0</b></div>
            <div class="narow"><span class="mut">Inactive</span><b id="naI">0</b></div>
            <div class="narow"><span class="mut">Total</span><b id="naT">0</b></div>
          </div>
          <div class="tile wide"><div class="tk">Node Runtime</div>
            <table class="rtt"><thead><tr><th>Node</th><th>First seen</th><th>Runtime</th></tr></thead>
            <tbody id="rtb"></tbody></table>
          </div>
        </div>
      </div>
    </section>
  </div>
  <section><h2>Real-Time Monitoring</h2><div class="rooms" id="rooms"></div></section>
</main>
<div id="ov"><div class="box">
  <h3><span id="dTitle">Node</span><span id="dBadge"></span><span class="x" onclick="closeD()">&times;</span></h3>
  <div class="did" id="dId"></div>
  <div class="dgrid" id="dGrid"></div>
  <div class="dfoot"><button class="btn" onclick="exportNode(openId)">Export</button></div>
</div></div>
<script>
const M={eco2:['eCO2','ppm'],tvoc:['TVOC','ppb'],temp:['Temp','°C'],hum:['Hum','%'],gas:['Gas','adc']};
const CARD=['eco2','tvoc','temp','hum'], DLG=['eco2','temp','tvoc','hum','gas'];
const HISTLEN=120, ACTIVE_S=15, SEVW=['OK','WARN','ALERT'];
const nodes={}, hist={}, lastSev={}, activeMap={}; let notes=[], seeded=false, openId=null;
function fmt(v){return v===null||v===undefined?'—':v}
function nameOf(r){return r.room||r.id||r.mac||'node'}
function col(s){return s>=2?'#ef4444':s>=1?'#eab308':'#5b9dff'}
function pad(n){return(n<10?'0':'')+n}
function pushNote(level,text){notes.unshift({level,text,t:Date.now()});if(notes.length>60)notes.pop()}
function median(a){if(!a.length)return null;const b=[...a].sort((x,y)=>x-y);const m=b.length>>1;return b.length%2?b[m]:(b[m-1]+b[m])/2}
function r1(v){return v==null?null:Math.round(v*10)/10}

// ---- polling + derivation -------------------------------------------------
async function poll(){
  let j; try{j=await(await fetch('/api/nodes')).json()}catch(e){return}
  const now=Date.now(); const list=j.nodes||[];
  list.forEach(n=>{
    const r=n.last; if(!r)return; const id=n.id||n.mac; if(!id)return;
    r.room=n.room||r.room||''; r.ip=n.ip; r.age=n.age||0;
    const fresh=!nodes[id];
    if(fresh){hist[id]={eco2:[],tvoc:[],temp:[],hum:[],gas:[]};lastSev[id]={};activeMap[id]=true;
      if(seeded)pushNote('info','New node '+(r.room?'['+r.room+'] ':'')+id+' joined the mesh');}
    const firstBoot=(r.up!=null)?now-r.up*1000:(nodes[id]?nodes[id].firstBoot:now);
    nodes[id]={id,mac:r.mac,room:r.room,ip:r.ip,age:r.age,up:r.up,last:r,firstBoot};
    ['eco2','tvoc','temp','hum','gas'].forEach(k=>{
      const v=r[k], s=r['s_'+k]||0, buf=hist[id][k];
      buf.push({v:(v===null?null:v),s}); if(buf.length>HISTLEN)buf.shift();
      const prev=lastSev[id][k]||0;
      if(s>prev){const tag=s===2?'alert':'warn';const w=s===2?'ALERT':'WARNING';
        pushNote(tag,w+' '+(r.room?'['+r.room+'] ':'')+M[k][0]+' '+fmt(v)+M[k][1]);}
      lastSev[id][k]=s;
    });
    const active=r.age<ACTIVE_S;
    if(!fresh){if(activeMap[id]&&!active)pushNote('info',(r.room?'['+r.room+'] ':'')+id+' left the mesh');
      else if(!activeMap[id]&&active)pushNote('info',(r.room?'['+r.room+'] ':'')+id+' rejoined the mesh');}
    activeMap[id]=active;
  });
  seeded=true; render(); if(openId)paintD();
}

// ---- charts ---------------------------------------------------------------
function drawBars(cv,arr){
  const x=cv.getContext('2d'),W=cv.width,H=cv.height;x.clearRect(0,0,W,H);
  if(!arr||!arr.length)return;const vs=arr.map(p=>p.v).filter(v=>v!=null);if(!vs.length)return;
  let mn=Math.min(...vs),mx=Math.max(...vs);if(mx-mn<1e-6)mx=mn+1;
  const n=arr.length,step=W/n,bw=Math.max(1,step-1);
  arr.forEach((p,i)=>{if(p.v==null)return;const h=Math.max(2,((p.v-mn)/(mx-mn))*(H-3));
    x.fillStyle=col(p.s);x.fillRect(i*step,H-h,bw,h);});
}
function drawAll(root){root.querySelectorAll('canvas[data-n]').forEach(cv=>{
  const b=(hist[cv.dataset.n]||{})[cv.dataset.m];drawBars(cv,b);});}

// ---- render ---------------------------------------------------------------
function render(){renderNotes();renderStats();renderSystem();renderRooms();}

function renderNotes(){
  const el=document.getElementById('notes');
  if(!notes.length){el.innerHTML='<div class="empty">No notifications yet.</div>';return;}
  el.innerHTML=notes.slice(0,20).map(n=>{
    const d=new Date(n.t),ts=pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
    const pre=n.level==='info'?'INFO ':'';
    return '<div class="note '+n.level+'">'+pre+n.text+'<time>'+ts+'</time></div>';}).join('');
}
function renderStats(){
  const el=document.getElementById('stats');const cards=[];
  Object.values(nodes).forEach(nd=>{const r=nd.last;
    ['eco2','tvoc','temp','hum','gas'].forEach(k=>{const s=r['s_'+k]||0;if(s<1)return;
      const vs=(hist[nd.id][k]||[]).map(p=>p.v).filter(v=>v!=null);if(!vs.length)return;
      cards.push({s,k,room:nd.room||nd.id,hi:Math.max(...vs),lo:Math.min(...vs),md:median(vs)});});});
  cards.sort((a,b)=>b.s-a.s);
  if(!cards.length){el.innerHTML='<div class="empty">All readings nominal — nothing out of the ordinary.</div>';return;}
  el.innerHTML=cards.slice(0,6).map(c=>{const u=M[c.k][1];
    return '<div class="stat"><div class="st">'+M[c.k][0]+'</div><div class="sr">'+c.room+'</div>'+
      '<div class="srow"><span>Highest</span><b class="hi'+c.s+'">'+r1(c.hi)+u+'</b></div>'+
      '<div class="srow"><span>Median</span><b>'+r1(c.md)+u+'</b></div>'+
      '<div class="srow"><span>Lowest</span><b>'+r1(c.lo)+u+'</b></div></div>';}).join('');
}
function renderSystem(){
  const ns=Object.values(nodes);const act=ns.filter(n=>n.age<ACTIVE_S).length;
  document.getElementById('naA').textContent=act;
  document.getElementById('naI').textContent=ns.length-act;
  document.getElementById('naT').textContent=ns.length;
  document.getElementById('rtb').innerHTML=ns.map(n=>{
    const fs=new Date(n.firstBoot),d=pad(fs.getDate())+'.'+pad(fs.getMonth()+1)+'.'+String(fs.getFullYear()).slice(2);
    return '<tr><td>'+(n.id||n.mac)+'</td><td class="mut">'+d+'</td><td>'+fmtDur(n.up)+'</td></tr>';}).join('')
    ||'<tr><td class="mut">no nodes</td><td></td><td></td></tr>';
}
function fmtDur(s){if(s==null)return'—';if(s<3600)return Math.floor(s/60)+'m';if(s<86400)return Math.floor(s/3600)+'h';return Math.floor(s/86400)+'d';}
function renderRooms(){
  const byRoom={};Object.values(nodes).forEach(n=>{(byRoom[n.room||'Unassigned']=byRoom[n.room||'Unassigned']||[]).push(n);});
  const el=document.getElementById('rooms');
  const keys=Object.keys(byRoom).sort();
  if(!keys.length){el.innerHTML='<div class="empty">Waiting for nodes…</div>';return;}
  el.innerHTML=keys.map(room=>'<div class="room panel"><div class="rh">'+room+'</div><div class="ncards">'+
    byRoom[room].map(n=>nodeCard(n)).join('')+'</div></div>').join('');
  drawAll(el);
}
function nodeCard(n){const r=n.last,s=r.sev||0,stale=n.age>=ACTIVE_S;
  let h='<div class="ncard s'+s+(stale?' off':'')+'" onclick="openD(\''+n.id+'\')">';
  h+='<div class="nh"><span class="nn">'+(n.id||n.mac)+'</span><span class="badge b'+s+'">'+SEVW[s]+'</span></div>';
  CARD.forEach(k=>{const sk=r['s_'+k]||0;
    h+='<div class="mrow"><span class="mut">'+M[k][0]+'</span><span class="mv s'+sk+'">'+fmt(r[k])+' '+M[k][1]+'</span></div>';
    h+='<canvas class="spark" width="240" height="30" data-n="'+n.id+'" data-m="'+k+'"></canvas>';});
  return h+'</div>';
}
function openD(id){openId=id;document.getElementById('ov').style.display='flex';paintD();}
function closeD(){openId=null;document.getElementById('ov').style.display='none';}
function paintD(){const n=nodes[openId];if(!n){closeD();return;}const r=n.last,s=r.sev||0;
  document.getElementById('dTitle').textContent=nameOf(r);
  document.getElementById('dBadge').innerHTML=s?'<span class="badge b'+s+'">'+SEVW[s]+'</span>':'';
  document.getElementById('dId').textContent=(n.id||'')+'  ·  '+(n.mac||'')+(n.ip?('  ·  '+n.ip):'');
  document.getElementById('dGrid').innerHTML=DLG.map(k=>{const sk=r['s_'+k]||0;
    return '<div class="dm"><div class="dh"><span class="dl">'+M[k][0]+'</span><span class="dv s'+sk+'">'+fmt(r[k])+' '+M[k][1]+'</span></div>'+
      '<canvas class="big" width="360" height="66" data-n="'+n.id+'" data-m="'+k+'"></canvas></div>';}).join('');
  drawAll(document.getElementById('dGrid'));
}

// ---- export ---------------------------------------------------------------
function csvFor(id){const h=hist[id];if(!h)return'';const n=Math.max(...DLG.map(k=>h[k].length));
  let out='sample,eco2,tvoc,temp,hum,gas\n';
  for(let i=0;i<n;i++){out+=i+','+DLG.map(k=>{const p=h[k][i];return p&&p.v!=null?p.v:'';}).join(',')+'\n';}
  return out;}
function dl(name,text){const b=new Blob([text],{type:'text/csv'}),u=URL.createObjectURL(b);
  const a=document.createElement('a');a.href=u;a.download=name;a.click();URL.revokeObjectURL(u);}
function exportNode(id){if(id)dl((id||'node')+'.csv',csvFor(id));}
function exportAll(){Object.keys(nodes).forEach(id=>dl(id+'.csv',csvFor(id)));}

// ---- clock + boot ---------------------------------------------------------
const WD=['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];
function clock(){const d=new Date();
  document.getElementById('ckT').textContent=pad(d.getHours())+':'+pad(d.getMinutes());
  document.getElementById('ckD').textContent=WD[d.getDay()];
  document.getElementById('ckDt').textContent=pad(d.getDate())+'.'+pad(d.getMonth()+1)+'.'+String(d.getFullYear()).slice(2);}
clock();setInterval(clock,1000);poll();setInterval(poll,2000);
</script></body></html>)HTML";
