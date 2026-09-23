#ifndef PORTAL_PAGE_H
#define PORTAL_PAGE_H

// ============================================================
// PortalPage.h  -  the entire web UI, embedded in flash.
//
// Deliberately NOT stored in LittleFS: embedding it means you just
// compile and upload as usual, with no separate "upload filesystem
// image" step (which needs an extra plugin in the Arduino IDE and
// is a common source of "why is my page blank").
//
// Served with server.send_P(), which streams straight out of flash -
// no 14 KB String copy on the heap.
//
// No external fonts/scripts/styles are referenced. There is no
// internet on this AP, so anything external would simply never load.
// Bengali text renders using the phone's own system Bengali font.
// ============================================================

#include <Arduino.h>

static const char PORTAL_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="bn"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LoRa Communicator</title>
<style>
*{box-sizing:border-box}
:root{--bg:#101418;--card:#1a2027;--line:#2b333c;--fg:#e8edf2;--dim:#8b98a5;--acc:#3ea6ff;--ok:#3ddc97;--warn:#ffb74d;--bad:#ff6b6b}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 system-ui,-apple-system,"Noto Sans Bengali","Segoe UI",Roboto,sans-serif;padding-bottom:24px}
header{padding:14px 16px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:10px;position:sticky;top:0;background:var(--bg);z-index:5}
header h1{font-size:16px;margin:0;font-weight:600;flex:1}
#batt{font-size:13px;color:var(--dim);white-space:nowrap}
nav{display:flex;border-bottom:1px solid var(--line);position:sticky;top:53px;background:var(--bg);z-index:4}
nav button{flex:1;background:none;border:0;border-bottom:2px solid transparent;color:var(--dim);padding:11px 2px;font:inherit;font-size:13px;cursor:pointer;white-space:nowrap}
nav button.on{color:var(--acc);border-bottom-color:var(--acc)}
main{padding:16px;max-width:640px;margin:0 auto}
section{display:none}section.on{display:block}
textarea,input,select{width:100%;background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:10px;font:inherit}
textarea{min-height:104px;resize:vertical}
label{display:block;font-size:12px;color:var(--dim);margin:12px 0 4px;text-transform:uppercase;letter-spacing:.04em}
button.go{width:100%;background:var(--acc);color:#06121c;border:0;border-radius:8px;padding:13px;font:inherit;font-weight:700;font-size:16px;margin-top:12px;cursor:pointer}
button.go:disabled{background:var(--line);color:var(--dim)}
button.sm{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:7px;padding:8px 12px;font:inherit;font-size:13px;cursor:pointer}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.row>*{flex:1}
.meta{display:flex;justify-content:space-between;font-size:12px;color:var(--dim);margin-top:6px;gap:8px}
.warn{color:var(--warn)}.bad{color:var(--bad)}.ok{color:var(--ok)}
.chips{display:flex;flex-wrap:wrap;gap:7px;margin-top:8px}
.chips button{background:var(--card);border:1px solid var(--line);color:var(--fg);border-radius:16px;padding:7px 13px;font:inherit;font-size:14px;cursor:pointer}
.chips button:active{border-color:var(--acc)}
.msg{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:11px 12px;margin-bottom:8px}
.msghd{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:4px}
.msg .n{font-size:11px;color:var(--dim)}
.msg .n em{font-style:normal;opacity:.75;margin-left:6px}
.msg .t{word-break:break-word}
.msg .rs{background:none;border:1px solid var(--line);color:var(--acc);border-radius:14px;padding:4px 12px;font:inherit;font-size:12px;cursor:pointer;flex:none}
.ro{font-size:12px;color:var(--dim);margin-bottom:7px}
.big{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px;text-align:center;margin-bottom:12px}
.big b{display:block;font-size:11px;color:var(--dim);font-weight:400;text-transform:uppercase;letter-spacing:.05em;margin-bottom:5px}
.big i{font-style:normal;font-size:26px;font-family:ui-monospace,monospace;letter-spacing:.06em}
.big.no i{color:var(--warn);font-size:19px}
.mono{font-family:ui-monospace,monospace}
.pre{display:flex;gap:7px;align-items:center;margin-bottom:7px}
.pre span{width:26px;font-size:12px;color:var(--dim);text-align:right;flex:none}
.pre input{flex:1}
.pre .e{color:var(--warn);font-size:15px;flex:none;width:14px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 10px}
.kv{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:9px 11px}
.kv b{display:block;font-size:11px;color:var(--dim);font-weight:400;text-transform:uppercase;letter-spacing:.04em}
.kv i{font-style:normal;font-size:17px;font-variant-numeric:tabular-nums}
.note{font-size:12px;color:var(--dim);margin-top:14px;border-left:2px solid var(--line);padding-left:10px}
#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:20px;background:var(--ok);color:#06121c;padding:11px 20px;border-radius:22px;font-weight:600;opacity:0;transition:opacity .25s;pointer-events:none;z-index:20;max-width:90vw;text-align:center}
#toast.on{opacity:1}
#toast.err{background:var(--bad);color:#fff}
</style></head><body>

<header><h1>LoRa কমিউনিকেটর</h1><div id="batt">...</div></header>

<nav>
  <button class="on" data-t="send">পাঠান</button>
  <button data-t="box">ইনবক্স</button>
  <button data-t="pre">প্রিসেট</button>
  <button data-t="cfg">রেডিও</button>
  <button data-t="pair">পেয়ার</button>
</nav>

<main>
<div class="note" id="route">মোড লোড হচ্ছে...</div>
<div class="note" id="requestBanner" hidden>
  <b id="requestText"></b>
  <button class="sm" id="acceptBanner">গ্রহণ করুন</button>
  <button class="sm" id="rejectBanner">প্রত্যাখ্যান করুন</button>
</div>

<section id="send" class="on">
  <label>নিজের বার্তা লিখুন</label>
  <textarea id="tx" placeholder="বাংলায় লিখুন..."></textarea>
  <div class="meta"><span id="bytes">0 / 180 বাইট</span><span id="air">airtime --</span></div>
  <button class="go" id="btnsend">পাঠান</button>
  <label>প্রিসেট বার্তা (সরাসরি পাঠান)</label>
  <div class="chips" id="chips"></div>
  <div class="note">বাংলা এক অক্ষর = ৩ বাইট। ধীর সেটিংসে (SF12) লম্বা বার্তা পাঠাতে অনেক সময় লাগে — উপরে airtime দেখুন।</div>
</section>

<section id="box">
  <div class="row">
    <button class="sm" id="btnref">রিফ্রেশ</button>
    <button class="sm" id="btndl">ডাউনলোড</button>
    <button class="sm" id="btnclr">মুছে ফেলুন</button>
  </div>
  <div class="ro" id="boxcount" style="margin-top:14px"></div>
  <div id="list"></div>
</section>

<section id="pre">
  <div class="ro" id="prero" hidden>এই বিল্ডে প্রিসেট শুধু দেখা যায়, বদলানো যায় না (pixel-perfect বিটম্যাপ অটুট রাখার জন্য)।</div>
  <div id="prelist"></div>
  <button class="go" id="btnpre">সংরক্ষণ করুন</button>
  <button class="sm" id="btnprer" style="width:100%;margin-top:9px">ডিফল্টে ফিরুন</button>
  <div class="note">⚠ চিহ্নিত প্রিসেট আপনি বদলেছেন। বদলানো লেখা e-paper এ pixel-perfect বিটম্যাপের বদলে fallback রেন্ডারার দিয়ে আঁকা হয় — যুক্তাক্ষর কিছু ক্ষেত্রে ভুল দেখাতে পারে। ২০টি প্রিসেটই এখন বিটম্যাপসহ আছে, তাই না বদলানোই ভালো।</div>
</section>

<section id="cfg">
  <div class="grid" id="stat"></div>
  <label>ফ্রিকোয়েন্সি (MHz)</label><input id="f" type="number" step="0.1" min="410" max="525">
  <label>Spreading factor</label>
  <select id="sf"><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select>
  <label>Bandwidth</label>
  <select id="bw"><option value="62500">62.5 kHz</option><option value="125000">125 kHz</option><option value="250000">250 kHz</option><option value="500000">500 kHz</option></select>
  <label>Coding rate</label>
  <select id="cr"><option value="5">4/5</option><option value="6">4/6</option><option value="7">4/7</option><option value="8">4/8</option></select>
  <label>TX power (dBm)</label><input id="pw" type="number" min="2" max="20">
  <label>Sync word (0-255)</label><input id="sy" type="number" min="0" max="255">
  <button class="go" id="btncfg">প্রয়োগ ও সংরক্ষণ</button>
  <div class="note">দুইটি নোডে <b>একই</b> ফ্রিকোয়েন্সি, SF, bandwidth, coding rate এবং sync word থাকতে হবে — না হলে যোগাযোগ হবে না। SF12 সবচেয়ে দূরে যায় কিন্তু সবচেয়ে ধীর; সাধারণ ব্যবহারে SF9 ভালো ভারসাম্য।</div>
</section>

<section id="pair">
  <div class="big" id="pairbig"></div>
  <div class="grid" id="pairkv"></div>
  <label>এই মডিউলের আইডি (একবারই সেট হয়)</label>
  <select id="identity"></select>
  <button class="go" id="saveIdentity">আইডি সংরক্ষণ</button>
  <button class="sm" id="scanIdentity" style="width:49%;margin-top:9px">আবার খুঁজুন</button>
  <button class="sm" id="resetIdentity" style="width:49%;margin-top:9px;float:right">আইডি মুছুন</button>
  <div class="note" id="identityNotice"></div>
  <div class="note">আইডি একবারই বাছতে হয়। সংরক্ষণের পর প্রতিবার চালু হলে সেটিই থাকবে, ই-পেপারে আর আইডির পর্দা আসবে না। বদলাতে হলে “আইডি মুছুন” চাপুন (নতুন কোড আপলোড করলেই মুছে যায় না)।</div>
  <div class="note">“ব্যবহৃত” লেখা আইডি অন্য মডিউল আগেই নিয়েছে, তাই সেগুলো বাছা যাবে না। কোনো মডিউল নতুন চালু হলে “আবার খুঁজুন” চাপলে তালিকা হালনাগাদ হবে।</div>
  <label>ব্যক্তিগত অনুরোধ পাঠান</label>
  <select id="destination"></select>
  <button class="go" id="requestPrivate">অনুরোধ পাঠান</button>
  <button class="sm" id="disconnectPrivate" style="width:100%;margin-top:9px">বাতিল / আনপেয়ার</button>
  <div class="note" id="privateNotice"></div>
  <div class="note">সাধারণ মোডে বার্তা অন্য সব উপলব্ধ মডিউল পাবে। ব্যক্তিগত সংযোগে শুধু নির্বাচিত দুই মডিউল পরস্পরের বার্তা পাবে। প্রাপক গ্রহণ করলে সংযোগ হবে। তালিকায় সব ১৯টি আইডি থাকবে; এটি অনলাইনে থাকার প্রমাণ নয়।</div>
  <div class="note">GPIO 27: তালিকা খুলুন / পরের আইডি। GPIO 13: পরের আইডি, দুইবার আগের আইডি। GPIO 14: নির্বাচন / গ্রহণ। GPIO 27 দুইবার: আনপেয়ার। ধরে রাখুন: Wi-Fi।</div>
  <div class="note">ব্যক্তিগত মোড ঠিকানা দিয়ে বার্তা বাছাই করে; রেডিও বার্তা এনক্রিপ্ট করা হয় না।</div>
</section>

</main>
<div id="toast"></div>

<script>
var LIMIT=180, presets=[], edited=[], canEdit=true, storeCap=100, fleetStatus=null;
var $=function(s){return document.querySelector(s)};

function toast(m,bad){var t=$('#toast');t.textContent=m;t.className='on'+(bad?' err':'');
  clearTimeout(t._h);t._h=setTimeout(function(){t.className=''},2200)}

function nbytes(s){return new TextEncoder().encode(s).length}

/* Semtech SX127x time-on-air. Explicit header, CRC on, preamble 8.
   Low-datarate-optimize is on automatically when symbol time > 16 ms,
   which arduino-LoRa also does inside setSpreadingFactor(). */
function airtime(pl,sf,bw,crDen){
  var de=(Math.pow(2,sf)/bw>0.016)?1:0, cr=crDen-4;
  var tsym=Math.pow(2,sf)/bw*1000;
  var num=8*pl-4*sf+28+16-0, den=4*(sf-2*de);
  var n=8+Math.max(Math.ceil(num/den)*(cr+4),0);
  return (8+4.25)*tsym+n*tsym;
}
function fmtAir(ms){return ms<1000?Math.round(ms)+' ms':(ms/1000).toFixed(1)+' s'}

var cfg={freq:433000000,sf:12,bw:125000,cr:8,pwr:20,sync:18};

function upd(){
  var b=nbytes($('#tx').value);
  var e=$('#bytes'), a=$('#air');
  e.textContent=b+' / '+LIMIT+' বাইট';
  e.className=b>LIMIT?'bad':(b>LIMIT*0.8?'warn':'');
  if(b===0){a.textContent='airtime --';a.className='';}
  else{var t=airtime(b+8,cfg.sf,cfg.bw,cfg.cr);
    a.textContent='airtime ~'+fmtAir(t);
    a.className=t>8000?'bad':(t>3000?'warn':'ok');}
  $('#btnsend').disabled=(b===0||b>LIMIT||!fleetStatus||!fleetStatus.nid||(fleetStatus.mode!==0&&fleetStatus.mode!==5));
}

function tab(n){
  document.querySelectorAll('nav button').forEach(function(x){x.classList.toggle('on',x.dataset.t===n)});
  document.querySelectorAll('section').forEach(function(x){x.classList.toggle('on',x.id===n)});
  if(n==='box')loadBox(); if(n==='cfg'||n==='pair')loadStat();
}
document.querySelectorAll('nav button').forEach(function(b){b.onclick=function(){tab(b.dataset.t)}});

function post(u,body){return fetch(u,{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:body||''})}

function checkedText(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t})}
function sendText(t){post('/api/send',t).then(checkedText).then(function(){toast('পাঠানো হচ্ছে...')}).catch(function(e){toast(e.message,1)})}
$('#tx').oninput=upd;

