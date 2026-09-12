#!/usr/bin/env python3
"""Loopback-only HTTP fixtures for real guest browser acceptance tests.

The guest reaches this server through QEMU user networking at 10.0.2.2.
These are deliberately labelled test documents, not captured Internet pages.
Every received request is logged so screenshots alone cannot masquerade as
network coverage. The server binds loopback and never serves filesystem paths.
"""
import argparse
import http.server
import json
import struct
import threading
import time


def bitmap():
    width, height = 24, 16
    stride = (width * 3 + 3) & ~3
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            pixels.extend((32, 180 if x < width // 2 else 60,
                           220 if y < height // 2 else 40))
        pixels.extend(bytes(stride - width * 3))
    return (struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
            + struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0,
                          len(pixels), 0, 0, 0, 0) + pixels)


def document(title, body):
    return ('<!doctype html><html><head><title>' + title
            + '</title></head><body>' + body + '</body></html>').encode('ascii')


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    lock = threading.Lock()

    def log_message(self, fmt, *args):
        with self.lock:
            print(json.dumps({'event': 'http', 'path': self.path,
                              'message': fmt % args}), flush=True)

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        status, content_type = 200, 'text/html; charset=us-ascii'
        if path == '/':
            payload = document('OutRun HTTP fixture',
                '<h1>Real guest HTTP</h1><p>Local acceptance fixture.</p>'
                '<p><a href="/second">Second document</a></p>'
                '<img src="/image.bmp" alt="Test colour grid">'
                '<p><a href="/chunked">Chunked document</a></p>'
                '<p><a href="/redirect">Redirect document</a></p>'
                + ''.join('<p>Scroll line %d: browser layout test.</p>' % i
                          for i in range(80)))
        elif path == '/second':
            payload = document('Second fixture',
                               '<h1>History destination</h1>'
                               '<a href="/">Return to first</a>')
        elif path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/second')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        elif path == '/image.bmp':
            payload, content_type = bitmap(), 'image/bmp'
        elif path in ('/chunked', '/slow'):
            payload = document('Chunked fixture',
                               '<h1>Chunk decoder reached the end</h1>')
        else:
            status = 404
            payload = document('Not found', '<h1>Fixture not found</h1>')
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Connection', 'close')
        chunked = path in ('/chunked', '/slow')
        self.send_header('Transfer-Encoding' if chunked else 'Content-Length',
                         'chunked' if chunked else str(len(payload)))
        self.end_headers()
        self.close_connection = True
        try:
            if chunked:
                for start in range(0, len(payload), 17):
                    piece = payload[start:start + 17]
                    self.wfile.write(('%x\r\n' % len(piece)).encode('ascii')
                                     + piece + b'\r\n')
                    self.wfile.flush()
                    if path == '/slow':
                        time.sleep(1)
                self.wfile.write(b'0\r\n\r\n')
            else:
                self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            self.log_message('client stopped transfer')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18080)
    args = parser.parse_args()
    with http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Fixture) as server:
        print(json.dumps({'event': 'ready', 'port': server.server_port}), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
