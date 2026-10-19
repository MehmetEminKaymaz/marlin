#!/usr/bin/env python3
# Dil sunucusu (marlin --lsp) testi; run.sh çalıştırır.
#
#   lsp_test.py <marlin>
#
# Bir LSP istemcisi gibi davranır: belge açar, değiştirir, kapatır ve gelen
# tanılamaları (hataları) beklenenlerle karşılaştırır. Başarılıysa çıkış kodu 0.

import json, os, pathlib, subprocess, sys, tempfile, threading

compiler = sys.argv[1]
work = tempfile.mkdtemp()
root = os.path.join(work, "ana.marl")
helper = os.path.join(work, "yardimci.marl")
with open(helper, "w") as f:
    f.write('public func iki() -> int {\n    return "iki";\n}\n')

server = subprocess.Popen([compiler, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
failures = []


def timed_out():
    # (Zamanlayıcı ayrı bir iş parçacığında çalışır; sys.exit orada yetmez.)
    print("LSP testi basarisiz: sunucu 5 saniyede yanit vermedi", flush=True)
    server.kill()
    os._exit(1)


def file_uri(path):
    # Linux: file:///tmp/..., Windows: file:///C:/...
    return pathlib.Path(path).as_uri()


def send(message):
    body = json.dumps(message).encode()
    server.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    server.stdin.flush()


def receive():
    timer = threading.Timer(5, timed_out)
    timer.start()
    length = 0
    while True:
        line = server.stdout.readline()
        if line in (b"\r\n", b""):
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    message = json.loads(server.stdout.read(length))
    timer.cancel()
    return message


def diagnostics(count):
    """count adet publishDiagnostics → {dosya adı: [(satır, sütun, mesaj)]}"""
    result = {}
    while count:
        message = receive()
        if message.get("method") != "textDocument/publishDiagnostics":
            continue
        params = message["params"]
        result[os.path.basename(params["uri"])] = [
            (d["range"]["start"]["line"] + 1, d["range"]["start"]["character"] + 1, d["message"])
            for d in params["diagnostics"]]
        count -= 1
    return result


def expect(step, got, file, wanted):
    """wanted: [(satır, sütun, mesajın parçası)]"""
    actual = got.get(file)
    if actual is None:
        failures.append(f"{step}: {file} icin tanilama gelmedi")
        return
    if len(actual) != len(wanted) or any(
            (line, col) != (w[0], w[1]) or w[2] not in msg for (line, col, msg), w in zip(actual, wanted)):
        failures.append(f"{step}: {file} beklenen {wanted}, gelen {actual}")


uri = file_uri(root)
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
caps = receive()["result"]["capabilities"]
if caps.get("textDocumentSync", {}).get("change") != 1:
    failures.append(f"initialize: beklenmeyen yetenekler {caps}")
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

# 1) Anlam hataları; Türkçe karakterli satırda sütun karakter sayılır.
text = 'func main() -> void {\n    string s = "ğüşı"; int sayi = "metin";\n    print(yok);\n}\n'
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": uri, "languageId": "marlin", "version": 1, "text": text}}})
expect("acilis", diagnostics(1), "ana.marl",
       [(2, 35, "tip uyusmazligi"), (3, 11, "tanimsiz isim: 'yok'")])

# 2) Düzeltme + import: hata artık import edilen dosyada.
text = "import yardimci;\n\nfunc main() -> void {\n    print(yardimci.iki());\n}\n"
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": text}]}})
got = diagnostics(2)
expect("import", got, "ana.marl", [])
expect("import", got, "yardimci.marl", [(2, 12, "tip uyusmazligi")])

# 3) Sözdizimi hatası; import edilen dosyanın hatası temizlenir.
send({"jsonrpc": "2.0", "method": "textDocument/didChange",
      "params": {"textDocument": {"uri": uri, "version": 3},
                 "contentChanges": [{"text": "func main( -> void {\n}\n"}]}})
got = diagnostics(2)
expect("sozdizimi", got, "yardimci.marl", [])
if not got.get("ana.marl") or "sozdizimi hatasi" not in got["ana.marl"][0][2]:
    failures.append(f"sozdizimi: beklenen sozdizimi hatasi, gelen {got.get('ana.marl')}")

# 4) main'i olmayan modül dosyası hata vermez.
module_uri = file_uri(os.path.join(work, "modul.marl"))
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": module_uri, "languageId": "marlin", "version": 1,
                                  "text": "public func bir() -> int { return 1; }\n"}}})
expect("modul", diagnostics(1), "modul.marl", [])

send({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
receive()
send({"jsonrpc": "2.0", "method": "exit"})
if server.wait(timeout=5) != 0:
    failures.append("sunucu 0 dışında bir kodla kapandı")

for f in failures:
    print("LSP testi basarisiz:", f)
sys.exit(1 if failures else 0)
