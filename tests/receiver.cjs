/* Real WebRTC receive + decoded stereo verification. Uses Playwright, not mocks. */
const fs = require("node:fs");
const assert = require("node:assert/strict");
const path = require("node:path");
const {chromium, webkit, firefox} = require(process.env.MCONNECT_PLAYWRIGHT || "playwright");

(async () => {
  const info = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
  const engine = process.argv[3] || "chromium";
  const url = new URL(info.url); url.hostname = "127.0.0.1";
  const base = url.origin, token = url.hash.slice(1);
  const launch={headless:true};
  if(process.platform==='darwin'&&engine==='webkit'&&process.env.MCONNECT_TEST_TIMING_LIBRARY)launch.env={...process.env,DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY,__XPC_DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY};
  const browser = await ({chromium, webkit, firefox}[engine]).launch(launch);
  const output = path.resolve("output/playwright"); fs.mkdirSync(output,{recursive:true});
  let context;
  try {
    context = await browser.newContext({viewport:{width:390,height:844},locale:"ru-RU"});
    const page = await context.newPage();
    const errors = [];
    page.on("pageerror", e => errors.push(e.message));
    let response = await context.request.get(base + "/info"); assert.equal(response.status(),403);
    response = await context.request.get(base + "/info",{headers:{"X-MConnect-Token":"bad"}}); assert.equal(response.status(),403);
    response = await context.request.get(base + "/source.zip"); assert.equal(response.status(),200);
    assert.equal((await response.body()).subarray(0,2).toString(),"PK");
    const unpaired = await context.newPage();
    await unpaired.goto(base); assert.equal(await unpaired.locator("#listen").isDisabled(),true);
    await unpaired.close();
    await page.addInitScript(() => {
      window.testSockets = [];
      window.testEvents = [];
      const NativePeer = window.RTCPeerConnection;
      window.RTCPeerConnection = class extends NativePeer {
        constructor(config) { super(config); window.testPeer = this; }
      };
      const NativeSocket = window.WebSocket;
      window.WebSocket = class extends NativeSocket {
        constructor(url) { super(url); window.testSockets.push(this); }
        send(message) {
          if (message.includes("webrtc-ready")) window.testEvents.push({at:performance.now(),message});
          super.send(message);
        }
      };
      const original = HTMLMediaElement.prototype.play;
      let blocked = false;
      HTMLMediaElement.prototype.play = function() {
        if (this.srcObject && !blocked) { blocked = true; return Promise.reject(new DOMException("Autoplay test","NotAllowedError")); }
        return original.call(this);
      };
      // WebKit also requires user activation for the verification AudioContext.
      // This graph is test-only; the shipped receiver uses its native audio tag.
      document.addEventListener("click", event => {
        const audio = document.querySelector("audio");
        if (!event.target.closest("#listen") || window.testAudio || !audio?.srcObject) return;
        const ctx = new AudioContext({sampleRate:48000});
        const source = ctx.createMediaStreamSource(audio.srcObject), splitter = ctx.createChannelSplitter(2);
        const analysers = [ctx.createAnalyser(),ctx.createAnalyser()];
        const quiet = ctx.createGain(); quiet.gain.value = 1e-6;
        source.connect(splitter);
        for (let c=0;c<2;c++) {
          analysers[c].fftSize=2048;
          splitter.connect(analysers[c],c); analysers[c].connect(quiet);
        }
        quiet.connect(ctx.destination);
        window.testAudio = {ctx,source,splitter,analysers,quiet};
        ctx.resume();
      },true);
    });
    await page.goto(url.href);
    assert.equal(await page.evaluate(() => Boolean(window.RTCPeerConnection)),true,"RTCPeerConnection unavailable");
    await page.waitForFunction(() => document.querySelector("#buttonText").textContent === "Слушать" && document.querySelector("audio").srcObject && !document.querySelector("#listen").disabled,{},{timeout:20000});
    await page.locator("#listen").click();
    await page.waitForFunction(() => !document.querySelector("#audio").paused && document.querySelector("#listen").hidden,{},{timeout:20000});
    await page.waitForFunction(() => window.testAudio?.ctx.state === "running",{},{timeout:20000});
    await page.waitForTimeout(5000);
    const measureAudio = () => page.evaluate(async () => {
      const {ctx,analysers} = window.testAudio;
      // Automatic recovery may replace the track during the warm-up. Keep the
      // verification graph attached to the same stream as the shipped player.
      const current = document.querySelector('audio').srcObject;
      if (window.testAudio.source.mediaStream !== current) {
        window.testAudio.source.disconnect();
        window.testAudio.source = ctx.createMediaStreamSource(current);
        window.testAudio.source.connect(window.testAudio.splitter);
      }
      await new Promise(resolve => setTimeout(resolve,500));
      const magnitude = (data,freq) => {
        let re=0,im=0;
        for(let i=0;i<data.length;i++) { const angle=2*Math.PI*freq*i/ctx.sampleRate; re+=data[i]*Math.cos(angle); im+=data[i]*Math.sin(angle); }
        return 2*Math.hypot(re,im)/data.length;
      };
      const sampleWindow = () => analysers.map(analyser => {
        const data = new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(data);
        const spectrum = new Float32Array(analyser.frequencyBinCount); analyser.getFloatFrequencyData(spectrum);
        let peak=1;for(let n=2;n<spectrum.length;n++)if(spectrum[n]>spectrum[peak])peak=n;
        return {rms:Math.sqrt(data.reduce((a,b)=>a+b*b,0)/data.length),tone997:magnitude(data,997),tone1499:magnitude(data,1499),peakHz:peak*ctx.sampleRate/analyser.fftSize};
      });
      // NetEq can splice/time-adjust a frame while filling or reducing a
      // reserve. One 43 ms phase correlation can then dip despite a correct
      // spectrum and continuous energy. Keep independent windows as evidence.
      const windows = [];
      for (let index=0;index<3;++index) {
        windows.push(sampleWindow());
        if(index<2) await new Promise(resolve=>setTimeout(resolve,200));
      }
      const channels = [0,1].map(channel => windows.map(window=>window[channel])
        .sort((a,b)=>channel===0 ? b.tone997-a.tone997 : b.tone1499-a.tone1499)[0]);
      const contextState = ctx.state, elapsed = ctx.currentTime;
      return {sampleRate:ctx.sampleRate,channels,windows,contextState,elapsed};
    });
    const measurement = await measureAudio();
    const assertStereo = (value, stage) => {
      const windows = value.windows || [value.channels];
      assert.ok(windows.filter(channels => channels[0].tone997>.12 && channels[1].tone1499>.12).length>=2,
        stage + ": sustained stereo tones missing");
      assert.ok(windows.every(channels => channels[0].rms>.1 && channels[1].rms>.1
        && Math.abs(channels[0].peakHz-997)<40 && Math.abs(channels[1].peakHz-1499)<40
        && channels[0].tone1499<.02 && channels[1].tone997<.02),stage + ": stereo energy/spectrum/crosstalk");
      assert.ok(value.channels[0].tone997 > .12, stage + ": left tone missing");
      assert.ok(value.channels[1].tone1499 > .12, stage + ": right tone missing");
      assert.ok(value.channels[0].tone1499 < .02 && value.channels[1].tone997 < .02,
        stage + ": received stereo collapsed or mixed");
    };
    response = await context.request.get(base + "/info",{headers:{"X-MConnect-Token":token}});
    const stats = await response.json();
    if (measurement.channels[0].tone997 <= .12 || measurement.channels[1].tone1499 <= .12) {
      const receiver = await page.evaluate(async () => {
        const reports = await window.testPeer.getStats(), inbound = [];
        reports.forEach(r => { if (r.type === "inbound-rtp") inbound.push(r); });
        return {events:window.testEvents,inbound,audioPaused:document.querySelector("audio").paused};
      });
      console.error(JSON.stringify({engine,inputSampleRate:info.sampleRate,measurement,sender:stats,receiver}));
    }
    assertStereo(measurement, "initial receive");
    assert.ok(stats.packetsSent>20); assert.ok(stats.stereoPackets>20); assert.equal(stats.encodeErrors,0);
    await page.screenshot({path:path.join(output,"receiver-"+engine+"-"+info.sampleRate+".png"),fullPage:true});
    // A dropped signaling connection must restore itself without a new tap.
    const oldSocketCount = await page.evaluate(() => window.testSockets.length);
    await page.evaluate(() => window.testSockets.at(-1).close());
    await page.waitForFunction((count) => window.testSockets.length > count && !document.querySelector("audio").paused && document.querySelector("#listen").hidden,oldSocketCount,{timeout:20000});
    await page.waitForTimeout(1500);
    const afterRecovery = await measureAudio();
    assertStereo(afterRecovery, "signaling recovery");
    let afterLongOutage = null;
    if (process.env.MCONNECT_TEST_CONTROL) {
      const control = process.env.MCONNECT_TEST_CONTROL;
      const command = async value => {
        fs.writeFileSync(control + ".pending", JSON.stringify(value));
        fs.renameSync(control + ".pending", control);
        for (let n = 0; fs.existsSync(control); ++n) {
          assert.ok(n < 150, "host did not acknowledge control command");
          await page.waitForTimeout(100);
        }
      };
      const beforeOutage = await page.evaluate(() => window.testSockets.length);
      await command({stream:false});
      await page.waitForTimeout(8000);
      await command({stream:true});
      await page.waitForFunction(count => window.testSockets.length > count &&
        !document.querySelector("audio").paused && document.querySelector("#listen").hidden,
        beforeOutage,{timeout:30000});
      await page.waitForTimeout(3000);
      afterLongOutage = await measureAudio();
      assertStereo(afterLongOutage, "eight-second server outage recovery");
    }
    const selectedIce = await page.evaluate(async () => {
      const reports = await window.testPeer.getStats();
      let pair;
      reports.forEach(value => {
        if (value.type === "transport" && value.selectedCandidatePairId)
          pair = reports.get(value.selectedCandidatePairId);
      });
      if (!pair) reports.forEach(value => {
        if (value.type === "candidate-pair" && value.state === "succeeded" && value.nominated) pair = value;
      });
      const remote = pair && reports.get(pair.remoteCandidateId);
      return remote ? {address:remote.address || remote.ip,candidateType:remote.candidateType,protocol:remote.protocol} : null;
    });
    if (selectedIce?.address) assert.equal(selectedIce.address,new URL(info.url).hostname,"media did not use the advertised LAN address");
    for(let iteration=0;iteration<3;iteration++) {
      await page.locator("#stop").click();
      await page.locator("#listen").click();
      await page.waitForFunction(() => !document.querySelector("audio").paused && document.querySelector("#listen").hidden,{},{timeout:20000});
      await page.waitForTimeout(200);
    }
    await page.locator("#stop").click();
    // Includes cancelling a newly connected receiver during its startup
    // prefill: queued audio must not leak out after Stop or keep capture busy.
    await page.waitForTimeout(300);
    const stoppedStats = () => context.request.get(base + "/info",{headers:{"X-MConnect-Token":token}}).then(r => r.json());
    const stopped = await stoppedStats();
    assert.equal(stopped.listeners,0,"stopped receiver kept a listener slot");
    await page.waitForTimeout(800);
    assert.equal((await stoppedStats()).packetsSent,stopped.packetsSent,"RTP continued after Stop");
    await page.evaluate(() => window.testAudio.ctx.close());
    assert.deepEqual(errors,[],"browser runtime errors");
    console.log(JSON.stringify({result:"PASS",version:stats.version,engine,inputSampleRate:info.sampleRate,blockFrames:info.blockFrames || 256,decoded:measurement,afterRecovery,afterLongOutage,selectedIce,packets:stats.packetsSent,stereoPackets:stats.stereoPackets,reconnects:3,automaticRecovery:true,pairingRejection:true,autoplayFallback:true}));
  } finally { if(context) await context.close(); await browser.close(); }
})().catch(e => { console.error("FAIL:",e.message); process.exitCode=1; });
