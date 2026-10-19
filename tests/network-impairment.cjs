/* Real UDP/SRTP impairment fixture; never launches a DAW or changes OS networking.
 * Pass info.json from a running software test host. On macOS apply the existing
 * MConnectBrowserTiming fixture to BOTH host and headless browser.
 * Defaults: healthy -> 10% real RTP loss -> healthy, ten seconds per phase.
 * MCONNECT_PROBE_JITTER=1 instead delays every fifth RTP datagram by 350 ms.
 * MCONNECT_PROBE_BUFFER changes the browser hint only. MCONNECT_PROBE_NACK=1
 * munges BOTH SDP descriptions as a diagnostic experiment, not a plugin fix.
 */
const fs=require('node:fs'),dgram=require('node:dgram'),assert=require('node:assert/strict');
const {chromium}=require(process.env.MCONNECT_PLAYWRIGHT);
const info=JSON.parse(fs.readFileSync(process.argv[2]));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
(async()=>{
 const address=new URL(info.url).hostname;
 const towardBrowser=dgram.createSocket('udp4'),towardSender=dgram.createSocket('udp4');
 await Promise.all([new Promise(r=>towardBrowser.bind(0,address,r)),new Promise(r=>towardSender.bind(0,address,r))]);
 let senderEndpoint=null,browserEndpoint=null,dropPercent=0,seen=0,dropped=0,recovered=0,seed=1234567;const missing=new Set(),pending=new Set();const delayMode=process.env.MCONNECT_PROBE_JITTER==='1';let delayEnabled=false,delayed=0;
 const media=packet=>packet.length>12&&(packet[0]&0xc0)===0x80&&!(packet[1]>=192&&packet[1]<=223);
 towardBrowser.on('message',(message,remote)=>{
  if(remote.port===towardBrowser.address().port||remote.port===towardSender.address().port)return;
  browserEndpoint=remote;
  if(senderEndpoint)towardSender.send(message,senderEndpoint.port,senderEndpoint.address);
 });
 towardSender.on('message',(message,remote)=>{
  if(remote.port===towardBrowser.address().port||remote.port===towardSender.address().port)return;
  senderEndpoint=remote;
  if(!browserEndpoint)return;
  if(media(message)){
   ++seen;const sequence=message.readUInt16BE(2);seed=(Math.imul(seed,1664525)+1013904223)>>>0;
   if(seed/4294967296*100<dropPercent){++dropped;missing.add(sequence);return;}
   if(missing.delete(sequence))++recovered;
  }
  if(delayEnabled&&media(message)&&seen%5===0){
   ++delayed;const target={...browserEndpoint};const timer=setTimeout(()=>{pending.delete(timer);towardBrowser.send(message,target.port,target.address);},350);pending.add(timer);
  }else towardBrowser.send(message,browserEndpoint.port,browserEndpoint.address);
 });
 const browser=await chromium.launch({headless:true,args:['--autoplay-policy=no-user-gesture-required'],env:{...process.env,DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY,__XPC_DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY}});
 try{
  const context=await browser.newContext();const page=await context.newPage();
  await page.exposeFunction('nativeCandidate',candidate=>{
   const parts=candidate.replace(/^a=/,'').trim().split(/\s+/);
   if(!senderEndpoint&&parts[2]?.toLowerCase()==='udp'&&parts[4]===address&&Number(parts[5])!==towardBrowser.address().port&&Number(parts[5])!==towardSender.address().port)senderEndpoint={address:parts[4],port:Number(parts[5])};
  });
  const nack=process.env.MCONNECT_PROBE_NACK==='1',floor=Number(process.env.MCONNECT_PROBE_BUFFER||0);
  await page.addInitScript(({address,toBrowser,toSender,nack,floor})=>{
   const addNack=sdp=>{
    const lines=sdp.split(/\r?\n/).map(line=>line.replace(/\r$/,''));
    const i=lines.findIndex(line=>/^a=rtpmap:\d+ opus\/48000/.test(line));if(i<0)return sdp;
    const pt=lines[i].match(/^a=rtpmap:(\d+)/)[1];
    if(!lines.includes('a=rtcp-fb:'+pt+' nack'))lines.splice(i+1,0,'a=rtcp-fb:'+pt+' nack');
    return lines.join('\r\n');
   };
   const P=RTCPeerConnection;
   window.RTCPeerConnection=class extends P{
    constructor(c){super(c);window.testPeer=this;window.testConnections??=[];this.addEventListener('connectionstatechange',()=>window.testConnections.push(this.connectionState));}
    async createOffer(...a){const d=await super.createOffer(...a);if(nack)d.sdp=addNack(d.sdp);return d;}
    setRemoteDescription(d){return super.setRemoteDescription(nack?{type:d.type,sdp:addNack(d.sdp)}:d);}
   };
   if(floor){const d=Object.getOwnPropertyDescriptor(RTCRtpReceiver.prototype,'jitterBufferTarget');Object.defineProperty(RTCRtpReceiver.prototype,'jitterBufferTarget',{...d,set(v){d.set.call(this,Math.max(floor,v));}});}
   const rewrite=(candidate,port)=>{
    const parts=candidate.replace(/^a=/,'').trim().split(/\s+/);
    if(parts[2]?.toLowerCase()!=='udp')return candidate;
    parts[4]=address;parts[5]=String(port);return parts.join(' ');
   };
   const W=WebSocket;
   window.WebSocket=class extends W{
    send(text){const d=JSON.parse(text);
     if(d.type==='webrtc-candidate')d.candidate=rewrite(d.candidate,toSender);
     if(d.type==='webrtc-offer')d.sdp=d.sdp.replace(/^a=candidate:.*$/gm,line=>'a='+rewrite(line,toSender));
     return super.send(JSON.stringify(d));
    }
    set onmessage(callback){if(!callback){super.onmessage=null;return;}let chain=Promise.resolve();super.onmessage=event=>{chain=chain.then(async()=>{
     const d=JSON.parse(event.data);
     if(d.type==='webrtc-candidate'){await window.nativeCandidate(d.candidate);d.candidate=rewrite(d.candidate,toBrowser);}
     if(d.type==='webrtc-answer'){
      for(const m of d.sdp.matchAll(/^a=candidate:.*$/gm))await window.nativeCandidate(m[0]);
      d.sdp=d.sdp.replace(/^a=candidate:.*$/gm,line=>'a='+rewrite(line,toBrowser));
     }
     callback(new MessageEvent('message',{data:JSON.stringify(d)}));
    });};}
   };
  },{address,toBrowser:towardBrowser.address().port,toSender:towardSender.address().port,nack,floor});
  const u=new URL(info.url);u.hostname='127.0.0.1';await page.goto(u.href);
  await page.waitForFunction(()=>document.querySelector('audio').srcObject&&(!document.querySelector('#listen').disabled||document.querySelector('#listen').hidden),{},{timeout:25000});
  await page.waitForFunction(()=>!document.querySelector('audio').paused);
  await page.evaluate(async()=>{
   const ctx=new AudioContext({sampleRate:48000}),input=ctx.createMediaStreamSource(document.querySelector('audio').srcObject),quiet=ctx.createGain();
   quiet.gain.value=1e-6;input.connect(quiet);quiet.connect(ctx.destination);window.testAudio={ctx,input,quiet};await ctx.resume();
  });
  const stats=()=>page.evaluate(async()=>{const all=await window.testPeer.getStats();let r;all.forEach(v=>{if(v.type==='inbound-rtp'&&(v.kind||v.mediaType)==='audio')r={received:v.packetsReceived,lost:v.packetsLost,concealed:v.concealedSamples,samples:v.totalSamplesReceived,nacks:v.nackCount,energy:v.totalAudioEnergy,buffer:v.jitterBufferEmittedCount?1000*v.jitterBufferDelay/v.jitterBufferEmittedCount:null};});return {...r,contextState:window.testAudio.ctx.state,contextTime:window.testAudio.ctx.currentTime,target:window.testPeer.getReceivers()[0].jitterBufferTarget};});
  await page.waitForFunction(async()=>{let ok=false;(await window.testPeer.getStats()).forEach(r=>{if(r.type==='inbound-rtp'&&(r.kind||r.mediaType)==='audio'&&r.packetsReceived>100&&r.totalAudioEnergy>.005)ok=true;});return ok;},{},{timeout:20000});
  await sleep(3000);const phases=[];
  for(const loss of [0,10,0]){
   dropPercent=delayMode?0:loss;delayEnabled=delayMode&&loss>0;
   const count=seen,drops=dropped,repairs=recovered,late=delayed,a=await stats();await sleep(10000);const b=await stats();
   phases.push({emulatedLossPercent:dropPercent,emulatedBurstDelayMs:delayEnabled?350:0,actuallyDelayedDatagrams:delayed-late,actualMediaDatagrams:seen-count,actualDroppedDatagrams:dropped-drops,actuallyRecoveredDatagrams:recovered-repairs,before:a,after:b,received:b.received-a.received,lost:b.lost-a.lost,concealed:b.concealed-a.concealed,concealmentPercent:100*(b.concealed-a.concealed)/Math.max(1,b.samples-a.samples),nacks:(b.nacks||0)-(a.nacks||0),audioEnergy:b.energy-a.energy});
  }

  const feedback=await page.evaluate(()=>({connections:window.testConnections,offer:window.testPeer.localDescription.sdp.split('\r\n').filter(x=>x.startsWith('a=rtcp-fb:')),answer:window.testPeer.remoteDescription.sdp.split('\r\n').filter(x=>x.startsWith('a=rtcp-fb:'))}));
  console.log(JSON.stringify({profile:{nack,bufferFloor:floor,jitter:delayMode},phases,feedback},null,2));
  assert.ok(delayMode?phases[1].actuallyDelayedDatagrams>=50:phases[1].actualDroppedDatagrams>=50,'no actual RTP impairment');assert.ok(phases[0].received>500,'media did not traverse relay: '+JSON.stringify(phases));
 }finally{await browser.close();for(const timer of pending)clearTimeout(timer);towardBrowser.close();towardSender.close();}
})().catch(e=>{console.error('FAIL:',e.message);process.exitCode=1;});
