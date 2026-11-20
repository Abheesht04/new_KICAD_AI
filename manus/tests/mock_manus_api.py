#!/usr/bin/env python3
"""A local stand-in for the Manus API v2 (test harness for manus/workspace/copper-prompt.ps1; no account, no credits).

Manus is used only for prompt ANALYSIS. This mock implements task.create, task.sendMessage (POST) and task.detail,
task.listMessages (GET) with the documented shapes (https://open.manus.ai/docs/v2/...) and behaves like an imperfect
analyser so the client's guards are exercised:
  - after task.create the task goes to "waiting" (the agent wants to ask something);
  - the follow-up ("Continue ...") yields the structured analysis given on the command line, which may contain a value
    the prompt never states (Copper must ignore it).
It rejects a request that is not analysis-only (any design data such as a parts list), and any key other than
"mock-key": the real key must never be sent anywhere but api.manus.ai. Every request is logged.

Usage: mock_manus_api.py REQUIREMENTS.json LOG.json   (prints "listening on port N")
"""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ANALYSIS = json.load(open(sys.argv[1], encoding='utf-8'))
LOG = sys.argv[2]
TASK = 'MockTask0000000000000A'
state = {'events': [], 'n': 0, 'stage': 'created', 'requests': [], 'credits': 0}
lock = threading.Lock()


def event(kind, payload):
    state['n'] += 1
    e = {'id': f'ev{state["n"]:04d}', 'type': kind, 'timestamp': 1763600000000 + state['n']}
    e[kind] = payload
    state['events'].append(e)


class H(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass

    def _record(self, method, path, body):
        state['requests'].append({'method': method, 'path': path, 'body': body,
                                  'key_is_mock': self.headers.get('x-manus-api-key') == 'mock-key'})
        with open(LOG, 'w', encoding='utf-8') as f:
            json.dump(state['requests'], f, indent=1)

    def _auth(self):
        if self.headers.get('x-manus-api-key') != 'mock-key':
            self._send(401, {'ok': False, 'request_id': 'r', 'error': {'code': 'unauthenticated', 'message': 'invalid api key'}})
            return False
        return True

    def do_POST(self):
        n = int(self.headers.get('Content-Length', '0'))
        body = json.loads(self.rfile.read(n).decode('utf-8'))
        p = urlparse(self.path).path
        with lock:
            self._record('POST', p, body)
            if not self._auth():
                return
            if p == '/v2/task.create':
                content = body['message']['content']
                schema = body.get('structured_output_schema') or {}
                analysis_only = ('USER REQUEST' in content and 'PARTS LIST' not in content
                                 and 'components' not in json.dumps(schema) and 'evidence' in json.dumps(schema))
                if body.get('interactive_mode') is not False or not analysis_only:
                    return self._send(400, {'ok': False, 'request_id': 'r', 'error': {'code': 'invalid_argument', 'message': 'not an analysis-only request'}})
                event('user_message', {'content': '(request)'})
                event('status_update', {'agent_status': 'running'})
                event('status_update', {'agent_status': 'waiting', 'status_detail': {'waiting_for_event_type': 'messageAskUser', 'waiting_description': 'Is the load inductive?'}})
                state['stage'] = 'waiting'
                return self._send(200, {'ok': True, 'request_id': 'r', 'task_id': TASK, 'task_title': 'generating title...',
                                        'task_url': f'https://manus.im/app/{TASK}', 'share_visibility': 'private'})
            if p == '/v2/task.sendMessage':
                event('user_message', {'content': body['message']['content'][:200]})
                event('status_update', {'agent_status': 'running'})
                event('assistant_message', {'content': 'Analysis complete.', 'attachments': []})
                event('status_update', {'agent_status': 'stopped'})
                event('structured_output_result', {'success': True, 'value': ANALYSIS, 'error': None})
                state['credits'] += 3
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
