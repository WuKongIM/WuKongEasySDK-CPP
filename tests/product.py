"""Start an isolated 256-hash-slot single-node cluster and exercise public endpoints."""
import argparse
import json
import os
from pathlib import Path
import secrets
import selectors
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def free_ports(count):
    sockets = [socket.socket() for _ in range(count)]
    try:
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


def request(base, path, data=None):
    body = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(base + path, data=body, headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=2) as response:
        return response.read()


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--js-entry', help='Built reference SDK dist/cjs/index.js; optional real JS interop')
    args = parser.parse_args()
    server, client = str(Path(args.server).resolve()), str(Path(args.client).resolve())
    cluster, api, gateway, manager = free_ports(4)
    base = f'http://127.0.0.1:{api}'
    url = f'ws://127.0.0.1:{gateway}/ws'
    with tempfile.TemporaryDirectory(prefix='wk-cpp-product-') as directory:
        root = Path(directory)
        config = root / 'wukongim.toml'
        config.write_text(f'''[node]
id = 1
data_dir = "{root / 'data'}"
[cluster]
id = "cpp-sdk-test"
listen_addr = "127.0.0.1:{cluster}"
nodes = [{{id=1,addr="127.0.0.1:{cluster}"}}]
initial_slot_count = 8
hash_slot_count = 256
slot_replica_n = 1
[api]
listen_addr = "127.0.0.1:{api}"
[manager]
listen_addr = "127.0.0.1:{manager}"
[gateway]
token_auth_on = true
listeners = [{{name="ws",network="websocket",address="127.0.0.1:{gateway}",transport="gnet",protocol="wsmux",path="/ws"}}]
[log]
level = "error"
console = false
dir = "{root / 'logs'}"
''')
        env = {key: value for key, value in os.environ.items() if not key.startswith('WK_')}
        env['WKIM_ALICE_TOKEN'], env['WKIM_BOB_TOKEN'] = secrets.token_hex(16), secrets.token_hex(16)
        process = peer = None
        with (root / 'server.log').open('wb') as output:
            try:
                process = subprocess.Popen([server, '-config', str(config)], cwd=root, env=env, stdout=output, stderr=output)
                deadline = time.monotonic() + 40
                while True:
                    if process.poll() is not None:
                        raise RuntimeError('Product server exited before readiness')
                    try:
                        request(base, '/readyz')
                        break
                    except (OSError, urllib.error.URLError):
                        if time.monotonic() >= deadline:
                            raise TimeoutError('Product readiness timeout')
                        time.sleep(0.1)
                for uid, token in [('cpp-alice', env['WKIM_ALICE_TOKEN']), ('cpp-bob', env['WKIM_BOB_TOKEN'])]:
                    request(base, '/user/token', {'uid': uid, 'token': token, 'device_flag': 2, 'device_level': 1})
                subprocess.run([client, url, 'cpp'], env=env, check=True, timeout=40)
                deadline = time.monotonic() + 10
                while True:
                    statuses = json.loads(request(base, '/user/onlinestatus', ['cpp-alice', 'cpp-bob']))
                    if all(item.get('online', 0) == 0 for item in statuses):
                        break
                    if time.monotonic() > deadline:
                        raise TimeoutError('Online presence did not clear after disconnect')
                    time.sleep(0.05)
                if args.js_entry:
                    env['WKIM_JS_ENTRY'] = str(Path(args.js_entry).resolve())
                    env['WKIM_URL'] = url
                    peer = subprocess.Popen(['node', str(Path(__file__).with_name('js_peer.cjs'))], env=env,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    with selectors.DefaultSelector() as selector:
                        selector.register(peer.stdout, selectors.EVENT_READ)
                        if not selector.select(timeout=10) or peer.stdout.readline() != b'READY\n':
                            raise RuntimeError('Reference JS SDK did not connect')
                    subprocess.run([client, url, 'js'], env=env, check=True, timeout=30)
                print('Product public-endpoint acceptance passed: token auth ON, 256 hash slots, clean disconnect')
            except Exception:
                # Print bounded diagnostics without credentials, raw frames or message payloads.
                print('Product acceptance failed; isolated process is being stopped')
                raise
            finally:
                stop(peer)
                stop(process)


if __name__ == '__main__':
    main()
