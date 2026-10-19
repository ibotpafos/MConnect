/* Production SDP + actual SRTP loss/jitter + decoded stereo, without a DAW.
 * node tests/network-reliability.cjs info.json [chromium|webkit] [loss|jitter|combined] [--bad-start]
 * Run the host and browser with the macOS BrowserTiming fixture. No SDP feedback,
 * getStats or buffer setters are overridden. Only ICE is routed through UDP.
 */
const fs = require('node:fs'), dgram = require('node:dgram'), assert = require('node:assert/strict');
const playwright = require(process.env.MCONNECT_PLAYWRIGHT || 'playwright');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const info = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const engine = process.argv[3] || 'chromium', profile = process.argv[4] || 'loss';
const badStart = process.argv.includes('--bad-start');
assert.ok(['loss', 'jitter', 'combined'].includes(profile), 'unknown network profile');
assert.ok(['chromium', 'webkit'].includes(engine), 'unknown browser engine');

(async () => {
  const address = new URL(info.url).hostname;
  const towardBrowser = dgram.createSocket('udp4'), towardSender = dgram.createSocket('udp4');
  const pending = new Set(), missing = new Set(), errors = [];
  let senderEndpoint = null, browserEndpoint = null, impaired = badStart;
  let seen = 0, dropped = 0, recovered = 0, delayed = 0, seed = 1234567, browser;
  const media = packet => packet.length > 12 && (packet[0] & 0xc0) === 0x80 && !(packet[1] >= 192 && packet[1] <= 223);
  // Combined: 10% loss + each fifth RTP delayed 350 ms + 80 ms in BOTH
  // network directions (160 ms RTT), including RTCP repair requests.
  const forward = (socket, packet, target, delay = 0) => {
    if (!target) return;
    if (!delay) { socket.send(packet, target.port, target.address); return; }
    const endpoint = {...target};
    const timer = setTimeout(() => {
      pending.delete(timer); socket.send(packet, endpoint.port, endpoint.address);
    }, delay);
    pending.add(timer);
  };
  try {
    await Promise.all([towardBrowser, towardSender].map(socket => new Promise((resolve, reject) => {
      socket.once('error', reject); socket.bind(0, address, resolve);
    })));
    const ownPort = port => [towardBrowser.address().port, towardSender.address().port].includes(port);
    towardBrowser.on('message', (message, remote) => {
      if (ownPort(remote.port)) return;
      browserEndpoint = remote;
      forward(towardSender, message, senderEndpoint, impaired && profile === 'combined' ? 80 : 0);
    });
    towardSender.on('message', (message, remote) => {
      if (ownPort(remote.port)) return;
      senderEndpoint = remote;
      if (!browserEndpoint) return;
      let delay = impaired && profile === 'combined' ? 80 : 0;
      if (media(message)) {
        ++seen;
        const sequence = message.readUInt16BE(2);
        seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
        if (impaired && profile !== 'jitter' && seed / 4294967296 < .1) {
          ++dropped; missing.add(sequence); return;
        }
        if (missing.delete(sequence)) ++recovered;
        if (impaired && profile !== 'loss' && seen % 5 === 0) { ++delayed; delay += 350; }
      }
      forward(towardBrowser, message, browserEndpoint, delay);
    });
    browser = await playwright[engine].launch({headless:true,
      ...(engine === 'chromium' ? {args:['--autoplay-policy=no-user-gesture-required']} : {}),
      ...(process.env.MCONNECT_TEST_TIMING_LIBRARY ? {env:{...process.env,
        DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY,
        __XPC_DYLD_INSERT_LIBRARIES:process.env.MCONNECT_TEST_TIMING_LIBRARY}} : {})});
    const page = await browser.newPage({viewport:{width:390,height:844}, locale:'ru-RU'});
    page.on('pageerror', error => errors.push(error.message));
    await page.exposeFunction('nativeCandidate', candidate => {
      const parts = candidate.replace(/^a=/, '').trim().split(/\s+/);
      if (!senderEndpoint && parts[2]?.toLowerCase() === 'udp' && parts[4] === address && !ownPort(Number(parts[5])))
        senderEndpoint = {address:parts[4],port:Number(parts[5])};
    });
    await page.addInitScript(({address, toBrowser, toSender}) => {
      const NativePeer = RTCPeerConnection;
      window.RTCPeerConnection = class extends NativePeer {
        constructor(config) {
          super(config); window.testPeer = this; window.testConnections ??= [];
          this.addEventListener('connectionstatechange', () => window.testConnections.push(this.connectionState));
        }
      };
      const rewrite = (candidate, port) => {
        const parts = candidate.replace(/^a=/, '').trim().split(/\s+/);
        if (parts[2]?.toLowerCase() !== 'udp') return candidate;
        parts[4] = address; parts[5] = String(port); return parts.join(' ');
      };
      const NativeSocket = WebSocket;
      window.WebSocket = class extends NativeSocket {
        send(text) {
          const data = JSON.parse(text);
          if (data.type === 'webrtc-candidate') data.candidate = rewrite(data.candidate, toSender);
          if (data.type === 'webrtc-offer') data.sdp = data.sdp.replace(/^a=candidate:.*$/gm, line => 'a=' + rewrite(line, toSender));
          return super.send(JSON.stringify(data));
        }
        set onmessage(callback) {
          if (!callback) { super.onmessage = null; return; }
          let chain = Promise.resolve();
          super.onmessage = event => { chain = chain.then(async () => {
            const data = JSON.parse(event.data);
            if (data.type === 'webrtc-candidate') {
              await window.nativeCandidate(data.candidate); data.candidate = rewrite(data.candidate, toBrowser);
            }
            if (data.type === 'webrtc-answer') {
              for (const match of data.sdp.matchAll(/^a=candidate:.*$/gm)) await window.nativeCandidate(match[0]);
              data.sdp = data.sdp.replace(/^a=candidate:.*$/gm, line => 'a=' + rewrite(line, toBrowser));
            }
            callback(new MessageEvent('message', {data:JSON.stringify(data)}));
          }).catch(error => window.testRelayError = error.message); };
        }
      };
      // Deterministic user activation for both engines. Only this verification
      // graph uses Web Audio; the shipped player still uses its audio element.
      const play = HTMLMediaElement.prototype.play; let blocked = false;
      HTMLMediaElement.prototype.play = function() {
        if (this.srcObject && !blocked) { blocked = true; return Promise.reject(new DOMException('Test activation', 'NotAllowedError')); }
        return play.call(this);
      };
      document.addEventListener('click', event => {
        const stream = document.querySelector('audio')?.srcObject;
        if (!event.target.closest('#listen') || !stream || window.testAudio) return;
        const ctx = new AudioContext({sampleRate:48000}), input = ctx.createMediaStreamSource(stream);
        const split = ctx.createChannelSplitter(2), analysers = [ctx.createAnalyser(), ctx.createAnalyser()], quiet = ctx.createGain();
        quiet.gain.value = 1e-6; input.connect(split);
        analysers.forEach((analyser, channel) => {
          analyser.fftSize = 2048; split.connect(analyser, channel); analyser.connect(quiet);
        });
        quiet.connect(ctx.destination); window.testAudio = {ctx,input,split,analysers,quiet}; ctx.resume();
      }, true);
    }, {address, toBrowser:towardBrowser.address().port, toSender:towardSender.address().port});
    const url = new URL(info.url); url.hostname = '127.0.0.1'; await page.goto(url.href);
    await page.waitForFunction(() => document.querySelector('audio').srcObject && !document.querySelector('#listen').disabled, {}, {timeout:25000});
    await page.locator('#listen').click();
    await page.waitForFunction(() => !document.querySelector('audio').paused && window.testAudio?.ctx.state === 'running', {}, {timeout:15000});
    const stats = () => page.evaluate(async () => {
      const all = await window.testPeer.getStats(); let inbound;
      all.forEach(value => {
        if (value.type === 'inbound-rtp' && (value.kind || value.mediaType) === 'audio') inbound = {
          received:value.packetsReceived, lost:value.packetsLost, concealed:value.concealedSamples,
          samples:value.totalSamplesReceived, nacks:value.nackCount, energy:value.totalAudioEnergy,
          emitted:value.jitterBufferEmittedCount, delay:value.jitterBufferDelay, jitter:value.jitter,
          targetDelay:value.jitterBufferTargetDelay, minimumDelay:value.jitterBufferMinimumDelay,
          inserted:value.insertedSamplesForDeceleration, removed:value.removedSamplesForAcceleration
        };
      });
      const receiver = window.testPeer.getReceivers()[0];
      return {...inbound, contextState:window.testAudio.ctx.state, contextTime:window.testAudio.ctx.currentTime,
        target:receiver.jitterBufferTarget ?? receiver.playoutDelayHint * 1000};
    });
    const stereo = () => page.evaluate(async () => {
      const {ctx, analysers} = window.testAudio;
      const sampleWindow = () => analysers.map(analyser => {
        const data = new Float32Array(analyser.fftSize); analyser.getFloatTimeDomainData(data);
        const tones = [997, 1499].map(frequency => {
          let re = 0, im = 0;
          for (let i = 0; i < data.length; ++i) {
            const phase = 2 * Math.PI * frequency * i / ctx.sampleRate;
            re += data[i] * Math.cos(phase); im += data[i] * Math.sin(phase);
          }
          return 2 * Math.hypot(re, im) / data.length;
        });
        const spectrum = new Float32Array(analyser.frequencyBinCount); analyser.getFloatFrequencyData(spectrum);
        let peak=1;for(let i=2;i<spectrum.length;++i)if(spectrum[i]>spectrum[peak])peak=i;
        return {tones,rms:Math.sqrt(data.reduce((sum,value)=>sum+value*value,0)/data.length),peakHz:peak*ctx.sampleRate/analyser.fftSize};
      });
      const windows=[];
      for(let index=0;index<3;++index){windows.push(sampleWindow());if(index<2)await new Promise(resolve=>setTimeout(resolve,200));}
      return windows;
    });
    await page.waitForFunction(async () => {
      let ok = false;
      (await window.testPeer.getStats()).forEach(r => {
        if (r.type === 'inbound-rtp' && (r.kind || r.mediaType) === 'audio' && r.packetsReceived > 100 && r.totalAudioEnergy > .005) ok = true;
      }); return ok;
    }, {}, {timeout:20000});
    // Headless WebKit may receive RTP well before its audio rendering clock
    // reaches real time. Packet count alone accepted a false healthy control.
    // Require sustained decoded sample/energy delivery before measuring loss.
    const warmupStarted = Date.now();
    let stable = 0, last = await stats(), lastTime = Date.now();
    while (stable < 3) {
      assert.ok(Date.now() - warmupStarted < 60000, 'audio clock did not reach real-time decoding');
      await sleep(1000);
      const current = await stats(), now = Date.now(), seconds = (now-lastTime)/1000;
      stable = current.samples-last.samples > seconds*40000 && current.energy-last.energy > seconds*.02 ? stable+1 : 0;
      last = current; lastTime = now;
    }
    const warmupSeconds = (Date.now()-warmupStarted)/1000;
    const phases = [];
    // Adaptation is separate: the first fault cannot be undone after playout.
    for (const [name, active, seconds] of [[badStart ? 'impaired-start' : 'healthy',badStart,8], ['adaptation',true,20], ['impaired',true,12], ['recovery',false,16]]) {
      impaired = active;
      const counters = {seen,dropped,recovered,delayed}, before = await stats();
      await sleep(seconds * 1000);
      const after = await stats(), samples = after.samples - before.samples;
      phases.push({name,seconds,emulatedLossPercent:active && profile !== 'jitter' ? 10 : 0,
        emulatedBurstDelayMs:active && profile !== 'loss' ? 350 : 0,
        emulatedOneWayDelayMs:active && profile === 'combined' ? 80 : 0,
        actualMediaDatagrams:seen-counters.seen, actualDroppedDatagrams:dropped-counters.dropped,
        actuallyRecoveredDatagrams:recovered-counters.recovered, actuallyDelayedDatagrams:delayed-counters.delayed,
        before,after,received:after.received-before.received,lost:after.lost-before.lost,
        concealed:after.concealed-before.concealed,concealmentPercent:100*(after.concealed-before.concealed)/Math.max(1,samples),
        intervalBufferMs:1000*(after.delay-before.delay)/Math.max(1,after.emitted-before.emitted),
        audioEnergy:after.energy-before.energy,decodedWindows:await stereo()});
    }
    const feedback = await page.evaluate(() => ({connections:window.testConnections, relayError:window.testRelayError,
      offer:window.testPeer.localDescription.sdp.split('\r\n').filter(line => line.startsWith('a=rtcp-fb:')),
      answer:window.testPeer.remoteDescription.sdp.split('\r\n').filter(line => line.startsWith('a=rtcp-fb:'))}));
    const result = {result:'MEASURED',engine,profile,badStart,warmupSeconds,productionNegotiation:true,phases,feedback,errors};
    try {
      assert.deepEqual(errors, []); assert.equal(feedback.relayError, undefined);
      assert.deepEqual(feedback.connections, ['connecting','connected'], 'peer restarted during network stress');
      for (const phase of phases) {
        assert.ok(phase.received > phase.seconds * 70, 'invalid sender/relay timing: ' + phase.name);
        assert.ok(phase.after.samples-phase.before.samples > phase.seconds*40000, 'decoded sample clock stalled: ' + phase.name);
        assert.ok(phase.after.contextTime-phase.before.contextTime > phase.seconds*.85, 'verification audio clock stalled: ' + phase.name);
        assert.ok(phase.audioEnergy > phase.seconds * .02, 'decoded audio energy missing: ' + phase.name);
        assert.equal(phase.after.contextState, 'running');
        // Time-adjusted NetEq frames can cancel one short phase correlation.
        // Preserve all windows; require sustained energy/spectrum/crosstalk
        // and the original tone thresholds in two independent windows.
        const windows = phase.decodedWindows;
        assert.ok(windows.filter(channels=>channels[0].tones[0]>.12&&channels[1].tones[1]>.12).length>=2
          && windows.every(channels=>channels[0].rms>.1&&channels[1].rms>.1
            &&Math.abs(channels[0].peakHz-997)<40&&Math.abs(channels[1].peakHz-1499)<40
            &&channels[0].tones[1]<.02&&channels[1].tones[0]<.02),
          'decoded stereo missing or mixed: ' + phase.name);
      }
      const bad = phases[2];
      assert.ok(profile === 'jitter' ? bad.actuallyDelayedDatagrams >= 50 : bad.actualDroppedDatagrams >= 50, 'no real RTP impairment');
      assert.ok(feedback.offer.some(line => / nack$/.test(line)) && feedback.answer.some(line => / nack$/.test(line)), 'production NACK not negotiated');
      if (profile !== 'jitter') assert.ok(bad.actuallyRecoveredDatagrams >= 40, 'no actual retransmission of dropped RTP');
      assert.ok(phases[0].concealmentPercent < (badStart ? 1 : .5), badStart ? 'impaired initial receive not stable' : 'invalid healthy control');
      assert.ok(bad.concealmentPercent < 1, 'settled bad-network concealment >= 1%');
      assert.ok(phases[3].concealmentPercent < .5, 'recovery concealment >= 0.5%');
      assert.ok(bad.after.target >= 300 && bad.after.target <= 600, 'automatic buffer outside bounds');
      result.result = 'PASS';
    } finally { console.log(JSON.stringify(result, null, 2)); }
  } finally {
    if (browser) await browser.close();
    for (const timer of pending) clearTimeout(timer);
    towardBrowser.close(); towardSender.close();
  }
})().catch(error => { console.error('FAIL:', error.message); process.exitCode = 1; });
