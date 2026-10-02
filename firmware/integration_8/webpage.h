// =====================================================
// webpage.h - the Bangla caregiver dashboard
//
// Served straight out of flash by the ESP32, so there is no
// separate filesystem upload step and no build tooling. Arduino
// compiles this alongside integration_8.ino automatically - you
// still just open the .ino.
//
// Kept in its own file on purpose: ~700 lines of HTML buried
// inside a 7,000-line sketch would make both unreadable.
//
// The history table is fed the RAW events.csv rather than JSON.
// Parsing CSV in the browser costs nothing; building JSON on the
// ESP32 would mean holding the whole log in RAM.
// =====================================================

#ifndef WEBPAGE_H
#define WEBPAGE_H

const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="bn">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ঔষধ যন্ত্র</title>
<style>
:root{
  --bg:#12161a; --card:#1b2127; --line:#2a333b; --ink:#e9edf1; --dim:#93a1ad;
  --brand:#2dd4bf; --brandink:#04211e;
  --ok:#4ade80; --okbg:#12301f; --bad:#f87171; --badbg:#331a1a;
  --warn:#fbbf24; --warnbg:#33280f;
}
*{box-sizing:border-box}
body{
  margin:0;background:var(--bg);color:var(--ink);
  font-family:"Noto Sans Bengali","Hind Siliguri","Nirmala UI",system-ui,sans-serif;
  line-height:1.75;font-size:16px;
}
header{
  position:sticky;top:0;z-index:9;display:flex;align-items:center;
  justify-content:space-between;gap:10px;padding:12px 16px;
  background:rgba(18,22,26,.92);backdrop-filter:blur(10px);
  border-bottom:1px solid var(--line);
}
h1{font-size:1.05rem;margin:0;font-weight:650}
.dot{width:9px;height:9px;border-radius:50%;background:#556;display:inline-block;margin-left:8px}
.dot.on{background:var(--ok);box-shadow:0 0 0 4px rgba(74,222,128,.2)}
nav{display:flex;gap:4px;overflow-x:auto;padding:8px 12px;border-bottom:1px solid var(--line)}
nav button{
  flex:none;border:0;background:transparent;color:var(--dim);cursor:pointer;
  padding:7px 14px;border-radius:999px;font:inherit;font-size:.93rem;white-space:nowrap;
}
nav button.on{background:var(--brand);color:var(--brandink);font-weight:600}
main{padding:16px;max-width:760px;margin:0 auto}
section{display:none}section.on{display:block}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.card h2{margin:0 0 10px;font-size:1rem}
.hero{background:linear-gradient(140deg,var(--brand),#0b7c72);color:var(--brandink);border:0}
.hero .lbl{font-size:.85rem;opacity:.85}
.hero .big{font-size:2rem;font-weight:700;line-height:1.35;margin:2px 0 4px}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:8px}
.stat{border-radius:14px;padding:14px 8px;text-align:center}
.stat b{display:block;font-size:1.7rem;line-height:1.2}
.stat span{font-size:.8rem;color:var(--dim)}
.s-ok{background:var(--okbg)} .s-ok b{color:var(--ok)}
.s-bad{background:var(--badbg)} .s-bad b{color:var(--bad)}
.s-warn{background:var(--warnbg)} .s-warn b{color:var(--warn)}
.hint{color:var(--dim);font-size:.86rem;margin:6px 2px 14px}
ul.health{list-style:none;margin:0;padding:0}
ul.health li{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--line);font-size:.93rem}
ul.health li:last-child{border:0}
.pill{font-size:.78rem;padding:2px 10px;border-radius:999px;font-weight:600}
.pill.y{background:var(--okbg);color:var(--ok)} .pill.n{background:var(--badbg);color:var(--bad)}
label{display:block;font-size:.84rem;color:var(--dim);margin-bottom:4px}
input,select{
  width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:10px;
  background:var(--bg);color:var(--ink);font:inherit;font-size:1rem;
}
input:focus,select:focus{outline:2px solid var(--brand)}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:10px}
button.go{
  width:100%;padding:11px;border:0;border-radius:10px;background:var(--brand);
  color:var(--brandink);font:inherit;font-weight:600;cursor:pointer;margin-top:4px;
}
button.ghost{
  padding:8px 14px;border-radius:999px;background:transparent;
  border:1px solid var(--line);color:var(--ink);font:inherit;font-size:.88rem;cursor:pointer;
}
.ev{display:flex;gap:10px;background:var(--card);border:1px solid var(--line);
    border-left:4px solid var(--line);border-radius:10px;padding:10px 12px;margin-bottom:8px}