$('#btnsend').onclick=function(){
  var t=$('#tx').value.trim(); if(!t)return;
  $('#btnsend').disabled=true;
  post('/api/send',t).then(checkedText).then(function(){
    $('#tx').value='';upd();toast('পাঠানো হচ্ছে...');
  }).catch(function(e){toast(e.message,1);upd()});
};

function loadPresets(){
  fetch('/api/presets').then(function(r){return r.json()}).then(function(j){
    presets=j.items;edited=j.edited;
    var c=$('#chips');c.innerHTML='';
    presets.forEach(function(p,i){
      var b=document.createElement('button');b.textContent=p;
      b.onclick=function(){sendText(p)};
      c.appendChild(b);
    });
    $('#prero').hidden = canEdit;
    $('#btnpre').hidden = !canEdit;
    $('#btnprer').hidden = !canEdit;
    var l=$('#prelist');l.innerHTML='';
    presets.forEach(function(p,i){
      var d=document.createElement('div');d.className='pre';
      d.innerHTML='<span>'+(i+1)+'</span><input><em class="e"></em>';
      var inp=d.querySelector('input');
      inp.value=p; inp.readOnly=!canEdit;
      d.querySelector('.e').textContent=edited[i]?'⚠':'';
      l.appendChild(d);
    });
  });
}

