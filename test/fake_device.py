#!/usr/bin/env python3
"""A fake Hertzinno online acoustic camera (HZ-FA-371, API V5.1) for tests.

HTTP: token login (md5(password + "lfxs")), detection settings, video settings, leak
estimation, detection area, detections that last detect_times seconds and leave a log with
photos and videos, log queries, file downloads and deletion.
WebSocket on the same port: fftData (5 Hz), prpdData and voiceprintData (1 Hz),
detectFeedback after each detection; clients silent for --ws-idle seconds are dropped, as the
manual says the camera does.
RTSP: answers DESCRIBE for --rtsp-path only (no media), to test path discovery.

Every request is logged as "METHOD path body" (--log) for the smoke test, and GET /debug/state
reports counters for the unit tests. Python 3.8, standard library only.
"""

import argparse
import base64
import hashlib
import json
import math
import secrets
import select
import socket
import socketserver
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
JPEG = base64.b64decode(
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDABQODxIPDRQSEBIXFRQYHjIhHhwcHj0sLiQySUBMS0dARkVQWnNiUFVtVkVGZIhl'
    'bXd7gYKBTmCNl4x9lnN+gXz/2wBDARUXFx4aHjshITt8U0ZTfHx8fHx8fHx8fHx8fHx8fHx8fHx8fHx8fHx8fHx8fHx8fHx8'
    'fHx8fHx8fHx8fHx8fHz/wAARCAAYACADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA'
    'AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6'
    'Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG'
    'x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA'
    'AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5'
    'OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE'
    'xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDFFv7U8W/tWiLf2p4t/asKNY6nXM4W/tTx'
    'b+1aIt/ani39q9ijWM3XLYt/ani39qKK+VoyZ5jmx4t/ani39qKK9ijJmbmz/9k=')
MP4 = b'\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom' + b'\x00' * 64
FFT_PEAK_BIN, FFT_PEAK = 320, 3000          # 30 kHz at 96 kHz / 1024
FFT_SIDE_BIN, FFT_SIDE = 100, 1500          # 9.4 kHz, outside the default band
RESOLUTIONS = {(2592, 1944), (1920, 1080), (1280, 720), (720, 480), (640, 480)}
LEAK_LIMITS = {
    'leakage_distance': (0, 3, True), 'leakage_unit': (0, 1, True), 'monetary_unit': (0, 4, True),
    'cost_display': (0, 1, True), 'working_hours': (0, 8784, True), 'value_of_gas': (0, 1e9, False),
    'energy_cost': (0, 1e9, False), 'specific_power': (0, 1e9, False),
}
CURRENCIES = ['CNY', 'GBP', 'USD', 'EUR', 'RUB']
PD_RESULTS = {
    'noise': (1, 3, (0.97, 0.01, 0.01, 0.01)),
    'corona': (2, 21, (0.05, 0.85, 0.05, 0.05)),
    'surface': (4, 17, (0.02, 0.03, 0.05, 0.90)),
    'floating': (8, 25, (0.04, 0.03, 0.88, 0.05)),
}


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def ws_frame(opcode, payload, fin=True):
    n = len(payload)
    head = bytes([(0x80 if fin else 0) | opcode])
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack('>H', n)
    else:
        head += bytes([127]) + struct.pack('>Q', n)
    return head + payload


def ws_parse(buf):
    """(fin, opcode, masked, payload, used) of the first frame, or None if incomplete."""
    if len(buf) < 2:
        return None
    fin, opcode = buf[0] & 0x80, buf[0] & 0x0f
    masked, n, pos = buf[1] & 0x80, buf[1] & 0x7f, 2
    if n == 126:
        if len(buf) < 4:
            return None
        n, pos = struct.unpack('>H', buf[2:4])[0], 4
    elif n == 127:
        if len(buf) < 10:
            return None
        n, pos = struct.unpack('>Q', buf[2:10])[0], 10
    mask = b''
    if masked:
        if len(buf) < pos + 4:
            return None
        mask, pos = buf[pos:pos + 4], pos + 4
    if len(buf) < pos + n:
        return None
    payload = bytearray(buf[pos:pos + n])
    if masked:
        for i in range(n):
            payload[i] ^= mask[i % 4]
    return bool(fin), opcode, bool(masked), bytes(payload), pos + n


