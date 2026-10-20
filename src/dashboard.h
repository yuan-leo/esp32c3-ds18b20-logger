#pragma once
#include <Arduino.h>
static const char DASHBOARD_HTML[] PROGMEM = R"LOGGERHTML(
<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-C3 Temperature Logger</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0b1320;color:#e6f2ff}
*{box-sizing:border-box}body{margin:0}main{max-width:1080px;margin:auto;padding:20px}
h1{margin:0 0 6px}.muted{color:#a4bad2;font-size:13px}h2{margin:0 0 12px;font-size:18px}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}
.card{background:#172437;border:1px solid #2c4561;border-radius:14px;padding:18px;margin:12px 0}
.metric{font-size:29px;font-weight:700;color:#38e0c0;margin:7px 0}
.controls{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0}
button,a.btn{background:#25415c;border:1px solid #42708f;color:white;border-radius:8px;padding:10px 13px;text-decoration:none;cursor:pointer;font:inherit}
button.active,a.btn{background:#39dbc1;color:#061c23}
canvas{width:100%;height:310px;display:block;touch-action:pan-y}
.info{display:grid;grid-template-columns:1fr auto;gap:9px;font-size:14px}
form{display:flex;gap:10px;flex-wrap:wrap;align-items:end}label{flex:1;min-width:160px;font-size:13px}
input{display:block;width:100%;padding:10px;margin-top:5px;border:1px solid #3e6885;border-radius:7px;background:#0b1320;color:white}
@media(max-width:730px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}canvas{height:245px}.metric{font-size:24px}}
</style></head><body><main>
<h1>Temperature Logger</h1><div class="muted" id="conn">Connecting…</div>
<div class="grid">
<div class="card"><div class="muted">CURRENT</div><div id="current" class="metric">--</div><div class="muted" id="last">No samples</div></div>
<div class="card"><div class="muted">MINIMUM</div><div id="min" class="metric">--</div></div>
<div class="card"><div class="muted">MAXIMUM</div><div id="max" class="metric">--</div></div>
<div class="card"><div class="muted">AVERAGE</div><div id="avg" class="metric">--</div></div>
</div>
<section class="card"><h2>Temperature history</h2>
<div class="controls" id="ranges">
<button data-n="720" class="active">1 hour</button><button data-n="4320">6 hours</button>
<button data-n="17280">24 hours</button><button data-n="120960">7 days</button>
<button data-n="0">All</button></div>
<canvas id="graph" aria-label="Temperature graph"></canvas>
<div class="muted" id="graphtext">Loading history…</div>
<div class="controls"><button id="units">Show °F</button><button id="refresh">Refresh</button>
<a class="btn" href="/export.csv">Export full CSV</a></div>
<div class="muted">One sensor reading every 5 seconds. Chart is downsampled for speed; CSV contains every retained sample.</div>
</section>
<div class="grid" style="grid-template-columns:repeat(auto-fit,minmax(280px,1fr))">
<section class="card"><h2>Logger status</h2><div class="info">
<span>Sensor</span><strong id="sensor">--</strong>
<span>Interval</span><strong>5 seconds</strong>
<span>Samples stored</span><strong id="count">--</strong>
<span>Flash used</span><strong id="space">--</strong>
<span>Clock</span><strong id="clock">--</strong>
<span>Home Wi-Fi</span><strong id="wifi">--</strong>
<span>LAN address</span><strong id="ip">--</strong>
<span>Hotspot</span><strong id="hotspot">--</strong>
<span>Wi-Fi TX power cap</span><strong id="txPower">--</strong>
<span>Lost samples</span><strong id="lost">--</strong>
</div></section>
<section class="card"><h2>Home Wi-Fi setup</h2>
<form id="wifiForm">
<label>Network name<input name="ssid" maxlength="32" required></label>
<label>Password<input name="password" type="password" maxlength="63"></label>
<button>Save and connect</button></form><div class="muted" id="wifiMessage"></div>
<p class="muted">The direct hotspot stays enabled. Use only on a trusted local network; HTTP is not encrypted.</p>
<button id="clear">Clear saved log</button><p class="muted">Export before clearing. Erasure cannot be undone.</p>
</section></div>
<div class="muted">No cloud, subscriptions, or external libraries required for the webpage.</div>
</main><script>
'use strict';
const el=id=>document.getElementById(id);
let rows=[],summary={},status=null,unit='C',limit=720;
try{unit=localStorage.getItem('temperatureUnit')==='F'?'F':'C'}catch(e){}
function value(c){return Number.isFinite(c)?(unit==='F'?c*1.8+32:c).toFixed(2)+' °'+unit:'--'}
function timeText(r){if(!r)return'No readings';return (r[2]&2)?new Date(r[0]*1000).toLocaleString():'Uptime '+r[0]+' s'}
function redraw(){
 const canvas=el('graph'),width=Math.max(200,canvas.clientWidth),height=Math.max(180,canvas.clientHeight);
 const dpr=window.devicePixelRatio||1;
 canvas.width=Math.round(width*dpr);canvas.height=Math.round(height*dpr);
 const g=canvas.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);
 g.clearRect(0,0,width,height);
 const left=53,right=15,top=15,bottom=35,plotW=width-left-right,plotH=height-top-bottom;
 const valid=rows.map((r,i)=>({r,i})).filter(x=>x.r[2]&1);
 if(!valid.length){g.fillStyle='#9baec6';g.font='14px system-ui';g.fillText('No valid samples yet',left+15,top+plotH/2);return}
 const nums=valid.map(x=>x.r[1]/100),low=Math.min(...nums),high=Math.max(...nums);
 const extra=Math.max((high-low)*.15,.6),lo=low-extra,hi=high+extra;
 const xx=i=>left+(rows.length===1?.5:i/(rows.length-1))*plotW;
 const yy=c=>top+plotH*(hi-c)/(hi-lo);
 g.font='12px system-ui';g.lineWidth=1;
 for(let k=0;k<=4;k++){const y=top+k*plotH/4;g.strokeStyle='#34506b';g.beginPath();g.moveTo(left,y);g.lineTo(width-right,y);g.stroke();g.fillStyle='#a6b9d0';g.fillText(value(hi-k*(hi-lo)/4),4,y+4)}
 g.fillStyle='#a6b9d0';const ticks=width<500?2:4;
 for(let k=0;k<=ticks;k++){const i=Math.round((rows.length-1)*k/ticks);if(!rows[i])continue;const r=rows[i],label=(r[2]&2)?new Date(r[0]*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}):'+'+Math.floor(r[0]/60)+'m';g.fillText(label,Math.max(2,Math.min(width-67,xx(i)-23)),height-9)}
 g.lineWidth=2;g.lineJoin='round';g.strokeStyle='#36e5c5';g.beginPath();let connected=false;
 rows.forEach((r,i)=>{if(!(r[2]&1)){connected=false;return}const x=xx(i),y=yy(r[1]/100);if(connected)g.lineTo(x,y);else g.moveTo(x,y);connected=true});g.stroke()
}
function refreshStats(){
 el('min').textContent=value(summary.min_c);
 el('max').textContent=value(summary.max_c);
 el('avg').textContent=value(summary.avg_c);
 el('graphtext').textContent=(summary.count||0).toLocaleString()+' valid readings in selected period; '+rows.length+' chart points';
 if(status&&status.latest_valid)el('current').textContent=value(status.latest_c);
 redraw()
}
async function getStatus(){
 try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const s=await r.json();status=s;
 el('conn').textContent=s.sensor_found?'Sensor connected':'Sensor disconnected';
 el('current').textContent=s.latest_valid?value(s.latest_c):'--';
 el('last').textContent=timeText(s.latest_record);
 el('sensor').textContent=s.sensor_found?'DS18B20 connected':'Not detected';
 el('count').textContent=s.samples.toLocaleString();
 el('space').textContent=(s.log_bytes/1048576).toFixed(2)+' MiB / '+(s.fs_total/1048576).toFixed(2)+' MiB';
 el('clock').textContent=s.clock_synced?'UTC':'Uptime only';
 el('wifi').textContent=s.wifi_connected?s.wifi_ssid:'Not connected';
 el('ip').textContent=s.wifi_connected?'http://'+s.sta_ip+'/':'--';
 el('hotspot').textContent=s.ap_ssid+' ('+s.ap_ip+')';
 el('txPower').textContent=s.wifi_tx_limit_dbm+' dBm max (reported '+s.wifi_tx_reported_dbm+' dBm)'+(s.wifi_tx_cap_ok?'':' — check firmware log');
 el('lost').textContent=s.dropped+(s.fs_ok?'':' — storage error')
 }catch(e){el('conn').textContent='Logger unreachable'}
}
async function getHistory(){
 try{const r=await fetch('/api/history?samples='+limit,{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const j=await r.json();rows=j.rows||[];summary=j.stats||{};refreshStats()}
 catch(e){el('graphtext').textContent='Could not retrieve history: '+e.message}
}
function refresh(){getStatus();getHistory()}
el('ranges').addEventListener('click',e=>{const b=e.target.closest('button[data-n]');if(!b)return;limit=Number(b.dataset.n);document.querySelectorAll('#ranges button').forEach(x=>x.classList.toggle('active',x===b));getHistory()});
el('units').addEventListener('click',()=>{unit=unit==='C'?'F':'C';el('units').textContent=unit==='C'?'Show °F':'Show °C';try{localStorage.setItem('temperatureUnit',unit)}catch(e){}refreshStats()});
el('refresh').addEventListener('click',refresh);
el('wifiForm').addEventListener('submit',async e=>{e.preventDefault();const body=new URLSearchParams(new FormData(e.target));try{const r=await fetch('/api/wifi',{method:'POST',body});if(!r.ok)throw Error('HTTP '+r.status);el('wifiMessage').textContent='Saved. Connecting; direct hotspot remains available.';e.target.querySelector('[name=password]').value=''}catch(err){el('wifiMessage').textContent='Failed: '+err.message}});
el('clear').addEventListener('click',async()=>{if(!confirm('Delete ALL saved log readings? Export first.'))return;if(!confirm('This cannot be undone. Continue?'))return;try{const r=await fetch('/api/clear',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'confirm=erase'});if(!r.ok)throw Error('HTTP '+r.status);refresh()}catch(e){alert(e.message)}});
el('graph').addEventListener('pointerdown',e=>{if(!rows.length)return;const r=e.currentTarget.getBoundingClientRect();const fraction=Math.max(0,Math.min(1,(e.clientX-r.left-53)/(r.width-68)));const i=Math.round(fraction*(rows.length-1));const x=rows[i];el('graphtext').textContent=timeText(x)+' — '+((x[2]&1)?value(x[1]/100):'sensor error')});
window.addEventListener('resize',redraw);
el('units').textContent=unit==='C'?'Show °F':'Show °C';
fetch('/api/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'epoch='+Math.floor(Date.now()/1000)}).catch(()=>{}).finally(refresh);
setInterval(getStatus,5000);setInterval(()=>{if(!document.hidden)getHistory()},15000);
</script></body></html>
)LOGGERHTML";
