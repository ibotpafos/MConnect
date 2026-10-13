"""HTTP/WebSocket pairing and same-origin checks against a running test host."""
import base64
import http.client
import io
import json
import os
import sys
import socket
import struct
import time
import urllib.parse
import zipfile

info = json.load(open(sys.argv[1]))
url = urllib.parse.urlparse(info["url"])
host, port, token = "127.0.0.1", info["port"], url.fragment

def request(route, headers=None):
    connection = http.client.HTTPConnection(host,port,timeout=5)
    connection.request("GET",route,headers=headers or {})
    response=connection.getresponse()
    return response.status,response.read(),dict(response.getheaders())

assert request("/healthz")[0] == 200
assert request("/info")[0] == 403
assert request("/info?token=wrong")[0] == 403
assert request("/info",{"X-MConnect-Token":token})[0] == 200
status,source,_ = request("/source.zip")
assert status == 200
with zipfile.ZipFile(io.BytesIO(source)) as archive:
    assert "MConnect/src/PluginProcessor.cpp" in archive.namelist()
    assert "MConnect/dependencies.lock.json" in archive.namelist()
assert request("/license")[0] == 200
assert request("/unknown")[0] == 404
status,_,headers = request("/")
assert status == 200 and headers.get("Referrer-Policy") == "no-referrer"
for origin,key,expected in [(f"http://{host}:{port}","wrong",403),("http://untrusted.example",token,403)]:
    connection=http.client.HTTPConnection(host,port,timeout=5)
    connection.request("GET","/ws?token="+key,headers={"Origin":origin,"Upgrade":"websocket","Connection":"Upgrade","Sec-WebSocket-Version":"13","Sec-WebSocket-Key":base64.b64encode(os.urandom(16)).decode()})
    try:
        assert connection.getresponse().status == expected
    except http.client.RemoteDisconnected:
        pass  # CivetWeb denies an upgrade by closing before the 101 response.
    connection.close()
print("PASS: pairing, WebSocket Origin rejection, embedded source/license, static routes and headers")

def websocket():
    stream=socket.create_connection((host,port),timeout=3)
    key=base64.b64encode(os.urandom(16)).decode()
    header=(f"GET /ws?token={token} HTTP/1.1\r\nHost: {host}:{port}\r\nOrigin: http://{host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n")
    stream.sendall(header.encode())
    response=b""
    while b"\r\n\r\n" not in response:
        chunk=stream.recv(4096)
        assert chunk,"WebSocket upgrade unexpectedly denied"
        response+=chunk
    assert b"101 Switching" in response,response[:80]
    return stream

for opcode,fin,payload in [(1,True,b"{"),(1,True,b'{"type":"webrtc-ready","ready":"false"}'),(1,True,b'['*2000+b'0'+b']'*2000),(1,False,b'{"type":"webrtc-'),(2,True,b"binary"),(1,True,b"x"*70000)]:
    stream=websocket();mask=os.urandom(4);size=len(payload)
    frame=bytes([(128 if fin else 0)|opcode])
    frame+=bytes([128|size]) if size<126 else bytes([128|126])+struct.pack('!H',size) if size<65536 else bytes([128|127])+struct.pack('!Q',size)
    frame+=mask+bytes(value^mask[index%4] for index,value in enumerate(payload))
    stream.sendall(frame)
    try:
        reply=stream.recv(4096)
        assert not reply or reply[0]&15==8,"invalid message accepted"
    except ConnectionResetError: pass
    stream.close();time.sleep(.05)
# Early closes must not permanently consume the four receiver slots.
for _ in range(12):
    stream=websocket();stream.close();time.sleep(.05)
stream=websocket();stream.close()
print("PASS: malformed/deep JSON, incorrect types, fragmented/binary/oversized frames and repeated slot release")
