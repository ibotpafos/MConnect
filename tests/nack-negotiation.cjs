/* Check actual native answers for current and older/non-NACK receivers.
 * Browser produces valid ICE/DTLS SDP; only offered RTCP feedback varies.
 * No audio device or DAW. node tests/nack-negotiation.cjs host-info.json
 */
const fs = require('node:fs'), assert = require('node:assert/strict');
const {chromium} = require(process.env.MCONNECT_PLAYWRIGHT || 'playwright');
(async () => {
  const info = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
  const url = new URL(info.url), token = url.hash.slice(1); url.hostname = '127.0.0.1'; url.hash = '';
  const browser = await chromium.launch({headless:true});
  try {
    const page = await browser.newPage(); await page.goto(url.href);
    const results = [];
    for (const [variant, expected] of [['none',false], ['exact-before-codec',true], ['wildcard',true], ['pli-only',false], ['other-payload',false]]) {
      const result = await page.evaluate(async ({variant,token}) => {
        const peer = new RTCPeerConnection({iceServers:[]});
        const transceiver = peer.addTransceiver('audio', {direction:'recvonly'});
        transceiver.setCodecPreferences(RTCRtpReceiver.getCapabilities('audio').codecs.filter(codec => codec.mimeType.toLowerCase() === 'audio/opus'));
        const socket = new WebSocket('ws://' + location.host + '/ws?token=' + token);
        try {
          await new Promise((resolve,reject) => { socket.onopen = resolve; socket.onerror = reject; });
          const offer = await peer.createOffer(); await peer.setLocalDescription(offer);
          const lines = offer.sdp.split(/\r?\n/).filter(line => !line.startsWith('a=rtcp-fb:'));
          const index = lines.findIndex(line => /^a=rtpmap:\d+ opus\/48000/.test(line));
          const pt = lines[index].match(/^a=rtpmap:(\d+)/)[1];
          const feedback = variant === 'exact-before-codec' ? `${pt} nack` : variant === 'wildcard' ? '* nack'
            : variant === 'pli-only' ? `${pt} nack pli` : variant === 'other-payload' ? `${Number(pt) === 112 ? 113 : 112} nack` : null;
          if (feedback) lines.splice(index,0,'a=rtcp-fb:' + feedback);
          const answer = await new Promise((resolve,reject) => {
            const deadline = setTimeout(() => reject(new Error('answer timeout: ' + variant)),5000);
            socket.onmessage = event => {
              const message = JSON.parse(event.data);
              if (message.type === 'webrtc-answer') { clearTimeout(deadline); resolve(message.sdp); }
              if (message.type === 'webrtc-error') { clearTimeout(deadline); reject(new Error(message.message)); }
            };
            socket.send(JSON.stringify({type:'webrtc-offer',sdp:lines.join('\r\n'),transportMode:'opus'}));
          });
          return {variant,opusPayload:Number(pt),feedback:answer.split(/\r?\n/).filter(line => line.startsWith('a=rtcp-fb:'))};
        } finally { socket.close(); peer.close(); }
      }, {variant,token});
      assert.equal(result.feedback.includes(`a=rtcp-fb:${result.opusPayload} nack`), expected, variant);
      results.push(result); await page.waitForTimeout(100);
    }
    console.log(JSON.stringify({result:'PASS',cases:results},null,2));
  } finally { await browser.close(); }
})().catch(error => { console.error('FAIL:',error.message); process.exitCode = 1; });
