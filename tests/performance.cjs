/* macOS benchmark: simulated DAW process, separately from the receiving browser. */
const fs = require("node:fs"), path = require("node:path"), os = require("node:os");
const assert = require("node:assert/strict");
const {spawn,execFileSync} = require("node:child_process");
const {chromium} = require(process.env.MCONNECT_PLAYWRIGHT || "playwright");
const sleep = ms => new Promise(resolve => setTimeout(resolve,ms));
const rate = Number(process.argv[2] || 48000);
const host = process.env.MCONNECT_TEST_HOST || path.resolve("build/MConnectHarness_artefacts/Release/MConnect test host.app/Contents/MacOS/MConnect test host");
const temporary = fs.mkdtempSync(path.join(process.platform === "darwin" ? "/private/tmp" : os.tmpdir(),"mconnect-perf-"));
const infoPath = path.join(temporary,"info.json");
const source = spawn(host,["--no-editor","--rate",String(rate),"--seconds","120","--info",infoPath],{stdio:"ignore"});
let browser;
function cpu() {
  const text = execFileSync("ps",["-p",String(source.pid),"-o","time="],{encoding:"utf8"}).trim();
  return text.split(":").reverse().reduce((sum,value,index)=>sum+Number(value)*60**index,0);
}
async function measure() {
  const start = cpu(), wall = performance.now(); await sleep(6000);
  return 100*(cpu()-start)/((performance.now()-wall)/1000);
}
(async()=>{
  try {
    for(let i=0;i<100&&!fs.existsSync(infoPath);i++) await sleep(100);
    const info = JSON.parse(fs.readFileSync(infoPath));
    const baseline = await measure();
    const launch={headless:true};
    if(process.platform==='darwin'&&process.env.MCONNECT_TEST_TIMING_LIBRARY)launch.env={...process.env,DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY,__XPC_DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY};
    browser = await chromium.launch(launch);
    const page = await browser.newPage();
    await page.addInitScript(()=>{
      const Native = window.RTCPeerConnection;
      window.RTCPeerConnection = class extends Native { constructor(config){super(config);window.benchmarkPeer=this;} };
    });
    const url = new URL(info.url); url.hostname="127.0.0.1";
    await page.goto(url.href);
    await page.waitForFunction(()=>document.querySelector("audio").srcObject && (!document.querySelector("#listen").disabled || document.querySelector("#listen").hidden),{},{timeout:20000});
    if(await page.locator("#listen").isVisible()) await page.locator("#listen").click();
    await page.waitForFunction(()=>!document.querySelector("audio").paused);
    await sleep(2000);
    const initial = await page.evaluate(async()=>{
      let result={}; (await window.benchmarkPeer.getStats()).forEach(r=>{if(r.type==='inbound-rtp'&&(r.kind||r.mediaType)==='audio')result={delay:r.jitterBufferDelay,emitted:r.jitterBufferEmittedCount,packets:r.packetsReceived};}); return result;
    });
    const startInfo = await page.request.get(url.origin+'/info',{headers:{'X-MConnect-Token':url.hash.slice(1)}}).then(r=>r.json());
    const windowStart = performance.now();
    const active = await measure();
    const rssKb = Number(execFileSync("ps",["-p",String(source.pid),"-o","rss="],{encoding:"utf8"}).trim());
    const receiver = await page.evaluate(async()=>{
      const reports=await window.benchmarkPeer.getStats();let result={};
      reports.forEach(r=>{if(r.type==="inbound-rtp"&&(r.kind||r.mediaType)==="audio")result={packets:r.packetsReceived,loss:r.packetsLost,jitterMs:r.jitter*1000,bufferMs:r.jitterBufferEmittedCount?r.jitterBufferDelay*1000/r.jitterBufferEmittedCount:null,targetMs:r.jitterBufferEmittedCount?r.jitterBufferTargetDelay*1000/r.jitterBufferEmittedCount:null,minimumMs:r.jitterBufferEmittedCount?r.jitterBufferMinimumDelay*1000/r.jitterBufferEmittedCount:null,concealed:r.concealedSamples,inserted:r.insertedSamplesForDeceleration,removed:r.removedSamplesForAcceleration,audioEnergy:r.totalAudioEnergy};});
      const receiver=window.benchmarkPeer.getReceivers()[0]; result.requestedTarget=receiver.jitterBufferTarget; result.playoutHint=receiver.playoutDelayHint; return result;
    });
    const endInfo = await page.request.get(url.origin+'/info',{headers:{'X-MConnect-Token':url.hash.slice(1)}}).then(r=>r.json());
    const windowSeconds = (performance.now()-windowStart)/1000;
    receiver.intervalBufferMs = receiver.bufferMs == null || !initial.emitted || receiver.packets <= initial.packets ? null :
      await page.evaluate(async (start)=> {
        let delay=null; (await window.benchmarkPeer.getStats()).forEach(r=>{if(r.type==='inbound-rtp'&&(r.kind||r.mediaType)==='audio'&&r.jitterBufferEmittedCount>start.emitted)delay=1000*(r.jitterBufferDelay-start.delay)/(r.jitterBufferEmittedCount-start.emitted);}); return delay;
      },initial);
    const workerWakeupsPerSecond = Number.isFinite(endInfo.workerWakeups) ? (endInfo.workerWakeups-startInfo.workerWakeups)/windowSeconds : null;
    if (!(receiver.packets>=500&&receiver.audioEnergy>.01)) console.error(JSON.stringify({inputSampleRate:rate,receiver,sender:endInfo}));
    assert.ok(receiver.packets >= 500 && receiver.audioEnergy > .01,"Insufficient audio delivery for a meaningful CPU benchmark");
    const result={version:endInfo.version,inputSampleRate:rate,baselineProcessCpuOneCorePercent:baseline,streamingProcessCpuOneCorePercent:active,incrementalStreamingCpuOneCorePercent:Math.max(0,active-baseline),processRssMiB:rssKb/1024,workerWakeupsPerSecond,receiver};
    fs.mkdirSync("test-results",{recursive:true});fs.writeFileSync("test-results/performance-"+rate+".json",JSON.stringify(result,null,2));
    console.log(JSON.stringify(result));
  } finally {if(browser)await browser.close();source.kill("SIGTERM");fs.rmSync(temporary,{recursive:true,force:true});}
})().catch(e=>{console.error("FAIL:",e.message);process.exitCode=1;});