class WsConn:
    def __init__(self, sock, path):
        self.sock, self.path = sock, path
        self.lock = threading.Lock()
        self.alive = True

    def send(self, opcode, payload, fin=True):
        with self.lock:
            if not self.alive:
                return False
            try:
                self.sock.sendall(ws_frame(opcode, payload, fin))
                return True
            except OSError:
                self.alive = False
                return False


class Device:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.log_lock = threading.Lock()
        self.log_file = open(args.log, 'a') if args.log else None
        self.tokens = {}
        self.counters = {'logins': 0, 'failed_logins': 0, 'heartbeats': 0, 'pongs': 0,
                         'unmasked': 0, 'ws_opened': 0, 'ws_idle_closed': 0, 'detections': 0}
        self.system = {'work_mode': args.mode, 'dB_limit': 10, 'freq_down_limit': 20,
                       'freq_up_limit': 40, 'detect_times': 3, 'samplerate': args.samplerate}
        self.video = {'video_bit_rate': 4096, 'video_fps': 25, 'video_height': 1080,
                      'video_port': 443, 'video_width': 1920}
        self.leakage = {'cost_display': 0, 'leakage_distance': 0, 'leakage_unit': 0,
                        'leakage_unit_str': 'l/min', 'monetary_unit': 0,
                        'monetary_unit_str': 'CNY', 'energy_cost': 0.0, 'specific_power': 0.0,
                        'value_of_gas': 0.0, 'working_hours': 0}
        self.area = {'area_height': -1, 'area_width': -1, 'detect_area_status': 0,
                     'left_top_x': -1, 'left_top_y': -1}
        self.detecting = False
        self.generation = 0
        self.logs = []
        self.files = {}
        self.next_id = 1
        self.feedback = set()
        self.result_name = args.result or ('leak' if args.mode == 2 else 'surface')

    def log(self, line):
        with self.log_lock:
            if self.log_file:
                self.log_file.write(line + '\n')
                self.log_file.flush()

    def count(self, name):
        with self.lock:
            self.counters[name] += 1

    # ---- auth
    def login(self, body):
        try:
            req = json.loads(body or b'{}')
        except ValueError:
            return None
        if req.get('username') != 'admin' or req.get('password') != md5(self.args.password + 'lfxs'):
            self.count('failed_logins')
            return None
        token = secrets.token_hex(16)
        with self.lock:
            ttl = self.args.token_ttl
            self.tokens[token] = time.monotonic() + ttl if ttl > 0 else float('inf')
            self.counters['logins'] += 1
        return token

    def valid(self, header):
        if not header or not header.startswith('Bearer '):
            return False
        with self.lock:
            expiry = self.tokens.get(header[len('Bearer '):].strip())
        return expiry is not None and time.monotonic() < expiry

    # ---- detection
    def start(self):
        with self.lock:
            if self.detecting:
                return
            self.detecting = True
            self.generation += 1
            gen = self.generation
        threading.Thread(target=self._run, args=(gen,), daemon=True).start()

    def stop(self):
        with self.lock:
            self.detecting = False
            self.generation += 1

    def _run(self, gen):
        while True:
            start = time.time()
            with self.lock:
                seconds = self.system['detect_times']
            end_at = time.monotonic() + seconds
            while time.monotonic() < end_at:
                time.sleep(0.02)
                with self.lock:
                    if self.generation != gen:
                        return
            result = self._finish(start, time.time())
            if not self.args.no_feedback:
                self.broadcast(json.dumps(result).encode())
            with self.lock:
                if self.generation != gen:
                    return
                if not self.args.continuous:
                    self.detecting = False
                    return

    def _finish(self, start, end):
        mode = self.system['work_mode']
        stamp = time.strftime('%Y_%m_%d_%H_%M_%S', time.localtime(start)) + \
            '_%03d' % int((start % 1) * 1000)
        result = {
            'detect_mode': mode,
            'start_time': time.strftime('%Y%m%d_%H%M%S', time.localtime(start)),
            'end_time': time.strftime('%Y%m%d_%H%M%S', time.localtime(end)),
            'saving_path': stamp + '/',
            'power_spect_photo_name': 'power_spectrum_%s.jpg' % stamp,
            'power_spect_video_name': 'power_spectrum_%s.mp4' % stamp,
            'soundmap_photo_name': 'sound_%s.jpg' % stamp,
            'soundmap_video_name': 'sound_%s.mp4' % stamp,
            'voiceprint_photo_name': 'voiceprint_%s.jpg' % stamp,
            'voiceprint_video_name': 'voiceprint_%s.mp4' % stamp,
        }
        if mode == 2:
            leak = self.result_name == 'leak'
            result.update({'status': 2 if leak else 1, 'soundmap_intensity': 46 if leak else 4,
                           'leakage_value': 1.25 if leak else 0.0,
                           'leakage_unit': self.leakage['leakage_unit_str'],
                           'leakage_cost': 812.5 if leak else 0.0,
                           'cost_unit': self.leakage['monetary_unit_str']})
        else:
            status, intensity, probs = PD_RESULTS[self.result_name]
            result.update({'status': status, 'soundmap_intensity': intensity,
                           'noise_prob': probs[0], 'corona_prob': probs[1],
                           'floating_prob': probs[2], 'surface_prob': probs[3],
                           'prpd_photo_name': 'prpd_%s.jpg' % stamp,
                           'prpd_video_name': 'prpd_%s.mp4' % stamp})
        with self.lock:
            entry = dict(result, id=self.next_id, files_status=1, _created=end)
            self.next_id += 1
            self.logs.append(entry)
            self.logs = self.logs[-100:]          # the camera keeps the latest 100
            for key, value in result.items():
                if key.endswith('_name'):
                    self.files[(stamp, value)] = JPEG if value.endswith('.jpg') else MP4
            self.counters['detections'] += 1
        self.log('DETECTION %s %s' % (stamp, self.result_name))
        return result

    def broadcast(self, payload):
        with self.lock:
            clients = list(self.feedback)
        for conn in clients:
            if self.args.ws_fragment and len(payload) > 3:
                third = len(payload) // 3
                ok = conn.send(0x1, payload[:third], fin=False)
                ok = ok and conn.send(0x9, b'mid')              # a ping between fragments
                ok = ok and conn.send(0x0, payload[third:2 * third], fin=False)
                ok = ok and conn.send(0x0, payload[2 * third:], fin=True)
            else:
                conn.send(0x1, payload)

    def query_logs(self, q):
        mode = int(q.get('detect_mode', ['0'])[0])
        start_col = max(1, int(q.get('start_col', ['1'])[0]))
        count = max(1, int(q.get('col_count', ['100'])[0]))
        t0 = float(q.get('start_time', ['0'])[0])
        t1 = float(q.get('end_time', ['2000000000'])[0])
        mask = int(q.get('discharge_type', ['15' if mode == 1 else '3'])[0])
        with self.lock:
            hits = [e for e in reversed(self.logs)            # newest first
                    if e['detect_mode'] == mode and e['status'] & mask and
                    t0 <= e['_created'] <= t1]
        page = [{k: v for k, v in e.items() if not k.startswith('_')}
                for e in hits[start_col - 1:start_col - 1 + count]]
        return {'logs': page, 'logs_count': len(hits)}