$('#btnpre').onclick=function(){
  var ins=document.querySelectorAll('#prelist input'), n=0, chain=Promise.resolve();
  ins.forEach(function(inp,i){
    var v=inp.value.trim();
    if(v && v!==presets[i]){n++;chain=chain.then(function(){return post('/api/preset?i='+i,v)})}
  });
  chain.then(function(){toast(n?n+' টি সংরক্ষিত':'কোনো পরিবর্তন নেই');loadPresets()});
};
$('#btnprer').onclick=function(){post('/api/presets/reset').then(function(){toast('ডিফল্টে ফেরানো হয়েছে');loadPresets()})};

function loadBox(){
  fetch('/api/inbox').then(function(r){return r.json()}).then(function(j){
    var l=$('#list');
    $('#boxcount').textContent=j.items.length+' / '+storeCap+' সংরক্ষিত';
    if(!j.items.length){l.innerHTML='<div class="note">কোনো সংরক্ষিত বার্তা নেই।</div>';return}
    l.innerHTML='';
    j.items.forEach(function(m,i){
      var txt=m.t, rssi=m.r;
      var d=document.createElement('div');d.className='msg';
      var hd=document.createElement('div');hd.className='msghd';
      var n=document.createElement('div');n.className='n';
      n.textContent='#'+(i+1)+(i?'':' — সর্বশেষ');
      if(rssi){var e=document.createElement('em');e.textContent='· '+rssi+' dBm';n.appendChild(e)}
      var b=document.createElement('button');b.className='rs';b.textContent='ফেরত পাঠান';
      b.onclick=function(){sendText(txt)};
      hd.appendChild(n);hd.appendChild(b);
      var t=document.createElement('div');t.className='t';t.textContent=txt;
      d.appendChild(hd);d.appendChild(t);l.appendChild(d);
    });
  });
}
$('#btnref').onclick=loadBox;
$('#btndl').onclick=function(){location.href='/api/export'};
$('#btnclr').onclick=function(){post('/api/inbox/clear').then(function(){toast('মুছে ফেলা হয়েছে');loadBox()})};

