#!/usr/bin/env python3
# HTTP testlerinin istemcisi (bkz. run.sh, "// send:" satırları).
#
#   http_gonder.py PORT 'GET / HTTP/1.1\r\nHost: a\r\n\r\n'
#
# İsteği olduğu gibi (ham bayt) gönderir; böylece geçersiz istekler de
# denenebilir. \r, \n, \t ve \\ kaçış dizileri çevrilir. Sunucu henüz
# dinlemiyorsa 10 saniye boyunca yeniden dener. Yanıtı bağlantı kapanana
# kadar okur ve satır sonlarını \n'e çevirerek yazar.

import socket
import sys
import time


def unescape(text):
    out, i = [], 0
    escapes = {"r": "\r", "n": "\n", "t": "\t", "\\": "\\"}
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text) and text[i + 1] in escapes:
            out.append(escapes[text[i + 1]])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out).encode("utf-8")


def connect(port):
    deadline = time.time() + 10
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=15)
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.05)


def main():
    port, request = int(sys.argv[1]), unescape(sys.argv[2])
    with connect(port) as sock:
        sock.sendall(request)
        chunks = []
        while True:
            data = sock.recv(65536)
            if not data:
                break
            chunks.append(data)
    reply = b"".join(chunks).decode("utf-8", "replace").replace("\r\n", "\n")
    if not reply.endswith("\n"):
        reply += "\n"
    sys.stdout.write(reply)


if __name__ == "__main__":
    main()
