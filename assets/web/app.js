"use strict";
(() => {
  const ru = navigator.language.toLowerCase().startsWith("ru");
  const copy = ru ? {
    local:"ЛОКАЛЬНЫЙ ЗВУК",eyebrow:"УСЛЫШАТЬ ПО-НОВОМУ",title:"Твой микс.<br>Твой телефон.",intro:"Слушай звук из DAW на знакомых динамиках. Вживую, прямо здесь.",
    hint:"Оставь страницу открытой. Громкость меняется кнопками телефона.",network:"Телефон и компьютер должны быть в одной Wi-Fi или локальной сети.",footer:"С мастер-шины — к тебе.",source:"Исходники",license:"Лицензия",stop:"Отключиться",listen:"Слушать",ready:"Готов к подключению",connecting:"Подключаемся…",retrying:"Восстанавливаем связь…",playing:"Слушаем мастер",tap:"Слушать",connected:"Подключено — включи звук",retry:"Подключить снова",paused:"Воспроизведение приостановлено",badLink:"Открой ссылку через QR-код в плагине MConnect.",unsupported:"Этот браузер не поддерживает WebRTC. Открой ссылку в актуальном Safari или Chrome.",failed:"Связь прервалась. Проверь, что плагин включён и телефон в той же сети.",timeout:"Не удалось установить связь. Проверь Wi-Fi и доступ DAW к локальной сети.",muted:"Звук подключён. Нажми кнопку и проверь громкость телефона.",buffer:"буфер",loss:"потери"
  } : {
    local:"LOCAL AUDIO",eyebrow:"A DIFFERENT PERSPECTIVE",title:"Your mix.<br>Your phone.",intro:"Hear your DAW through the speakers you know. Live, right here.",hint:"Keep this page open. Use your phone's volume buttons.",network:"Your phone and computer need to be on the same Wi-Fi or local network.",footer:"From the master bus, to you.",source:"Source",license:"License",stop:"Disconnect",listen:"Listen",ready:"Ready to listen",connecting:"Connecting…",retrying:"Reconnecting…",playing:"Listening to the master",tap:"Listen",connected:"Connected — tap to play",retry:"Reconnect",paused:"Playback paused",badLink:"Open the pairing link from the QR code in MConnect.",unsupported:"This browser does not support WebRTC. Open in a current Safari or Chrome.",failed:"Connection lost. Check that streaming is on and both devices share a network.",timeout:"Could not connect. Check Wi-Fi and the DAW's local network access.",muted:"Audio is connected. Tap Play and check your phone's volume.",buffer:"buffer",loss:"loss"
  };
  document.documentElement.lang = ru ? "ru" : "en";
  for (const el of document.querySelectorAll("[data-i18n]")) {
    const key = el.dataset.i18n;
    if (key === "title") el.innerHTML = copy[key]; else el.textContent = copy[key];
  }
  const $ = (id) => document.getElementById(id);
  const audio = $("audio"), button = $("listen"), stopButton = $("stop");
  const token = location.hash.slice(1);
  // Leave time for a LAN retransmission; trade more latency for continuity
  // automatically when a receiver reports loss or late audio.
  const minimumBuffer = 300, maximumBuffer = 600;
  let peer = null, socket = null, generation = 0, mode = "idle";
  let timer = null, timeout = null, statsBusy = false, reconnectTimer = null;
  let wantsAudio = false, retryAttempts = 0, receiverForBuffer = null, lastBuffer = null;
  let previousReport = null, healthySince = 0, autoBuffer = minimumBuffer, lastPacketTime = 0;
  let statsAbort = null, suspendedIntent = false, sentReady = null, playAttempt = 0;
  let messageChain = Promise.resolve(), candidates = [], localCandidates = [], offerSent = false;
  const paired = /^[a-f0-9]{32}$/i.test(token);

  function state(next, label) {
    mode = next;
    $("status").textContent = label;
    $("dot").className = "dot" + (["connecting", "retrying"].includes(next) ? " connecting" : ["playing", "tap"].includes(next) ? " connected" : "");
    button.disabled = ["connecting", "retrying"].includes(next);
    button.hidden = next === "playing";
    stopButton.hidden = !["connecting", "retrying", "playing", "tap"].includes(next);
    $("buttonText").textContent = next === "tap" ? copy.tap : next === "error" ? copy.retry : next === "retrying" ? copy.retrying : next === "connecting" ? copy.connecting : copy.listen;
    $("buttonIcon").textContent = next === "connecting" ? "◌" : "▶";
    if (next === "playing" || next === "tap") clearTimeout(timeout);
  }
  function error(text) { $("error").textContent = text; $("error").hidden = false; }
  function cleanup() {
    ++generation;
    ++playAttempt;
    statsAbort?.abort(); statsAbort = null;
    clearInterval(timer); clearTimeout(timeout); clearTimeout(reconnectTimer); timer = timeout = reconnectTimer = null;
    const oldSocket = socket, oldPeer = peer;
    socket = peer = null;
    if (oldSocket) { oldSocket.onclose = oldSocket.onerror = oldSocket.onmessage = oldSocket.onopen = null; oldSocket.close(); }
    if (oldPeer) { oldPeer.onconnectionstatechange = oldPeer.ontrack = oldPeer.onicecandidate = null; oldPeer.close(); }
    mode = "idle";
    audio.pause(); audio.srcObject = null; audio.hidden = true;
    candidates = []; localCandidates = []; offerSent = false; statsBusy = false;
    receiverForBuffer = lastBuffer = null;
    previousReport = null; healthySince = lastPacketTime = 0; autoBuffer = minimumBuffer; sentReady = null;
    $("details").hidden = true;
    $("leftMeter").style.transform = $("rightMeter").style.transform = "scaleX(0)";
  }
  function fail(message, fatal = false) {
    const retry = wantsAudio && paired && !fatal;
    cleanup();
    if (retry) {
      state("retrying", copy.retrying); $("error").hidden = true;
      const delay = Math.min(10000, 500 * Math.pow(2, Math.min(5, retryAttempts++)));
      reconnectTimer = setTimeout(connect, delay);
    } else { wantsAudio = false; state("error", copy.failed); error(message); }
  }
  function applyBuffer(milliseconds) {
    if (!receiverForBuffer || !Number.isFinite(milliseconds) || lastBuffer === milliseconds) return;
    try {
      if ("jitterBufferTarget" in receiverForBuffer) receiverForBuffer.jitterBufferTarget = milliseconds;
      else if ("playoutDelayHint" in receiverForBuffer) receiverForBuffer.playoutDelayHint = milliseconds / 1000;
      lastBuffer = milliseconds;
    } catch (_) { /* Unsupported hints leave the browser's own jitter buffer active. */ }
  }
  function send(data) {
    if (socket?.readyState !== WebSocket.OPEN) return false;
    try { socket.send(JSON.stringify(data)); return true; } catch (_) { return false; }
  }
  function ready(value) {
    if (sentReady !== value && send({type:"webrtc-ready",ready:value})) sentReady = value;
  }
  async function info(mine) {
    const controller = new AbortController(); statsAbort = controller;
    const deadline = setTimeout(() => controller.abort(),5000);
    try {
      const response = await fetch("/info",{headers:{"X-MConnect-Token":token},signal:controller.signal});
      if (response.status === 403) throw Object.assign(new Error(copy.badLink),{fatal:true});
      if (!response.ok) throw new Error(copy.failed);
      const result = await response.json();
      if (mine !== generation) throw new Error("Stale session");
      return result;
    } finally { clearTimeout(deadline); if (statsAbort === controller) statsAbort = null; }
  }
  function play() {
    // Called directly from the button handler for iOS user activation.
    const mine = generation;
    const attempt = ++playAttempt;
    let promise;
    try { promise = audio.play(); } catch (_) { state("tap",copy.connected); return; }
    // play() sets paused=false synchronously when playback is allowed. A denied
    // autoplay must not send even one packet and seed a stalled jitter timeline.
    if (!audio.paused) ready(true);
    if (promise) promise.catch(() => {
      if (mine !== generation || attempt !== playAttempt || !audio.srcObject || !audio.paused) return;
      ready(false);
      if (peer?.connectionState === "connected") state("tap", copy.connected);
      $("hint").textContent = copy.muted;
    });
  }
  function stereo(sdp) {
    const lines = sdp.split(/\r?\n/);
    const codec = lines.findIndex((line) => /^a=rtpmap:\d+ opus\/48000(?:\/2)?$/i.test(line));
    const match = codec >= 0 && lines[codec].match(/^a=rtpmap:(\d+)/);
    if (!match) throw new Error("Opus unavailable");
    const pt = match[1];
    const fmtp = lines.findIndex((line) => line.startsWith("a=fmtp:" + pt + " "));
    const params = (fmtp >= 0 ? lines[fmtp].split(" ").slice(1).join(" ").trim().split(";") : []).filter((value) => !/^(stereo|sprop-stereo|maxaveragebitrate)=/.test(value));
    params.push("stereo=1", "sprop-stereo=1", "maxaveragebitrate=510000");
    const line = "a=fmtp:" + pt + " " + params.join(";");
    if (fmtp >= 0) lines[fmtp] = line; else lines.splice(codec + 1, 0, line);
    // Browsers implement audio NACK but do not all offer it by default. The
    // sender answers only this offered capability; older senders still work.
    const nack = "a=rtcp-fb:" + pt + " nack";
    if (!lines.includes(nack)) lines.splice(codec + 1, 0, nack);
    return lines.join("\r\n");
  }
  async function poll(mine) {
    if (mine !== generation || !peer || statsBusy) return;
    statsBusy = true;
    try {
      const details = await info(mine);
      if (mine !== generation) return;

      $("quality").textContent = Math.round(details.bitrate / 1000) + " kbps · Opus";

      for (const [id, value] of [["leftMeter", details.peakL], ["rightMeter", details.peakR]]) {
        const fill = Math.max(0, Math.min(1, (20 * Math.log10(Math.max(1e-5, value)) + 60) / 60));
        $(id).style.transform = "scaleX(" + fill + ")";
      }
      const reports = await peer.getStats();
      if (mine !== generation) return;
      let roundTripMs = 0;
      reports.forEach((report) => {
        if (report.type === "candidate-pair" && report.state === "succeeded" && (report.nominated || report.selected)
            && Number.isFinite(report.currentRoundTripTime)) roundTripMs = Math.max(roundTripMs,report.currentRoundTripTime*1000);
      });
      reports.forEach((report) => {
        if (report.type !== "inbound-rtp" || (report.kind || report.mediaType) !== "audio") return;
        const received = report.packetsReceived || 0, lost = Math.max(0, report.packetsLost || 0);
        const before = previousReport?.id === report.id && received >= previousReport.received ? previousReport : null;
        const newLoss = before ? Math.max(0,lost-before.lost) : 0, newPackets = before ? received-before.received : 0;
        const emitted = report.jitterBufferEmittedCount || 0, delay = report.jitterBufferDelay || 0;
        const concealed = report.concealedSamples || 0, samples = report.totalSamplesReceived || 0;
        const lossRatio = newLoss / Math.max(1,newPackets+newLoss);
        const concealRatio = before ? Math.max(0,concealed-before.concealed) / Math.max(1,samples-before.samples) : 0;
        const impaired = lossRatio > .01 || concealRatio > .01;
        const networkMinimum = Math.ceil(Math.max(minimumBuffer,details.bufferMs || 0,(report.jitter || 0)*4000,roundTripMs+40));
        // Grow quickly on late/missing audio, including loss with low jitter.
        // Hold the reserve through short improvements instead of oscillating.
        const wanted = Math.min(maximumBuffer,impaired ? Math.max(networkMinimum,autoBuffer+80,autoBuffer*1.5) : networkMinimum);
        const now = performance.now();
        if (newPackets > 0) lastPacketTime = now;
        if (impaired) healthySince = 0;
        if (wanted > autoBuffer+5) { autoBuffer = Math.ceil(wanted); healthySince = 0; }
        else if (newPackets > 0 && !impaired) {
          if (!healthySince) healthySince = now;
          if (now-healthySince >= 12000) { autoBuffer = Math.max(wanted,autoBuffer-20); healthySince = now; retryAttempts = 0; }
        } else healthySince = 0;
        const intervalDelay = before && emitted > before.emitted ? 1000*(delay-before.delay)/(emitted-before.emitted) : null;
        previousReport = {id:report.id,received,lost,emitted,delay,concealed,samples};
        applyBuffer(autoBuffer);
        $("loss").textContent = (100*lossRatio).toFixed(1) + "% " + copy.loss;
        $("buffer").textContent = intervalDelay == null ? "WebRTC" : Math.round(intervalDelay) + " ms " + copy.buffer;
      });
      if (mode === "playing" && lastPacketTime && performance.now()-lastPacketTime > 10000) fail(copy.failed);
    } catch (e) { if (mine === generation && e.fatal) fail(e.message,true); }
    finally { if (mine === generation) statsBusy = false; }
  }
  async function connect() {
    cleanup(); $("error").hidden = true; $("hint").textContent = copy.hint;
    const mine = generation;
    state("connecting", copy.connecting);
    timeout = setTimeout(() => { if (mine === generation) fail(copy.timeout); }, 15000);
    try {
      await info(mine); // Detect expired pairing keys without an endless retry loop.
      if (mine !== generation) return;
      peer = new RTCPeerConnection({iceServers:[],bundlePolicy:"max-bundle"});
      const currentPeer = peer;
      const transceiver = peer.addTransceiver("audio", {direction:"recvonly"});
      if (transceiver.setCodecPreferences && window.RTCRtpReceiver?.getCapabilities) {
        const opus = RTCRtpReceiver.getCapabilities("audio")?.codecs.filter((codec) => codec.mimeType.toLowerCase() === "audio/opus");
        if (opus?.length) transceiver.setCodecPreferences(opus);
      }
      peer.ontrack = (event) => {
        if (mine !== generation) return;
        audio.srcObject = event.streams[0] || new MediaStream([event.track]);
        receiverForBuffer = event.receiver;
        applyBuffer(autoBuffer);
        if (currentPeer.connectionState === "connected") play();
      };
      peer.onconnectionstatechange = () => {
        if (mine !== generation) return;
        if (["failed", "closed"].includes(currentPeer.connectionState)) fail(copy.failed);
        if (currentPeer.connectionState === "connected") {
          lastPacketTime = performance.now();
          clearTimeout(timeout);
          if (audio.paused && audio.srcObject) play();
          if (!timer) { timer = setInterval(() => poll(mine), 2000); poll(mine); }
        }
        if (currentPeer.connectionState === "disconnected") {
          clearTimeout(timeout);
          timeout = setTimeout(() => { if (mine === generation && currentPeer.connectionState === "disconnected") fail(copy.failed); }, 6000);
        }
      };
      peer.onicecandidate = (event) => {
        if (mine !== generation || !event.candidate) return;
        const message = {type:"webrtc-candidate", candidate:event.candidate.candidate, mid:event.candidate.sdpMid};
        if (offerSent) send(message); else localCandidates.push(message);
      };
      socket = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws?token=" + encodeURIComponent(token));
      socket.onclose = socket.onerror = () => { if (mine === generation) fail(copy.failed); };
      messageChain = Promise.resolve();
      socket.onmessage = (event) => {
        messageChain = messageChain.then(async () => {
          if (mine !== generation) return;
          const message = JSON.parse(event.data);
          if (message.type === "webrtc-error") throw new Error(message.message || copy.failed);
          if (message.type === "webrtc-answer") {
            await currentPeer.setRemoteDescription({type:"answer", sdp:message.sdp});
            if (mine !== generation) return;
            const pending = candidates; candidates = [];
            for (const candidate of pending) { await currentPeer.addIceCandidate(candidate); if (mine !== generation) return; }
          } else if (message.type === "webrtc-candidate") {
            const candidate = {candidate:message.candidate, sdpMid:message.mid};
            if (currentPeer.remoteDescription) await currentPeer.addIceCandidate(candidate); else candidates.push(candidate);
          }
        }).catch(() => { if (mine === generation) fail(copy.failed); });
      };
      socket.onopen = async () => {
        try {
          const offer = await currentPeer.createOffer();
          offer.sdp = stereo(offer.sdp);
          await currentPeer.setLocalDescription(offer);
          if (mine !== generation) return;
          send({type:"webrtc-offer", sdp:currentPeer.localDescription.sdp, transportMode:"opus"});
          offerSent = true;
          for (const candidate of localCandidates) send(candidate);
          localCandidates = [];
        } catch (_) { if (mine === generation) fail(copy.failed); }
      };
    } catch (e) { if (mine === generation) fail(e.message || copy.failed,Boolean(e.fatal)); }
  }
  button.addEventListener("click", () => { if (mode === "tap") play(); else { wantsAudio = true; retryAttempts = 0; connect(); } });
  stopButton.addEventListener("click", () => { wantsAudio = false; send({type:"webrtc-stop"}); cleanup(); $("error").hidden = true; state("idle", copy.ready); });
  audio.addEventListener("playing", () => { if (peer && audio.srcObject && !audio.paused) state("playing", copy.playing); });
  audio.addEventListener("play", () => { if (peer && audio.srcObject && !audio.paused) ready(true); });
  audio.addEventListener("pause", () => {
    if (peer && audio.srcObject && audio.paused && mode !== "idle") {
      ready(false); state("tap", copy.paused);
    }
  });
  window.addEventListener("pagehide", () => { suspendedIntent = wantsAudio; wantsAudio = false; cleanup(); });
  window.addEventListener("pageshow", () => { if (suspendedIntent && paired) { suspendedIntent = false; wantsAudio = true; connect(); } });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden || !wantsAudio) return;
    if (mode === "retrying") connect();
    else if (mode === "tap" && audio.paused && peer?.connectionState === "connected") play();
  });
  window.addEventListener("online", () => { if (wantsAudio && mode === "retrying") connect(); });
  // A second QR can change only the fragment in an already-open Safari tab.
  window.addEventListener("hashchange", () => location.reload());
  if ("mediaSession" in navigator) {
    try { navigator.mediaSession.metadata = new MediaMetadata({title:"Live master",artist:"MConnect"}); } catch (_) {}
  }
  state("idle", copy.ready);
  if (!paired || !window.RTCPeerConnection) {
    error(paired ? copy.unsupported : copy.badLink); button.disabled = true;
  } else { wantsAudio = true; connect(); }
})();
