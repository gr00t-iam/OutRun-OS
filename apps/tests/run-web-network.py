#!/usr/bin/env python3
"""Real loopback TCP/UDP/TLS integration, ephemeral certificates and ports.
Never downloads or substitutes Internet responses. Not a guest network test.
"""
import datetime as dt
import importlib.util
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('fixture', ROOT/'metal/tools/desktop-web-fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def main():
    binary = sys.argv[1]
    now = dt.datetime.now(dt.timezone.utc)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'OutRun test CA')])
    ca = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
          .public_key(key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now-dt.timedelta(days=1)).not_valid_after(now+dt.timedelta(days=2))
          .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
          .sign(key, hashes.SHA256()))
    servers = []
    with tempfile.TemporaryDirectory(prefix='outrun-web-') as temp:
        directory = Path(temp)
        roots = directory/'ca.pem'
        roots.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
        leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        (directory/'key.pem').write_bytes(leaf_key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

        def server(tls=False, expired=False):
            s = ThreadingHTTPServer(('127.0.0.1', 0), fixture.Fixture)
            if tls:
                subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'web.test')])
                cert = (x509.CertificateBuilder().subject_name(subject).issuer_name(name)
                        .public_key(leaf_key.public_key()).serial_number(x509.random_serial_number())
                        .not_valid_before(now-dt.timedelta(days=3))
                        .not_valid_after(now+dt.timedelta(days=-1 if expired else 1))
                        .add_extension(x509.SubjectAlternativeName([x509.DNSName('web.test')]),False)
                        .sign(key, hashes.SHA256()))
                certfile = directory/('expired.pem' if expired else 'server.pem')
                certfile.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                ctx.minimum_version = ssl.TLSVersion.TLSv1_2
                ctx.maximum_version = ssl.TLSVersion.TLSv1_2
                ctx.load_cert_chain(certfile, directory/'key.pem')
                s.socket = ctx.wrap_socket(s.socket, server_side=True)
            servers.append(s)
            threading.Thread(target=s.serve_forever, daemon=True).start()
            return s.server_port

        plain, tls, expired = server(), server(True), server(True, True)
        # Authoritative UDP fixture for the reserved .test zone.
        dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dns.bind(('127.0.0.1', 0))
        def dns_serve():
            while True:
                try:
                    data, addr = dns.recvfrom(512)
                    if len(data)<17:
                        continue
                    reply = (data[:2]+b'\x81\x80\x00\x01\x00\x01'+bytes(4)+data[12:]
                             + b'\xc0\x0c\x00\x01\x00\x01'+struct.pack('!IH', 10, 4)
                             + socket.inet_aton('127.0.0.1'))
                    dns.sendto(reply, addr)
                except OSError:
                    return
        threading.Thread(target=dns_serve,daemon=True).start()
        env = dict(os.environ, WEB_DNS_IP='127.0.0.1', WEB_DNS_PORT=str(dns.getsockname()[1]))
        tests = [
            (roots, f'http://127.0.0.1:{plain}/', 'Real guest HTTP'),
            (roots, f'http://web.test:{plain}/second', 'History destination'),
            (roots, f'http://web.test:{plain}/redirect', 'History destination'),
            (roots, f'http://web.test:{plain}/chunked', 'Chunk decoder reached the end'),
            (roots, f'http://web.test:{plain}/slow', 'Chunk decoder reached the end'),
            (roots, f'https://web.test:{tls}/', 'Real guest HTTP'),
            (roots, f'https://wrong.test:{tls}/', 'ERROR'),
            (roots, f'https://web.test:{expired}/', 'ERROR'),
            (ROOT/'apps/vendor/cacert.pem', f'https://web.test:{tls}/', 'ERROR'),
        ]
        try:
            for trust,url,expect in tests:
                print('TEST',url,expect,flush=True)
                subprocess.run([binary,str(trust),url,expect],env=env,check=True,timeout=40)
            print(f'web network: {len(tests)} real HTTP/DNS/TLS cases PASS')
        finally:
            dns.close()
            for s in servers:
                s.shutdown(); s.server_close()


if __name__ == '__main__':
    main()
