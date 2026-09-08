"""Bounded real-socket WS/WSS conformance tests; no WuKongIM process required."""
import argparse
import asyncio
import base64
import json
from pathlib import Path
import ssl
import subprocess
import tempfile
import websockets

async def scenario(binary, mode, context=None, ca=None):
    connections = 0
    errors = []

    async def handler(ws):
        nonlocal connections
        connections += 1
        connection = connections
        pings = 0
        acked = False
        async def send(value):
            await ws.send(json.dumps(value, ensure_ascii=False))
        async def event(kind, data=None):
            await send({'method': 'event', 'params': {'id': kind, 'type': kind, 'timestamp': 0, 'data': data}})
        try:
            async for raw in ws:
                request = json.loads(raw)
                method = request['method']
                if method == 'connect':
                    assert isinstance(request['id'], str)
                    assert request['params']['uid'] == 'alice'
                    assert request['params']['token'] == 'CANARY_TOKEN'
                    assert request['params']['deviceFlag'] == 2
                    assert request['params']['deviceId'] == 'device-alice'
                    if mode in ('connect_timeout', 'destroy_connect'):
                        continue
                    if mode == 'reconnect_exhausted' and connection > 1:
                        await ws.close()
                        return
                    if mode == 'bad_auth':
                        await send({'id': request['id'], 'error': {'code': 2, 'message': 'CANARY_TOKEN', 'data': 'CANARY_TOKEN'}})
                        continue
                    await send({'id': request['id'], 'result': {'reasonCode': 2 if mode == 'bad_connack' else 1}})
                    if mode in ('reconnect', 'cancel_reconnect', 'reconnect_exhausted') and connection == 1:
                        await ws.close()
                    if mode == 'server_disconnect':
                        await send({'method': 'disconnect', 'params': {'reasonCode': 12, 'reason': 'CANARY_TOKEN'}})
                    if mode == 'malformed':
                        await ws.send('{broken CANARY_TOKEN')
                    if mode == 'oversize':
                        await ws.send('x' * (1024 * 1024 + 1))
                    if mode in ('queue_limit', 'destroy_callback'):
                        await asyncio.sleep(0.1) # Give the client time to attach the test-only listener.
                        await event('barrier')
                    if mode == 'cancel_reconnect' and connection == 2:
                        await asyncio.sleep(0.3)
                        assert connections == 2
                        await event('stable')
                elif method == 'send':
                    params = request['params']
                    assert isinstance(request['id'], str)
                    assert params['channelId'] == 'bob' and params['channelType'] == 1
                    assert params['header']['redDot'] is True
                    assert params['clientMsgNo']
                    payload = json.loads(base64.b64decode(params['payload'], validate=True))
                    if mode in ('send_timeout', 'pending_disconnect', 'queue_limit'):
                        continue
                    if mode == 'send_error':
                        await send({'id': request['id'], 'error': {'code': 11, 'message': 'CANARY_TOKEN'}})
                        continue
                    assert payload == {'type': 1, 'content': '你好 🌏'}
                    response = {'id': request['id'], 'result': {'messageId': '9007199254740993', 'messageSeq': 42, 'reasonCode': 1}}
                    await send(response)
                    await send(response) # Duplicate response must not emit a second SENDACK.
                    await send({'id': 'unknown', 'result': {}})
                    await send({'method': 'recv', 'params': {'header': {}, 'messageId': 18446744073709551615,
                        'messageSeq': 43, 'timestamp': 1700000000, 'channelId': 'alice', 'channelType': 1,
                        'fromUid': 'bob', 'payload': params['payload']}})
                    await event('changed', '{"ok":true}')
                elif method == 'recvack':
                    assert 'id' not in request
                    assert request['params']['messageId'] == '18446744073709551615'
                    assert request['params']['messageSeq'] == 43
                    acked = True
                elif method == 'ping':
                    if mode in ('heartbeat_timeout', 'uncorrelated_pong') and connection == 1:
                        if mode == 'uncorrelated_pong':
                            await send({'id': 'wrong-id', 'result': None})
                            await send({'method': 'pong', 'params': {}})
                        continue
                    pings += 1
                    await send({'id': request['id'], 'result': None})
                    if pings == 3 and acked:
                        await event('verified')
                else:
                    raise AssertionError('Unexpected method')
        except websockets.ConnectionClosed:
            pass
        except Exception as error:
            errors.append(str(error) or type(error).__name__)
            await ws.close()

    server = await websockets.serve(handler, '127.0.0.1', 0, ssl=context)
    port = server.sockets[0].getsockname()[1]
    host = '127.0.0.1' if mode == 'tls_host' else 'localhost'
    url = f'{"wss" if context else "ws"}://{host}:{port}/ws'
    proc = await asyncio.create_subprocess_exec(binary, url, mode, *([str(ca)] if ca else []),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        out, err = await asyncio.wait_for(proc.communicate(), 10)
        assert proc.returncode == 0, f'{mode}: {err.decode()} {out.decode()}'
        assert not errors, f'{mode}: {errors}'
        assert b'CANARY' not in out + err, 'Sensitive material escaped through SDK diagnostics'
        print(out.decode().strip())
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        server.close()
        await server.wait_closed()

async def upgrade_timeout(binary):
    clients = set()
    async def stall(reader, writer):
        clients.add(writer)
        try:
            await reader.read()
        finally:
            writer.close()
            await writer.wait_closed()
            clients.discard(writer)
    server = await asyncio.start_server(stall, '127.0.0.1', 0)
    port = server.sockets[0].getsockname()[1]
    proc = await asyncio.create_subprocess_exec(binary, f'ws://127.0.0.1:{port}/ws', 'upgrade_timeout',
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        out, err = await asyncio.wait_for(proc.communicate(), 10)
        assert proc.returncode == 0, err.decode()
        print(out.decode().strip())
    finally:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        server.close()
        await server.wait_closed()
        for writer in list(clients):
            writer.close()

async def main(binary):
    for mode in ('exchange', 'bad_auth', 'bad_connack', 'connect_timeout', 'destroy_connect', 'send_error',
                 'send_timeout', 'pending_disconnect', 'heartbeat_timeout', 'uncorrelated_pong', 'reconnect',
                 'reconnect_exhausted', 'cancel_reconnect', 'server_disconnect', 'malformed', 'oversize',
                 'queue_limit', 'listener_cleanup', 'destroy_callback'):
        await scenario(binary, mode)
    await upgrade_timeout(binary)
    with tempfile.TemporaryDirectory(prefix='wukong-cpp-tls-') as directory:
        root = Path(directory)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost',
            '-addext', 'subjectAltName=DNS:localhost'], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        await scenario(binary, 'tls_ok', context, cert)
        await scenario(binary, 'tls_untrusted', context)
        await scenario(binary, 'tls_host', context, cert)
    print('23 WS/WSS integration scenarios passed')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', required=True)
    args = parser.parse_args()
    asyncio.run(main(str(Path(args.client).resolve())))
