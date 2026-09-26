#pragma once
#include <pgmspace.h>

// VietHUD web portal — single mobile-first page served at "/" (net/WebPortal.cpp).
// Fully self-contained (no external CSS/JS/fonts) so it works with NO internet.
// The "Dữ liệu" tab implements the Phone Update Bridge client side: it fetches
// the signed manifest + data from GitHub over the PHONE's own internet (4G/5G),
// verifies SHA-256 in JS (crypto.subtle is unavailable on an http:// origin), and
// streams the files to the device in resumable chunks (/api/v1/update/*).
// Protocol: docs/WIFI_PORTAL_UPDATE_BRIDGE_PLAN.md §8-§10.
static const char kPortalHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0B0F14"><title>VietHUD</title>
<style>
:root{--bg:#0B0F14;--card:#151C24;--card2:#0F1620;--line:#24303E;--tx:#E6EDF3;--mut:#8A98A8;--acc:#3DA5FF;--ok:#34C46A;--warn:#E8B931;--bad:#FF5A4F;--r:14px}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
header{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:10px;padding:12px 16px calc(10px);background:rgba(11,15,20,.92);backdrop-filter:blur(8px);border-bottom:1px solid var(--line)}
header h1{font-size:18px;margin:0;font-weight:700;letter-spacing:.2px}header .id{font-size:12px;color:var(--mut);border:1px solid var(--line);border-radius:20px;padding:2px 8px}
header .dot{margin-left:auto;display:flex;align-items:center;gap:6px;font-size:12px;color:var(--mut)}
.dot i{width:9px;height:9px;border-radius:50%;background:var(--bad);display:inline-block}.dot.on i{background:var(--ok)}
main{padding:14px 14px 96px;max-width:640px;margin:0 auto}
.tab{display:none}.tab.show{display:block}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:14px;margin-bottom:12px}
.card h2{font-size:13px;font-weight:600;color:var(--mut);text-transform:uppercase;letter-spacing:.6px;margin:0 0 10px}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:8px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}.row .k{color:var(--mut)}.row .v{text-align:right;font-weight:600}
.ds{display:flex;align-items:center;gap:12px;padding:10px 0;border-top:1px solid var(--line)}.ds:first-of-type{border-top:0}
.ds .ic{width:38px;height:38px;border-radius:10px;background:var(--card2);display:grid;place-items:center;font-size:20px;flex:none}
.ds .t{flex:1;min-width:0}.ds .t b{display:block}.ds .t small{color:var(--mut)}
.badge{font-size:12px;font-weight:600;border-radius:20px;padding:3px 9px;white-space:nowrap}
.b-ok{background:rgba(52,196,106,.15);color:var(--ok)}.b-new{background:rgba(61,165,255,.15);color:var(--acc)}
.b-mut{background:#1B2430;color:var(--mut)}.b-warn{background:rgba(232,185,49,.15);color:var(--warn)}.b-bad{background:rgba(255,90,79,.15);color:var(--bad)}
.net{display:flex;align-items:center;gap:10px;padding:8px 0}.net+.net{border-top:1px solid var(--line)}
.net .ic{font-size:20px;width:28px;text-align:center}.net .t{flex:1}.net small{display:block;color:var(--mut);font-size:12px}
.led{width:10px;height:10px;border-radius:50%;background:#3A4656;flex:none}.led.ok{background:var(--ok);box-shadow:0 0 8px var(--ok)}.led.warn{background:var(--warn)}.led.bad{background:var(--bad)}
.btn{display:block;width:100%;border:0;border-radius:12px;padding:14px;font-size:16px;font-weight:700;color:#fff;background:var(--acc);cursor:pointer}
.btn:disabled{opacity:.45}.btn.sec{background:#1E2A38;color:#CFE0F0;border:1px solid #2E3F52;font-weight:600}
.btn.danger{background:#2A1718;color:#FF9A9A;border:1px solid #6A2E2E}.btn.go{background:var(--ok)}
.btns{display:grid;gap:10px}.btns.two{grid-template-columns:1fr 1fr}
.steps{margin:4px 0 0}.step{padding:9px 0;border-top:1px solid var(--line)}.step:first-child{border-top:0}
.step .h{display:flex;align-items:center;gap:10px}.step .n{width:24px;height:24px;border-radius:50%;background:#1E2A38;color:var(--mut);display:grid;place-items:center;font-size:12px;font-weight:700;flex:none}
.step.act .n{background:var(--acc);color:#fff}.step.done .n{background:var(--ok);color:#fff}.step.err .n{background:var(--bad);color:#fff}
.step .l{flex:1}.step .p{color:var(--mut);font-size:13px;font-variant-numeric:tabular-nums}
.bar{height:6px;background:#1E2A38;border-radius:4px;overflow:hidden;margin:8px 0 0 34px}.bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .25s}
.step.done .bar i{background:var(--ok)}
.msg{border-radius:12px;padding:12px 14px;margin-bottom:12px;font-size:14px}.msg.ok{background:rgba(52,196,106,.12);border:1px solid rgba(52,196,106,.4)}
.msg.warn{background:rgba(232,185,49,.1);border:1px solid rgba(232,185,49,.4)}.msg.bad{background:rgba(255,90,79,.1);border:1px solid rgba(255,90,79,.4)}.msg.info{background:rgba(61,165,255,.1);border:1px solid rgba(61,165,255,.35)}
details{background:var(--card);border:1px solid var(--line);border-radius:var(--r);margin-bottom:12px}
summary{padding:14px;cursor:pointer;font-weight:600;list-style:none}summary::-webkit-details-marker{display:none}summary:after{content:"›";float:right;color:var(--mut);transition:.2s}details[open] summary:after{transform:rotate(90deg)}
details .in{padding:0 14px 14px;color:#C8D3DE;font-size:14px}details ol,details ul{padding-left:20px;margin:6px 0}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.tile{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px}
.tile .k{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.5px}.tile .v{font-size:20px;font-weight:700;margin-top:3px}
.tile.wide{grid-column:1/-1}.tile.cam{background:#3A2A08;border-color:#7A5A10}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.mut{color:var(--mut)}
input[type=text],input[type=password]{width:100%;background:var(--card2);color:var(--tx);border:1px solid var(--line);border-radius:10px;padding:12px;font-size:16px;margin:6px 0}
.list .it{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:11px 0;border-top:1px solid var(--line);cursor:pointer}.list .it:first-child{border-top:0}
.x{color:#FF9A9A;border:1px solid #6A2E2E;border-radius:8px;padding:4px 10px;font-size:13px}
nav{position:fixed;left:0;right:0;bottom:0;z-index:6;display:flex;background:rgba(15,22,32,.97);border-top:1px solid var(--line);padding:6px 4px calc(6px + env(safe-area-inset-bottom))}
nav a{flex:1;text-align:center;color:var(--mut);text-decoration:none;font-size:11px;padding:4px 0}nav a span{display:block;font-size:20px;line-height:1.3}nav a.on{color:var(--acc)}
.small{font-size:13px;color:var(--mut)}.mt{margin-top:10px}.hide{display:none!important}
</style></head><body>
<header><h1>VietHUD</h1><span class="id" id="hid">…</span><span class="dot" id="hdot"><i></i><span id="hdt">Đang kết nối</span></span></header>
<main>
<!-- ================= DỮ LIỆU ================= -->
<section class="tab show" id="t-data">
  <div id="lastMsg"></div>
  <div class="card"><h2>Kết nối Internet</h2>
    <div class="net"><div class="ic">📱</div><div class="t">Điện thoại<small id="phNetD">Đang kiểm tra…</small></div><div class="led" id="phNet"></div></div>
    <div class="net"><div class="ic">📟</div><div class="t">VietHUD<small id="hudNetD">…</small></div><div class="led" id="hudNet"></div></div>
  </div>
  <div class="card"><h2>Dữ liệu trên VietHUD</h2><div id="dsList"><div class="small">Đang đọc…</div></div></div>
  <div id="upMsg"></div>
  <div class="btns" id="mainBtns">
    <button class="btn" id="bCheck" onclick="checkUpd()">Kiểm tra cập nhật</button>
    <button class="btn go hide" id="bUpd" onclick="runUpd()">Cập nhật ngay</button>
  </div>
  <div class="card mt hide" id="prog"><h2>Tiến trình</h2><div class="steps">
    <div class="step" id="s1"><div class="h"><div class="n">1</div><div class="l">Tải về điện thoại</div><div class="p" id="s1p"></div></div><div class="bar"><i id="s1b"></i></div></div>
    <div class="step" id="s2"><div class="h"><div class="n">2</div><div class="l">Truyền sang VietHUD</div><div class="p" id="s2p"></div></div><div class="bar"><i id="s2b"></i></div></div>
    <div class="step" id="s3"><div class="h"><div class="n">3</div><div class="l">Kiểm tra dữ liệu</div><div class="p" id="s3p"></div></div></div>
    <div class="step" id="s4"><div class="h"><div class="n">4</div><div class="l">Cài đặt &amp; khởi động lại</div><div class="p" id="s4p"></div></div></div>
  </div><button class="btn sec mt hide" id="bCancel" onclick="cancelUpd()">Hủy</button></div>
  <div class="btns mt hide" id="directBox"><button class="btn sec" onclick="direct()">📶 Để VietHUD tự tải qua Wi-Fi</button></div>
  <details class="mt"><summary>Không kiểm tra được cập nhật?</summary><div class="in">
    <b>iPhone</b><ol><li>Nếu đang ở cửa sổ <i>“Captive Wi-Fi”</i> tự bật lên: bấm <b>Xong/Hủy</b> → chọn <b>Dùng không có Internet</b>.</li>
    <li>Mở <b>Safari</b> và vào <b>192.168.4.1</b> (hoặc quét mã QR thứ 2 trên màn hình VietHUD).</li>
    <li>Cài đặt → Di động → bật dữ liệu di động cho <b>Safari</b>.</li></ol>
    <b>Android</b><ol><li>Khi máy hỏi “Wi-Fi không có Internet”, chọn <b>Giữ kết nối</b> nhưng vẫn bật dữ liệu di động.</li><li>Nếu vẫn không được: dùng mục “Dùng tệp đã tải sẵn” bên dưới.</li></ol>
    Cấu hình, trạng thái và các chức năng khác vẫn dùng bình thường khi không có Internet.</div></details>
  <details><summary>Dùng tệp đã tải sẵn</summary><div class="in">
    Chọn cùng lúc <b>manifest.txt</b>, <b>manifest.txt.sig</b> và các tệp <b>.bin</b> (tải từ trang phát hành dữ liệu VietHUD). VietHUD kiểm tra chữ ký trước khi cài.
    <input type="file" id="fpick" multiple class="mt" style="width:100%">
    <button class="btn sec mt" onclick="useFiles()">Cài từ tệp</button></div></details>
</section>
<!-- ================= TRẠNG THÁI ================= -->
<section class="tab" id="t-live"><div class="grid" id="liveGrid"><div class="small">Đang tải…</div></div></section>
<!-- ================= WI-FI ================= -->
<section class="tab" id="t-wifi">
  <div class="msg info">Không bắt buộc. Chỉ cần nếu muốn VietHUD <b>tự</b> tải cập nhật hoặc đồng bộ giờ qua Internet. Cập nhật qua điện thoại không cần mục này.</div>
  <div class="card"><h2>Mạng đã lưu</h2><div class="list" id="wSaved"><div class="small">…</div></div></div>
  <div class="card"><h2>Thêm mạng</h2>
    <button class="btn sec" onclick="wscan(1)">🔄 Quét Wi-Fi xung quanh</button><div class="small mt" id="wMsg"></div>
    <div class="list mt" id="wScan"></div>
    <input type="text" id="wSsid" placeholder="Tên Wi-Fi (SSID)" autocapitalize="off" autocorrect="off">
    <input type="password" id="wPass" placeholder="Mật khẩu">
    <button class="btn mt" onclick="wadd()">Lưu &amp; kết nối</button>
    <div class="small mt">VietHUD chỉ dùng Wi-Fi 2.4 GHz. Với hotspot iPhone: bật “Tối đa hoá tương thích”.</div></div>
</section>
<!-- ================= HỆ THỐNG ================= -->
<section class="tab" id="t-sys">
  <div class="card"><h2>Thiết bị</h2><div id="sysInfo"></div></div>
  <div class="card"><h2>Công cụ</h2><div class="btns two">
    <button class="btn sec" onclick="act('audiotest')">🔊 Thử loa</button><button class="btn sec" onclick="act('demo')">🎬 Bật/tắt demo</button>
    <button class="btn sec" onclick="location='/triplog'">📄 Nhật ký chuyến</button><button class="btn sec" onclick="location='/update'">⬆ Firmware</button>
    <button class="btn danger" onclick="if(confirm('Xoá toàn bộ nhật ký chuyến đi?'))act('clearlogs')">🗑 Xoá nhật ký</button>
    <button class="btn danger" onclick="if(confirm('Khởi động lại VietHUD?'))act('reboot')">🔁 Khởi động lại</button></div>
    <div class="small mt" id="actMsg"></div></div>
</section>
</main>
<nav id="nav">
  <a href="#data" data-t="data" class="on"><span>⬇</span>Dữ liệu</a>
  <a href="#live" data-t="live"><span>📍</span>Trạng thái</a>
  <a href="/config"><span>⚙</span>Cài đặt</a>
  <a href="#wifi" data-t="wifi"><span>📶</span>Wi-Fi</a>
  <a href="#sys" data-t="sys"><span>ℹ</span>Hệ thống</a>
</nav>
<script>
"use strict";
const $=id=>document.getElementById(id);
const H={'X-VietHUD':'1'};
let ST=null,REM=null,NEED=[],BUSY=false,liveT=null,curTab='data';
const MB=b=>(b/1048576).toFixed(b<10485760?1:0).replace('.',',')+' MB';
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
// ---------- SHA-256 (crypto.subtle is unavailable on http:// pages) ----------
const K=new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
function sha256(d){const h=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]),w=new Uint32Array(64);
 const n=d.length,tot=((n+9+63)>>6)<<6,tail=new Uint8Array(tot-(n&~63));tail.set(d.subarray(n&~63));tail[n&63]=0x80;
 const dv=new DataView(tail.buffer);dv.setUint32(tail.length-4,(n*8)>>>0);dv.setUint32(tail.length-8,Math.floor(n/536870912));
 const blk=(b,o)=>{for(let i=0;i<16;i++)w[i]=(b[o+i*4]<<24)|(b[o+i*4+1]<<16)|(b[o+i*4+2]<<8)|b[o+i*4+3];
  for(let i=16;i<64;i++){const a=w[i-15],c=w[i-2];w[i]=(((a>>>7|a<<25)^(a>>>18|a<<14)^(a>>>3))+w[i-7]+((c>>>17|c<<15)^(c>>>19|c<<13)^(c>>>10))+w[i-16])|0;}
  let A=h[0],B=h[1],C=h[2],D=h[3],E=h[4],F=h[5],G=h[6],Hh=h[7];
  for(let i=0;i<64;i++){const t1=(Hh+((E>>>6|E<<26)^(E>>>11|E<<21)^(E>>>25|E<<7))+((E&F)^(~E&G))+K[i]+w[i])|0,t2=(((A>>>2|A<<30)^(A>>>13|A<<19)^(A>>>22|A<<10))+((A&B)^(A&C)^(B&C)))|0;Hh=G;G=F;F=E;E=(D+t1)|0;D=C;C=B;B=A;A=(t1+t2)|0;}
  h[0]+=A;h[1]+=B;h[2]+=C;h[3]+=D;h[4]+=E;h[5]+=F;h[6]+=G;h[7]+=Hh;};
 for(let o=0;o+64<=n;o+=64)blk(d,o);for(let o=0;o<tail.length;o+=64)blk(tail,o);
 return Array.from(h,x=>(x>>>0).toString(16).padStart(8,'0')).join('');}
// ---------- tabs ----------
function show(t){curTab=t;document.querySelectorAll('.tab').forEach(s=>s.classList.toggle('show',s.id==='t-'+t));
 document.querySelectorAll('nav a[data-t]').forEach(a=>a.classList.toggle('on',a.dataset.t===t));
 clearInterval(liveT);if(t==='live'){live();liveT=setInterval(live,1000);}if(t==='wifi')wsaved();if(t==='sys')sysInfo();}
window.addEventListener('hashchange',()=>show((location.hash||'#data').slice(1)));
// ---------- device state ----------
async function loadState(){const r=await fetch('/api/v1/update/state',{cache:'no-store'});ST=await r.json();
 $('hid').textContent=ST.device.id;$('hdot').classList.add('on');$('hdt').textContent='Đã kết nối';renderState();return ST;}
function dsRemoteVer(){return REM?REM.version:null}
function renderState(){if(!ST)return;const d=ST.data;
 $('hudNet').className='led '+(ST.sta.connected?'ok':'');
 $('hudNetD').textContent=ST.sta.connected?('Wi-Fi “'+ST.sta.ssid+'” · có thể tự tải'):'Chỉ phát Wi-Fi — không cần Internet';
 $('directBox').classList.toggle('hide',!ST.sta.connected||BUSY);
 let h='';d.datasets.forEach(ds=>{const need=NEED.filter(f=>ds.files.includes(f.n));let b;
  if(!REM)b='<span class="badge b-mut">Chưa kiểm tra</span>';else if(need.length)b='<span class="badge b-new">Có bản mới</span>';else b='<span class="badge b-ok">Mới nhất ✓</span>';
  const sz=need.reduce((a,f)=>a+f.s,0);
  h+='<div class="ds"><div class="ic">'+(ds.id==='map'?'🗺':'⚠️')+'</div><div class="t"><b>'+esc(ds.label)+'</b><small>'+esc(ds.version||'chưa có')+
   (need.length?' → '+esc(REM.version)+' · '+MB(sz):'')+'</small></div>'+b+'</div>';});
 $('dsList').innerHTML=h||'<div class="small">Chưa có dữ liệu</div>';
 const L=d.last;$('lastMsg').innerHTML=L&&L.result==='rollback'?'<div class="msg bad">⚠ Lần cập nhật trước bị lỗi ('+esc(L.detail)+'). VietHUD đã tự khôi phục dữ liệu cũ.</div>':'';
 if(!BUSY&&d.session&&d.session.bytesTotal){const pc=Math.floor(100*d.session.bytesDone/d.session.bytesTotal);
  $('upMsg').innerHTML='<div class="msg info">Có một lần cập nhật đang dở ('+pc+'%). Bấm <b>Kiểm tra cập nhật</b> rồi <b>Cập nhật ngay</b> để tiếp tục — phần đã truyền sẽ không phải gửi lại.</div>';}}
// ---------- phone internet probe ----------
function base(){let u=(ST&&ST.url)||'';if(u&&!u.endsWith('/'))u+='/';return u}
async function fetchT(url,ms,opt){const c=new AbortController();const t=setTimeout(()=>c.abort(),ms);try{return await fetch(url,Object.assign({cache:'no-store',signal:c.signal},opt||{}))}finally{clearTimeout(t)}}
function setPh(state,txt){$('phNet').className='led '+state;$('phNetD').textContent=txt}
async function getRemote(){const t0=Date.now();
 try{const r=await fetchT(base()+'manifest.txt?t='+t0,9000);if(!r.ok)throw new Error('HTTP '+r.status);const text=await r.text();
  if(!/^version /.test(text))throw new Error('bad');
  const s=await fetchT(base()+'manifest.txt.sig?t='+t0,9000);if(!s.ok)throw new Error('nosig');const sig=(await s.text()).trim();
  const files=[];let version='';text.split('\n').forEach(l=>{const p=l.trim().split(/\s+/);if(p[0]==='version')version=p[1];else if(p.length===3)files.push({n:p[0],s:+p[1],h:p[2].toLowerCase()})});
  setPh('ok','Có Internet (4G/5G) · cầu nối cập nhật sẵn sàng');return {text,sig,version,files};
 }catch(e){const quick=Date.now()-t0<2500;
  if(e.message==='nosig')setPh('warn','Có Internet — nhưng bản dữ liệu mới chưa được ký, chưa cài được');
  else if(quick)setPh('bad','Không ra được Internet từ cửa sổ này — xem hướng dẫn bên dưới');
  else setPh('bad','Không có Internet — cấu hình vẫn dùng bình thường');throw e;}}
function diff(){const loc={};(ST.data.local.files||[]).forEach(f=>loc[f.n]=f.h.toLowerCase());NEED=REM.files.filter(f=>loc[f.n]!==f.h);}
async function checkUpd(){if(BUSY)return;$('bCheck').disabled=true;$('bCheck').textContent='Đang kiểm tra…';$('upMsg').innerHTML='';
 try{await loadState();REM=await getRemote();diff();renderState();
  if(!NEED.length){$('upMsg').innerHTML='<div class="msg ok">✓ Dữ liệu trên VietHUD đã là bản mới nhất ('+esc(REM.version)+').</div>';$('bUpd').classList.add('hide');}
  else{const sz=NEED.reduce((a,f)=>a+f.s,0);$('bUpd').textContent='Cập nhật ngay · '+MB(sz);$('bUpd').classList.remove('hide');}
 }catch(e){REM=null;NEED=[];renderState();$('bUpd').classList.add('hide');}
 $('bCheck').disabled=false;$('bCheck').textContent='Kiểm tra lại';}
// ---------- progress UI ----------
function step(i,cls,p,pct){const s=$('s'+i);s.className='step '+(cls||'');if(p!==undefined)$('s'+i+'p').textContent=p;const b=$('s'+i+'b');if(b&&pct!==undefined)b.style.width=pct+'%';}
function progReset(){$('prog').classList.remove('hide');for(let i=1;i<=4;i++)step(i,'','',0);}
function fail(msg,kind){$('upMsg').innerHTML='<div class="msg '+(kind||'bad')+'">'+msg+'</div>';}
let CANCEL=false;
async function cancelUpd(){CANCEL=true;try{await fetch('/api/v1/update/session',{method:'DELETE',headers:H})}catch(e){}}
// ---------- the bridge ----------
async function download(f,onp){const r=await fetch(base()+f.n+'?v='+encodeURIComponent(REM.version),{cache:'no-store'});
 if(!r.ok)throw new Error('Tải '+f.n+' lỗi (HTTP '+r.status+')');const out=new Uint8Array(f.s);let o=0;
 if(r.body&&r.body.getReader){const rd=r.body.getReader();for(;;){const {done,value}=await rd.read();if(done)break;
  if(o+value.length>f.s)throw new Error('Tệp '+f.n+' lớn hơn dự kiến');out.set(value,o);o+=value.length;onp(o);if(CANCEL)throw new Error('cancel')}}
 else{const b=new Uint8Array(await r.arrayBuffer());if(b.length>f.s)throw new Error('size');out.set(b);o=b.length;onp(o)}
 if(o!==f.s)throw new Error('Tệp '+f.n+' tải thiếu');return out;}
async function openSession(){let r;for(let i=0;;i++){try{r=await fetchT('/api/v1/update/session',20000,{method:'POST',headers:Object.assign({'Content-Type':'text/plain'},H),body:'sig '+REM.sig+'\n'+REM.text});break}
  catch(e){if(i>=3)throw new Error('Không kết nối được với VietHUD');await sleep(1500)}}
 const j=await r.json();if(!r.ok)throw new Error(j.detail||('Lỗi '+r.status));return j;}
async function sessionNow(){const r=await fetch('/api/v1/update/session',{cache:'no-store'});return (await r.json()).session;}
async function upload(sess,data,onp){const CH=1436*120;// multiple of the device's 1436 B read block (see UpdateApi.cpp)
 for(const f of sess.files){const buf=data[f.n];if(!buf)throw new Error('Thiếu dữ liệu '+f.n);let off=f.r,tries=0;
  while(off<f.s){if(CANCEL)throw new Error('cancel');const chunk=buf.subarray(off,Math.min(off+CH,f.s));
   try{const r=await fetchT('/api/v1/update/file',30000,{method:'PUT',headers:Object.assign({'Content-Type':'application/octet-stream','X-Sid':sess.sid,'X-Name':f.n,'X-Offset':String(off)},H),body:chunk});
    const j=await r.json();if(r.status===409){off=j.expected;continue}if(!r.ok)throw new Error(j.detail||('HTTP '+r.status));off=j.received;tries=0;onp(f.n,off);
   }catch(e){if(e.message==='cancel')throw e;if(++tries>8)throw new Error('Mất kết nối với VietHUD khi đang truyền');
    step(2,'act','Kết nối lại… ('+tries+')');await sleep(1500);const s=await sessionNow().catch(()=>null);
    if(s&&s.sid===sess.sid){const x=s.files.find(q=>q.n===f.n);if(x)off=x.r}}}}}
async function waitReboot(){step(4,'act','Đang khởi động lại…');const t0=Date.now();await sleep(6000);
 while(Date.now()-t0<150000){try{const r=await fetchT('/api/v1/ping',2500);const j=await r.json();if(j.up<120000)return true}catch(e){}
  step(4,'act','Chờ VietHUD bật lại Wi-Fi… '+Math.round((Date.now()-t0)/1000)+' s');await sleep(2000);}return false;}
async function runUpd(){if(BUSY||!REM)return;BUSY=true;CANCEL=false;$('upMsg').innerHTML='';$('bUpd').classList.add('hide');$('bCheck').classList.add('hide');
 $('bCancel').classList.remove('hide');$('directBox').classList.add('hide');progReset();
 try{// 1. download on the phone (only files the device doesn't already have)
  const data={};const total=NEED.reduce((a,f)=>a+f.s,0);let doneB=0;step(1,'act','0%',0);
  for(const f of NEED){let last=0;const buf=await download(f,o=>{doneB+=o-last;last=o;const p=Math.floor(100*doneB/total);step(1,'act',p+'% · '+MB(doneB)+'/'+MB(total),p)});
   if(sha256(buf)!==f.h)throw new Error('Tệp '+f.n+' tải về bị lỗi (SHA-256 không khớp). Máy chủ có thể đang cập nhật — thử lại sau vài phút.');data[f.n]=buf;}
  step(1,'done','✓ '+MB(total),100);
  // 2. transfer over the local WiFi (resumes from whatever the device already has)
  const j=await openSession();if(j.upToDate){step(2,'done','Không cần',100);step(3,'done','✓');step(4,'done','✓');fail('✓ VietHUD đã có dữ liệu mới nhất.','ok');return;}
  const sess=j.session;const tot=sess.files.reduce((a,f)=>a+f.s,0);const got={};sess.files.forEach(f=>got[f.n]=f.r);
  const upd=()=>{const d=Object.values(got).reduce((a,b)=>a+b,0);const p=Math.floor(100*d/tot);step(2,'act',p+'% · '+MB(d)+'/'+MB(tot),p)};upd();
  await upload(sess,data,(n,o)=>{got[n]=o;upd()});step(2,'done','✓ '+MB(tot),100);
  // 3+4. device verifies (readback SHA-256 + signature), stages, reboots to install
  step(3,'act','VietHUD đang kiểm tra…');$('bCancel').classList.add('hide');
  let c,cj,lost=false;for(;;){try{c=await fetchT('/api/v1/update/commit',60000,{method:'POST',headers:Object.assign({'X-Sid':sess.sid},H)});cj=await c.json();}
   catch(e){lost=true;break}// reply lost: the device may already be rebooting to install; the result check below decides
   if(c.status===423){step(3,'act','Dừng xe để cài đặt…');await sleep(3000);continue}break;}
  if(!lost&&!c.ok){if(c.status===422){step(3,'err','Có tệp lỗi');throw new Error((cj.detail||'Dữ liệu lỗi')+'. Bấm Cập nhật ngay để gửi lại tệp đó.')}throw new Error(cj.detail||('Lỗi '+c.status))}
  step(3,'done',lost?'':'✓ Hợp lệ');
  const back=await waitReboot();if(!back){step(4,'err','');fail('VietHUD đã khởi động lại để cài đặt. Hãy nối lại Wi-Fi <b>'+esc(ST.device.ap)+'</b> và mở lại trang này để xem kết quả.','warn');return;}
  await loadState();const L=ST.data.last;
  if(lost&&ST.data.local.version!==REM.version&&L.result!=='rollback'){step(4,'err','');throw new Error('Mất kết nối khi VietHUD đang kiểm tra dữ liệu. Bấm Kiểm tra cập nhật để thử lại — phần đã truyền được giữ lại.')}
  if(L.result==='rollback'){step(4,'err','Đã khôi phục bản cũ');fail('⚠ Cài đặt không thành công ('+esc(L.detail)+'). VietHUD đã tự khôi phục dữ liệu cũ và vẫn hoạt động bình thường.');}
  else{step(4,'done','✓');NEED=[];renderState();fail('✓ Cập nhật hoàn tất — VietHUD đang dùng dữ liệu <b>'+esc(ST.data.local.version)+'</b>.','ok');}
 }catch(e){if(e.message==='cancel')fail('Đã hủy cập nhật. Dữ liệu trên VietHUD không thay đổi.','warn');
  else fail('⚠ '+esc(e.message)+'<br><span class="small">Dữ liệu đang dùng trên VietHUD không bị ảnh hưởng.</span>');
  for(let i=1;i<=4;i++){const s=$('s'+i);if(s.classList.contains('act'))s.className='step err'}}
 finally{BUSY=false;$('bCancel').classList.add('hide');$('bCheck').classList.remove('hide');$('bCheck').textContent='Kiểm tra lại';loadState().catch(()=>{});}}
// ---------- offline files (no internet on the phone at all) ----------
async function useFiles(){const fl=[...$('fpick').files];if(!fl.length)return;const by={};fl.forEach(f=>by[f.name]=f);
 if(!by['manifest.txt']||!by['manifest.txt.sig']){alert('Cần chọn cả manifest.txt và manifest.txt.sig');return}
 await loadState();const text=await by['manifest.txt'].text(),sig=(await by['manifest.txt.sig'].text()).trim();
 const files=[];let version='';text.split('\n').forEach(l=>{const p=l.trim().split(/\s+/);if(p[0]==='version')version=p[1];else if(p.length===3)files.push({n:p[0],s:+p[1],h:p[2].toLowerCase()})});
 REM={text,sig,version,files};diff();const miss=NEED.filter(f=>!by[f.n]);if(miss.length){alert('Còn thiếu tệp: '+miss.map(f=>f.n).join(', '));return}
 const realBase=base;window.base=()=>'';const origDl=download;
 window.download=async(f,onp)=>{const b=new Uint8Array(await by[f.n].arrayBuffer());onp(b.length);return b};
 try{await runUpd()}finally{window.download=origDl;window.base=realBase}}
// ---------- mode A ----------
async function direct(){if(!confirm('VietHUD sẽ khởi động lại và tự tải dữ liệu qua Wi-Fi “'+ST.sta.ssid+'”. Tiếp tục?'))return;
 const r=await fetch('/api/v1/update/direct',{method:'POST',headers:H});const j=await r.json();
 fail(r.ok?'VietHUD đang khởi động lại để tự tải cập nhật. Theo dõi tiến trình trên màn hình VietHUD.':('⚠ '+esc(j.detail)),r.ok?'info':'bad');}
// ---------- live status ----------
function tile(k,v,c,w){return '<div class="tile'+(w?' wide':'')+'"><div class="k">'+k+'</div><div class="v '+(c||'')+'">'+v+'</div></div>'}
async function live(){if(BUSY)return;try{const d=await (await fetch('/api/status',{cache:'no-store'})).json();let h='';
 if(d.cameraAhead)h+='<div class="tile wide cam"><div class="k">Camera phía trước</div><div class="v">'+d.cameraAhead.distanceM.toFixed(0)+' m'+(d.cameraAhead.speedLimitKmh!==null?' · '+d.cameraAhead.speedLimitKmh.toFixed(0)+' km/h':'')+'</div></div>';
 const g=d.gnss;h+=tile('GPS',g.fix?'Đã định vị':(g.linkAlive?'Đang tìm':'Lỗi'),g.fix?'ok':(g.linkAlive?'warn':'bad'));h+=tile('Vệ tinh',g.satCount);
 h+=tile('Tốc độ',g.speedKmh.toFixed(0)+' km/h');h+=tile('Giới hạn',d.speedMap.limitValid?d.speedMap.limitKmh.toFixed(0)+' km/h':'—');
 h+=tile('Hướng',g.headingValid?g.dir+' '+g.headingDeg.toFixed(0)+'°':'—');h+=tile('Bản đồ',d.speedMap.loaded?'Đã nạp':'Chưa nạp',d.speedMap.loaded?'ok':'bad');
 const t=d.boardTempC;h+=tile('Nhiệt độ',t.toFixed(0)+' °C',t>=92?'bad':t>=80?'warn':'ok');h+=tile('Thiết bị kết nối',d.wifi.clients);
 h+=tile('RAM trống',d.mem.freeInternalKB+' KB');h+=tile('Thời gian chạy',Math.floor(d.uptimeMs/60000)+' phút');$('liveGrid').innerHTML=h;}catch(e){}}
// ---------- wifi manager ----------
async function wsaved(){try{const a=await (await fetch('/api/wifi/saved')).json();
 $('wSaved').innerHTML=a.length?a.map(n=>'<div class="it"><b>'+esc(n.ssid)+'</b><span class="x" onclick="wdel('+n.i+')">Xoá</span></div>').join(''):'<div class="small">Chưa lưu mạng nào</div>'}catch(e){}}
async function wscan(go){$('wMsg').textContent='Đang quét…';try{const d=await (await fetch('/api/wifi/scan'+(go?'?rescan=1':''))).json();
 if(d.state===-2){setTimeout(()=>wscan(0),1200);return}$('wMsg').textContent=(d.results||[]).length+' mạng';
 $('wScan').innerHTML=(d.results||[]).map((n,i)=>'<div class="it" data-s="'+esc(n.ssid)+'" onclick="wpick(this)"><b>'+esc(n.ssid)+' '+(n.locked?'🔒':'')+'</b><span class="small">'+n.rssi+' dBm</span></div>').join('')}catch(e){$('wMsg').textContent='Lỗi quét'}}
function wpick(el){$('wSsid').value=el.dataset.s;$('wPass').focus()}
async function wadd(){const b=new URLSearchParams();b.append('ssid',$('wSsid').value);b.append('password',$('wPass').value);if(!$('wSsid').value)return;
 const r=await fetch('/api/wifi/add',{method:'POST',body:b});$('wMsg').textContent=await r.text();$('wPass').value='';wsaved()}
async function wdel(i){if(!confirm('Xoá mạng đã lưu?'))return;const b=new URLSearchParams();b.append('idx',i);await fetch('/api/wifi/del',{method:'POST',body:b});wsaved()}
// ---------- system ----------
async function act(a){$('actMsg').textContent='…';try{const r=await fetch('/api/action?do='+a,{method:'POST'});$('actMsg').textContent=await r.text()}catch(e){$('actMsg').textContent='Lỗi'}}
async function sysInfo(){try{await loadState();const d=ST.device,s=ST.data;
 $('sysInfo').innerHTML=[['Mã thiết bị',d.id],['Firmware',d.fw],['Wi-Fi của VietHUD',d.ap],['Dữ liệu',s.local.version||'—'],['Thẻ nhớ trống',s.sd.freeMB+' MB'],['Internet (Wi-Fi)',ST.sta.connected?ST.sta.ssid:'Không']]
 .map(r=>'<div class="row"><span class="k">'+r[0]+'</span><span class="v">'+esc(r[1])+'</span></div>').join('')}catch(e){}}
// ---------- boot ----------
(async()=>{show((location.hash||'#data').slice(1));try{await loadState()}catch(e){$('hdt').textContent='Mất kết nối'}
 if(curTab==='data')checkUpd();setInterval(async()=>{if(BUSY)return;try{await fetchT('/api/v1/ping',3000);$('hdot').classList.add('on');$('hdt').textContent='Đã kết nối'}catch(e){$('hdot').classList.remove('on');$('hdt').textContent='Mất kết nối'}},5000);})();
</script></body></html>)HTML";