DEVICE = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def reply(self, code, obj=None, data=None, ctype='application/json'):
        body = data if data is not None else json.dumps(obj if obj is not None else {}).encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def error(self, code, msg):
        self.reply(code, {'code': code, 'msg': msg})

    def ok(self):
        self.reply(200, {'code': 0, 'msg': 'success'})

    def do_GET(self):
        if self.headers.get('Upgrade', '').lower() == 'websocket':
            return self.websocket()
        self.route('GET')

    def do_POST(self):
        self.route('POST')

    def do_PUT(self):
        self.route('PUT')

    def do_DELETE(self):
        self.route('DELETE')

    def route(self, method):
        dev = DEVICE
        url = urlsplit(self.path)
        path, query = url.path, parse_qs(url.query)
        length = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(length) if length else b''
        dev.log('%s %s %s' % (method, self.path, body.decode(errors='replace')))
        if dev.args.latency:
            time.sleep(dev.args.latency)

        if path == '/debug/state':
            with dev.lock:
                state = dict(dev.counters, detecting=dev.detecting, logs=len(dev.logs),
                             feedback_clients=len(dev.feedback))
            return self.reply(200, state)
        if method == 'POST' and path == '/login':
            token = dev.login(body)
            if not token:
                return self.error(401, 'wrong username or password')
            return self.reply(200, {'token': token})
        if not dev.valid(self.headers.get('Authorization')):
            return self.error(401, 'token invalid or expired')

        try:
            req = json.loads(body) if body else {}
        except ValueError:
            return self.error(400, 'body is not JSON')

        if path == '/system':
            if method == 'GET':
                with dev.lock:
                    return self.reply(200, dict(dev.system))
            if method == 'PUT':
                return self.put_system(req)
        elif path == '/video_config':
            if method == 'GET':
                with dev.lock:
                    return self.reply(200, dict(dev.video))
            if method == 'PUT':
                return self.put_video(req)
        elif path == '/leakage_estimation':
            if dev.system['work_mode'] != 2:
                return self.error(400, 'leakage estimation is only available in leakage mode')
            if method == 'GET':
                with dev.lock:
                    return self.reply(200, dict(dev.leakage))
            if method == 'PUT':
                return self.put_leakage(req)
        elif path == '/signal_filtering_area':
            if method == 'GET':
                with dev.lock:
                    return self.reply(200, dict(dev.area))
            if method == 'PUT':
                if set(req) != set(dev.area):
                    return self.error(400, 'all five fields are required')
                with dev.lock:
                    dev.area = {k: int(v) for k, v in req.items()}
                return self.ok()
        elif path == '/turn_on_detect' and method == 'POST':
            dev.start()
            return self.ok()
        elif path == '/turn_off_detect' and method == 'POST':
            dev.stop()
            return self.ok()
        elif path == '/detect_status' and method == 'GET':
            with dev.lock:
                return self.reply(200, {'detect_status': 1 if dev.detecting else 0})
        elif path == '/detect_logs':
            if method == 'GET':
                if 'detect_mode' not in query:
                    return self.error(400, 'detect_mode is required')
                return self.reply(200, dev.query_logs(query))
            if method == 'DELETE':
                ids = {int(i) for i in req.get('id', [])}
                with dev.lock:
                    dev.logs = [e for e in dev.logs if e['id'] not in ids]
                return self.ok()
        elif path.startswith('/detect_file/') and method == 'GET':
            parts = path[len('/detect_file/'):].split('/')
            if len(parts) == 2:
                with dev.lock:
                    data = dev.files.get((parts[0], parts[1]))
                if data is not None:
                    ctype = 'image/jpeg' if parts[1].endswith('.jpg') else 'video/mp4'
                    return self.reply(200, data=data, ctype=ctype)
            return self.error(404, 'no such file')
        self.error(404, 'no such interface')

    def put_system(self, req):
        dev = DEVICE
        with dev.lock:
            s = dict(dev.system)
        if ('freq_down_limit' in req) != ('freq_up_limit' in req):
            return self.error(400, 'freq_down_limit and freq_up_limit must be sent together')
        if 'freq_down_limit' in req:
            down, up = int(req['freq_down_limit']), int(req['freq_up_limit'])
            if not (0 <= down < up and 2 <= up - down <= 20 and up <= s['samplerate'] // 2000):
                return self.error(400, 'invalid band %d..%d kHz' % (down, up))
            s['freq_down_limit'], s['freq_up_limit'] = down, up
        if 'dB_limit' in req:
            if not 0 <= int(req['dB_limit']) <= 100:
                return self.error(400, 'dB_limit out of range')
            s['dB_limit'] = int(req['dB_limit'])
        if 'detect_times' in req:
            if int(req['detect_times']) < 1:
                return self.error(400, 'detect_times out of range')
            s['detect_times'] = int(req['detect_times'])
        with dev.lock:
            dev.system = s
        self.ok()

    def put_video(self, req):
        dev = DEVICE
        with dev.lock:
            v = dict(dev.video)
        if ('video_width' in req) != ('video_height' in req):
            return self.error(400, 'video_width and video_height must be sent together')
        if 'video_width' in req:
            size = (int(req['video_width']), int(req['video_height']))
            if size not in RESOLUTIONS:
                return self.error(400, 'unsupported resolution')
            v['video_width'], v['video_height'] = size
        if 'video_fps' in req:
            if not 1 <= int(req['video_fps']) <= 30:
                return self.error(400, 'video_fps out of range')
            v['video_fps'] = int(req['video_fps'])
        if 'video_bit_rate' in req:
            if int(req['video_bit_rate']) not in (4096, 2048, 1024, 512):
                return self.error(400, 'unsupported bit rate')
            v['video_bit_rate'] = int(req['video_bit_rate'])
        if 'video_port' in req:
            v['video_port'] = int(req['video_port'])
        with dev.lock:
            dev.video = v
        self.ok()

    def put_leakage(self, req):
        dev = DEVICE
        with dev.lock:
            c = dict(dev.leakage)
        for key, value in req.items():
            if key not in LEAK_LIMITS:
                return self.error(400, 'unknown field %s' % key)
            lo, hi, integer = LEAK_LIMITS[key]
            if not lo <= float(value) <= hi or (integer and float(value) != int(value)):
                return self.error(400, '%s out of range' % key)
            c[key] = int(value) if integer else float(value)
        c['leakage_unit_str'] = 'ml/s' if c['leakage_unit'] == 1 else 'l/min'
        c['monetary_unit_str'] = CURRENCIES[c['monetary_unit']]
        with dev.lock:
            dev.leakage = c
        self.ok()

    # ---- WebSocket
    def websocket(self):
        dev = DEVICE
        path = urlsplit(self.path).path
        key = self.headers.get('Sec-WebSocket-Key')
        self.close_connection = True
        dev.log('WS %s' % path)
        if path not in ('/fftData', '/prpdData', '/voiceprintData', '/detectFeedback'):
            return self.error(404, 'no such stream')
        if not key:
            return self.error(400, 'no Sec-WebSocket-Key')
        if dev.args.ws_auth and not dev.valid(self.headers.get('Authorization')):
            return self.error(401, 'token invalid or expired')
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101, 'Switching Protocols')
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        self.wfile.flush()
        dev.count('ws_opened')

        conn = WsConn(self.connection, path)
        stop = threading.Event()
        if path == '/detectFeedback':
            with dev.lock:
                dev.feedback.add(conn)
        else:
            threading.Thread(target=self.ws_sender, args=(conn, stop), daemon=True).start()
        try:
            self.ws_reader(conn, stop)
        finally:
            stop.set()
            conn.alive = False
            with dev.lock:
                dev.feedback.discard(conn)

    def ws_sender(self, conn, stop):
        period = 0.2 if conn.path == '/fftData' else 1.0
        n = 0
        while not stop.wait(period):
            n += 1
            if conn.path == '/fftData':
                values = [100 + (n + i) % 7 for i in range(512)]
                values[FFT_PEAK_BIN] = FFT_PEAK
                values[FFT_SIDE_BIN] = FFT_SIDE
                payload = struct.pack('<512h', *values)
            elif conn.path == '/prpdData':
                payload = struct.pack('<360i', *[int(50 + 40 * math.sin(math.radians(i)))
                                                for i in range(360)])
            else:
                payload = struct.pack('<2048h', *[(i * 7 + n) % 256 for i in range(2048)])
            if not conn.send(0x2, payload):
                return
            if DEVICE.args.ws_ping and n % 5 == 0:
                conn.send(0x9, b'ping')

    def ws_reader(self, conn, stop):
        dev = DEVICE
        sock = conn.sock
        buf = b''
        last = time.monotonic()
        while not stop.is_set():
            ready, _, _ = select.select([sock], [], [], 0.1)
            if ready:
                try:
                    data = sock.recv(65536)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                while True:
                    frame = ws_parse(buf)
                    if frame is None:
                        break
                    fin, opcode, masked, payload, used = frame
                    buf = buf[used:]
                    if not masked:
                        dev.count('unmasked')
                    if opcode == 0x8:
                        conn.send(0x8, payload[:2])
                        return
                    if opcode == 0x9:
                        conn.send(0xA, payload)
                    elif opcode == 0xA:
                        dev.count('pongs')
                    elif opcode in (0x1, 0x2, 0x0):
                        last = time.monotonic()
                        dev.count('heartbeats')
            if time.monotonic() - last > dev.args.ws_idle:
                dev.count('ws_idle_closed')
                dev.log('WS idle close %s' % conn.path)
                conn.send(0x8, struct.pack('>H', 1000) + b'idle')
                return