function loadStat(){
  return fetch('/api/status').then(function(r){return r.json()}).then(function(j){
    cfg={freq:j.freq,sf:j.sf,bw:j.bw,cr:j.cr,pwr:j.pwr,sync:j.sync};
    canEdit=(j.pe!==false);
    storeCap=j.scap||100;
    renderPair(j);
    $('#f').value=(j.freq/1e6).toFixed(1);
    $('#sf').value=j.sf;$('#bw').value=j.bw;$('#cr').value=j.cr;
    $('#pw').value=j.pwr;$('#sy').value=j.sync;
    var b=j.batt<0?'—':j.batt+'%';
    $('#stat').innerHTML=
      kv('ব্যাটারি',b+(j.batt<0?'':' · '+j.mv.toFixed(2)+'V'))+
      kv('শেষ RSSI',j.rssi?j.rssi+' dBm':'—')+
      kv('শেষ SNR',j.rssi?j.snr.toFixed(1)+' dB':'—')+
      kv('পাঠানো / পাওয়া',j.tx+' / '+j.rx)+
      kv('ফ্রি RAM',(j.heap/1024).toFixed(0)+' KB')+
      kv('আপটাইম',Math.floor(j.up/60)+'m '+(j.up%60)+'s')+
      kv('সংরক্ষিত বার্তা',j.sc+' / '+j.scap)+
      kv('ইনবক্স ফাইল',(j.sbytes/1024).toFixed(0)+' KB');
    upd();
  });
}
function kv(k,v){return '<div class="kv"><b>'+k+'</b><i>'+v+'</i></div>'}

