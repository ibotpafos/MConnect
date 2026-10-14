/* Sustained decoded-audio/recovery test. Runs only the software source, never a DAW. */
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const {spawn, execFileSync} = require('node:child_process');
const {chromium, webkit} = require(process.env.MCONNECT_PLAYWRIGHT || 'playwright');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const duration = Number(process.env.MCONNECT_SOAK_SECONDS || 600);
assert.ok(Number.isFinite(duration) && duration >= 60 && duration <= 3600);
const host = process.env.MCONNECT_TEST_HOST || path.resolve('build/MConnectHarness_artefacts/Release/MConnect test host.app/Contents/MacOS/MConnect test host');
const temp = fs.mkdtempSync('/private/tmp/mconnect-soak-');
const infoPath = path.join(temp, 'info.json'), control = path.join(temp, 'control.json');
const sourceLog = fs.openSync(path.join(temp, 'source.log'), 'w');
const source = spawn(host, ['--no-editor', '--seconds', String(duration + 180), '--info', infoPath, '--control', control], {stdio:['ignore', sourceLog, sourceLog]});
const sourceExit = new Promise(resolve => source.once('exit', (code, signal) => resolve({code, signal})));
let stage = 'startup';
const browsers = [], contexts = [], clients = [], errors = [], samples = [], events = [];
async function until(check, milliseconds = 30000) {
  const deadline = Date.now() + milliseconds;
  while (Date.now() < deadline) { if (await check()) return; if (source.exitCode !== null) throw new Error('source exited at ' + stage); await sleep(100); }
  throw new Error('Timed out at ' + stage);
}
async function command(value) {
  const pending = control + '.pending'; fs.writeFileSync(pending, JSON.stringify(value)); fs.renameSync(pending, control);
  await until(() => !fs.existsSync(control), 15000);
}
function rss() { return Number(execFileSync('ps', ['-p', String(source.pid), '-o', 'rss='], {encoding:'utf8'}).trim()) / 1024; }
function installProbe() {
  window.soakSockets = [];
  const W = WebSocket, P = RTCPeerConnection, play = HTMLMediaElement.prototype.play;
  window.WebSocket = class extends W { constructor(url) { super(url); window.soakSockets.push(this); } };
  window.RTCPeerConnection = class extends P { constructor(config) { super(config); window.soakPeer = this; } };
  let blocked = false;
  HTMLMediaElement.prototype.play = function() {
    if (this.srcObject && !blocked) { blocked = true; return Promise.reject(new DOMException('Activation fixture', 'NotAllowedError')); }
    return play.call(this);
  };
  document.addEventListener('click', event => {
    if (!event.target.closest('#listen') || window.soakAudio || !document.querySelector('audio')?.srcObject) return;
    const ctx = new AudioContext({sampleRate:48000}), splitter = ctx.createChannelSplitter(2), quiet = ctx.createGain();
    const input = ctx.createMediaStreamSource(document.querySelector('audio').srcObject);
    const analysers = [ctx.createAnalyser(), ctx.createAnalyser()];
    quiet.gain.value = 1e-6; input.connect(splitter);
    analysers.forEach((analyser, channel) => { analyser.fftSize = 2048; splitter.connect(analyser, channel); analyser.connect(quiet); });
    quiet.connect(ctx.destination); window.soakAudio = {ctx, input, splitter, analysers}; ctx.resume();
  }, true);
}
async function decoded(page) {
  return page.evaluate(async () => {
    const graph = window.soakAudio, stream = document.querySelector('audio').srcObject;
    if (!graph || graph.ctx.state !== 'running' || !stream) return {active:false};
    if (graph.input.mediaStream !== stream) { graph.input.disconnect(); graph.input = graph.ctx.createMediaStreamSource(stream); graph.input.connect(graph.splitter); }
    await new Promise(resolve => setTimeout(resolve, 500));
    const amplitude = (data, frequency) => {
      let real=0, imaginary=0;
      data.forEach((value, index) => { const angle=2*Math.PI*frequency*index/graph.ctx.sampleRate; real+=value*Math.cos(angle); imaginary+=value*Math.sin(angle); });
      return 2*Math.hypot(real, imaginary)/data.length;
    };
    // A single 43 ms correlation can straddle a concealed/time-adjusted frame.
    // Keep the original tone thresholds, check three independent windows and
    // require sustained energy in every window. Retain all measurements so a
    // transient dip is visible in the report, rather than silently retrying it.
    const windows=[];
    for(let index=0;index<3;index++) {
      windows.push(graph.analysers.map(analyser => {
        const data=new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(data);
        return {left:amplitude(data,997), right:amplitude(data,1499), rms:Math.sqrt(data.reduce((sum,value)=>sum+value*value,0)/data.length)};
      }));
      if(index<2) await new Promise(resolve=>setTimeout(resolve,200));
    }
    let inbound,route;
    const reports=await window.soakPeer.getStats();
    reports.forEach(report => {
      if(report.type==='inbound-rtp'&&(report.kind||report.mediaType)==='audio') inbound={packets:report.packetsReceived, loss:report.packetsLost, energy:report.totalAudioEnergy, concealed:report.concealedSamples, inserted:report.insertedSamplesForDeceleration, removed:report.removedSamplesForAcceleration};
      if(report.type==='transport'&&report.selectedCandidatePairId){const pair=reports.get(report.selectedCandidatePairId),remote=reports.get(pair.remoteCandidateId);route={remoteAddress:remote.address||remote.ip,roundTripMs:pair.currentRoundTripTime*1000};}
    });
    const receiver=window.soakPeer.getReceivers()[0];
    return {active:!document.querySelector('audio').paused, windows, inbound, route, bufferTargetMs:receiver.jitterBufferTarget??receiver.playoutDelayHint*1000, contextTime:graph.ctx.currentTime};
  });
}
function checkStereo(value, label, expectedAddress) {
  assert.ok(value.active && value.windows?.length===3, label + ': audio stopped');
  assert.ok(value.windows.every(channels=>channels.every(channel=>channel.rms>.1)),label+': decoded audio became silent');
  assert.ok(value.windows.filter(channels=>channels[0].left>.12&&channels[1].right>.12).length>=2,label+': decoded stereo missing');
  assert.ok(value.windows.every(channels=>channels[0].right<.02&&channels[1].left<.02),label+': stereo mixed');
  assert.ok(value.inbound?.packets > 20, label + ': no RTP packets');
  if(value.route?.remoteAddress)assert.equal(value.route.remoteAddress,expectedAddress,label+': media left the selected LAN interface');
}
(async () => {
  try {
    await until(() => fs.existsSync(infoPath));
    const info = JSON.parse(fs.readFileSync(infoPath)), url = new URL(info.url),lanAddress=url.hostname; url.hostname='127.0.0.1';
    const launch = {headless:true};
    if (process.platform==='darwin' && process.env.MCONNECT_TEST_TIMING_LIBRARY)
      launch.env={...process.env,DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY,__XPC_DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY};
    for (const engine of [chromium, webkit]) browsers.push(await engine.launch(launch));
    for (const browser of browsers) contexts.push(await browser.newContext({viewport:{width:390,height:844},locale:'ru-RU'}));
    const nativeInfo = async () => {
      const response=await contexts[0].request.get(url.origin+'/info',{headers:{'X-MConnect-Token':url.hash.slice(1)}});
      assert.equal(response.status(),200); return response.json();
    };
    async function openClient(index) {
      const page=await contexts[index===3?1:0].newPage(); page.on('pageerror',error=>errors.push(error.message));
      await page.addInitScript(installProbe); await page.goto(url.href);
      await page.waitForFunction(()=>document.querySelector('audio').srcObject&&!document.querySelector('#listen').disabled,{},{timeout:30000});
      await page.locator('#listen').click();
      await page.waitForFunction(()=>window.soakAudio?.ctx.state==='running'&&document.querySelector('#listen').hidden,{},{timeout:30000});
      return page;
    }
    stage='connect four browsers'; for(let index=0;index<4;index++) clients.push(await openClient(index));
    await until(async()=>(await nativeInfo()).listeners===4);
    await sleep(5000);
    const warmRss=rss(), start=Date.now();
    const rates=[48000,44100,192000,96000,32000];
    let iteration=0, reconnects=0, outages=0, swaps=0;
    while ((Date.now()-start)/1000 < duration) {
      stage='sample '+iteration;
      const measured=await Promise.all(clients.map(decoded));
      if (measured.some(value=>!value.active||!value.windows||value.windows.some(channels=>channels[0].left<=.12||channels[1].right<=.12)))
        console.error(JSON.stringify({stage,measured,sender:await nativeInfo()}));
      measured.forEach((value,index)=>checkStereo(value, 'client '+index,lanAddress));
      const sender=await nativeInfo();
      assert.equal(sender.listeners,4); assert.equal(sender.encodeErrors,0); assert.equal(sender.conversionErrors,0);
      assert.ok(sender.senderQueueMs <= Math.max(50, sender.sourceBlockMs*2+20), 'unbounded sender queue');
      samples.push({elapsedSeconds:(Date.now()-start)/1000,rssMiB:rss(),sampleRate:sender.sampleRate,senderQueueMs:sender.senderQueueMs,receivers:measured});
      console.log(JSON.stringify({stage,elapsed:Math.round((Date.now()-start)/1000),listeners:sender.listeners,rate:sender.sampleRate,rssMiB:samples.at(-1).rssMiB}));
      if (iteration>0 && iteration%3===0) {
        stage='automatic signaling recovery'; const index=reconnects++%4, page=clients[index];
        const before=await page.evaluate(()=>window.soakSockets.length);
        await page.evaluate(()=>window.soakSockets.at(-1).close());
        await until(()=>page.evaluate(count=>window.soakSockets.length>count&&document.querySelector('#listen').hidden&&!document.querySelector('audio').paused,before));
        await until(async()=>(await nativeInfo()).listeners===4); events.push({type:'signaling',client:index}); await sleep(2000);
      }
      if (iteration>0 && iteration%5===0) {
        stage='eight-second server outage'; await command({stream:false}); await sleep(8000); await command({stream:true});
        await until(async()=>(await nativeInfo()).listeners===4,45000); outages++; events.push({type:'outage',seconds:8}); await sleep(5000);
      }
      if (iteration>0 && iteration%4===0) {
        stage='source rate change'; const rate=rates[(iteration/4)%rates.length];
        await command({rate,block:iteration%8===0?2048:256});
        await until(async()=>(await nativeInfo()).sampleRate===rate); events.push({type:'rate',rate}); await sleep(5000);
      }
      if (iteration>0 && iteration%7===0) {
        stage='receiver replacement'; const index=swaps++%4; await clients[index].close(); clients[index]=await openClient(index);
        await until(async()=>(await nativeInfo()).listeners===4); events.push({type:'replacement',client:index}); await sleep(3000);
      }
      iteration++; await sleep(15000);
    }
    const final=await Promise.all(clients.map(decoded)); final.forEach((value,index)=>checkStereo(value,'final client '+index,lanAddress));
    const finalRss=rss(); assert.ok(finalRss-warmRss<32, 'test-host memory grew by more than 32 MiB after warm-up');
    assert.deepEqual(errors,[]);
    stage='graceful active shutdown'; await command({quit:true});
    const exit=await Promise.race([sourceExit,sleep(10000).then(()=>{throw new Error('source shutdown hung');})]); assert.equal(exit.code,0);
    const transientToneWindows=samples.reduce((count,sample)=>count+sample.receivers.reduce((sum,receiver)=>sum+receiver.windows.filter(channels=>channels[0].left<=.12||channels[1].right<=.12).length,0),0);
    const result={result:'PASS',durationSeconds:(Date.now()-start)/1000,clients:4,engines:['chromium','webkit'],snapshots:samples.length,transientToneWindows,reconnects,outages,replacements:swaps,warmRssMiB:warmRss,finalRssMiB:finalRss,events,samples};
    fs.mkdirSync('test-results',{recursive:true}); fs.writeFileSync('test-results/soak.json',JSON.stringify(result,null,2)); console.log(JSON.stringify({...result,events:undefined,samples:undefined}));
  } finally {
    for (const context of contexts) await context.close().catch(()=>{});
    for (const browser of browsers) await browser.close().catch(()=>{});
    if (source.exitCode===null) { source.kill('SIGTERM'); await Promise.race([sourceExit,sleep(5000)]); if(source.exitCode===null)source.kill('SIGKILL'); }
    fs.closeSync(sourceLog);
    const text=fs.readFileSync(path.join(temp,'source.log'),'utf8');
    if (text) { fs.mkdirSync('test-results',{recursive:true}); fs.writeFileSync('test-results/soak-source.log',text); }
    fs.rmSync(temp,{recursive:true,force:true});
  }
})().catch(error=>{console.error('FAIL:',stage,error.message);process.exitCode=1;});
