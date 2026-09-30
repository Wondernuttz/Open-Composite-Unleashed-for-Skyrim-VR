// Execute the actual report script with a minimal DOM for text/selection tests.
// This is not a browser-rendering or headset test.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('DrvOpenXR/DapaCaptureViewer.h','utf8');
const script=[...source.matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1].replace(/\nrefresh\(\);\s*$/,'');
const metadata=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
const elements={};
const document={getElementById(id){return elements[id]??= {textContent:'',value:({eye:'left',base:'real',mode:'real',mix:'50'})[id],getContext(){return {drawImage(){}}}};}};
const context=vm.createContext({window:{captureMetadata:metadata},document,console});
vm.runInContext(script,context);
vm.runInContext('loaded={real:{width:640,height:360},prediction:{width:640,height:360},next:{width:640,height:360}};draw();',context);
assert.match(elements.movement.textContent,/BACKWARD 100.0/);
assert.match(elements.movement.textContent,/Tracked eye-pose change/);
elements.base.value='next';vm.runInContext('draw()',context);
assert.match(elements.movement.textContent,/PLAYER STATIONARY/);
elements.eye.value='right';vm.runInContext('draw()',context);
assert.match(elements.movement.textContent,/PLAYER STATIONARY/);
metadata.nextMovement.velocityValid=false;vm.runInContext('draw()',context);
assert.match(elements.movement.textContent,/unavailable/);
assert.doesNotMatch(elements.movement.textContent,/PLAYER STATIONARY/);
console.log('PASS: actual viewer JS, backward speed, stationary player, separate tracked-eye movement, both eyes, unavailable data.');