function fleetName(id){return id>=1&&id<=20 ? String.fromCharCode(65+Math.floor((id-1)/10))+((id-1)%10) : '--'}
// taken is the bitmask from /api/status: bit 0 = A0 ... bit 19 = B9.
// An ID another module has claimed is listed but not selectable, so you
// can see why it is missing rather than wondering where it went.
function fillFleet(select,exclude,taken,lock){
  var previous=select.value; select.innerHTML='';
  for(var i=1;i<=20;i++) if(i!==exclude){
    var used=lock&&taken?((taken>>>(i-1))&1):0;
    var o=document.createElement('option');
    o.value=fleetName(i); o.textContent=fleetName(i)+(used?' — ব্যবহৃত':'');
    o.disabled=!!used; select.appendChild(o);
  }
  if(Array.from(select.options).some(function(o){return o.value===previous&&!o.disabled}))select.value=previous;
}
fillFleet($('#identity'),0,0,1);
function renderPair(j){
  var old=fleetStatus; fleetStatus=j;
  var modes=['Broadcast','Request sent','Incoming request','Connecting','Connecting','Private'];
  var mode=j.nid ? modes[j.mode] : 'Choose ID';
  $('#pairbig').innerHTML='<b>My ID: '+fleetName(j.nid)+'</b><i>'+mode+'</i>';
  $('#pairkv').innerHTML=kv('Peer',fleetName(j.peer))+kv('Time left',j.pleft?j.pleft+'s':'—');
  $('#route').textContent='My ID: '+fleetName(j.nid)+' · '+mode+(j.peer?' · '+fleetName(j.peer):' · ALL');
  $('#privateNotice').textContent=j.notice;
  $('#requestBanner').hidden=j.mode!==2;
  $('#requestText').textContent='Private request from '+fleetName(j.peer)+' ('+j.pleft+'s)';
  if(!old||old.nid!==j.nid||old.taken!==j.taken){
    fillFleet($('#destination'),j.nid,j.taken,0);
    fillFleet($('#identity'),0,j.taken,1);
    if(j.nid)$('#identity').value=fleetName(j.nid);
  }
  $('#identityNotice').textContent = j.scan
      ? 'অন্য মডিউলগুলো কোন আইডি নিয়েছে তা খোঁজা হচ্ছে…'
      : (j.idset ? 'আইডি সংরক্ষিত: '+fleetName(j.nid)+' — চালু হলে আর জিজ্ঞেস করা হবে না।'
                 : 'এখনো আইডি বাছা হয়নি।');
  $('#requestPrivate').disabled=!j.nid||j.mode!==0;
  // Once an ID is committed the save control is locked: changing it is
  // what "আইডি মুছুন" is for, and doing it by accident would leave the
  // other modules addressing a unit that no longer answers.
  $('#saveIdentity').disabled=j.mode!==0||!!j.idset;
  $('#identity').disabled=j.mode!==0||!!j.idset;
  $('#resetIdentity').disabled=j.mode!==0||!j.idset;
  upd();
}
function fleetAction(url){
  return post(url).then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t);return t})})
    .then(function(){toast('ঠিক আছে');return loadStat()}).catch(function(e){toast(e.message,1)});
}
$('#saveIdentity').onclick=function(){fleetAction('/api/identity?id='+encodeURIComponent($('#identity').value))};
$('#scanIdentity').onclick=function(){fleetAction('/api/identity/scan?forget=1')};
$('#resetIdentity').onclick=function(){
  if(confirm('আইডি মুছে ফেলা হবে। মডিউল আবার আইডি জিজ্ঞেস করবে। চালিয়ে যাবেন?'))
    fleetAction('/api/identity/reset');
};
$('#requestPrivate').onclick=function(){fleetAction('/api/private/request?id='+encodeURIComponent($('#destination').value))};
$('#acceptBanner').onclick=function(){if(fleetStatus)fleetAction('/api/private/accept?id='+fleetName(fleetStatus.peer)+'&session='+fleetStatus.session)};
$('#rejectBanner').onclick=$('#disconnectPrivate').onclick=function(){fleetAction('/api/private/disconnect')};

$('#btncfg').onclick=function(){
  var q='/api/config?freq='+Math.round(parseFloat($('#f').value)*1e6)+
    '&sf='+$('#sf').value+'&bw='+$('#bw').value+'&cr='+$('#cr').value+
    '&pwr='+$('#pw').value+'&sync='+$('#sy').value;
  post(q).then(function(r){return r.text()}).then(function(t){
    if(t.indexOf('ok')===0){toast('প্রয়োগ করা হয়েছে');loadStat()}else{toast('ভুল মান',1)}
  });
};

function poll(){
  fetch('/api/status').then(function(r){return r.json()}).then(function(j){
    $('#batt').textContent=(j.batt<0?'':j.batt+'% · ')+'RX '+j.rx+' · TX '+j.tx;
    renderPair(j);
  }).catch(function(){});
}

loadStat().then(loadPresets);upd();setInterval(poll,3000);poll();
</script></body></html>)PAGE";

#endif  // PORTAL_PAGE_H