class RtspHandler(socketserver.StreamRequestHandler):
    def handle(self):
        dev = DEVICE
        while True:
            line = self.rfile.readline()
            if not line:
                return
            request = line.decode(errors='replace').strip()
            if not request:
                continue
            headers = {}
            while True:
                h = self.rfile.readline().decode(errors='replace')
                if h in ('\r\n', '\n', ''):
                    break
                name, _, value = h.partition(':')
                headers[name.strip().lower()] = value.strip()
            parts = request.split(' ')
            if len(parts) < 3:
                return
            method, url = parts[0], parts[1]
            cseq = headers.get('cseq', '1')
            path = urlsplit(url).path
            dev.log('RTSP %s %s' % (method, path))
            if method == 'OPTIONS':
                self.answer(cseq, '200 OK', ['Public: OPTIONS, DESCRIBE'])
            elif method == 'DESCRIBE':
                if dev.args.rtsp_auth and not self.authorized(headers, method, url):
                    self.answer(cseq, '401 Unauthorized',
                                ['WWW-Authenticate: Digest realm="fake", nonce="0123abcd"'])
                elif path.strip('/') == dev.args.rtsp_path.strip('/'):
                    sdp = ('v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=fake\r\nt=0 0\r\n'
                           'm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n'
                           'a=control:trackID=0\r\n')
                    self.answer(cseq, '200 OK', ['Content-Type: application/sdp',
                                                 'Content-Base: %s/' % url], sdp)
                else:
                    self.answer(cseq, '404 Not Found')
            else:
                self.answer(cseq, '454 Session Not Found')
                return

    def authorized(self, headers, method, uri):
        auth = headers.get('authorization', '')
        if not auth.startswith('Digest '):
            return False
        fields = {}
        for item in auth[len('Digest '):].split(','):
            k, _, v = item.strip().partition('=')
            fields[k] = v.strip('"')
        ha1 = md5('admin:fake:%s' % DEVICE.args.password)
        ha2 = md5('%s:%s' % (method, fields.get('uri', uri)))
        return fields.get('response') == md5('%s:0123abcd:%s' % (ha1, ha2))

    def answer(self, cseq, status, headers=(), body=''):
        lines = ['RTSP/1.0 ' + status, 'CSeq: ' + cseq] + list(headers)
        if body:
            lines.append('Content-Length: %d' % len(body.encode()))
        self.wfile.write(('\r\n'.join(lines) + '\r\n\r\n' + body).encode())
        self.wfile.flush()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class RtspServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global DEVICE
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--port', type=int, default=18090)
    p.add_argument('--password', default='123456')
    p.add_argument('--mode', type=int, choices=(1, 2), default=1,
                   help='1 partial discharge, 2 gas leakage')
    p.add_argument('--result', choices=sorted(PD_RESULTS) + ['leak', 'no_leak'],
                   help='what every detection finds (surface / leak)')
    p.add_argument('--samplerate', type=int, default=96000)
    p.add_argument('--token-ttl', type=float, default=0.0, help='s, 0: tokens never expire')
    p.add_argument('--ws-idle', type=float, default=60.0, help='s without a client message')
    p.add_argument('--ws-auth', action='store_true', help='WebSockets need the token too')
    p.add_argument('--ws-ping', action='store_true', help='ping stream clients')
    p.add_argument('--ws-fragment', action='store_true', help='fragment detectFeedback')
    p.add_argument('--continuous', action='store_true', help='detection restarts by itself')
    p.add_argument('--no-feedback', action='store_true', help='never send detectFeedback')
    p.add_argument('--latency', type=float, default=0.0)
    p.add_argument('--rtsp-port', type=int, default=0)
    p.add_argument('--rtsp-path', default='live')
    p.add_argument('--rtsp-auth', action='store_true')
    p.add_argument('--log')
    args = p.parse_args()
    DEVICE = Device(args)

    if args.rtsp_port:
        rtsp = RtspServer(('127.0.0.1', args.rtsp_port), RtspHandler)
        threading.Thread(target=rtsp.serve_forever, daemon=True).start()
    server = Server(('127.0.0.1', args.port), Handler)
    print('fake acoustic camera on http://127.0.0.1:%d' % args.port, flush=True)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