.ev.TAKEN{border-left-color:var(--ok)}
.ev.MISSED,.ev.MISSED_POWER_OUT{border-left-color:var(--bad)}
.ev.UNCONFIRMED,.ev.WRONG_USER,.ev.UNKNOWN_FINGER,.ev.OVERRIDE{border-left-color:var(--warn)}
.ev b{font-size:.95rem} .ev small{display:block;color:var(--dim);font-size:.82rem}
.empty{text-align:center;color:var(--dim);padding:28px 10px;font-size:.92rem}
.tghelp{background:#0d2b2b;border:1px solid #174a48;border-radius:10px;
  padding:10px 12px;font-size:13px;line-height:1.7;color:#a9ccc8;margin-top:10px}
.tghelp b{color:#cfeee9}
.tghelp .warn{margin-top:6px;color:#f0b37e}
.row2{display:flex;gap:8px;align-items:stretch;margin-top:6px}
.medwarn{margin-top:6px;font-size:12.5px;line-height:1.6;color:#f0b37e}
.person{display:flex;align-items:center;gap:10px;padding:10px 0;
  border-bottom:1px solid #163a38}
.person:last-child{border-bottom:0}
.person .pn{flex:1;font-size:15px;color:#cfeee9}
.person .pn small{display:block;font-size:12px;color:#7fa8a3;margin-top:2px}
.boss{font-size:11px;background:#3b2d14;color:#f0b37e;border-radius:6px;
  padding:2px 7px;margin-left:6px;white-space:nowrap}
.enrbox{margin-top:12px;background:#0d2b2b;border:1px solid #174a48;
  border-radius:10px;padding:12px;font-size:14px;color:#cfeee9}
.enrbar{height:6px;background:#0a1f1f;border-radius:4px;margin-top:10px;
  overflow:hidden}
.enrbar i{display:block;height:100%;width:0;background:#2dd4bf;
  transition:width .3s}
.chk{display:flex;align-items:center;gap:10px;margin-top:12px;
  font-size:15px;color:#cfeee9;cursor:pointer}
.chk input{width:20px;height:20px;flex:none;margin:0;accent-color:#2dd4bf}
.row2 input{flex:1;margin:0}
.go.sm{width:auto;white-space:nowrap;padding:0 14px;margin:0;font-size:14px}
.sheet{position:fixed;inset:0;z-index:50;display:grid;place-items:center;
       padding:20px;background:rgba(0,0,0,.55)}
[hidden]{display:none !important}
.sheet .card{width:min(360px,100%);margin:0}
.sheet input{text-align:center;letter-spacing:.4em;font-size:1.3rem;margin-top:8px}
.acts{display:flex;gap:10px;margin-top:14px}.acts>*{flex:1}
.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);z-index:60;
       background:var(--ink);color:var(--bg);padding:10px 18px;border-radius:999px;font-size:.9rem}
.toast.bad{background:var(--bad);color:#fff}
</style>
</head>
<body>

<header>
  <h1>ঔষধ যন্ত্র<span class="dot" id="live"></span></h1>
  <button class="ghost" id="pinBtn">পিন</button>
</header>

<nav>
  <button class="on" data-p="sum">সারসংক্ষেপ</button>
  <button data-p="sch">সময়সূচি</button>
  <button data-p="his">ইতিহাস</button>
  <button data-p="dev">যন্ত্র</button>
</nav>

<main>
  <section id="sum" class="on">
    <div class="card hero">
      <div class="lbl">পরবর্তী ঔষধ</div>
      <div class="big" id="nTime">—</div>
      <div><span id="nUser">—</span> · <span id="nMed">—</span></div>
    </div>
    <div class="stats">
      <div class="stat s-ok"><b id="c1">০</b><span>গ্রহণ সম্পন্ন</span></div>
      <div class="stat s-bad"><b id="c2">০</b><span>করা হয়নি</span></div>
      <div class="stat s-warn"><b id="c3">০</b><span>নিশ্চিত হয়নি</span></div>
    </div>
    <p class="hint">সাম্প্রতিক রেকর্ড অনুযায়ী</p>
  </section>

  <section id="sch">
    <p class="hint">ঔষধের নাম এখানে লিখুন — যন্ত্রের বোতাম দিয়ে বাংলা লেখা যায় না।</p>
    <div id="slots"></div>
  </section>

  <section id="his">
    <div id="hist"></div>
  </section>

  <section id="dev">
    <div class="card">
      <h2>যন্ত্রের অবস্থা</h2>
      <ul class="health" id="health"></ul>
    </div>
    <div class="card">
      <h2>সময়</h2>
      <p class="hint" id="clk">—</p>
      <button class="go" data-do="time">এই ফোনের সময় বসান</button>
    </div>
    <div class="card">
      <h2>পিন বদলান</h2>
      <p class="hint">শুরুতে পিন ১২৩৪। নিজের পিন দিয়ে বদলে নিন।</p>
      <label>নতুন পিন (৪–৬ সংখ্যা)</label>
      <input id="np" inputmode="numeric" maxlength="6" autocomplete="off">
      <button class="go" data-do="pin">পিন বদলান</button>
    </div>

    <div class="card">
      <h2>যন্ত্র পুনরায় চালু করুন</h2>
      <p class="hint">কিছু আটকে গেলে এখান থেকেই চালু করা যায় — যন্ত্র খোলার দরকার নেই।</p>
      <button class="go" data-do="restart" style="background:#f87171;color:#fff">পুনরায় চালু</button>
    </div>

    <div class="card">
      <h2>হোম ওয়াই-ফাই</h2>
      <p class="hint">বাসার ওয়াই-ফাইয়ে যুক্ত করলে যন্ত্র টেলিগ্রামে খবর পাঠাতে পারবে। ছাড়াই যন্ত্র সম্পূর্ণ চলে।</p>
      <p class="hint" id="staTxt">—</p>
      <label>নেটওয়ার্কের নাম</label><input id="wS" autocomplete="off">
      <label style="margin-top:8px">পাসওয়ার্ড</label><input id="wP" type="password" autocomplete="off">
      <button class="go" data-do="wifi">যুক্ত করুন</button>
    </div>

    <div class="card">
      <h2>আঙুলের ছাপ</h2>
      <p class="hint">ছাপ বদলাতে হলে আগে যাঁর ছাপ, তাঁর পুরানো ছাপটি দিতে হবে। তারপর নতুন ছাপ তিন দিক থেকে নেওয়া হবে — মধ্য, বাম, ডান। পর্দায় যা লেখা ওঠে তাই করুন।</p>
      <div id="ppl"></div>
      <div class="enrbox" id="enrBox" hidden>
        <b id="enrMsg">—</b>
        <div class="enrbar"><i id="enrBar"></i></div>
        <button class="go sm" data-do="enrstop" style="background:#f87171;color:#fff;margin-top:10px">বাতিল</button>
      </div>
    </div>

    <div class="card">
      <h2>টেলিগ্রামে খবর</h2>
      <p class="hint">ঔষধ বাদ পড়লে সঙ্গে সঙ্গে, আর প্রতিদিন রাতে সারা দিনের হিসাব ফোনে চলে যাবে।</p>

      <div class="tghelp">
        <b>একবারই করতে হবে</b>
        <div>১। টেলিগ্রামে <b>@BotFather</b>-কে <b>/newbot</b> লিখুন। সে যে লম্বা সঙ্কেতটি দেবে, সেটি নিচে বসান।</div>
        <div>২। যাঁরা খবর পাবেন, তাঁরা নতুন বটটিকে <b>/start</b> লিখে পাঠান।</div>
        <div>৩। তারপর নিচের <b>যুক্ত করুন</b> বোতাম চাপুন — নম্বর নিজে থেকেই বসবে।</div>
        <div class="warn">ফোন নম্বর দিয়ে টেলিগ্রামে খবর পাঠানো যায় না — তাই এই নম্বর।</div>
      </div>

      <label class="chk"><input type="checkbox" id="tgEn"> চালু করুন</label>

      <label style="margin-top:8px">বটের সঙ্কেত (token)</label>
      <input id="tgTok" autocomplete="off" placeholder="1234567:AAE…">

      <label style="margin-top:12px">১ম জন — নাম</label>
      <input id="tgN1" autocomplete="off" placeholder="যেমন: বাবা">
      <div class="row2">
        <input id="tgC1" inputmode="numeric" autocomplete="off" placeholder="চ্যাট নম্বর">
        <button class="go sm" data-do="tglink" data-n="1">যুক্ত করুন</button>
      </div>

      <label style="margin-top:12px">২য় জন — নাম</label>
      <input id="tgN2" autocomplete="off" placeholder="যেমন: মা">
      <div class="row2">
        <input id="tgC2" inputmode="numeric" autocomplete="off" placeholder="চ্যাট নম্বর">
        <button class="go sm" data-do="tglink" data-n="2">যুক্ত করুন</button>
      </div>

      <label style="margin-top:12px">দিনের হিসাব কখন পাঠাবে</label>
      <input type="time" id="tgH">

      <button class="go" data-do="tgsave" style="margin-top:12px">সংরক্ষণ</button>
      <button class="go" data-do="tgtest" style="margin-top:8px;background:#0f3b3a;color:#7fe3d8">একটি পরীক্ষা মেসেজ পাঠান</button>
    </div>
  </section>
</main>

<div class="sheet" id="pinSheet" hidden>
  <div class="card">
    <h2>পিন দিন</h2>
    <p class="hint">সময়সূচি বদলাতে পিন লাগে। দেখতে লাগে না।</p>
    <input id="pinIn" type="password" inputmode="numeric" placeholder="••••">
    <div class="acts">
      <button class="ghost" id="pinNo">বাতিল</button>
      <button class="go" id="pinOk" style="margin:0">ঠিক আছে</button>
    </div>
  </div>
</div>

<div class="toast" id="toast" hidden></div>

<script>
const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
const BD=['০','১','২','৩','৪','৫','৬','৭','৮','৯'];
const bn=v=>String(v).replace(/\d/g,d=>BD[+d]);
const p2=n=>String(n).padStart(2,'0');
function bnTime(h,m){
  const P=h>=5&&h<12?'সকাল':h>=12&&h<16?'দুপুর':h>=16&&h<18?'বিকাল':'রাত';
  return P+' '+bn((h%12)||12)+':'+bn(p2(m));
}
const EV={
  TAKEN:'ঔষধ গ্রহণ সম্পন্ন', MISSED:'ঔষধ গ্রহণ করা হয়নি',
  UNCONFIRMED:'ঔষধ গ্রহণ নিশ্চিত হয়নি', CANCELLED:'বাতিল করা হয়েছে',
  WRONG_USER:'ভুল ব্যবহারকারী', UNKNOWN_FINGER:'অচেনা আঙুলের ছাপ',
  OVERRIDE:'বিশেষ অনুমতিতে প্রবেশ', AUTH_SUCCESS:'সনাক্তকরণ সফল',
  SCHEDULED:'সময় হয়েছে', REMINDER_STARTED:'স্মরণ শুরু',
  SYSTEM_BOOT:'যন্ত্র চালু হয়েছে', COURSE_COMPLETE:'ঔষধের কোর্স সম্পন্ন',
  MISSED_POWER_OUT:'বিদ্যুৎ ছিল না — ঔষধ গ্রহণ করা হয়নি'
};
let PIN=sessionStorage.getItem('pin')||'', slots=[];

function toast(m,bad){const t=$('#toast');t.textContent=m;t.className='toast'+(bad?' bad':'');
  t.hidden=false;clearTimeout(toast.t);toast.t=setTimeout(()=>t.hidden=true,2600);}

function askPin(){return new Promise(r=>{
  const s=$('#pinSheet');$('#pinIn').value='';s.hidden=false;setTimeout(()=>$('#pinIn').focus(),50);
  const fin=ok=>{s.hidden=true;$('#pinOk').onclick=null;$('#pinNo').onclick=null;r(ok);};
  $('#pinOk').onclick=()=>{PIN=$('#pinIn').value.trim();sessionStorage.setItem('pin',PIN);fin(true);};
  $('#pinNo').onclick=()=>fin(false);
  $('#pinIn').onkeydown=e=>{if(e.key==='Enter')$('#pinOk').click();};
});}
const needPin=async()=>PIN?true:askPin();

async function post(url,f){
  const b=new URLSearchParams({...f,pin:PIN});
  const r=await fetch(url,{method:'POST',body:b});
  const t=await r.text();
  if(r.status===401){PIN='';sessionStorage.removeItem('pin');throw new Error('ভুল পিন');}
  if(!r.ok) throw new Error(t||'ব্যর্থ');
  return t;
}
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

async function loadStatus(){
  try{
    const d=await (await fetch('/api/status',{cache:'no-store'})).json();
    $('#live').classList.add('on');
    $('#nTime').textContent=d.nextTime||'কিছু নির্ধারিত নেই';
    $('#nUser').textContent=d.nextUser||'—';
    $('#nMed').textContent=d.nextMed||'—';
    $('#c1').textContent=bn(d.taken||0);
    $('#c2').textContent=bn(d.missed||0);
    $('#c3').textContent=bn(d.unconf||0);
    $('#clk').textContent='যন্ত্রের সময়: '+d.clock;
    const rows=[['ঘড়ি',d.rtc],['আঙুলের ছাপ যন্ত্র',d.fp],['রেকর্ড',d.log]];
    $('#health').innerHTML=rows.map(([k,v])=>
      `<li><span>${k}</span><span class="pill ${v?'y':'n'}">${v?'ঠিক আছে':'সমস্যা'}</span></li>`).join('')
      // The sound module fails in two quite different ways and
      // they need different things done about them, so say which.
      +`<li><span>শব্দ যন্ত্র</span><span class="pill ${d.mp3?'y':'n'}">${d.mp3?'ঠিক আছে':(d.mp3mod===false?'যন্ত্র সাড়া দেয় না':'কার্ড পড়া যাচ্ছে না')}</span></li>`
      +`<li><span>সংরক্ষিত ছাপ</span><span>${bn(d.tpl||0)} টি</span></li>`
      +`<li><span>নিজস্ব ওয়াই-ফাই</span><span>${esc(d.ip||'—')}</span></li>`
      +`<li><span>ইন্টারনেট</span><span class="pill ${d.sta?'y':'n'}">${d.sta?'যুক্ত':'নেই'}</span></li>`
      +`<li><span>টেলিগ্রাম</span><span class="pill ${d.tg?'y':'n'}">${d.tg?'চালু':'বন্ধ'}</span></li>`;

    $('#staTxt').textContent = d.sta
      ? ('যুক্ত আছে: '+esc(d.staSsid||'')+'  ('+esc(d.staIp||'')+')')
      : 'এখনো কোনো হোম ওয়াই-ফাইয়ে যুক্ত নয়।';
  }catch(e){$('#live').classList.remove('on');}
}


async function loadTg(){
  try{
    const d=await (await fetch('/api/telegram',{cache:'no-store'})).json();
    $('#tgEn').checked=!!d.en;
    $('#tgC1').value=d.chat1||'';$('#tgC2').value=d.chat2||'';
    $('#tgN1').value=d.name1||'';$('#tgN2').value=d.name2||'';
    if(d.hasTok&&!$('#tgTok').value)$('#tgTok').placeholder=d.tok+' (সংরক্ষিত)';
    $('#tgH').value=p2(d.hour||0)+':'+p2(d.min||0);
  }catch(e){}
}

let enrTimer=null;

async function loadPeople(){
  try{
    const d=await (await fetch('/api/users',{cache:'no-store'})).json();
    $('#ppl').innerHTML=d.map(u=>`
      <div class="person">
        <div class="pn">${esc(u.name)}${u.boss?'<span class="boss">সব খুলতে পারে</span>':''}
          <small>${u.prints?bn(u.prints)+' টি ছাপ আছে':'কোনো ছাপ নেই'}</small>
        </div>
        <button class="go sm" data-do="enroll" data-u="${u.id}">${u.prints?'বদলান':'যুক্ত করুন'}</button>
      </div>`).join('');
  }catch(e){}
}

// 0 idle, 1 verify, 2 press, 3 lift, 4 done, 5 failed
function enrWatch(){
  if(enrTimer) clearInterval(enrTimer);
  $('#enrBox').hidden=false;
  enrTimer=setInterval(async()=>{
    let d;
    try{ d=await (await fetch('/api/enroll',{cache:'no-store'})).json(); }
    catch(e){ return; }
    $('#enrMsg').textContent=d.msg||'…';
    const steps=(d.angle*2)+(d.press?1:0);
    $('#enrBar').style.width=(d.state>=4?100:Math.round(steps/6*100))+'%';
    if(d.state===0||d.state===4||d.state===5){
      clearInterval(enrTimer);enrTimer=null;
      setTimeout(()=>{$('#enrBox').hidden=true;loadPeople();loadStatus();},2500);
      if(d.state===4)toast('ছাপ সংরক্ষিত হয়েছে');
      if(d.state===5)toast(d.msg||'ব্যর্থ',1);
    }
  },900);
}

async function loadSlots(){
  slots=await (await fetch('/api/slots',{cache:'no-store'})).json();
  $('#slots').innerHTML=slots.map(s=>`
  <div class="card">
    <h2>${bnTime(s.h,s.m)} · ${esc(s.user)}</h2>
    <label>ঔষধের নাম</label>
    <input data-med="${s.i}" value="${esc(s.med)}">
    <div class="medwarn" data-warn="${s.i}" hidden>যন্ত্রের পর্দায় শুধু বাংলা দেখা যায় — ইংরেজি লেখা ফাঁকা বাক্স হয়ে দেখাবে।</div>
    <div class="row" style="margin-top:10px">
      <div><label>সময়</label><input type="time" data-time="${s.i}" value="${p2(s.h)}:${p2(s.m)}"></div>
      <div><label>চালু</label>
        <select data-en="${s.i}">
          <option value="1"${s.on?' selected':''}>হ্যাঁ</option>
          <option value="0"${s.on?'':' selected'}>না</option>
        </select></div>
    </div>
    <button class="go" data-save="${s.i}">সংরক্ষণ</button>
  </div>`).join('');

  // Latin characters have no glyph in the device font.
  const checkMed=i=>{
    const v=$(`[data-med="${i}"]`).value;
    $(`[data-warn="${i}"]`).hidden=!/[A-Za-z]/.test(v);
  };
  slots.forEach(s=>{
    checkMed(s.i);
    $(`[data-med="${s.i}"]`).addEventListener('input',()=>checkMed(s.i));
  });
}

async function loadHist(){
  const t=await (await fetch('/api/history',{cache:'no-store'})).text();
  const rows=t.trim().split('\n').slice(1).filter(Boolean).reverse();
  if(!rows.length){$('#hist').innerHTML='<div class="empty">কোনো রেকর্ড নেই</div>';return;}
  $('#hist').innerHTML=rows.slice(0,120).map(r=>{
    const c=r.split(',');
    const ev=c[1]||'';
    return `<div class="ev ${esc(ev)}"><div>
      <b>${EV[ev]||esc(ev)}</b>
      <small>${esc(c[0]||'')} · ${esc(c[3]||'')} ${c[6]?'· '+esc(c[6]):''}</small>
    </div></div>`;}).join('');
}

document.addEventListener('DOMContentLoaded',()=>{
  $$('nav button').forEach(b=>b.onclick=()=>{
    $$('nav button').forEach(x=>x.classList.toggle('on',x===b));
    $$('section').forEach(s=>s.classList.toggle('on',s.id===b.dataset.p));
    if(b.dataset.p==='sch')loadSlots();
    if(b.dataset.p==='his')loadHist();
    if(b.dataset.p==='dev'){loadTg();loadPeople();}
  });
  $('#pinBtn').onclick=askPin;

  document.addEventListener('click',async e=>{
    const b=e.target.closest('button'); if(!b) return;
    if(b.dataset.save!==undefined){
      if(!await needPin())return;
      const i=b.dataset.save, [h,m]=($(`[data-time="${i}"]`).value||'09:00').split(':');
      try{
        await post('/api/slot',{i,h:+h,m:+m,
          med:$(`[data-med="${i}"]`).value, en:$(`[data-en="${i}"]`).value});
        toast('সংরক্ষিত হয়েছে');loadSlots();loadStatus();
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='time'){
      if(!await needPin())return;
      const d=new Date();
      try{await post('/api/time',{y:d.getFullYear(),mo:d.getMonth()+1,d:d.getDate(),
        h:d.getHours(),mi:d.getMinutes(),s:d.getSeconds()});
        toast('সময় বসানো হয়েছে');loadStatus();}catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='pin'){
      if(!await needPin())return;
      const np=$('#np').value.trim();
      try{
        await post('/api/pin',{newpin:np});
        PIN=np;sessionStorage.setItem('pin',np);$('#np').value='';
        toast('পিন বদলানো হয়েছে');
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='restart'){
      if(!await needPin())return;
      if(!confirm('যন্ত্র পুনরায় চালু করবেন?'))return;
      try{
        await post('/api/restart',{});
        toast('চালু হচ্ছে... ২০ সেকেন্ড পরে আবার সংযোগ করুন');
        $('#live').classList.remove('on');
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='wifi'){
      if(!await needPin())return;
      try{await post('/api/wifi',{ssid:$('#wS').value,pass:$('#wP').value});
        toast('যুক্ত হচ্ছে... কয়েক সেকেন্ড অপেক্ষা করুন');
        setTimeout(loadStatus,4000);setTimeout(loadStatus,9000);
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='enroll'){
      if(!await needPin())return;
      const u=b.dataset.u;
      try{
        await post('/api/enroll',{user:u});
        toast('পর্দায় দেখুন — যন্ত্রে ছাপ দিন');
        enrWatch();
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='enrstop'){
      if(!await needPin())return;
      try{await post('/api/enrollstop',{});
        if(enrTimer){clearInterval(enrTimer);enrTimer=null;}
        $('#enrBox').hidden=true;loadPeople();
        toast('বাতিল করা হয়েছে');
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='tgsave'){
      if(!await needPin())return;
      try{
        const [sh,sm]=($('#tgH').value||'21:00').split(':');
        await post('/api/telegram',{
          en:$('#tgEn').checked?'1':'0',
          token:$('#tgTok').value.trim(),
          chat1:$('#tgC1').value.trim(),chat2:$('#tgC2').value.trim(),
          name1:$('#tgN1').value,name2:$('#tgN2').value,
          hour:+sh,min:+sm});
        $('#tgTok').value='';
        toast('সংরক্ষিত হয়েছে');loadTg();loadStatus();
      }catch(err){toast(err.message,1);}
    }
    if(b.dataset.do==='tglink'){
      if(!await needPin())return;
      // The token has to be on the device before it can ask Telegram
      // anything, so save whatever is typed first.
      try{
        if($('#tgTok').value.trim()){
          const [sh,sm]=($('#tgH').value||'21:00').split(':');
          await post('/api/telegram',{en:$('#tgEn').checked?'1':'0',
            token:$('#tgTok').value.trim(),hour:+sh,min:+sm});
        }
        await post('/api/tglink',{});
        toast('খুঁজছি...');
        // The device does the lookup in its main loop, not in the web
        // handler, so the answer arrives a moment later.
        b.disabled=true;
        let r=null;
        for(let i=0;i<20;i++){
          await new Promise(x=>setTimeout(x,700));
          r=await (await fetch('/api/tglink',{cache:'no-store'})).json();
          if(r&&r.state!==1)break;
        }
        b.disabled=false;
        if(!r||r.state===undefined||r.state===1){
          toast('সাড়া পাওয়া যায়নি',1);}
        else if(r.state===3){toast(r.value||'পাওয়া যায়নি',1);}
        else if(!r.value){
          toast('নম্বর পাওয়া যায়নি',1);}
        else{
          $(b.dataset.n==='1'?'#tgC1':'#tgC2').value=r.value;
          toast('পাওয়া গেছে — এখন সংরক্ষণ চাপুন');
        }
      }catch(err){b.disabled=false;toast(err.message,1);}
    }
    if(b.dataset.do==='tgtest'){
      if(!await needPin())return;
      // Sending takes a few seconds, and an impatient second tap would
      // otherwise queue a second identical message.
      b.disabled=true;
      try{await post('/api/tgtest',{});
        toast('পাঠানো হচ্ছে... ফোন দেখুন');
      }catch(err){toast(err.message,1);}
      setTimeout(()=>{b.disabled=false;},8000);
    }
  });

  loadStatus();
  setInterval(loadStatus,6000);
});
</script>
</body>
</html>
)HTMLPAGE";

#endif
