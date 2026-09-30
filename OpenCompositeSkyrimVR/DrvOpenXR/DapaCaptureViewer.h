#pragma once
inline constexpr char s_dapaCaptureViewer[] = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DAPA — matched frame inspection</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#11151b;color:#e6eaf0;font:16px system-ui,sans-serif}
main{max-width:1500px;margin:auto;padding:26px}h1{font-size:27px;margin:0 0 8px}p{line-height:1.5}
.muted{color:#abb8c9}.warn{border-left:4px solid #e6b659;padding:10px 15px;background:#29241a}
.controls{display:flex;gap:18px;flex-wrap:wrap;align-items:end;padding:20px 0}label{display:grid;gap:7px}
select,button{background:#243040;color:#fff;border:1px solid #526276;border-radius:6px;padding:10px;font:inherit}
button{cursor:pointer}input{accent-color:#68c9ed;width:200px}.frame{overflow:auto;background:#080a0e;border:1px solid #455267;border-radius:8px}
canvas{display:block;max-width:100%;height:auto;margin:auto}#caption{font-weight:600;margin:12px 0}
.stats{display:flex;gap:24px;flex-wrap:wrap;padding:14px 0}a{color:#8ad4f4}details{margin-top:22px}
.links{display:flex;gap:18px;flex-wrap:wrap}#error{color:#ff8e8e}code{word-break:break-all}
</style>
<main><h1>DAPA · real vs reprojection</h1>
<p class="muted">A local, full-resolution capture of each eye. The interactive preview is resized to at most 1600 pixels wide; the linked PNGs retain the captured resolution.</p>
<p class="warn">These frames have different timestamps. A visible difference is not automatically a prediction error.
This is before runtime timewarp and Virtual Desktop streaming—not exactly what the headset displayed. Capture work can disturb timing.</p>
<div id="timing" class="stats"></div>
<div id="movement" class="warn"></div>
<div class="controls">
<label>Eye<select id="eye"><option value="left">Left eye</option><option value="right">Right eye</option></select></label>
<label>Compare prediction against<select id="base"><option value="real">Original real frame (earlier)</option><option value="next">Next real frame (later)</option></select></label>
<label>View<select id="mode"><option value="overlay">50/50 overlay</option><option value="edges">Cyan real / magenta DAPA edges</option>
<option value="split">Side by side</option><option value="real">Real only</option><option value="prediction">DAPA only (clean)</option>
<option value="difference">Absolute difference ×4</option><option value="mask">Prediction confidence / rejection</option>
<option value="depth">Source depth (logarithmic)</option></select></label>
<label>DAPA opacity <span id="mixLabel">50%</span><input id="mix" type="range" min="0" max="100" value="50"></label>
<button id="save">Save this preview PNG</button></div>
<div id="caption"></div><div id="error" role="alert"></div>
<div class="frame"><canvas id="view" width="800" height="450" aria-label="Frame comparison"></canvas></div>
<p id="legend"></p><div id="links" class="links"></div>
<details><summary>Capture metadata and interpretation</summary><p>For source-depth-boundary-v2, green means a source lookup was solved; red can mean an unresolved lookup filled from background, or inactive warping. For legacy captures, green means correction was accepted and red means little or none was accepted. When the warp itself was inactive, this is not evidence of a depth rejection.
The depth image is reconstructed from the captured projection: white is near, black is far. Raw device-depth float files are included for analysis.
Animations, occlusions and different frame times prevent the next real frame from being an exact ground-truth image for the intermediate time.</p>
<pre id="metadata" style="white-space:pre-wrap"></pre></details></main>
<script src="metadata.js"></script><script src="preview-images.js"></script><script>
'use strict';
const el=id=>document.getElementById(id),meta=window.captureMetadata,canvas=el('view'),ctx=canvas.getContext('2d');
let loaded={},token=0;
if(!meta)el('error').textContent='Metadata missing: keep this HTML beside the PNGs and metadata.js.';
else {
el('timing').textContent='Real → DAPA: '+meta.horizonMs.toFixed(2)+' ms · DAPA → next real: '+meta.nextAfterPredictionMs.toFixed(2)+' ms · XR submission: '+(meta.submissionKnown?meta.submissionResult:'not recorded');
el('metadata').textContent=JSON.stringify(meta,null,2);
}
function load(url){return new Promise((resolve,reject)=>{const image=new Image();image.onload=()=>resolve(image);image.onerror=()=>reject(new Error('Cannot load '+url));image.src=url;});}
async function refresh(){
const current=++token,e=el('eye').value;el('error').textContent='';
try {const kinds=['real','prediction','next','diagnostic','depth'];const images=await Promise.all(kinds.map(k=>load(window.captureImages?.[k+'-'+e]||k+'-'+e+'.png')));
if(current!==token)return;loaded=Object.fromEntries(kinds.map((k,i)=>[k,images[i]]));draw();
el('links').replaceChildren();for(const k of [...kinds,'overlay-original','overlay-next','difference-original']){
const a=document.createElement('a');a.href=k+'-'+e+'.png';a.textContent=k+' PNG';a.target='_blank';el('links').append(a);}
}catch(error){el('error').textContent=error.message;}
}
function pixels(image,w,h){const c=document.createElement('canvas');c.width=w;c.height=h;const x=c.getContext('2d',{willReadFrequently:true});x.drawImage(image,0,0,w,h);return x.getImageData(0,0,w,h);}
function draw(){
if(!loaded.real)return;const mode=el('mode').value,base=el('base').value,eye=el('eye').value;
const a=loaded[base],b=loaded.prediction,w=Math.min(1600,a.width),h=Math.round(a.height*w/a.width);
const measured=base==='next'?meta.nextMovement:meta.movement,ei=eye==='left'?0:1;
if(!measured?.velocityValid)el('movement').textContent='Measured player speed unavailable for this frame. Do not infer standing still from an inactive prediction.';
else {
 const v=measured.eyeVelocityRightUpForward?.[ei],valid=measured.eyeVelocityValid?.[ei];
 let text=(measured.playerStationary?'PLAYER STATIONARY — ':'MEASURED PLAYER SPEED: ')+measured.speedWorldUnitsPerSecond.toFixed(1)+' Skyrim units/s (wall clock). ';
 if(valid && !measured.playerStationary)text+='Sideways: '+(v[0]<0?'LEFT ':'RIGHT ')+Math.abs(v[0]).toFixed(1)+' · Forward/back: '+(v[2]<0?'BACKWARD ':'FORWARD ')+Math.abs(v[2]).toFixed(1)+' units/s. ';
 else if(!valid)text+='View-relative direction unavailable. ';
 text+='Measured over '+(measured.cpuIntervalSeconds*1000).toFixed(1)+' ms, independently of DAPA confidence. ';
 const input=measured.physicalStickInput;
 if(input?.fresh)text+='Physical sticks L(x,y) / R(x,y): '+input.leftX_leftY_rightX_rightY.map((n,i)=>input.active[i]?n.toFixed(2):'unavailable').join(', ')+'. ';
 else text+='Stick input unavailable/stale. ';
 const c=meta.eyes[ei].warpConstants;
 text+='DAPA applied translation (view units): ['+[c[3],c[7],c[11]].map(n=>n.toFixed(3)).join(', ')+']. This is not measured NPC motion.';
 el('movement').textContent=text;
}
const poseEye=meta.eyes[ei],pa=poseEye.cachedPose,pb=base==='next'?poseEye.nextPose:poseEye.targetPose;
if(pa?.length===7 && pb?.length===7){
 const mm=Math.hypot(pb[4]-pa[4],pb[5]-pa[5],pb[6]-pa[6])*1000;
 const normA=Math.hypot(...pa.slice(0,4)),normB=Math.hypot(...pb.slice(0,4));
 const dot=pa.slice(0,4).reduce((sum,n,i)=>sum+n*pb[i],0)/(normA*normB);
 const degrees=2*Math.acos(Math.min(1,Math.abs(dot)))*180/Math.PI;
 el('movement').textContent+=' Tracked eye-pose change (source → '+(base==='next'?'next real':'predicted slot')+'): '+mm.toFixed(2)+' mm / '+degrees.toFixed(2)+'°. Player stationary does not mean head or NPC stationary.';
}
canvas.width=mode==='split'?w*2:w;canvas.height=h;const mix=Number(el('mix').value)/100;
el('mixLabel').textContent=Math.round(mix*100)+'%';const native=meta.eyes[eye==='left'?0:1];el('caption').textContent=eye.toUpperCase()+' EYE · '+(base==='real'?'original real':'next real')+' / DAPA · captured '+native.width+' × '+native.height;
el('legend').textContent=mode==='edges'?'Cyan = selected real-frame edges. Magenta = DAPA edges. White = overlap. Outlines are an inspection aid, not object segmentation.':
mode==='mask'?(meta.eyes[ei].warpConstants[65]>=2?'Green = source lookup solved or player pixel protected. Red = unresolved/background fill or inactive; red does not mean the pixel stayed in its old position.':'Green = accepted correction. Red = rejected or inactive. This mask belongs to the original → DAPA warp.'):
mode==='depth'?'Source depth only. Logarithmic view-depth preview; see raw .f32 files for exact device-depth samples.':
'Use Real only and DAPA only to identify the actual NPC/building. The opacity slider controls the overlay.';
if(mode==='split'){ctx.drawImage(a,0,0,w,h);ctx.drawImage(b,w,0,w,h);return;}
if(mode==='real'||mode==='prediction'||mode==='depth'){ctx.drawImage(mode==='real'?a:mode==='depth'?loaded.depth:b,0,0,w,h);return;}
if(mode==='overlay'){ctx.drawImage(a,0,0,w,h);ctx.globalAlpha=mix;ctx.drawImage(b,0,0,w,h);ctx.globalAlpha=1;return;}
const ad=pixels(a,w,h),bd=pixels(b,w,h),result=ctx.createImageData(w,h);
if(mode==='mask'){
const mask=pixels(loaded.diagnostic,w,h),active=meta.eyes[eye==='left'?0:1].warpConstants[64]>0.5;
if(!active)el('legend').textContent='Warp inactive for this eye: red here does NOT establish that the depth checks rejected movement.';
for(let i=0;i<result.data.length;i+=4){const confidence=mask.data[i]/255;result.data[i]=255*(1-confidence);result.data[i+1]=220*confidence;result.data[i+2]=35;result.data[i+3]=255;}
}else if(mode==='difference'){
for(let i=0;i<result.data.length;i+=4){for(let c=0;c<3;c++)result.data[i+c]=Math.min(255,4*Math.abs(ad.data[i+c]-bd.data[i+c]));result.data[i+3]=255;}
}else{
const lum=(d,i)=>(d[i]*0.2126+d[i+1]*0.7152+d[i+2]*0.0722);
for(let y=0;y<h;y++)for(let x=0;x<w;x++){const i=(y*w+x)*4;let ea=0,eb=0;
if(x>0&&x<w-1&&y>0&&y<h-1){
ea=Math.min(1,(Math.abs(lum(ad.data,i+4)-lum(ad.data,i-4))+Math.abs(lum(ad.data,i+w*4)-lum(ad.data,i-w*4)))/65);
eb=Math.min(1,(Math.abs(lum(bd.data,i+4)-lum(bd.data,i-4))+Math.abs(lum(bd.data,i+w*4)-lum(bd.data,i-w*4)))/65);}
const bg=lum(ad.data,i)*0.18;result.data[i]=bg+235*eb;result.data[i+1]=bg+235*ea;result.data[i+2]=bg+235*Math.max(ea,eb);result.data[i+3]=255;}
}
ctx.putImageData(result,0,0);
}
el('eye').onchange=refresh;for(const id of ['base','mode','mix'])el(id).oninput=draw;
el('save').onclick=()=>{const a=document.createElement('a');a.download='dapa-'+el('eye').value+'-'+el('mode').value+'.png';a.href=canvas.toDataURL('image/png');a.click();};
refresh();
</script></html>)HTML";
