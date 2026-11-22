/**
 * 传图网页实现：一个 WebServer + 一张内嵌 HTML（PROGMEM，不占 RAM）
 * 图像处理全部在手机浏览器里完成，设备只收 5776 字节的双层点阵。
 * ==========================================================================*/
#include "photo_server.h"

#include <WebServer.h>
#include <WiFi.h>
#include <string.h>

#include "city.h"
#include "log.h"

/* ============================================================================
 * 网页源码
 * ----------------------------------------------------------------------------
 * 流程：选图 -> 拖拽/双指缩放裁成 1:1 -> 608x608 取样 -> 4x4 面积平均到
 *       152x152 -> 亮度转墨量 -> 抖动（默认）或 Otsu 硬阈值 -> 按「黑色变红」
 *       落到黑层或红层 -> base64 上传。
 * 为什么是 base64：ESP32 的 WebServer 把裸 body 存进 String 时走的是
 *       String(const char*) 构造，遇到 0x00 就截断，二进制直传会丢数据；
 *       base64 是纯文本，多 33% 流量（7.7KB）换来零歧义。
 * ==========================================================================*/
static constexpr char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="theme-color" content="#f4f4f0">
<title>墨水屏</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:14px 14px 30px;font:15px/1.55 -apple-system,"PingFang SC","Microsoft YaHei",sans-serif;background:#f4f4f0;color:#1b1b1b}
h1{font-size:17px;margin:2px 0 4px;font-weight:600}
.tip{color:#8a8a84;font-size:12px;margin:0 0 14px}
.card{background:#fff;border-radius:14px;padding:12px;margin-bottom:12px;box-shadow:0 1px 2px rgba(0,0,0,.05)}
.hd{display:flex;align-items:center;gap:8px;cursor:pointer}
.hd .lab{font-size:12px;color:#8a8a84}
.hd .cur{flex:1;font-size:17px;font-weight:600}
.hd .arr{font-size:12px;color:#8a8a84}
#cityBody{margin-top:12px}
#geo{width:100%;padding:12px;border-radius:10px;background:#1b1b1b;color:#fff;font:inherit;font-size:15px}
#geo:disabled{background:#b9b9b2}
#q{width:100%;margin-top:10px;padding:10px 12px;border:1px solid #e4e4de;border-radius:10px;background:#fafaf7;font:inherit;outline:0}
#q:focus{border-color:#c9c9c0}
.clist{margin-top:8px;max-height:216px;overflow:auto;-webkit-overflow-scrolling:touch}
.ci{display:flex;justify-content:space-between;align-items:baseline;padding:9px 2px;border-bottom:1px solid #f2f2ec;font-size:15px}
.ci small{color:#a8a8a0;font-size:12px}
.cmsg{margin-top:8px;font-size:13px;color:#666;min-height:18px}
.cmsg.ok{color:#1a7f37}
.cmsg.err{color:#b3261e}
.pick{display:block;text-align:center;padding:24px 12px;border:1.5px dashed #cfcfc8;border-radius:12px;color:#666}
.pick b{display:block;font-size:16px;color:#111;margin-bottom:4px}
.pick small{color:#8a8a84}
#crop{position:relative;margin:0 auto;background:#e8e8e2;border-radius:10px;overflow:hidden;touch-action:none}
#crop canvas{display:block;width:100%;height:100%}
#crop::after{content:"";position:absolute;inset:8px;border:1px dashed rgba(0,0,0,.16);border-radius:6px;pointer-events:none}
.row{display:flex;align-items:center;gap:10px;margin-top:12px}
.row label{font-size:13px;color:#666;white-space:nowrap}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:22px;background:transparent}
input[type=range]::-webkit-slider-runnable-track{height:4px;background:#e4e4de;border-radius:2px}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;margin-top:-8px;border-radius:50%;background:#1b1b1b;border:0}
.btns{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:12px}
button{font:inherit;border:0;border-radius:10px;padding:11px 12px;background:#ececE6;color:#1b1b1b}
button:active{opacity:.7}
.sw{display:flex;align-items:center;justify-content:center;gap:7px;font-size:13px;color:#555;background:#ececE6;border-radius:10px}
.wide{grid-column:1/-1}
.sw input{width:17px;height:17px;accent-color:#1b1b1b}
.prev{display:flex;gap:12px;align-items:center;margin-top:14px}
#out{width:152px;height:152px;image-rendering:pixelated;border:1px solid #e2e2dc;border-radius:6px;background:#fff;flex:0 0 auto}
.prev .hint{font-size:12px;color:#8a8a84;line-height:1.5}
.prev b{display:block;color:#1b1b1b;font-size:13px;margin-bottom:3px}
.primary{display:block;width:100%;margin-top:14px;background:#1b1b1b;color:#fff;padding:14px;font-size:16px;border-radius:12px}
.primary:disabled{background:#b9b9b2}
#msg{margin-top:10px;font-size:13px;color:#666;min-height:19px}
#msg.ok{color:#1a7f37}
#msg.err{color:#b3261e}
</style>
</head>
<body>
<h1>墨水屏</h1>
<p class="tip">手机与设备连同一个 Wi-Fi。这一页可以改天气城市，也可以传图。</p>

<div class="card">
  <div class="hd" id="cityHd">
    <span class="lab">城市</span><span class="cur" id="cur">…</span><span class="arr" id="arr">展开</span>
  </div>
  <div id="cityBody" hidden>
    <button id="geo">用当前位置自动选</button>
    <input id="q" type="search" placeholder="搜索城市或省份：苏州 / 江苏">
    <div class="clist" id="clist"></div>
    <div class="cmsg" id="cmsg"></div>
  </div>
</div>

<div class="card">
  <label class="pick" for="file">
    <b>选择照片</b>
    <small>相册或相机，任意尺寸都能裁成 1:1</small>
  </label>
  <input id="file" type="file" accept="image/*" hidden>
</div>

<div class="card" id="editCard" hidden>
  <div id="crop"><canvas id="view"></canvas></div>
  <div class="row"><label>缩放</label><input id="zoom" type="range" min="1" max="5" step="0.01" value="1"></div>
  <div class="row"><label>对比度</label><input id="contrast" type="range" min="0.6" max="2" step="0.05" value="1.05"></div>
  <div class="btns">
    <button id="rotate">旋转 90°</button>
    <label class="sw"><input id="toRed" type="checkbox">黑色变红</label>
    <label class="sw wide"><input id="dither" type="checkbox" checked>抖动（照片更细腻）</label>
  </div>
  <div class="prev">
    <canvas id="out" width="152" height="152"></canvas>
    <div class="hint"><b>屏幕实际效果</b><span id="hintText"></span>拖动可移动取景，双指可缩放。</div>
  </div>
  <button id="send" class="primary">上传到屏幕</button>
  <div id="msg"></div>
</div>

<script>
(function(){
"use strict";
var $=function(s){return document.querySelector(s)};
var crop=$("#crop"), view=$("#view"), vctx=view.getContext("2d");
var outCv=$("#out"), octx=outCv.getContext("2d");
var zoomEl=$("#zoom"), conEl=$("#contrast"), toRedEl=$("#toRed"), ditEl=$("#dither");
var hintEl=$("#hintText");
var msgEl=$("#msg"), sendBtn=$("#send"), fileEl=$("#file");
var W=152, OUT=608, BYTES=W*W/8;
var img=null, zoom=1, side=0, ox=0, oy=0, contrast=1.05, timer=0;

function clamp(v,a,b){return v<a?a:(v>b?b:v)}
function say(t,cls){msgEl.textContent=t||"";msgEl.className=cls||""}

/* ---------- 城市：搜索 352 个城市，或按定位自动选 ---------- */
var curEl=$("#cur"), arrEl=$("#arr"), cityHd=$("#cityHd"), cityBody=$("#cityBody");
var qEl=$("#q"), clistEl=$("#clist"), cmsgEl=$("#cmsg"), geoEl=$("#geo");
var PROVS=[], CITIES=[], CURCODE=0, cityLocked=false;

function csay(t,cls){cmsgEl.textContent=t||"";cmsgEl.className="cmsg "+(cls||"")}
function setOpen(open){cityBody.hidden=!open;arrEl.textContent=open?"收起":"展开"}

function drawList(q){
  var h="",i,c,pn;
  for(i=0;i<CITIES.length;i++){
    c=CITIES[i]; pn=PROVS[c[2]];
    if(q&&c[0].indexOf(q)<0&&pn.indexOf(q)<0) continue;
    if(pn===c[0]) pn="";                     /* 直辖市：省市同名不重复写 */
    h+='<div class="ci'+(c[1]===CURCODE?" on":"")+'" data-c="'+c[1]+'" data-n="'+c[0]+'">'
      +'<span>'+c[0]+(c[1]===CURCODE?" ·当前":"")+'</span><small>'+pn+'</small></div>';
  }
  clistEl.innerHTML=h||'<div class="ci"><span>没有匹配的城市</span></div>';
}

function lockCity(){
  cityLocked=true;
  qEl.disabled=true;
  geoEl.disabled=true;
  clistEl.style.pointerEvents="none";
}

function applied(name){
  curEl.textContent=name;
  lockCity();
  csay("已切换到 "+name+"，屏幕正在刷新（约 20 秒）…","ok");
}

fetch("/cities").then(function(r){return r.json()}).then(function(j){
  PROVS=j.p||[]; CITIES=j.l||[]; CURCODE=j.c||0;
  curEl.textContent=j.n||"";
  drawList("");
  if(location.hash==="#city") setOpen(true);
}).catch(function(){curEl.textContent="读取失败"});

cityHd.addEventListener("click",function(){setOpen(cityBody.hidden)});
qEl.addEventListener("input",function(){drawList(qEl.value.trim())});

clistEl.addEventListener("click",function(e){
  if(cityLocked) return;
  var d=e.target.closest?e.target.closest(".ci"):null;
  if(!d||!d.getAttribute("data-c")) return;
  var code=d.getAttribute("data-c"), name=d.getAttribute("data-n");
  csay("正在切换到 "+name+"…");
  fetch("/city",{method:"POST",headers:{"Content-Type":"text/plain"},body:code})
    .then(function(r){return r.json()}).then(function(j){
      if(!j||!j.ok) throw new Error((j&&j.err)||"设备拒绝");
      applied(j.n||name);
    }).catch(function(err){csay("切换失败："+err.message,"err")});
});

function geoApply(lat,lon){
  var u="/geo"+(lat==null?"":("?lat="+lat+"&lon="+lon));
  fetch(u,{method:"POST"}).then(function(r){return r.json()}).then(function(j){
    if(!j||!j.ok) throw new Error((j&&j.err)||"定位失败");
    applied(j.n);
  }).catch(function(err){
    geoEl.disabled=false;
    csay(err.message+"，请在下面手动选","err");
  });
}

geoEl.addEventListener("click",function(){
  geoEl.disabled=true;
  csay("正在定位…");
  function byIp(){geoApply(null,null)}
  /* 网页是 http://192.168.x.x，不是安全上下文，多数浏览器会拒绝 GPS；
     真给了坐标就用坐标，否则退回设备侧按公网 IP 定位（同一个出口 IP）。 */
  if(window.isSecureContext&&navigator.geolocation){
    navigator.geolocation.getCurrentPosition(
      function(p){geoApply(p.coords.latitude,p.coords.longitude)},
      byIp,{timeout:6000,maximumAge:600000});
  }else byIp();
});

/* ---------- 取景框 ---------- */
function layout(){
  side=Math.round(Math.max(200,Math.min(window.innerWidth-56,360)));
  crop.style.width=side+"px";crop.style.height=side+"px";
  view.width=side;view.height=side;
  if(img){coverReset(false);draw();}
}
window.addEventListener("resize",layout);

function coverScale(){return Math.max(side/img.width,side/img.height)}

function clampOff(){
  var s=coverScale()*zoom;
  ox=Math.min(0,Math.max(side-img.width*s,ox));
  oy=Math.min(0,Math.max(side-img.height*s,oy));
}
function coverReset(redraw){
  var s=coverScale()*zoom;
  ox=(side-img.width*s)/2; oy=(side-img.height*s)/2;
  clampOff();
  if(redraw!==false) draw();
}
function draw(){
  if(!img) return;
  var s=coverScale()*zoom;
  vctx.clearRect(0,0,side,side);
  vctx.imageSmoothingEnabled=true;
  vctx.imageSmoothingQuality="high";
  vctx.drawImage(img,ox,oy,img.width*s,img.height*s);
}
function setZoom(z){
  z=clamp(z,1,5);
  var k=z/zoom,c=side/2;
  ox=c-(c-ox)*k; oy=c-(c-oy)*k; zoom=z;
  clampOff();draw();zoomEl.value=zoom;
}

/* ---------- 指针：拖动 + 双指缩放 ---------- */
var pts={}, np=0, lastD=0;
function dist(){
  var a=[],k;
  for(k in pts) a.push(pts[k]);
  if(a.length<2) return 0;
  var dx=a[0].x-a[1].x, dy=a[0].y-a[1].y;
  return Math.sqrt(dx*dx+dy*dy);
}
crop.addEventListener("pointerdown",function(e){
  if(!img) return;
  try{crop.setPointerCapture(e.pointerId)}catch(x){}
  pts[e.pointerId]={x:e.clientX,y:e.clientY,px:e.clientX,py:e.clientY};
  np++; if(np===2) lastD=dist();
});
crop.addEventListener("pointermove",function(e){
  var p=pts[e.pointerId]; if(!p||!img) return;
  if(np===1){
    ox+=e.clientX-p.px; oy+=e.clientY-p.py;
    p.px=e.clientX; p.py=e.clientY;
    clampOff(); draw();
  }else{
    p.x=e.clientX; p.y=e.clientY;
    var d=dist();
    if(lastD>0&&d>0) setZoom(zoom*d/lastD);
    lastD=d;
  }
});
function up(e){
  if(pts[e.pointerId]){delete pts[e.pointerId];np=Math.max(0,np-1);}
  if(np<2) lastD=0;
  if(np===1){for(var k in pts) pts[k].px=pts[k].x, pts[k].py=pts[k].y;}
}
crop.addEventListener("pointerup",up);
crop.addEventListener("pointercancel",up);

/* ---------- 采样：608x608 -> 4x4 面积平均 -> 152x152 ---------- */
function sample(){
  var cv=document.createElement("canvas");cv.width=OUT;cv.height=OUT;
  var c=cv.getContext("2d");
  c.imageSmoothingEnabled=true;c.imageSmoothingQuality="high";
  var k=OUT/side,s=coverScale()*zoom;
  c.drawImage(img,ox*k,oy*k,img.width*s*k,img.height*s*k);
  var d=c.getImageData(0,0,OUT,OUT).data;
  var n=OUT/W, inv=1/(n*n);
  var r=new Float32Array(W*W),g=new Float32Array(W*W),b=new Float32Array(W*W),i=0;
  for(var y=0;y<OUT;y+=n){
    for(var x=0;x<OUT;x+=n){
      var sr=0,sg=0,sb=0;
      for(var j=0;j<n;j++){
        var row=((y+j)*OUT+x)*4;
        for(var m=0;m<n;m++){
          sr+=d[row+m*4];sg+=d[row+m*4+1];sb+=d[row+m*4+2];
        }
      }
      r[i]=sr*inv;g[i]=sg*inv;b[i]=sb*inv;i++;
    }
  }
  return {r:r,g:g,b:b};
}

/* Floyd–Steinberg：输入 0..255 墨量（越大越该着色）。
   on=false 走硬阈值：不做误差扩散，边缘保持锐利，适合二维码/截图/线稿。
   thr 是黑白分界，抖动时固定 128，硬阈值时由 otsu() 按图片直方图算。 */
function fs(src,on,thr){
  var w=W,n=w*w,bits=new Uint8Array(n),i;
  if(!on){
    for(i=0;i<n;i++) bits[i]=src[i]>=thr?1:0;
    return bits;
  }
  var buf=Float32Array.from(src);
  for(var y=0;y<w;y++){
    var l2r=(y%2===0),dx=l2r?1:-1;
    for(var k=0;k<w;k++){
      var x=l2r?k:(w-1-k),idx=y*w+x;
      var v=buf[idx], on2=v>=128?1:0;
      bits[idx]=on2;
      var e=v-(on2?255:0);
      var xr=x+dx;
      if(xr>=0&&xr<w) buf[idx+dx]+=e*0.4375;
      if(y+1<w){
        if(x-dx>=0&&x-dx<w) buf[(y+1)*w+x-dx]+=e*0.1875;
        buf[(y+1)*w+x]+=e*0.3125;
        if(xr>=0&&xr<w) buf[(y+1)*w+xr]+=e*0.0625;
      }
    }
  }
  return bits;
}

/* Otsu 大津法：按直方图找「类间方差最大」的灰度分界，不依赖经验常数。
   二维码、文档这类双峰图能一次分出黑白；照片也能给出合理的中间调切分。 */
function otsu(src){
  var hist=new Float64Array(256),i,n=src.length;
  for(i=0;i<n;i++){
    var v=Math.round(src[i]);
    hist[v<0?0:(v>255?255:v)]++;
  }
  var sum=0;
  for(i=0;i<256;i++) sum+=i*hist[i];
  var sumB=0,wB=0,best=128,bestVar=-1;
  for(i=0;i<256;i++){
    wB+=hist[i];
    if(!wB) continue;
    var wF=n-wB;
    if(!wF) break;
    sumB+=i*hist[i];
    var d=sumB/wB-(sum-sumB)/wF;
    var varBetween=wB*wF*d*d;
    if(varBetween>bestVar){bestVar=varBetween;best=i;}
  }
  return best;
}

/* 亮度 -> 墨量（0..255，越大越该着色）。对比度以 128 为轴放大。
   整个管线只有一条墨量通道：颜色在最后一步决定，不在这里分黑红。 */
function inkFor(r,g,b,c){
  var lum=0.299*r+0.587*g+0.114*b;
  lum=(lum-128)*c+128;
  return 255-clamp(lum,0,255);
}

function render(){
  if(!img) return null;
  var s=sample(),n=W*W,c=contrast,i;
  var K=new Float32Array(n);
  for(i=0;i<n;i++) K[i]=inkFor(s.r[i],s.g[i],s.b[i],c);

  var dither=ditEl.checked;
  // 抖动：误差扩散；关抖动：Otsu 自适应阈值，二维码/线稿边缘才干净
  var on = dither ? fs(K,true,128) : fs(K,false,otsu(K));

  // 着色层二选一：黑色变红时整层搬去红色层，两层永远互斥
  var asRed=toRedEl.checked;
  var black=new Uint8Array(BYTES),red=new Uint8Array(BYTES);
  var id=octx.createImageData(W,W), px=id.data;
  for(var y=0;y<W;y++){
    for(var x=0;x<W;x++){
      var idx=y*W+x,bit=0x80>>(x&7),off=y*19+(x>>3),o=idx*4;
      px[o+3]=255;
      if(on[idx]){
        (asRed?red:black)[off]|=bit;
        if(asRed){px[o]=192;px[o+1]=40;px[o+2]=32;}
        else{px[o]=18;px[o+1]=18;px[o+2]=18;}
      }else{px[o]=255;px[o+1]=255;px[o+2]=255;}
    }
  }
  octx.putImageData(id,0,0);
  return {black:black,red:red};
}

function schedulePreview(){
  clearTimeout(timer);
  timer=setTimeout(function(){render()},90);
}

/* ---------- 上传 ---------- */
function b64(bytes){
  var s="",chunk=4096;
  for(var i=0;i<bytes.length;i+=chunk){
    s+=String.fromCharCode.apply(null,bytes.subarray(i,i+chunk));
  }
  return btoa(s);
}

var busyTimer=0;
function countdown(sec){
  var left=sec;
  say("已发送，屏幕正在刷新（约 "+left+" 秒）…");
  clearInterval(busyTimer);
  busyTimer=setInterval(function(){
    left--;
    if(left<=0){
      clearInterval(busyTimer);
      sendBtn.disabled=false;
      say("完成，可以继续上传下一张。","ok");
    }else if(left>3){
      say("已发送，屏幕正在刷新（约 "+left+" 秒）…");
    }else{
      say("快好了…");
    }
  },1000);
}

sendBtn.addEventListener("click",function(){
  var f=render();
  if(!f){say("还没有选择图片","err");return;}
  sendBtn.disabled=true;
  say("正在发送…");
  /* 两层要拼成一段再 base64：分别编码会出现两段 '=' 填充，拼起来解码会错位 */
  var all=new Uint8Array(BYTES*2);
  all.set(f.black,0); all.set(f.red,BYTES);
  fetch("/upload",{
    method:"POST",
    headers:{"Content-Type":"text/plain;charset=utf-8"},
    body:b64(all)
  }).then(function(r){return r.json()}).then(function(j){
    if(!j||!j.ok) throw new Error((j&&j.err)||"设备拒绝");
    countdown(22);
  }).catch(function(e){
    sendBtn.disabled=false;
    say("发送失败："+e.message,"err");
  });
});

/* ---------- 事件 ---------- */
fileEl.addEventListener("change",function(){
  var f=fileEl.files&&fileEl.files[0];
  fileEl.value="";
  if(!f) return;
  var url=URL.createObjectURL(f);
  var im=new Image();
  im.onload=function(){
    URL.revokeObjectURL(url);
    img=im; zoom=1; zoomEl.value=1;
    $("#editCard").hidden=false;
    layout(); coverReset(); schedulePreview();
    say("");
    window.scrollTo(0,$("#editCard").offsetTop-10);
  };
  im.onerror=function(){URL.revokeObjectURL(url);say("这张图打不开，换一张试试","err")};
  im.src=url;
});

zoomEl.addEventListener("input",function(){setZoom(parseFloat(zoomEl.value))});
conEl.addEventListener("input",function(){contrast=parseFloat(conEl.value);schedulePreview()});
toRedEl.addEventListener("change",schedulePreview);

/* 抖动开关：关掉走硬阈值，二维码/线稿边缘才不会被误差扩散打毛 */
function syncHint(){
  hintEl.textContent=ditEl.checked
    ? "抖动开：灰阶靠点阵疏密过渡，照片更自然。"
    : "抖动关：纯黑白二值，边缘更锐利。";
}
ditEl.addEventListener("change",function(){syncHint();schedulePreview()});

$("#rotate").addEventListener("click",function(){
  if(!img) return;
  var cv=document.createElement("canvas");
  cv.width=img.height;cv.height=img.width;
  var c=cv.getContext("2d");
  c.translate(cv.width/2,cv.height/2);
  c.rotate(Math.PI/2);
  c.drawImage(img,-img.width/2,-img.height/2);
  img=cv; zoom=1; zoomEl.value=1;
  coverReset(); schedulePreview();
});

syncHint();
layout();
})();
</script>
</body>
</html>
)HTML";

/* ============================================================================
 * 服务状态
 * ==========================================================================*/
namespace
{
    WebServer g_srv(80);

    // 收下的帧先存在这里：主循环可能在刷屏（20s），不能在 HTTP 回调里直接刷
    uint8_t g_pending[PHOTO_BODY_BYTES];
    volatile bool g_hasFrame = false;
    volatile bool g_busy = false;
    uint32_t g_count = 0;
    bool g_routesReady = false;

    // 手机改过城市：等一小会儿让响应发完，再由主循环接手（停网页 -> 回天气页刷新）
    volatile bool g_cityPending = false;
    uint32_t g_cityAt = 0;

    void requestCityApply()
    {
        g_cityAt = millis() + 1200;
        g_cityPending = true;
    }

    int b64val(char c)
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1; // '='、空白、换行都跳过
    }

    size_t b64decode(const char* s, size_t n, uint8_t* out, size_t cap)
    {
        uint32_t acc = 0;
        int bits = 0;
        size_t o = 0;
        for (size_t i = 0; i < n; i++)
        {
            const int v = b64val(s[i]);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                if (o < cap) out[o++] = (uint8_t)((acc >> bits) & 0xFF);
            }
        }
        return o;
    }

    void handleRoot()
    {
        g_srv.sendHeader("Cache-Control", "no-store");
        g_srv.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
    }

    void handleUpload()
    {
        if (g_busy || g_hasFrame)
        {
            g_srv.send(503, "application/json", "{\"ok\":0,\"err\":\"上一张还在刷屏\"}");
            return;
        }
        const String& body = g_srv.arg("plain");
        const size_t n = b64decode(body.c_str(), body.length(), g_pending, sizeof(g_pending));
        if (n != sizeof(g_pending))
        {
            logf("[PHOTO] bad length: decoded %u bytes, expected %u (got %u chars)\n",
                 (unsigned)n, (unsigned)sizeof(g_pending), (unsigned)body.length());
            g_srv.send(400, "application/json", "{\"ok\":0,\"err\":\"数据不完整\"}");
            return;
        }
        g_hasFrame = true;
        g_count++;
        logf("[PHOTO] received photo #%u, %u bytes\n", (unsigned)g_count, (unsigned)n);
        g_srv.send(200, "application/json", "{\"ok\":1}");
    }

    void handleStatus()
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"busy\":%d,\"count\":%u}",
                 (g_hasFrame || g_busy) ? 1 : 0, (unsigned)g_count);
        g_srv.send(200, "application/json", buf);
    }

    /**
     * GET /cities：城市清单。
     * 352 条连同省名约 11KB，分块发（HTTP chunked），不整段堆在 RAM 里。
     * 格式：{"n":当前城市,"c":当前编号,"p":[省...],"l":[["市",编号,省下标],...]}
     */
    void handleCities()
    {
        g_srv.sendHeader("Cache-Control", "no-store");
        g_srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
        g_srv.send(200, "application/json", "");

        String out;
        out.reserve(1024);
        char tmp[72];

        out += "{\"n\":\"";
        out += cityCurrent().name;
        snprintf(tmp, sizeof(tmp), "\",\"c\":%lu,\"p\":[", (unsigned long)cityCurrent().code);
        out += tmp;

        for (int i = 0; i < CITY_PROVINCE_COUNT; i++)
        {
            if (i) out += ',';
            out += '"';
            out += CITY_PROVINCES[i];
            out += '"';
            if (out.length() >= 800)
            {
                g_srv.sendContent(out);
                out = "";
            }
        }

        out += "],\"l\":[";
        for (int i = 0; i < CITY_COUNT; i++)
        {
            snprintf(tmp, sizeof(tmp), "%s[\"%s\",%lu,%u]",
                     i ? "," : "", CITY_TABLE[i].name,
                     (unsigned long)CITY_TABLE[i].code, (unsigned)CITY_TABLE[i].prov);
            out += tmp;
            if (out.length() >= 800)
            {
                g_srv.sendContent(out);
                out = "";
            }
        }

        out += "]}";
        g_srv.sendContent(out);
        g_srv.sendContent(""); // 结束分块
        logf("[CITY] sent city table, %d entries\n", CITY_COUNT);
    }

    /** POST /city：body 是编号（纯数字，或 form 形式的 code=数字） */
    void handleSetCity()
    {
        const String& body = g_srv.arg("plain");
        int i = 0;
        while (i < (int)body.length() && (body[i] < '0' || body[i] > '9')) i++;
        const uint32_t code = (uint32_t)strtoul(body.c_str() + i, nullptr, 10);

        if (code == 0 || !citySelect(code))
        {
            logf("[CITY] requested code not in table: %s\n", body.c_str());
            g_srv.send(404, "application/json", "{\"ok\":0,\"err\":\"城市不在列表里\"}");
            return;
        }

        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":1,\"n\":\"%s\"}", cityCurrent().name);
        g_srv.send(200, "application/json", buf);
        requestCityApply();
    }

    /** POST /geo[?lat=&lon=]：手机给了坐标就用坐标，否则按公网 IP 定位 */
    void handleGeo()
    {
        const CityRec* c = nullptr;
        if (g_srv.hasArg("lat") && g_srv.hasArg("lon"))
        {
            c = cityNearest(g_srv.arg("lat").toFloat(), g_srv.arg("lon").toFloat());
        }
        else
        {
            logLine("[CITY] phone asked for IP geo");
            c = cityLocateByIP();
        }

        if (c == nullptr || !citySelect(c->code))
        {
            g_srv.send(200, "application/json", "{\"ok\":0,\"err\":\"定位失败\"}");
            return;
        }

        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":1,\"n\":\"%s\",\"c\":%lu}",
                 c->name, (unsigned long)c->code);
        g_srv.send(200, "application/json", buf);
        requestCityApply();
    }

    void handleNotFound()
    {
        g_srv.send(404, "text/plain", "not found");
    }
} // namespace

bool photoServerStart()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        logLine("[PHOTO] WiFi not connected, cannot start web server");
        return false;
    }
    if (!g_routesReady)
    {
        // 路由只注册一次：on() 是往数组里追加，重复 begin 会重复注册
        g_srv.on("/", HTTP_GET, handleRoot);
        g_srv.on("/cities", HTTP_GET, handleCities);
        g_srv.on("/city", HTTP_POST, handleSetCity);
        g_srv.on("/geo", HTTP_POST, handleGeo);
        g_srv.on("/upload", HTTP_POST, handleUpload);
        g_srv.on("/status", HTTP_GET, handleStatus);
        g_srv.onNotFound(handleNotFound);
        g_routesReady = true;
    }
    g_srv.begin();
    WiFi.setSleep(false); // 关省电，手机点一下就能响应
    logf("[PHOTO] web server up at http://%s/\n",
         WiFi.localIP().toString().c_str());
    return true;
}

void photoServerLoop()
{
    g_srv.handleClient();
}

void photoServerStop()
{
    g_srv.stop();
    WiFi.setSleep(true);
    g_hasFrame = false;
    g_busy = false;
    g_cityPending = false;
    logLine("[PHOTO] web server stopped");
}

String photoServerUrl(bool forCity)
{
    String url = String("http://") + WiFi.localIP().toString() + "/";
    if (forCity) url += "#city"; // 片段不会发给板子，只用来让网页滚到城市区
    return url;
}

bool photoCityChangeDue()
{
    if (!g_cityPending) return false;
    if ((int32_t)(millis() - g_cityAt) < 0) return false;
    g_cityPending = false;
    return true;
}

bool photoTakeFrame(uint8_t* black, uint8_t* red)
{
    if (!g_hasFrame) return false;
    // 网页按「1 = 该颜色着色」生成，屏幕要求「0 = 着色」，整层取反
    for (size_t i = 0; i < PHOTO_FRAME_BYTES; i++)
    {
        black[i] = (uint8_t)~g_pending[i];
        red[i] = (uint8_t)~g_pending[PHOTO_FRAME_BYTES + i];
    }
    g_hasFrame = false;
    g_busy = true;
    return true;
}

void photoMarkDone()
{
    g_busy = false;
}

uint32_t photoFrameCount()
{
    return g_count;
}
