#!/usr/bin/env python3
"""A local stand-in for the Manus API v2 (test harness for manus/workspace/copper-prompt.ps1; no Manus account used).

Implements task.create, task.sendMessage (POST) and task.detail, task.listMessages (GET) with the documented shapes
(https://open.manus.ai/docs/v2/...). It behaves like an imperfect model so the client's guards are exercised:
  - after task.create the task goes to "waiting" (the agent wants to ask something);
  - the first follow-up ("Continue ...") yields a structured result whose LED polarity is swapped (a hallucination);
  - a follow-up carrying the grounding-gate findings (PIN_NAME_MISMATCH) yields the correct intent.
Every request must carry x-manus-api-key "mock-key": the real key must never be sent anywhere but api.manus.ai.
All requests are logged to the file given as the second argument.

Usage: mock_manus_api.py GOOD_INTENT.json LOG.json   (prints "listening on port N")
"""
import copy
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

GOOD = json.load(open(sys.argv[1], encoding='utf-8'))
LOG = sys.argv[2]
BAD = copy.deepcopy(GOOD)
for comp in BAD['components']:
    if comp['symbol_id'] == 'Device:LED':
        for pin in comp['pins']:
            pin['pin_name'] = {'K': 'A', 'A': 'K'}.get(pin['pin_name'], pin['pin_name'])
TASK = 'MockTask0000000000000A'
state = {'events': [], 'n': 0, 'stage': 'created', 'requests': [], 'credits': 0}
lock = threading.Lock()


def event(kind, payload):
    state['n'] += 1
    e = {'id': f'ev{state["n"]:04d}', 'type': kind, 'timestamp': 1763600000000 + state['n']}
    e[kind] = payload
    state['events'].append(e)


def result(intent, note):
    event('assistant_message', {'content': note, 'attachments': []})
    event('status_update', {'agent_status': 'stopped'})
    event('structured_output_result', {'success': True, 'value': {
        'intent': intent,
        'assumptions': ['5 V supply on J1', 'about 3 mA LED current with 1k'],
        'open_questions': ['Which LED colour and brightness are wanted?']}, 'error': None})
    state['credits'] += 7


class H(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth(self):
        if self.headers.get('x-manus-api-key') != 'mock-key':
            self._send(401, {'ok': False, 'request_id': 'r', 'error': {'code': 'unauthenticated', 'message': 'unexpected key'}})
            return False
        return True

    def log_message(self, *a):
        pass

    def _record(self, method, path, body):
        state['requests'].append({'method': method, 'path': path, 'body': body,
                                  'key_is_mock': self.headers.get('x-manus-api-key') == 'mock-key'})
        with open(LOG, 'w', encoding='utf-8') as f:
            json.dump(state['requests'], f, indent=1)

    def do_POST(self):
        n = int(self.headers.get('Content-Length', '0'))
        body = json.loads(self.rfile.read(n).decode('utf-8'))
        p = urlparse(self.path).path
        with lock:
            self._record('POST', p, body)
            if not self._auth():
                return
            if p == '/v2/task.create':
                ok = (body.get('interactive_mode') is False and isinstance(body.get('structured_output_schema'), dict)
                      and 'PARTS LIST' in body['message']['content'])
                if not ok:
                    return self._send(400, {'ok': False, 'request_id': 'r', 'error': {'code': 'invalid_argument', 'message': 'missing schema / parts list / interactive_mode'}})
                event('user_message', {'content': '(request)'})
                event('status_update', {'agent_status': 'running'})
                event('status_update', {'agent_status': 'waiting', 'status_detail': {'waiting_for_event_type': 'messageAskUser', 'waiting_description': 'Which LED colour?'}})
                state['stage'] = 'waiting'
                return self._send(200, {'ok': True, 'request_id': 'r', 'task_id': TASK, 'task_title': 'generating title...',
                                        'task_url': f'https://manus.im/app/{TASK}', 'share_visibility': 'private'})
            if p == '/v2/task.sendMessage':
                text = body['message']['content']
                event('user_message', {'content': text[:200]})
                event('status_update', {'agent_status': 'running'})
                if state['stage'] == 'waiting' and text.startswith('Continue'):
                    result(BAD, 'Here is the design.')
                    state['stage'] = 'first_result'
                elif 'PIN_NAME_MISMATCH' in text:
                    result(GOOD, 'Corrected the LED pin names from the parts list.')
                    state['stage'] = 'corrected'
                else:
                    result(BAD, 'Unchanged.')
                return self._send(200, {'ok': True, 'request_id': 'r', 'task_id': TASK})
        self._send(404, {'ok': False, 'error': {'code': 'not_found', 'message': p}})

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        with lock:
            self._record('GET', u.path, q)
            if not self._auth():
                return
            if u.path == '/v2/task.listMessages':
                return self._send(200, {'ok': True, 'request_id': 'r', 'task_id': TASK, 'messages': state['events'], 'has_more': False})
            if u.path == '/v2/task.detail':
                last = [e for e in state['events'] if e['type'] == 'status_update'][-1]['status_update']['agent_status']
                return self._send(200, {'ok': True, 'request_id': 'r', 'task': {'id': TASK, 'status': last, 'has_running_background_jobs': False,
                                                                                'credit_usage': state['credits'], 'task_url': f'https://manus.im/app/{TASK}'}})
        self._send(404, {'ok': False, 'error': {'code': 'not_found', 'message': u.path}})


srv = ThreadingHTTPServer(('127.0.0.1', 0), H)
print(f'listening on port {srv.server_address[1]}', flush=True)
srv.serve_forever()
