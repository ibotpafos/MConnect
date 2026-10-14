/* No DAW is launched: software tone source and real browser/WebRTC only. */
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {spawn}=require('node:child_process');
const {chromium}=require(process.env.MCONNECT_PLAYWRIGHT || 'playwright');
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
// TSan can be 5–15x slower than production and miss real-time deadlines. Its
// profile verifies concurrent lifecycle/race safety; release and ASan profiles
// retain the decoded stereo checks without changing their thresholds.
const raceOnly=process.env.MCONNECT_TEST_RACE_ONLY==='1';
const temp=fs.mkdtempSync('/private/tmp/mconnect-robust-'),infoPath=path.join(temp,'info.json'),control=path.join(temp,'control.json');
const host=process.env.MCONNECT_TEST_HOST || path.resolve('build/MConnectHarness_artefacts/Release/MConnect test host.app/Contents/MacOS/MConnect test host');
const hostLog=process.env.MCONNECT_TEST_HOST_LOG?fs.openSync(process.env.MCONNECT_TEST_HOST_LOG,'w'):null;
const source=spawn(host,['--no-editor','--seconds','180','--block','1024','--info',infoPath,'--control',control],{stdio:['ignore',hostLog??'ignore',hostLog??'ignore']});
const exit=new Promise(resolve=>source.once('exit',code=>resolve(code)));
let browser,stage='startup';
async function command(data){const pending=control+'.pending';fs.writeFileSync(pending,JSON.stringify(data));fs.renameSync(pending,control);await until(()=>!fs.existsSync(control),15000);await sleep(400);}
async function until(check,ms=20000){const deadline=Date.now()+ms;while(Date.now()<deadline){if(await check())return;if(source.exitCode!==null||source.signalCode!==null)throw new Error('source exited at '+stage);await sleep(100);}throw new Error('Timed out: '+stage);}
(async()=>{
 try {
  await until(()=>fs.existsSync(infoPath));
  const info=JSON.parse(fs.readFileSync(infoPath)),url=new URL(info.url);url.hostname='127.0.0.1';
  const launch={headless:true,args:['--autoplay-policy=no-user-gesture-required']};
  if(process.platform==='darwin'&&process.env.MCONNECT_TEST_TIMING_LIBRARY)launch.env={...process.env,DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY,__XPC_DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY};
  browser=await chromium.launch(launch);
  const context=await browser.newContext(),errors=[];
  const nativeInfo=async()=>{const r=await context.request.get(url.origin+'/info',{headers:{'X-MConnect-Token':url.hash.slice(1)}});return r.json();};
  async function open(){
   const p=await context.newPage();p.on('pageerror',e=>errors.push(e.message));
   await p.addInitScript(()=>{
    window.testSockets=[];const W=window.WebSocket,P=window.RTCPeerConnection;
    window.WebSocket=class extends W{constructor(url){super(url);window.testSockets.push(this);}};
    window.RTCPeerConnection=class extends P{constructor(c){super(c);window.testPeer=this;}};
   });await p.goto(url.href);return p;
  }
  stage='four listeners';const pages=await Promise.all(Array.from({length:4},open));
  await until(async()=>(await nativeInfo()).listeners===4);
  stage='fifth receiver slot';const fifth=await open();await sleep(1200);assert.equal((await nativeInfo()).listeners,4);
  await pages.pop().close();await until(async()=>(await nativeInfo()).listeners===4);
  await until(()=>fifth.evaluate(()=>document.querySelector('#listen').hidden));
  await fifth.close();for(const p of pages.slice(1))await p.close();
  stage='single remaining listener';const p=pages[0];await until(async()=>(await nativeInfo()).listeners===1);
  // Low reported jitter must not hide packet loss from the buffer controller.
  // Override only telemetry; RTP and the actual browser receiver remain live.
  stage='buffer reacts to low-jitter packet loss';
  await p.evaluate(()=>{
   const peer=window.testPeer;window.bufferOriginalStats=peer.getStats.bind(peer);window.bufferProbeMode='loss';let tick=0,lost=0,concealed=0;
   peer.getStats=async()=>{tick++;if(window.bufferProbeMode==='loss')lost+=4;if(window.bufferProbeMode==='conceal')concealed+=1920;return new Map([['buffer-regression',{type:'inbound-rtp',kind:'audio',id:'buffer-regression',packetsReceived:tick*200,packetsLost:lost,jitter:0,concealedSamples:concealed,totalSamplesReceived:tick*96000}]]);};
  });
  const bufferTarget=()=>p.evaluate(()=>{const receiver=window.testPeer.getReceivers()[0];return receiver.jitterBufferTarget??receiver.playoutDelayHint*1000;});
  await until(async()=>(await bufferTarget())>=50,12000);
  stage='buffer reacts to concealment without packet loss';const lossTarget=await bufferTarget();
  await p.evaluate(()=>{window.bufferProbeMode='conceal';});
  await until(async()=>(await bufferTarget())>=lossTarget+20,8000);
  stage='buffer falls after healthy recovery';const raisedTarget=await bufferTarget();
  await p.evaluate(()=>{window.bufferProbeMode='healthy';});
  await until(async()=>(await bufferTarget())<raisedTarget,14000);
  await p.evaluate(()=>{window.testPeer.getStats=window.bufferOriginalStats;delete window.bufferOriginalStats;delete window.bufferProbeMode;});
  // A page-cache restore must reconnect without a button press.
  stage='page cache restore';const before=await p.evaluate(()=>window.testSockets.length);
  await p.evaluate(()=>{window.dispatchEvent(new PageTransitionEvent('pagehide',{persisted:true}));window.dispatchEvent(new PageTransitionEvent('pageshow',{persisted:true}));});
  await until(()=>p.evaluate(n=>window.testSockets.length>n&&document.querySelector('#listen').hidden,before));
  // A stale queued pause event must not turn off a currently playing receiver.
  await p.evaluate(()=>document.querySelector('audio').dispatchEvent(new Event('pause')));
  await sleep(200);assert.equal((await nativeInfo()).listeners,1);
  // Longer interruption than the old three-attempt reconnect budget.
  stage='long outage recovery';await command({stream:false});await sleep(8000);await command({stream:true});
  await until(()=>p.evaluate(()=>document.querySelector('#listen').hidden&&!document.querySelector('audio').paused),30000);
  await until(async()=>(await nativeInfo()).listeners===1);
  for(const rate of [192000,44100,48000]){
   stage='rate '+rate;await command({rate,block:2048});await until(async()=>(await nativeInfo()).sampleRate===rate);
   await sleep(4000);
   if(raceOnly)continue;
   const measured=await p.evaluate(async()=>{
    const c=new AudioContext({sampleRate:48000});await c.resume();
    const s=c.createMediaStreamSource(document.querySelector('audio').srcObject),split=c.createChannelSplitter(2),quiet=c.createGain();quiet.gain.value=1e-6;
    const a=[c.createAnalyser(),c.createAnalyser()];s.connect(split);
    a.forEach((v,n)=>{v.fftSize=2048;split.connect(v,n);v.connect(quiet);});quiet.connect(c.destination);
    await new Promise(r=>setTimeout(r,500));
    const amp=(v,f)=>{let x=0,y=0;for(let n=0;n<v.length;n++){const k=2*Math.PI*f*n/c.sampleRate;x+=v[n]*Math.cos(k);y+=v[n]*Math.sin(k);}return 2*Math.hypot(x,y)/v.length;};
    const result=a.map(v=>{const d=new Float32Array(v.fftSize);v.getFloatTimeDomainData(d);return [amp(d,997),amp(d,1499)];});await c.close();return result;
   });if(!(measured[0][0]>.12&&measured[1][1]>.12))console.error(JSON.stringify({rate,measured,sender:await nativeInfo(),inbound:await p.evaluate(async()=>{let r; (await window.testPeer.getStats()).forEach(v=>{if(v.type==='inbound-rtp')r=v;});return r;})}));
   assert.ok(measured[0][0]>.12&&measured[1][1]>.12,'rate change lost decoded stereo');
   assert.ok(measured[0][1]<.02&&measured[1][0]<.02,'rate change mixed stereo channels');
  }
  await command({pause:true});await sleep(2000);
  const stats=()=>p.evaluate(async()=>{let v; (await window.testPeer.getStats()).forEach(r=>{if(r.type==='inbound-rtp'&&r.kind==='audio')v={packets:r.packetsReceived,energy:r.totalAudioEnergy};});return v;});
  const a=await stats();await sleep(1000);const b=await stats();
  assert.ok(b.packets-a.packets>50,'DAW idle stopped media keepalive');assert.ok(b.energy-a.energy<.01,'DAW idle transmitted stale audio');
  await command({pause:false});await sleep(1200);
  assert.equal((await nativeInfo()).encodeErrors,0);assert.equal((await nativeInfo()).conversionErrors,0);
  // Quit normally with live receiver callbacks, rather than killing the process.
  stage='graceful active shutdown';
  await command({quit:true});assert.equal(await Promise.race([exit,sleep(6000).then(()=>{throw new Error('shutdown hung');})]),0);
  assert.deepEqual(errors,[]);
  const result={result:'PASS',decodedAudioChecked:!raceOnly,fourReceivers:true,fifthRecoversWhenSlotFreed:true,bufferRespondsToLowJitterLoss:true,bufferRespondsToConcealment:true,bufferFallsAfterRecovery:true,pageCacheRestore:true,stalePauseIgnored:true,longOutageRecovery:true,liveRateChanges:[192000,44100,48000],largeBlockFrames:2048,idleSilence:true,activeShutdown:true};
  fs.writeFileSync('test-results/robustness.json',JSON.stringify(result,null,2));console.log(JSON.stringify(result));
 }finally{if(browser)await browser.close();if(source.exitCode===null){source.kill('SIGTERM');await Promise.race([exit,sleep(5000)]);if(source.exitCode===null)source.kill('SIGKILL');}if(hostLog!==null)fs.closeSync(hostLog);fs.rmSync(temp,{recursive:true,force:true});}
})().catch(e=>{console.error('FAIL:',stage,e.message.replace(/X-MConnect-Token: [a-f0-9]+/gi,'X-MConnect-Token: [redacted]'));process.exitCode=1;});
