#!/usr/bin/env python3
# HTTP testlerinin istemcisi (bkz. run.sh, "// send:" satırları).
#
#   http_gonder.py PORT 'GET / HTTP/1.1\r\nHost: a\r\n\r\n'
#
# İsteği olduğu gibi (ham bayt) gönderir; böylece geçersiz istekler de
# denenebilir. \r, \n, \t ve \\ kaçış dizileri çevrilir. Sunucu henüz
# dinlemiyorsa 10 saniye boyunca yeniden dener.
#
# Yanıtlar başlıklarına göre okunur (Content-Length; HEAD, 204 ve 304'te gövde
# yok), bağlantının kapanması beklenmez: sunucu bağlantıyı açık tutabilir
# (keep-alive). Gönderilen metindeki her istek satırı ("GET /a HTTP/1.1") için
# bir yanıt okunur; yanıt "Connection: close" derse okuma biter. Okunan her şey
# satır sonları \n'e çevrilerek yazılır.
#
# Aynı bağlantıda sırayla birden çok adım: parçalar "|||" ile ayrılır.
#
#   'GET /a ...\r\n\r\n|||GET /b ...\r\n\r\n'   a'nın yanıtı gelince b gönderilir
#   'GET /a ...\r\n\r\nGET /b ...\r\n\r\n'      ikisi birlikte (pipelining)
#   '@bekle 6'                                  6 saniye bekler
#   '@gonder POST ...'                          gönderir, yanıt beklemez (isteğin
#                                               bir parçası; yanıtı sonraki parçadan
#                                               sonra okunur)
#   '@kapandi-mi'                               sunucu bağlantıyı kapattı mı?
#                                               "[kapandi]" ya da "[acik]" yazar

import re
import socket
import sys
import time

# İstek satırı; satır başında olmayabilir (gövdenin hemen arkasından gelen
# pipelining isteği: "...abcGET /b HTTP/1.1").
REQUEST_LINE = re.compile(rb"([A-Z]+) /\S* HTTP/\d\.\d\r?\n")


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


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""
        self.closed = False

    def fill(self):
        if self.closed:
            return False
        data = self.sock.recv(65536)
        if not data:
            self.closed = True
            return False
        self.buffer += data
        return True

    def take(self, n):
        part, self.buffer = self.buffer[:n], self.buffer[n:]
        return part

    def response(self, method):
        """Bir yanıtı okur: (baytlar, bağlantı kapanacak mı). Okunacak yoksa None."""
        while b"\r\n\r\n" not in self.buffer:
            if not self.fill():
                return (self.take(len(self.buffer)), True) if self.buffer else None
        head = self.take(self.buffer.index(b"\r\n\r\n") + 4)
        lines = head.decode("latin-1").split("\r\n")
        status = int(lines[0].split(" ")[1])
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        close = headers.get("connection", "").lower() == "close"
        if status < 200 or status in (204, 304) or method == b"HEAD":
            return head, close
        if "content-length" in headers:
            n = int(headers["content-length"])
            while len(self.buffer) < n and self.fill():
                pass
            return head + self.take(n), close
        while self.fill():  # uzunluk yok: bağlantı kapanana kadar
            pass
        return head + self.take(len(self.buffer)), True


def main():
    port = int(sys.argv[1])
    out = []
    with connect(port) as sock:
        reader = Reader(sock)
        closed = False
        for part in sys.argv[2].split("|||"):
            if part.startswith("@bekle "):
                time.sleep(float(part.split()[1]))
                continue
            if part.startswith("@gonder "):
                sock.sendall(unescape(part[len("@gonder "):]))
                continue
            if part == "@kapandi-mi":
                sock.settimeout(2)
                try:
                    still_open = reader.buffer or reader.fill()
                except socket.timeout:
                    still_open = True
                except OSError:  # (Windows: bağlantı sıfırlandı)
                    still_open = False
                sock.settimeout(15)
                out.append(b"[acik]\n" if still_open else b"[kapandi]\n")
                continue
            if closed:
                break
            request = unescape(part)
            sock.sendall(request)
            methods = REQUEST_LINE.findall(request) or [b""]
            for method in methods:
                got = reader.response(method)
                if got is None:
                    closed = True
                    break
                data, closing = got
                # 100 Continue ara yanıttır: asıl yanıt arkasından gelir.
                while data.startswith(b"HTTP/1.1 100"):
                    out.append(data)
                    got = reader.response(method)
                    if got is None:
                        break
                    data, closing = got
                out.append(data)
                if closing:
                    closed = True
                    break
    reply = b"".join(out).decode("utf-8", "replace").replace("\r\n", "\n")
    if not reply.endswith("\n"):
        reply += "\n"
    # Bayt olarak UTF-8: Windows'ta metin akışı konsolun kod sayfasıyla
    # (cp1252) yazar ve Türkçe harflerde hata verir.
    sys.stdout.buffer.write(reply.encode("utf-8"))


if __name__ == "__main__":
    main()
