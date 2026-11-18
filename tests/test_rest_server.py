#!/usr/bin/env python3
"""copper-core serve-http over real HTTP (a test harness; the server is C, src/v2_src/rest_server.c).

Starts the server on a free localhost port with the repository as root, drives every route with urllib, checks
statuses and bodies, then POST /v1/shutdown and expects the process to exit 0.
Usage: test_rest_server.py COPPER_CORE"""
import http.client, json, subprocess, sys, time, urllib.error, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / 'build-local/copper-core')


def request(base, method, path, body=None, content_type='application/json'):
    data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
    headers = {'Content-Type': content_type} if content_type else {}
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'null')


def main():
    proc = subprocess.Popen([CORE, 'serve-http', '--port', '0', '--root', str(ROOT)], stdout=subprocess.PIPE, text=True)
    try:
        line = proc.stdout.readline()
        assert line.startswith('listening on port '), line
        base = f'http://127.0.0.1:{int(line.split()[-1])}'
        status, body = request(base, 'GET', '/v1/health')
        assert (status, body['status']) == (200, 'ok'), (status, body)
        status, body = request(base, 'GET', '/v1/tools')
        assert status == 200 and [t['name'] for t in body['result']['tools']] == ['board.load', 'dfm.check', 'export.fab', 'excellon.check'], body
        status, body = request(base, 'POST', '/v1/tools/board.load', {'path': 'examples/generated/divider/design.kicad_pcb'})
        assert status == 200 and body['result']['footprints'] == 3 and body['result']['pads'] == 7, body
        bid = body['result']['board_id']
        status, body = request(base, 'POST', '/v1/tools/dfm.check', {'board_id': bid, 'profile': 'fab_profiles/jlcpcb_2layer.json'})
        rules = sorted({f['rule'] for f in body['result']['report']['findings'] if f['status'] == 'FAIL'})
        assert status == 200 and body['result']['report']['verdict'] == 'FAIL' and rules == ['clearance/pth_to_track_min_mm'], body
        status, body = request(base, 'POST', '/v1/tools/excellon.check', {'path': 'manufacturing_audit/drill_test/design.drl', 'board_id': bid})
        assert status == 200 and body['result']['report']['verdict'] == 'PASS', body
        status, body = request(base, 'POST', '/v1/tools/board.load', {'path': 'missing.kicad_pcb'})
        assert status == 404 and body['error']['data']['kind'] == 'BOARD_NOT_FOUND', (status, body)
        status, body = request(base, 'POST', '/v1/tools/board.load', b'not json')
        assert status == 400 and body['error']['data']['kind'] == 'BAD_ARGUMENTS', (status, body)
        status, body = request(base, 'GET', '/nowhere')
        assert status == 404, status
        status, body = request(base, 'POST', '/v1/rpc', {'jsonrpc': '2.0', 'id': 'x', 'method': 'initialize'})
        assert status == 200 and body['id'] == 'x' and body['result']['protocolVersion'] == 'copper.tools/1', body
        # A connection that sends nothing (a browser's speculative preconnect) must not block the next request.
        import socket
        idle = [socket.create_connection(('127.0.0.1', int(base.rsplit(':', 1)[1]))) for _ in range(2)]
        t0 = time.monotonic()
        status, body = request(base, 'GET', '/v1/health')
        waited = time.monotonic() - t0
        for s in idle:
            s.close()
        assert status == 200 and waited < 10, (status, waited)
        # No page without --ui; a POST must be JSON (a cross-site page cannot send that type without a preflight).
        status, body = request(base, 'GET', '/')
        assert status == 404 and body['error']['data']['kind'] == 'NO_PAGE', (status, body)
        for ctype in ('text/plain', 'application/x-www-form-urlencoded'):
            status, body = request(base, 'POST', '/v1/tools/board.load', {'path': 'examples/generated/divider/design.kicad_pcb'}, ctype)
            assert status == 415 and body['error']['data']['kind'] == 'UNSUPPORTED_MEDIA_TYPE', (ctype, status, body)
        conn = http.client.HTTPConnection('127.0.0.1', int(base.rsplit(':', 1)[1]), timeout=60)   # no Content-Type at all
        conn.request('POST', '/v1/tools/board.load', body=b'{"path": "examples/generated/divider/design.kicad_pcb"}')
        resp = conn.getresponse()
        status, body = resp.status, json.loads(resp.read())
        conn.close()
        assert status == 415 and body['error']['data']['kind'] == 'UNSUPPORTED_MEDIA_TYPE', (status, body)
        status, body = request(base, 'POST', '/v1/tools/board.load', {'path': 'examples/generated/divider/design.kicad_pcb'}, 'Application/JSON; charset=utf-8')
        assert status == 200, (status, body)
        status, body = request(base, 'POST', '/v1/shutdown')
        assert status == 200, status
        rc = proc.wait(timeout=30)
        assert rc == 0, rc
    finally:
        if proc.poll() is None:
            proc.kill()
    # With --ui the page is served at GET / as HTML, and the JSON API is unchanged.
    page = ROOT / 'apps' / 'web' / 'index.html'
    proc = subprocess.Popen([CORE, 'serve-http', '--port', '0', '--root', str(ROOT), '--ui', str(page)], stdout=subprocess.PIPE, text=True)
    try:
        line = proc.stdout.readline()
        assert line.startswith('listening on port '), line
        base = f'http://127.0.0.1:{int(line.split()[-1])}'
        with urllib.request.urlopen(base + '/', timeout=60) as r:
            assert r.status == 200 and r.headers['Content-Type'].startswith('text/html'), (r.status, r.headers['Content-Type'])
            assert r.read() == page.read_bytes()
        status, body = request(base, 'GET', '/v1/health')
        assert (status, body['status']) == (200, 'ok'), (status, body)
        status, body = request(base, 'POST', '/v1/shutdown')
        assert status == 200 and proc.wait(timeout=30) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
    bad = subprocess.run([CORE, 'serve-http', '--port', '0', '--ui', str(ROOT / 'no-such-page.html')], capture_output=True, text=True, timeout=30)
    assert bad.returncode == 64 and 'cannot read the page' in bad.stderr, (bad.returncode, bad.stderr)
    print('rest_server_http: every route answered as expected; the page is served with --ui; non-JSON POSTs are refused; the server stopped cleanly')
    return 0


if __name__ == '__main__':
    sys.exit(main())
