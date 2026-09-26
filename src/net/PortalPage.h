#pragma once
#include <pgmspace.h>

// VietHUD web portal — ONE self-contained mobile page served at "/" (no
// external assets, works with no internet). Sections: data update (Phone
// Update Bridge client: downloads the signed manifest + data from GitHub over
// the PHONE's own 4G, checks SHA-256 in JS since crypto.subtle is unavailable on
// http://, streams the files to /api/v1/update/* in 1436-B-aligned chunks),
// status, settings (/api/v1/config), optional device WiFi (/api/wifi/*) and
// system (trip logs, data-from-files, firmware /update, tools).
// Protocol: docs/WIFI_PORTAL_UPDATE_BRIDGE_PLAN.md §8-§10.
static const char kPortalHtml[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0E1116"><title>VietHUD</title>
<style>
:root{--bg:#0E1116;--c:#171B22;--l:#262C36;--t:#E8ECF1;--m:#8B95A3;--a:#3B9EFF;--ok:#3CC46E;--w:#E5B53A;--bad:#F2594B}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--t);font:15px/1.45 -apple-system,system-ui,Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:14px 14px 40px}
header{display:flex;align-items:center;gap:8px;margin:2px 2px 14px}header b{font-size:19px}
#id{color:var(--m);font-size:13px}#cn{margin-left:auto;font-size:13px;color:var(--m)}#cn:before{content:"● ";color:var(--bad)}#cn.on:before{color:var(--ok)}
.card,details{background:var(--c);border:1px solid var(--l);border-radius:14px;margin-bottom:12px}
.card{padding:16px}summary{padding:15px 16px;font-weight:600;cursor:pointer;list-style:none}summary::-webkit-details-marker{display:none}
summary:after{content:"›";float:right;color:var(--m)}details[open] summary:after{transform:rotate(90deg)}.in{padding:0 16px 16px}
h2{margin:0 0 6px;font-size:13px;color:var(--m);font-weight:600;text-transform:uppercase;letter-spacing:.5px}
#uS{font-size:17px;font-weight:600;margin:4px 0}.s{color:var(--m);font-size:13px}
.ok{color:var(--ok)}.w{color:var(--w)}.bad{color:var(--bad)}
.bar{height:8px;background:var(--l);border-radius:5px;overflow:hidden;margin:12px 0 4px}.bar i{display:block;height:100%;width:0;background:var(--a);transition:width .3s}
button{width:100%;border:0;border-radius:11px;padding:13px;font-size:16px;font-weight:600;color:#fff;background:var(--a);margin-top:12px;cursor:pointer}
button:disabled{opacity:.45}button.g{background:#232A34;color:#D5DDE6}button.r{background:#2A1A1B;color:#FF9C92}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:8px}.row2 button{margin-top:8px;font-size:14px;padding:11px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.grid div{background:#12161C;border-radius:10px;padding:9px}
.grid small{display:block;color:var(--m);font-size:11px}.grid b{font-size:16px}
.f{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:9px 0;border-top:1px solid var(--l)}.f:first-child{border-top:0}
.f span{flex:1}.f input[type=number]{width:84px}.f input[type=range]{width:44%}
input{background:#0E1116;color:var(--t);border:1px solid var(--l);border-radius:9px;padding:9px;font-size:16px}
input[type=text],input[type=password],input[type=url]{width:100%;margin-top:6px}input[type=checkbox]{width:22px;height:22px;accent-color:var(--a)}
input[type=file]{width:100%;margin-top:8px;font-size:14px}
.li{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-top:1px solid var(--l);gap:10px}.li:first-child{border-top:0}
.li a{color:var(--a);text-decoration:none}.x{color:#FF9C92;font-size:14px;cursor:pointer}.hide{display:none!important}
h3{font-size:14px;margin:16px 0 4px;color:var(--m);font-weight:600}
</style></head><body><main>
<header><b>VietHUD</b><span id="id"></span><span id="cn">Đang kết nối</span></header>

<section class="card">
 <h2>Dữ liệu bản đồ &amp; cảnh báo</h2>
 <div id="uS">Đang kiểm tra…</div><div class="s" id="uD"></div>
 <div class="bar hide" id="uB"><i></i></div><div class="s" id="uP"></div>
 <button id="uBtn" onclick="uClick()" disabled>Kiểm tra cập nhật</button>
 <div class="s hide" id="uH" style="margin-top:10px">Điện thoại chưa ra được Internet.<br>• iPhone: mở <b>Safari</b> và vào <b>192.168.4.1</b>, không dùng cửa sổ “Captive Wi-Fi”; bật Dữ liệu di động cho Safari.<br>• Android: chọn “Giữ kết nối” khi được hỏi và bật dữ liệu di động.</div>
</section>

<section class="card"><h2>Trạng thái</h2><div class="grid" id="st"></div></section>

<details id="dCfg"><summary>Cài đặt</summary><div class="in" id="cfg"></div></details>

<details id="dWifi"><summary>Wi-Fi Internet cho VietHUD <span class="s">(tùy chọn)</span></summary><div class="in">
 <div class="s">Chỉ cần nếu muốn VietHUD tự tải cập nhật. Cập nhật qua điện thoại không cần mục này. VietHUD chỉ dùng Wi-Fi 2.4 GHz.</div>
 <div id="wL"></div>
 <button class="g" onclick="wScan()">Tìm mạng Wi-Fi</button><div id="wS"></div>
 <input type="text" id="wN" placeholder="Tên Wi-Fi" autocapitalize="off"><input type="password" id="wP" placeholder="Mật khẩu">
 <button onclick="wAdd()">Lưu &amp; kết nối</button><div class="s" id="wM"></div>
 <button class="g hide" id="dirBtn" onclick="direct()">Để VietHUD tự tải cập nhật qua Wi-Fi</button>
</div></details>

<details id="dSys"><summary>Hệ thống</summary><div class="in">
 <div id="sys"></div>
 <h3>Nhật ký chuyến đi</h3><div id="tl" class="s">…</div>
 <h3>Cài dữ liệu từ tệp</h3><div class="s">Chọn cùng lúc manifest.txt, manifest.txt.sig và các tệp .bin.</div>
 <input type="file" id="dF" multiple><button class="g" onclick="fromFiles()">Cài dữ liệu từ tệp</button>
 <h3>Firmware</h3><input type="file" id="fwF" accept=".bin"><button class="g" onclick="fwUp()">Nạp firmware</button><div class="s" id="fwM"></div>
 <h3>Công cụ</h3><div class="row2">
  <button class="g" onclick="act('audiotest')">Thử loa</button><button class="g" onclick="act('demo')">Bật/tắt demo</button>
  <button class="r" onclick="if(confirm('Xoá toàn bộ nhật ký chuyến đi?'))act('clearlogs').then(tlLoad)">Xoá nhật ký</button>
  <button class="r" onclick="if(confirm('Khởi động lại VietHUD?'))act('reboot')">Khởi động lại</button></div>
 <div class="s" id="aM"></div>
</div></details>
</main>
<script>
"use strict";
const $=i=>document.getElementById(i),H={'X-VietHUD':'1'},sleep=t=>new Promise(r=>setTimeout(r,t));
const MB=b=>(b/1048576).toFixed(1).replace('.',',')+' MB',E=s=>String(s).replace(/[&<>"]/g,c=>'&#'+c.charCodeAt(0)+';');
let ST=null,REM=null,NEED=[],BUSY=false;
async function tfetch(u,ms,o){const c=new AbortController(),t=setTimeout(()=>c.abort(),ms);try{return await fetch(u,Object.assign({cache:'no-store',signal:c.signal},o||{}))}finally{clearTimeout(t)}}
async function jget(u){return (await tfetch(u,8000)).json()}
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
// ---------- device state + update check ----------
async function loadState(){ST=await jget('/api/v1/update/state');$('id').textContent=ST.device.ap;$('dirBtn').classList.toggle('hide',!ST.sta.connected);return ST}
function base(){let u=ST.url||'';return u.endsWith('/')?u:u+'/'}
function parseMf(text){const files=[];let version='';text.split('\n').forEach(l=>{const p=l.trim().split(/\s+/);if(p[0]==='version')version=p[1];else if(p.length===3)files.push({n:p[0],s:+p[1],h:p[2].toLowerCase()})});return {text,version,files}}
function diff(){const loc={};ST.data.local.files.forEach(f=>loc[f.n]=f.h.toLowerCase());NEED=REM.files.filter(f=>loc[f.n]!==f.h)}
function sets(){return ST.data.datasets.map(d=>{const n=NEED.filter(f=>d.files.includes(f.n)).length;return d.label+': '+(REM?(n?'<span class="w">có bản mới</span>':'<span class="ok">mới nhất</span>'):E(d.version||'—'))}).join(' · ')}
function ui(status,cls,detail,btn,btnCls){$('uS').className=cls||'';$('uS').innerHTML=status;$('uD').innerHTML=detail||'';$('uBtn').textContent=btn;$('uBtn').className=btnCls||'';$('uBtn').disabled=false}
async function check(){if(BUSY)return;$('uBtn').disabled=true;$('uH').classList.add('hide');$('uS').className='';$('uS').textContent='Đang kiểm tra…';
 try{await loadState()}catch(e){return ui('Mất kết nối với VietHUD','bad','','Thử lại')}
 const t0=Date.now();
 try{const r=await tfetch(base()+'manifest.txt?t='+t0,9000);if(!r.ok)throw 0;const m=parseMf(await r.text());
  const g=await tfetch(base()+'manifest.txt.sig?t='+t0,9000);if(!g.ok)throw 0;m.sig=(await g.text()).trim();REM=m;diff();
 }catch(e){REM=null;NEED=[];$('uH').classList.remove('hide');
  return ui('Không kiểm tra được bản mới','w','Đang dùng dữ liệu '+E(ST.data.local.version||'—')+'. VietHUD vẫn hoạt động bình thường.','Thử lại','g')}
 const lv=ST.data.local.version||'',dated=v=>/^\d{4}\.\d\d\.\d\d/.test(v);
 if(NEED.length&&dated(lv)&&dated(REM.version)&&REM.version<lv){NEED=[];return ui('✓ Dữ liệu trên VietHUD mới hơn bản phát hành','ok','Đang dùng '+E(lv)+' · bản phát hành '+E(REM.version),'Kiểm tra lại','g')}
 if(ST.data.last.result==='rollback'&&!NEED.length)return ui('Dữ liệu mới nhất','ok','Lần cài trước lỗi nên VietHUD đã tự khôi phục bản cũ. '+sets(),'Kiểm tra lại','g');
 if(!NEED.length)return ui('✓ Dữ liệu mới nhất','ok','Phiên bản '+E(REM.version)+' · '+sets(),'Kiểm tra lại','g');
 const sz=NEED.reduce((a,f)=>a+f.s,0),res=ST.data.session?' · sẽ tiếp tục phần đã truyền':'';
 ui('Có bản mới '+E(REM.version),'','Đang dùng '+E(ST.data.local.version||'—')+' · '+sets()+res,'Cập nhật · '+MB(sz));}
function uClick(){if(BUSY)return;NEED.length&&REM?run(null):check()}
// ---------- the update (Phone Update Bridge) ----------
function prog(a,b,f,txt){const p=Math.round(a+(b-a)*Math.min(1,f));$('uB').firstChild.style.width=p+'%';$('uP').textContent=txt+(txt.endsWith('…')?'':' '+p+'%')}
function busy(on){BUSY=on;$('uBtn').disabled=on;$('uB').classList.toggle('hide',!on);if(on){$('uP').textContent='';$('uH').classList.add('hide')}}
async function dl(f,onp){const r=await fetch(base()+f.n+'?v='+encodeURIComponent(REM.version),{cache:'no-store'});if(!r.ok)throw Error('Tải '+f.n+' lỗi (HTTP '+r.status+')');
 const out=new Uint8Array(f.s);let o=0;const rd=r.body.getReader();
 for(;;){const {done,value}=await rd.read();if(done)break;if(o+value.length>f.s)throw Error('Tệp '+f.n+' sai kích thước');out.set(value,o);o+=value.length;onp(o)}
 return out}
async function post(u,body,hdr,ms){for(let i=0;;i++){try{return await tfetch(u,ms||20000,{method:'POST',headers:Object.assign({},H,hdr||{}),body})}catch(e){if(i>=3)throw Error('Mất kết nối với VietHUD');await sleep(1500)}}}
async function waitBack(){await sleep(6000);const t0=Date.now();while(Date.now()-t0<150000){try{const j=await (await tfetch('/api/v1/ping',2500)).json();if(j.up<120000)return true}catch(e){}await sleep(2000)}return false}
async function run(local){busy(true);
 try{const tot=NEED.reduce((a,f)=>a+f.s,0),data={};let done=0;
  for(const f of NEED){const buf=local?new Uint8Array(await local[f.n].arrayBuffer()):await dl(f,o=>prog(0,45,(done+o)/tot,'Tải về điện thoại'));
   if(buf.length!==f.s||sha256(buf)!==f.h)throw Error('Tệp '+f.n+' bị lỗi khi tải (SHA-256 không khớp). Thử lại sau vài phút.');data[f.n]=buf;done+=f.s}
  const r=await post('/api/v1/update/session','sig '+REM.sig+'\n'+REM.text,{'Content-Type':'text/plain'}),j=await r.json();
  if(!r.ok)throw Error(j.detail||('Lỗi '+r.status));
  if(j.upToDate){busy(false);return check()}
  const s=j.session,tt=s.files.reduce((a,f)=>a+f.s,0),got={};s.files.forEach(f=>got[f.n]=f.r);
  const up=()=>prog(45,92,Object.values(got).reduce((a,b)=>a+b,0)/tt,'Truyền sang VietHUD');up();
  for(const f of s.files){let off=got[f.n],tries=0;const CH=1436*120;
   while(off<f.s){try{const q=await tfetch('/api/v1/update/file',30000,{method:'PUT',headers:Object.assign({'Content-Type':'application/octet-stream','X-Sid':s.sid,'X-Name':f.n,'X-Offset':String(off)},H),body:data[f.n].subarray(off,Math.min(off+CH,f.s))});
     const a=await q.json();if(q.status===409){off=a.expected;continue}if(!q.ok)throw Error(a.detail||'HTTP '+q.status);off=a.received;tries=0;got[f.n]=off;up();
    }catch(e){if(++tries>8)throw Error('Mất kết nối với VietHUD khi đang truyền. Bấm Cập nhật để tiếp tục.');$('uP').textContent='Đang kết nối lại…';await sleep(1500);
     try{const x=(await jget('/api/v1/update/session')).session.files.find(q=>q.n===f.n);if(x)off=x.r}catch(e2){}}}}
  prog(92,96,1,'VietHUD đang kiểm tra dữ liệu…');let c,cj,lost=false;
  for(;;){try{c=await tfetch('/api/v1/update/commit',60000,{method:'POST',headers:Object.assign({'X-Sid':s.sid},H)});cj=await c.json()}catch(e){lost=true;break}
   if(c.status!==423)break;$('uP').textContent='Hãy dừng xe để cài đặt…';await sleep(3000)}
  if(!lost&&!c.ok)throw Error(cj.detail||('Lỗi '+c.status));
  prog(96,100,1,'Đang cài đặt — VietHUD khởi động lại…');
  if(!await waitBack()){busy(false);return ui('VietHUD đang khởi động lại','w','Hãy nối lại Wi-Fi '+E(ST.device.ap)+' rồi tải lại trang để xem kết quả.','Kiểm tra lại','g')}
  await loadState();busy(false);
  if(ST.data.last.result==='rollback')return ui('Cài đặt không thành công','bad','VietHUD đã tự khôi phục dữ liệu cũ ('+E(ST.data.last.detail)+').','Kiểm tra lại','g');
  if(ST.data.local.version!==REM.version)return ui('Chưa cài xong','w','Bấm Cập nhật để thử lại — phần đã truyền được giữ lại.','Kiểm tra lại','g');
  NEED=[];ui('✓ Đã cập nhật','ok','VietHUD đang dùng dữ liệu '+E(REM.version)+' · '+sets(),'Kiểm tra lại','g');
 }catch(e){busy(false);ui('Cập nhật chưa xong','bad',E(e.message)+'<br>Dữ liệu đang dùng trên VietHUD không bị ảnh hưởng.','Thử lại','g')}}
async function fromFiles(){const fl=[...$('dF').files],by={};fl.forEach(f=>by[f.name]=f);
 if(!by['manifest.txt']||!by['manifest.txt.sig'])return alert('Cần chọn cả manifest.txt và manifest.txt.sig');
 await loadState();REM=parseMf(await by['manifest.txt'].text());REM.sig=(await by['manifest.txt.sig'].text()).trim();diff();
 const miss=NEED.filter(f=>!by[f.n]);if(miss.length)return alert('Còn thiếu: '+miss.map(f=>f.n).join(', '));
 if(!NEED.length)return ui('✓ Dữ liệu đã giống bản trong tệp','ok','','Kiểm tra lại','g');
 window.scrollTo(0,0);run(by)}
async function direct(){if(!confirm('VietHUD sẽ khởi động lại và tự tải dữ liệu qua Wi-Fi “'+ST.sta.ssid+'”. Tiếp tục?'))return;
 const r=await post('/api/v1/update/direct','');const j=await r.json();$('wM').textContent=r.ok?'VietHUD đang khởi động lại để tự tải. Theo dõi trên màn hình VietHUD.':j.detail}
// ---------- status ----------
async function status(){if(BUSY||document.hidden)return;try{const d=await jget('/api/status'),g=d.gnss,t=d.boardTempC;
 const c=(k,v,cl)=>'<div><small>'+k+'</small><b class="'+(cl||'')+'">'+v+'</b></div>';
 $('st').innerHTML=c('GPS',g.fix?g.satCount+' vệ tinh':(g.linkAlive?'Đang tìm':'Lỗi'),g.fix?'ok':g.linkAlive?'w':'bad')+c('Tốc độ',g.speedKmh.toFixed(0)+' km/h')
  +c('Giới hạn',d.speedMap.limitValid?d.speedMap.limitKmh.toFixed(0)+' km/h':'—')+c('Bản đồ',d.speedMap.loaded?'Đã nạp':'Chưa nạp',d.speedMap.loaded?'ok':'bad')
  +c('Nhiệt độ',t.toFixed(0)+'°C',t>=92?'bad':t>=80?'w':'')+c('Wi-Fi Internet',d.wifi.staConnected?E(d.wifi.staSsid):'Không')
  +(d.speedMap.road?'<div style="grid-column:1/-1"><small>Đường</small><b>'+E(d.speedMap.road)+'</b></div>':'');
 $('cn').className='on';$('cn').textContent='Đã kết nối'}catch(e){$('cn').className='';$('cn').textContent='Mất kết nối'}}
// ---------- settings ----------
const CF=[['audioEnabled','Âm thanh cảnh báo','b'],['audioVolume','Âm lượng (%)','r',0,100],['brightness','Độ sáng (%)','r',5,100],
 ['overspeedOffsetKmh','Cảnh báo khi vượt quá (km/h)','n',0,10],['defaultLimitKmh','Giới hạn khi không rõ (km/h, 0 = tắt)','n',0,90],
 ['autoDimMin','Giảm sáng khi dừng sau (phút, 0 = tắt)','n',0,30],['gnssSpeedCalibrationPct','Hiệu chỉnh tốc độ GPS (%)','n',-15,15],['tripLoggingEnabled','Ghi nhật ký chuyến đi','b'],
 ['-','Nâng cao'],['gnssSpeedFilterAlpha','Làm mượt tốc độ (0,05–0,9)','n',0.05,0.9],['gnssFixTimeoutS','Báo mất GPS sau (giây)','n',1,10],
 ['wifiAutoOffMin','Tự tắt Wi-Fi khi không dùng (phút, 0 = không)','n',0,120],['wifiSsid','Tên Wi-Fi của VietHUD','t'],['wifiPassword','Mật khẩu Wi-Fi mới (để trống = giữ nguyên)','p'],['dataUpdateUrl','Địa chỉ dữ liệu','t']];
async function cfgLoad(){const v=await jget('/api/v1/config');$('cfg').innerHTML=CF.map(([k,l,t,a,b])=>{
 if(k==='-')return '<h3>'+l+'</h3>';if(t==='t'||t==='p')return '<div class="f" style="display:block"><span class="s">'+l+'</span><input type="'+(t==='p'?'password':'text')+'" id="c_'+k+'" value="'+E(t==='p'?'':v[k])+'" autocapitalize="off"></div>';
 if(t==='b')return '<label class="f"><span>'+l+'</span><input type="checkbox" id="c_'+k+'"'+(v[k]?' checked':'')+'></label>';
 if(t==='r')return '<div class="f"><span>'+l+' <b id="o_'+k+'">'+v[k]+'</b></span><input type="range" id="c_'+k+'" min="'+a+'" max="'+b+'" value="'+v[k]+'" oninput="$(\'o_'+k+'\').textContent=this.value"></div>';
 return '<div class="f"><span>'+l+'</span><input type="number" step="any" id="c_'+k+'" min="'+a+'" max="'+b+'" value="'+v[k]+'"></div>'}).join('')+'<button onclick="cfgSave()">Lưu cài đặt</button><div class="s" id="cM"></div>'}
async function cfgSave(){const p=new URLSearchParams();CF.forEach(([k,,t])=>{if(k==='-')return;const e=$('c_'+k);p.append(k,t==='b'?(e.checked?'1':'0'):e.value)});
 const r=await post('/api/v1/config',p);let j={};try{j=await r.json()}catch(e){}$('cM').innerHTML=r.ok?'<span class="ok">✓ Đã lưu</span>':'<span class="bad">'+E(j.detail||'Lỗi lưu')+'</span>';if(r.ok)cfgLoad()}
// ---------- wifi ----------
async function wList(){const a=await jget('/api/wifi/saved');$('wL').innerHTML=a.length?a.map(n=>'<div class="li"><span>'+E(n.ssid)+'</span><span class="x" onclick="wDel('+n.i+')">Xoá</span></div>').join(''):'<div class="s">Chưa lưu mạng nào.</div>'}
async function wScan(){$('wM').textContent='Đang tìm…';let d;for(let i=0;i<15;i++){d=await jget('/api/wifi/scan'+(i?'':'?rescan=1'));if(d.state!==-2)break;await sleep(1200)}
 $('wM').textContent='';$('wS').innerHTML=(d.results||[]).map(n=>'<div class="li" style="cursor:pointer" onclick="$(\'wN\').value=this.dataset.s;$(\'wP\').focus()" data-s="'+E(n.ssid)+'"><span>'+E(n.ssid)+(n.locked?' 🔒':'')+'</span><span class="s">'+n.rssi+' dBm</span></div>').join('')}
async function wAdd(){if(!$('wN').value)return;const p=new URLSearchParams({ssid:$('wN').value,password:$('wP').value});const r=await fetch('/api/wifi/add',{method:'POST',body:p});$('wM').textContent=r.ok?'Đã lưu. VietHUD đang kết nối…':'Lỗi';$('wP').value='';wList()}
async function wDel(i){if(!confirm('Xoá mạng này?'))return;await fetch('/api/wifi/del',{method:'POST',body:new URLSearchParams({idx:i})});wList()}
// ---------- system ----------
async function act(a){$('aM').textContent='…';try{$('aM').textContent=await (await fetch('/api/action?do='+a,{method:'POST'})).text()}catch(e){$('aM').textContent='Lỗi'}}
async function sysLoad(){await loadState();const d=ST.device,s=ST.data;
 $('sys').innerHTML=[['Firmware',d.fw],['Dữ liệu',s.local.version||'—'],['Wi-Fi VietHUD',d.ap],['Thẻ nhớ trống',s.sd.freeMB+' MB']].map(r=>'<div class="li"><span class="s">'+r[0]+'</span><span>'+E(r[1])+'</span></div>').join('');tlLoad()}
async function tlLoad(){const a=await jget('/api/v1/triplogs');$('tl').innerHTML=a.length?a.map(t=>'<div class="li"><a href="/triplog/get?id='+t.id+'">session_'+String(t.id).padStart(4,'0')+'.csv</a><span class="s">'+Math.ceil(t.size/1024)+' KB</span></div>').join(''):'Chưa có nhật ký.'}
async function fwUp(){const f=$('fwF').files[0];if(!f)return;const b=new Uint8Array(await f.slice(0,1).arrayBuffer());
 if(b[0]!==0xE9)return $('fwM').textContent='Tệp này không phải firmware ESP32.';if(!confirm('Nạp firmware '+f.name+'? VietHUD sẽ khởi động lại.'))return;
 const fd=new FormData();fd.append('update',f,f.name);const x=new XMLHttpRequest();x.open('POST','/update');x.setRequestHeader('X-VietHUD','1');BUSY=true;
 x.upload.onprogress=e=>$('fwM').textContent='Đang nạp '+Math.round(100*e.loaded/e.total)+'%';
 x.onload=async()=>{$('fwM').textContent=x.responseText;if(x.status===200){await waitBack();location.reload()}BUSY=false};x.onerror=()=>{$('fwM').textContent='Mất kết nối';BUSY=false};x.send(fd)}
// ---------- start ----------
$('dCfg').ontoggle=e=>e.target.open&&cfgLoad();$('dWifi').ontoggle=e=>e.target.open&&wList();$('dSys').ontoggle=e=>e.target.open&&sysLoad();
check();status();setInterval(status,2000);
</script></body></html>
)HTML";
