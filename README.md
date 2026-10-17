# marlin

MLIR ve LLVM üzerine kurulu küçük bir programlama dili ve derleyicisi. Uzun vadeli hedef, aynı kaynak koddan hem CPU'da hem GPU'da çalışan program üretmek. Şu an CPU için çalıştırılabilir dosya üretiyor.

Bu dosya projeyi nasıl derleyeceğini, kendi programlarını nasıl çalıştıracağını ve nasıl test edeceğini anlatır.

---

## 1. En önemli bilgi: her şey WSL içinde çalışır

Proje dosyaları Windows'ta duruyor ama **derleme ve çalıştırma WSL (Ubuntu) içinde** yapılıyor. Bunun iki sonucu var:

- Komutları bir **WSL terminalinde** yazman gerekir (Başlat menüsünden "Ubuntu" ya da PowerShell'de `wsl` yazarak açabilirsin).
- Proje dizini WSL içinde şu yoldadır:

```bash
cd /mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace
```

Aşağıdaki bütün komutlar bu dizinde çalıştırıldığını varsayar.

### "Bir .exe dosyası elde edebilir miyim?"

Çalıştırılabilir bir dosya elde edersin, ama bu bir **Linux programıdır**, Windows `.exe` dosyası değildir.

| | |
|---|---|
| Ürettiğimiz dosya | Linux çalıştırılabilir dosyası (ELF). Uzantısı yoktur, örneğin `program`. |
| Nerede çalışır | WSL içinde: `./program` |
| Windows'tan çalıştırmak | PowerShell'de `wsl ./program` yazabilirsin; yine WSL içinde çalışır. |
| Çift tıklayınca çalışır mı | Hayır. Windows bu dosyayı tanımaz. |

Gerçek bir Windows `.exe` üretmek mümkün, ama bunun için LLVM'in Windows hedefiyle derlenmesi ve Windows bağlayıcısı gerekir. Şu an buna ihtiyacımız yok: testler için WSL içinde çalışan dosya yeterli.

---

## 2. Derleyiciyi derlemek

"Derleyici" dediğimiz şey `marlin_lang` adlı programdır. Kaynak kodda (C++ dosyalarında ya da `marlin.g4`'te) bir değişiklik yaptığında onu yeniden derlemen gerekir.

**CLion ile:** Her zamanki gibi Build (çekiç simgesi). CLion bunu WSL üzerinden yapar.

**Terminalden:**

```bash
cmake --build cmake-build-debug -j 6
```

İlk derleme birkaç dakika sürebilir, sonrakiler yalnızca değişen dosyaları derler. Sonunda iki dosya oluşur:

- `cmake-build-debug/marlin_lang` — derleyici
- `cmake-build-debug/libmarlinrt.a` — derlenen programlara eklenen yardımcı kütüphane (`print` bunun içinde)

> Ekrana LLVM başlıklarından gelen çok uzun `TypeRange ... operator==` uyarıları basılır. Bunlar zararsızdır; bizim kodumuzla ilgili değildir. Asıl bakman gereken satır içinde `error` geçenlerdir.

`cmake-build-debug` dizinini silersen ya da hiç yoksa önce şunu çalıştır:

```bash
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
```

---

## 3. İlk programını çalıştırmak

Bir dosya oluştur, örneğin `deneme.marl`:

```
func main() -> void {
    int toplam = 0;
    for i in 0 .. 5 {
        toplam = toplam + i;
    }
    print("toplam =", toplam);
    print("karekok:", sqrt(toplam));
}
```

Derle ve çalıştır:

```bash
cmake-build-debug/marlin_lang deneme.marl -o deneme
./deneme
```

Çıktı:

```
toplam = 10
karekok: 3.1622776601683795
```

`-o deneme` çıktı dosyasının adını belirler. Vermezsen dosya `a.out` adıyla oluşur.

Hiç dosya vermeden `cmake-build-debug/marlin_lang` çalıştırırsan `main.cpp` içine gömülü örnek program derlenir.

### Derleme hatası alırsan

Hatalar dosya, satır ve sütun numarasıyla gösterilir:

```
deneme.marl:3:13: hata: tip uyusmazligi: beklenen int, bulunan bool
```

Bu "deneme.marl dosyasının 3. satırının 13. sütununda" demektir.

---

### VS Code ile yazmak

`editors/vscode/` dizinindeki eklenti şunları sağlar:
- renklendirme
- yazarken hata gösterme: derleyicinin kendisi dil sunucusu olarak çalışır, hatalar derleyicininkilerle aynıdır
- "Derle ve Çalıştır" düğmesi
- snippet'ler

Kurulum (bir kez):

1. VS Code'a **WSL** eklentisini (Microsoft) kur.
2. WSL terminalinde proje dizinine geç ve `code .` yaz. VS Code, klasörü WSL içinde açar (sol altta `WSL: Ubuntu` yazar).
3. Eklentiler panelinde `...` menüsünden **Install from VSIX...** seç ve `editors/vscode/marlin-0.1.0.vsix` dosyasını göster. WSL'e bağlıyken kurulduğu için WSL tarafına kurulur.

Kullanım:
- `.marl` dosyası açınca renkler ve hatalar kendiliğinden gelir.
- Sağ üstteki ▷ düğmesi ya da `Ctrl+Alt+R` dosyayı derleyip çalıştırır.
- `Ctrl+Shift+P` → "Marlin: Testleri Çalıştır".

Derleyiciyi yeniden derledikten sonra `Ctrl+Shift+P` → "Marlin: Dil Sunucusunu Yeniden Başlat".

Eklentiyi değiştirirsen yeniden paketlemek için (Windows PowerShell'de): `cd editors\vscode; npm install; npm run compile; npm run package`.

## 4. Derleyicinin seçenekleri

```
marlin_lang [kaynak.marl] [secenekler]
```

| Seçenek | Ne yapar |
|---|---|
| `-o dosya` | Çıktı dosyasının adı |
| `-O0` `-O1` `-O2` `-O3` | Optimizasyon seviyesi. Varsayılan `-O0` (optimizasyon yok). |
| `--emit=...` | Derlemeyi hangi aşamada durduracağını seçer (aşağıya bak) |
| `-I dizin` | `import` edilen modüllerin aranacağı ek dizin (birden çok kez verilebilir) |
| `--print-ast` | Programın ağaç yapısını (AST) ekrana yazar |
| `--gpu=off` | `@GPU` fonksiyonları da CPU için derler. Ekran kartı olmayan makinede çalıştırmak ya da CPU ile GPU'yu karşılaştırmak için. |
| `--help` | Kısa yardım |

### Ara aşamaları görmek: `--emit`

Derleyici programı adım adım dönüştürür. `--emit` ile istediğin adımda durup o andaki hali görebilirsin. Bir şey yanlış çalışıyorsa sorunun hangi adımda başladığını bulmak için çok işe yarar.

```
.marl → AST → MLIR → LLVM'e indirilmiş MLIR → LLVM IR → nesne dosyası → çalıştırılabilir
```

| Komut | Gösterdiği |
|---|---|
| `--emit=mlir` | Üst seviye MLIR. Döngülerin `scf.for` mu yoksa `cf` bloğu mu olduğunu burada görürsün. |
| `--emit=gpu-mlir` | GPU'da çalışacak kısımlar ayrıldıktan sonraki MLIR. Her GPU çekirdeği bir `gpu.module` içinde, CPU tarafında ise onu başlatan `gpu.launch_func` çağrısı görünür. |
| `--emit=llvm-mlir` | LLVM dialect'ine indirilmiş MLIR |
| `--emit=llvm-ir` | LLVM IR (`.ll` dosyalarındaki biçim) |
| `--emit=obj` | Nesne dosyası (`.o`), henüz bağlanmamış |
| `--emit=exe` | Çalıştırılabilir dosya (varsayılan) |

Metin çıktıları `-o` verilmezse ekrana yazılır. Örnekler:

```bash
# Üretilen MLIR'e bak
cmake-build-debug/marlin_lang deneme.marl --emit=mlir

# Optimizasyondan sonra LLVM IR'ı dosyaya yaz
cmake-build-debug/marlin_lang deneme.marl --emit=llvm-ir -O2 -o deneme.ll

# AST'yi gör
cmake-build-debug/marlin_lang deneme.marl --print-ast --emit=mlir
```

---

## 5. Hazır testleri çalıştırmak

`tests/` dizininde hazır test programları var. Hepsini tek komutla çalıştır:

```bash
bash tests/run.sh
```

Örnek çıktı:

```
  GECTI  break_continue.marl -O0
  GECTI  break_continue.marl -O2
  GECTI  hatalar.marl
  ...
16 gecti, 0 kaldi
```

Her program iki kez denenir: bir kez optimizasyonsuz (`-O0`), bir kez optimizasyonlu (`-O2`). İkisinde de aynı sonucu vermesi gerekir.

**Ne zaman çalıştırmalısın:** Derleyicide bir şey değiştirdikten sonra, her seferinde. Bir test `KALDI` derse az önce yaptığın değişiklik eskiden çalışan bir şeyi bozmuş demektir.

Windows PowerShell'den WSL'e girmeden çalıştırmak istersen:

```powershell
wsl -e bash -lc "cd '/mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace' && bash tests/run.sh"
```

### Mevcut testler

| Dosya | Neyi dener |
|---|---|
| `yapisal_donguler.marl` | Dışarı atlamayan `for` ve `while` döngüleri |
| `break_continue.marl` | `break` ve `continue`, iç içe döngüler |
| `erken_return.marl` | Döngü ve `if` içinden `return` |
| `tipler.marl` | Tip dönüşümleri, işaretsiz sayılar, `&&` ve `\|\|` |
| `fonksiyonlar.marl` | Global değişkenler, özyineleme, lambdalar |
| `fonksiyon_degerleri.marl` | Fonksiyon değerlerinin struct alanında, listede ve struct listesinde (yönlendirme tablosu) durması, yeniden atama, fonksiyon döndüren fonksiyon |
| `print.marl` | `print` ile her tipin yazdırılması |
| `matematik.marl` | `sqrt`, `pow`, `min`, `max` ve diğerleri |
| `hatalar.marl` | Derleyicinin yakalaması gereken hatalar |
| `hatalar_yerlesik.marl` | `print` ve matematik fonksiyonlarının hatalı kullanımı |
| `listeler.marl` | `list<T>`: `push`, `pop`, `len`, başvuru davranışı, büyüme |
| `liste_sinir_disi.marl` | Sınır dışı liste indeksinde programın mesajla durması |
| `liste_bos_pop.marl` | Boş listeden `pop` yapılınca programın mesajla durması |
| `hatalar_liste.marl` | Listelerin hatalı kullanımı |
| `matrisler.marl` | `matrix<T>`: oluşturma, eleman eleman işlemler, `matmul`, değer davranışı |
| `matris_boyut_uyusmazligi.marl` | Farklı boyutta matrisler toplanınca programın mesajla durması |
| `matris_sinir_disi.marl` | Sınır dışı matris indeksinde programın mesajla durması |
| `matris_matmul_boyut.marl` | Boyutları uygun olmayan `matmul` |
| `hatalar_matris.marl` | Matrislerin hatalı kullanımı |
| `cok_boyutlu.marl` | `matrix<T, N>`: vektör, üç ve dört boyut, `shape`, çok seviyeli literal |
| `cok_boyutlu_calisma_hatalari.marl` | Üç boyutta sınır dışı indeks |
| `cok_boyutlu_sekil_uyusmazligi.marl` | Üç boyutta uzunlukları farklı matrislerle işlem |
| `hatalar_cok_boyutlu.marl` | Boyut sayısıyla ilgili derleme hataları |
| `paralel.marl` | `parallel for`: liste, dizi ve matrise paralel yazma, iç içe paralel döngü |
| `hatalar_paralel.marl` | Paralel döngüde yarışa yol açabilecek kullanımların reddedilmesi |
| `zaman.marl` | `time()` ile süre ölçme; çekirdeklere dağıtılan `matmul` |
| `gpu.marl` | `@GPU` fonksiyonlar: GPU'da `parallel for`, matris işlemleri, `matmul`, `sum`, CPU ↔ GPU kopyalama |
| `gpu_matmul.marl` | GPU'daki döşemeli `matmul`: döşeme boyutunun katı olmayan boyutlar, tamsayı / `float` / `float64` |
| `hatalar_gpu.marl` | `@GPU` fonksiyonlarda izin verilmeyen kullanımlar |
| `stringler.marl` | Stringler: birleştirme, karşılaştırma, indeksleme, `str` / `to_int` / `to_float`, string listesi ve dizisi |
| `string_bellek.marl` | Stringlerin belleğinin kendiliğinden bırakılması: ara değerler, yeniden atama, liste / dizi / struct alanı, fonksiyonlar, döngüler; program bitince bırakılmamış string kalmamalı |
| `string_sinir_disi.marl`, `string_sayi_degil.marl` | Sınır dışı string indeksi ve sayı olmayan metnin çevrilmesi: programın mesajla durması |
| `hatalar_string.marl` | Stringlerin hatalı kullanımı |
| `std_string.marl`, `std_file.marl`, `std_diger.marl` | Standart kütüphane: string fonksiyonları, dosya işlemleri, `os` / `time` / `random` / `math` / `io` |
| `std_dosya_yok.marl` | Olmayan dosyayı okumak: programın mesajla durması |
| `http_sunucu.marl` | `std.http`: yönlendirme, sorgu, gövde, başlıklar, HEAD / 204 / 404 / 405; ayrıştırıcının reddettiği geçersiz istekler |
| `http_gorev.marl` | Her isteğin ayrı bir görevde (`marl`) yanıtlanması |
| `gorevler.marl` | `marl` / `task<T>` / `await`: sonuç tipleri, görev listesi, görev içinde görev (`fib`), her biri paralel döngü çalıştıran 64 görev, kopyalanan liste argümanı, fonksiyon değeriyle görev, görevde `matmul`; sızıntı yok |
| `gorev_gpu.marl` | Dört görevin aynı anda `@GPU` fonksiyon çağırması |
| `global_struct.marl` | Global struct ve global string: okuma, bütün / alan atama, başka modülün global struct'ı |
| `liste_literal_arguman.marl` | Fonksiyona doğrudan verilen liste literalinin (ödünç alınıyorsa) bırakılması; döndürülen literalin bırakılmaması |
| `scan.marl`, `scan_sutun_yok.marl`, `hatalar_scan.marl` | `scan`: satırları struct'a doldurma (bellekte sahte bir satır kaynağıyla), NULL, tek satır; eksik sütun; hatalı kullanım |
| `sqlite.marl` | `std.sqlite`: tablo, `$n` parametreleri, NULL, kısıt hatası, `scan`, işlem, etkilenen satır |
| `postgres.marl` | `std.postgres` (sunucu gerekir): SQLSTATE kodları, `scan`, işlem, global havuzu kullanan görevler |
| `hatalar_gorev.marl` | Görev kurallarının ihlalleri: global'e yazan görev, `await` içeren fonksiyonun değer olması, paralel döngüde `marl` / `await`, desteklenmeyen sonuç tipleri |
| `hatalar_std.marl` | Kullanıcı kodunda `extern`, std'nin iç fonksiyonlarına erişim, paralel döngüden yan etkili fonksiyon |
| `struct.marl` | Struct'lar: oluşturma, kopyalama, her tipten alan, iç içe struct, fonksiyona verme ve döndürme, lambdalar, paralel döngü |
| `modul_struct.marl` | Başka modülde tanımlı struct'ların kullanılması |
| `hatalar_struct.marl` | Struct'ların hatalı kullanımı |
| `struct_liste.marl` | `list<struct>`: ekleme, çıkarma, alan okuma ve yazma, iç içe struct, struct içinde struct listesi, paralel döngü |
| `struct_liste_alanlar.marl` | Elemanı matris, dizi ve liste alanı taşıyan struct listeleri |
| `hatalar_struct_liste.marl` | Struct listelerinin hatalı kullanımı |
| `map.marl` | `map<K, V>`: ekleme, okuma, `has`, `remove`, `keys`, eklenme sırası, büyüme, struct değer ve alan, struct alanı olarak map, başvuru davranışı |
| `map_anahtar_yok.marl` | Olmayan anahtarı okumak programı durdurur |
| `hatalar_map.marl` | Map'lerin hatalı kullanımı: anahtar/değer tipleri, paralel döngüde yazma, göreve verme, global map |
| `paralel_ic_ice.marl` | İç içe ve çağrı üzerinden iç içe paralel döngüler (az çekirdekte kilitlenmemeli) |
| `modul_import.marl` | `import`, `as`, `from`: başka dosyalardaki fonksiyon ve globaller, alt dizin, birbirini import eden modüller |
| `modul_gpu.marl` | Başka modüldeki `@GPU` fonksiyonların CPU'dan, GPU'dan ve çekirdek içinden çağrılması |
| `hatalar_modul*.marl` | Bulunamayan modül, modülde olmayan isim, isim çakışması, private tanımın import edilmesi, modüldeki sözdizimi hatası, paralel döngüden güvensiz modül fonksiyonu |

`tests/moduller/` dizinindeki dosyalar test değil, bu testlerin import ettiği modüllerdir (`run.sh` yalnızca `tests/` içindeki `.marl` dosyalarını çalıştırır).

`@GPU` içeren testler iki kez çalıştırılır: ekran kartında ve `--gpu=off` ile tümüyle CPU'da. İkisi de aynı çıktıyı vermelidir. Makinede NVIDIA ekran kartı yoksa yalnızca `--gpu=off` ile çalışırlar.

---

## 6. Kendi testini yazmak

Yeni bir test eklemek için `tests/` dizinine `.marl` uzantılı bir dosya koyman yeterli; `run.sh` onu kendiliğinden bulur. Testin ne beklediğini dosyanın başındaki özel yorum satırlarıyla söylersin.

### Çıktıyı denetleyen test

Programın ekrana yazması gereken satırları `// output:` ile sırayla yaz:

```
// output: 10
// output: merhaba
func main() -> void {
    print(4 + 6);
    print("merhaba");
}
```

Programın çıktısı bu satırlarla **birebir aynı** olmalıdır; fazladan ya da eksik satır testi bozar. Boş bir satır bekliyorsan yalnızca `// output:` yaz.

### Dönüş değerini denetleyen test

`main` bir `int` döndürüyorsa o değeri `// expect:` ile denetleyebilirsin:

```
// expect: 42
func main() -> int {
    return 40 + 2;
}
```

Dikkat: bu değer işletim sisteminin "çıkış kodu"dur ve yalnızca **0 ile 255 arasında** olabilir. Daha büyük sayıları denetlemek için `print` ve `// output:` kullan. `// expect:` yazmazsan 0 beklenir.

İkisini aynı dosyada birlikte kullanabilirsin.

### Derleme hatası bekleyen test

Derleyicinin bir hatayı yakaladığını denemek için `// error:` kullan. Yazdığın metin hata mesajının içinde geçmelidir:

```
// error: tanimsiz isim: 'y'
func main() -> void {
    y = 3;
}
```

Bir dosyada `// error:` satırı varsa program çalıştırılmaz; yalnızca derlemenin başarısız olduğuna ve mesajların çıktığına bakılır. Bir dosyaya birden çok `// error:` satırı yazabilirsin.

### Çalışma zamanı hatası bekleyen test

Program derlenir ama çalışırken hata verip durması gerekiyorsa (sınır dışı liste indeksi gibi), beklenen hata mesajını `// stderr:` ile yaz. Program bu durumda 1 koduyla çıkar, bu yüzden `// expect: 1` de eklenir:

```
// expect: 1
// output: once
// stderr: liste indeksi sinir disinda (indeks 5, uzunluk 3)
func main() -> void {
    list<int> l = [1, 2, 3];
    print("once");
    int i = 5;
    print(l[i]);
}
```

`// output:` satırları hatadan önce yazılanları denetler.

### String sızıntısını denetleyen test

Stringlerin belleğini derleyici yönetir. Bir programın hiç string sızdırmadığını denetlemek için dosyanın başına `// leaks: 0` yaz; program biterken bırakılmamış string kalırsa test kalır:

```
// leaks: 0
// output: ab
func main() -> void {
    list<string> l = ["a" + "b"];
    print(l[0]);
    free(l);        // listeler elle bırakılır; içindeki stringler de onunla
}
```

Kendi programında aynı denetimi `MARLIN_STRING_CHECK=1 ./program` ile yapabilirsin: sızıntı varsa program biterken sayısını hata çıktısına yazar.

### HTTP sunucusu testi

Program bir sunucuysa `// http: PORT` yaz. `run.sh` programı arka planda başlatır, `// send:` satırlarındaki istekleri sırayla o porta gönderir ve gelen yanıtları `// reply:` satırlarıyla karşılaştırır. İstekler ham olarak gönderilir (`\r\n` yazılabilir), böylece bozuk istekler de denenebilir. Yanıtlardaki satır sonları `\n`'e çevrilir. Program, beklediği istekleri işledikten sonra kendisi bitmelidir (`http.handle` ile sayılı döngü):

```
// http: 18500
// output: bitti
// send: GET / HTTP/1.1\r\nHost: a\r\n\r\n
// reply: HTTP/1.1 200 OK
// reply: Content-Type: text/plain; charset=utf-8
// reply: Content-Length: 3
// reply: Connection: close
// reply:
// reply: ok
import std.http as http;

func kok(http.Request req) -> http.Response { return http.text(200, "ok\n"); }

func main() -> void {
    list<http.Route> rotalar = [];
    push(rotalar, http.get("/", kok));
    http.Server s = http.listen(18500);
    http.handle(s, rotalar);
    http.close(s);
    free(rotalar);
    print("bitti");
}
```

İstemci `tests/http_gonder.py`'dir (`python3` gerekir). Her test farklı bir port kullanmalıdır.

### PostgreSQL testi

`// requires: postgres` yazan testler bir PostgreSQL sunucusu ister. Bağlantı bilgisi `MARLIN_PG_TEST` ortam değişkeninden okunur; tanımlı değilse test atlanır (`ATLANDI`). Yerel bir test sunucusu (yönetici izni gerekmez):

```bash
bash tests/pg_test_sunucu.sh
export MARLIN_PG_TEST="host=localhost port=54329 dbname=marlin_test user=marlin password=marlin"
bash tests/run.sh
bash tests/pg_test_sunucu.sh dur     # işin bitince
```

### Bir test kaldığında

`run.sh` neyin farklı olduğunu gösterir:

```
  KALDI  deneme.marl -O0: cikti farkli:
           -10
           +11
```

`-` ile başlayan satır beklenen, `+` ile başlayan satır programın gerçekte yazdığıdır.

Sorunu aramak için o tek dosyayı elle derleyip çalıştır:

```bash
cmake-build-debug/marlin_lang tests/deneme.marl -o /tmp/deneme && /tmp/deneme
echo "cikis kodu: $?"
```

Sonuç yalnızca `-O2` ile yanlışsa (ya da yalnızca `-O0` ile), `--emit=mlir` ve `--emit=llvm-ir` çıktılarına bakmak iyi bir başlangıçtır.

---

## 7. Başka neler denenebilir

**Üretilen MLIR'i incelemek.** Bir döngünün yapısal (`scf.for`, `scf.while`) mı yoksa bloklu (`cf.br`) mı üretildiğini görmek için:

```bash
cmake-build-debug/marlin_lang tests/break_continue.marl --emit=mlir | grep -E 'scf\.|cf\.'
```

Kural: gövdesinde `break` ya da `return` olmayan döngüler `scf` olarak üretilir. Bu, ileride GPU'ya göndereceğimiz döngülerin tanınabilmesi için önemlidir.

**MLIR araçlarıyla oynamak.** LLVM derlemenle birlikte gelen araçlar `/home/emin/llvm-project/build/bin` dizinindedir. Örneğin ürettiğimiz MLIR'i `mlir-opt` ile kendin dönüştürebilirsin:

```bash
cmake-build-debug/marlin_lang deneme.marl --emit=mlir -o deneme.mlir
/home/emin/llvm-project/build/bin/mlir-opt deneme.mlir --convert-scf-to-cf
```

Yeni bir MLIR pass'ini derleyiciye eklemeden önce etkisini görmek için kullanışlıdır.

**Optimizasyonun etkisini görmek.** Aynı programın `-O0` ve `-O2` LLVM IR'ını karşılaştır:

```bash
cmake-build-debug/marlin_lang deneme.marl --emit=llvm-ir -O0 -o o0.ll
cmake-build-debug/marlin_lang deneme.marl --emit=llvm-ir -O2 -o o2.ll
```

**Çalışma süresini ölçmek.**

```bash
time ./deneme
```

**Bellek hatalarını aramak.** Program beklenmedik şekilde çöküyorsa (ör. dizi sınırı dışına yazma):

```bash
valgrind ./deneme        # gerekirse: sudo apt install valgrind
```

Sabit boyutlu dizilerin (`int[5]`) indeksleri çalışma zamanında denetlenmez; yalnızca sabit indeksler (`d[7]` gibi) derleme sırasında denetlenir. Listelerde (`list<int>`) ise her erişim denetlenir: sınır dışı indekste program mesajla durur.

---

## 8. Dilin kısa özeti

```
// Global değişkenler (başlangıç değeri sabit olmalı)
int sayac = 0;
float oran = 0.5;
// Başlangıç değeri olmadan (sıfır / boş string): dizi, string ve alanları
// sayı / bool / char / string olan struct. Değerleri fonksiyonlarda atanır.
string son_mesaj;
Ayar ayar;            // ayar.ad, ayar.deger ...

func topla(int a, int b) -> int {
    return a + b;
}

func main() -> void {
    // Değişkenler
    int x = 5;
    let y = 2.5;                  // tip başlangıç değerinden çıkarılır (float64)
    bool tamam = x > 3 && y < 10;
    string mesaj = "merhaba";
    int[4] dizi = [1, 2, 3, 4];
    dizi[0] = 10;

    // Koşullar
    if x > 10 {
        print("buyuk");
    } else if x > 3 {
        print("orta");
    } else {
        print("kucuk");
    }

    // Döngüler
    for i in 0 .. 10 { }              // 0'dan 9'a
    for i in 0 .. 10 step 2 { }       // 0, 2, 4, 6, 8
    for eleman in dizi { }            // dizinin her elemanı
    while x > 0 { x = x - 1; }
    parallel for i in 0 .. 4 { dizi[i] = i * i; }   // yinelemeler çekirdeklere dağıtılır
    // break ve continue döngülerin içinde kullanılabilir

    // Lambdalar
    let kare = (int n) -> int | n * n;
    print(kare(4), topla(2, 3));

    // Listeler: büyüyebilen diziler
    list<int> l = [1, 2, 3];
    list<float> bos = [];             // boş liste (tipi yazmak şart)
    push(l, 4);                       // sona ekle
    int son = pop(l);                 // sondan çıkar ve döndür
    l[0] = 10;
    print(len(l), l[0], son);
    for eleman in l { }

    // Matrisler
    matrix<float> a = [[1, 2], [3, 4]];
    matrix<float> b = ones(2, 2);     // zeros(r, c), ones(r, c), fill(r, c, deger)
    matrix<float> c = a + b * 2;      // eleman eleman; sayıyla işlem her elemana uygulanır
    matrix<float> d = matmul(a, b);   // matris çarpımı
    c[0, 1] = 7;                      // eleman erişimi: [satır, sütun]
    print(rows(c), cols(c), sum(c));
    print(transpose(a));              // matris tek başına yazdırılır, her satır ayrı satırda

    // Boyut sayısı: matrix<T, N>.  matrix<T> ile matrix<T, 2> aynı tiptir.
    matrix<int, 1> v = [1, 2, 3];             // tek boyut (vektör)
    matrix<float, 3> t = zeros(4, 8, 8);      // üç boyut
    t[0, 2, 5] = 1;
    print(shape(t, 0), shape(t, 2));          // 4 8
}
```

**Tipler:** `int8` `int16` `int` `int64`, `uint8` `uint16` `uint` `uint64`, `float` (32 bit) `float64`, `bool`, `char`, `string`, sabit boyutlu diziler (`int[5]`), listeler (`list<int>`), map'ler (`map<string, int>`), matrisler (`matrix<float>`), fonksiyon tipleri (`(int) -> int`), struct'lar (`struct Nokta { ... }`).

**Dizi mi, liste mi?**

| | Dizi `int[5]` | Liste `list<int>` |
|---|---|---|
| Boyut | Kaynak kodda sabit | Çalışırken değişir (`push`, `pop`) |
| Fonksiyona verme | Boyut tipin parçası: `int[5]` alan fonksiyon yalnızca 5 elemanlı dizi alır | Her uzunlukta liste alır |
| Fonksiyondan döndürme | Yok | Var |
| Sınır denetimi | Yalnızca sabit indekste, derlerken | Her erişimde, çalışırken |
| `[1, 2, 3]` yazınca | `let a = [1, 2, 3];` dizi oluşturur | Tipi yazarsan liste olur: `list<int> l = [1, 2, 3];` |

Listeler **başvuru** olarak dolaşır: `list<int> b = a;` listeyi kopyalamaz, `a` ve `b` aynı listeyi gösterir; birine eklenen eleman diğerinde de görünür. Fonksiyona verilen liste de aynı listedir, fonksiyon onu değiştirebilir.

Listelerin belleği kendiliğinden geri verilmez; program bitene kadar yaşar. Uzun çalışan bir programda işi biten listeyi `free(l)` ile bırakabilirsin (bıraktıktan sonra o listeyi kullanma). Liste elemanları sayı, `bool`, `char`, `string`, fonksiyon, görev (`task<T>`) ya da struct olabilir.

Bir istisna: fonksiyona **doğrudan verilen liste literali** (`toplam([1, 2, 3])`, `pg.query(db, sql, [pg.arg_int(5)])`) çağrının deyimi bitince kendiliğinden bırakılır. Literalin adı olmadığı için onu `free` edemezdin. Bunun koşulu, fonksiyonun listeyi yalnızca **ödünç alması**: listeyi okuması, üzerinde dönmesi, uzunluğunu sorması ya da yine ödünç alan bir fonksiyona vermesi. Listeyi döndüren, bir yere saklayan ya da `free` eden fonksiyona verilen literal bırakılmaz. Derleyici bunu kendisi çıkarır.

**Matrisler**

`matrix<T, N>` N boyutlu bir sayı tablosudur. Eleman tipi sayısal olmalıdır (`int`, `float64` ...).

| Yazım | Anlamı |
|---|---|
| `matrix<int, 1>` | Tek boyut: vektör |
| `matrix<int>` ya da `matrix<int, 2>` | İki boyut: matris (ikisi aynı tiptir) |
| `matrix<float, 3>` | Üç boyut |

- **Boyut sayısı kaynak kodda sabit bir sayı olmalıdır** (`matrix<int, n>` olmaz) ve tipin parçasıdır: `matrix<int, 2>` ile `matrix<int, 3>` farklı tiplerdir, birbirine atanamaz, aralarında işlem yapılamaz.
- **Her boyutun uzunluğu ise çalışırken belli olur:** `zeros(n, m, k)` yazılabilir.
- **İndeksleme boyut sayısı kadar indeks ister:** `v[i]`, `m[i, j]`, `t[i, j, k]`.
- **`matmul`, `transpose`, `rows`, `cols` yalnızca iki boyutlu matrislerde çalışır.** Diğer her şey (eleman eleman işlemler, `sum`, `print`, `shape`) her boyut sayısında çalışır.
- **`shape(m, i)`** i. boyutun uzunluğunu verir; `i` sabit bir sayı olmalıdır (`shape(t, 0)`).
- Tek boyutlu matris ile liste farklı şeylerdir: `list<float>` büyüyen bir kaptır ve başvurudur; `matrix<float, 1>` sabit uzunlukta bir vektördür ve değerdir.

- **`+ - * /` eleman eleman çalışır.** `a * b` karşılıklı elemanları çarpar; matris çarpımı için `matmul(a, b)` kullanılır. Karşı taraf bir sayıysa her elemana uygulanır (`a * 2`, `a + 1`).
- **Matrisler değer olarak davranır.** `matrix<int> b = a;` bir kopya oluşturur; `b`'yi değiştirmek `a`'yı etkilemez. (Listelerde durum tersidir: onlar başvurudur.) Bir işlemin sonucu her zaman yeni bir matristir: `a = a + b` yazınca `a` yeni sonucu gösterir.
- **Fonksiyona verilen matris kopyalanmaz ama fonksiyon onu değiştiremez.** Değiştirmek gerekiyorsa içeride bir kopya alınır: `matrix<int> m = parametre;`.
- **Bir ifadedeki eleman eleman işlemler tek seferde hesaplanır.** `a + b * 2 - c` üç ayrı döngü ve iki geçici matris yerine tek döngü olur. `sum(a * b)` de çarpım için matris ayırmaz. Bu yüzden işlemi tek ifadede yazmak, ara değişkenlere bölmekten daha hızlıdır.
- **Belleği derleyici yönetir.** Matrisler kendiliğinden bırakılır; `free` gerekmez.
- **Boyutlar ve indeksler çalışırken denetlenir.** Farklı boyutta iki matrisi toplamak, sınır dışı bir elemana erişmek ya da boyutları uymayan `matmul` programı açıklayıcı bir mesajla durdurur.
- Matris tanımlarken başlangıç değeri şarttır (`zeros(3, 4)` gibi). `[[1, 2], [3, 4]]` yazımı yalnızca tip belliyken matris olur; iç içelik derinliği boyut sayısı kadar olmalıdır.
- `print` son boyutu bir satır olarak yazar: vektör tek satır, matris satır satır, üç ve üstü boyutta iki boyutlu dilimler aralarında boş satırla.

**Stringler**

```
string ad = "dunya";
string selam = "merhaba " + ad;       // birleştirme: yeni bir string üretir
print(selam, len(selam));             // merhaba dunya 13
print(selam[0]);                      // m   (s[i] bir char'dır)

if ad == "dunya" && "abc" < "abd" { } // içeriğe göre, sözlük sırasıyla

string metin = "x = " + str(42);      // str: sayı, bool ya da char → string
int64 n = to_int("40") + 2;           // to_int / to_float: string → sayı
float64 f = to_float("2.5");

list<string> isimler = ["ali", "veli"];
push(isimler, "ayse");
```

- **Stringler değişmez.** `s[0] = 'a'` yazılamaz; her işlem (`+`, `str`) yeni bir string üretir. Bu yüzden atama ve fonksiyona verme kopyalamaz: `string b = a;` aynı metni gösterir ve bu güvenlidir.
- **Belleği derleyici yönetir.** Stringler kendiliğinden bırakılır; `free` gerekmez. Her stringin onu kaç yerin (değişken, liste ya da dizi elemanı, struct alanı) tuttuğunu sayan bir sayacı vardır; tutan kalmayınca bellek geri verilir. `a + b + c` ya da `print("x" + str(n))` gibi ifadelerdeki ara değerler deyim bitince bırakılır; `s = s + "x"` eski değeri bırakır.
  - Bir string listeye konduğunda liste de onu tutar. Listeler elle bırakıldığı için içindeki stringler `free(l)` çağrılınca (ya da eleman çıkarılınca, üzerine yazılınca) bırakılır.
  - `free(s)` hâlâ yazılabilir ama hiçbir şey yapmaz.
- **Uzunluk ve indeks bayt sayar.** Metin UTF-8'dir: Türkçe harfler (ş, ğ, ı ...) iki bayt tutar, yani `len("ş")` 2'dir ve `s[i]` böyle bir harfin yarısını verebilir.
- **Karşılaştırma** bayt bayt, sözlük sırasıyladır (`"Z" < "a"`: büyük harfler önce gelir).
- **`to_int` ve `to_float`** metnin tamamı bir sayı değilse programı mesajla durdurur (baştaki ve sondaki boşluklar sayılmaz).
- `+` yalnızca iki string arasında çalışır; sayıyı eklemek için `str(...)` kullan.
- Başlangıç değeri verilmeyen string boştur. String; listede, dizide ve struct alanında tutulabilir. Global string değişkeni ve `@GPU` fonksiyonlarda string yoktur.
- Sınır dışı indeks (`s[100]`) programı mesajla durdurur.

**Struct'lar**

Struct, adlandırılmış alanlardan oluşan kendi tipindir.

```
struct Nokta {
    float64 x;
    float64 y;
}

struct Cember {
    Nokta merkez;                 // alan başka bir struct olabilir
    float64 r;
    list<int> etiketler;
}

func otele(Nokta p, float64 dx) -> Nokta {
    p.x = p.x + dx;
    return p;
}

func main() -> void {
    Nokta a = Nokta(1, 2);        // alanlar tanım sırasıyla verilir
    Nokta b = a;                  // kopya
    b.x = 10;                     // a.x hâlâ 1
    Nokta c;                      // başlangıç değeri yok: sayılar 0, liste boş
    c = otele(a, 5);
    print(c.x, otele(a, 1).x);

    Cember cm = Cember(Nokta(0, 0), 2.5, [1, 2]);
    cm.merkez.x = 7;
    push(cm.etiketler, 3);
}
```

- **Alan tipleri:** sayılar, `bool`, `char`, `string`, dizi (`int[3]`), liste, matris, fonksiyon (`(int) -> int f;`) ve başka bir struct. Bir struct kendisini içeremez.
- **Oluşturma:** `Nokta(1, 2)`. Argümanlar alanların tanım sırasıyladır ve hepsi verilmelidir.
- **Struct üzerindeki her işlem, alanlarına tek tek uygulanan işlemdir.** Bu yüzden her alan kendi tipinin kuralına uyar:

  | Alan tipi | `b = a` ile | Fonksiyona verilince |
  |---|---|---|
  | Sayı, `bool`, `char`, `string`, fonksiyon | Kopyalanır | Kopyalanır |
  | Dizi | Kopyalanır | Aynı dizi verilir: fonksiyon elemanlarını değiştirirse çağıran da görür |
  | Matris | Kopyalanır | Kopyalanmaz ama fonksiyon değiştiremez |
  | Liste | Paylaşılır: ikisi aynı listeyi gösterir | Aynı liste verilir |

- Fonksiyonlar struct alabilir ve döndürebilir; lambdalar da. Dönen struct'ın alanına doğrudan erişilebilir: `otele(a, 1).x`.
- `public struct` başka modüllerden kullanılabilir (`s.Nokta p = s.Nokta(1, 2);` ya da `from sekil import Nokta;`). Yazılmazsa struct private'tır.
- Matris ya da fonksiyon alanı olan struct başlangıç değeri olmadan tanımlanamaz.
- **Fonksiyon alanı** çağrılarak kullanılır; struct listesiyle bir yönlendirme tablosu kurulabilir:

  ```
  struct Komut {
      string ad;
      (int) -> int isle;
  }

  list<Komut> komutlar = [Komut("kare", (int x) -> int | x * x), Komut("ikikat", (int x) -> int | x * 2)];
  for i in 0 .. len(komutlar) {
      if komutlar[i].ad == "kare" { print(komutlar[i].isle(5)); }   // 25
  }
  ```
- Struct'lar `+`, `==` gibi işleçlerle kullanılamaz ve `print` ile bütün olarak yazdırılamaz; alanları tek tek kullanılır.
- **Global struct:** alanları sayı, `bool`, `char` ya da string olan struct global olarak tanımlanabilir (başlangıç değeri olmadan; alanlar sıfır ve boş string başlar). Bütün olarak (`ayar = Ayar("hiz", 7);`) ya da alan alan (`ayar.deger = 8;`) atanır; başka modülden `m.ayar.deger` ile okunur. Görevler okuyabilir ama yazamaz.
- **Henüz olmayanlar:** struct dizisi (`Nokta[3]`), liste / matris / fonksiyon alanlı global struct, `@GPU` fonksiyonlarda struct.

**Struct listesi (`list<Nokta>`)**

```
list<Nokta> l = [Nokta(1, 2), Nokta(3, 4)];   // ya da: list<Nokta> l;  (boş)
push(l, Nokta(5, 6));

print(len(l), l[0].x);       // alan okuma
l[1].x = 30.0;               // tek alana yazma
l[2] = Nokta(7, 8);          // elemanı bütün olarak değiştirme
Nokta p = l[0];              // eleman okunur: p bir kopyadır
Nokta son = pop(l);

for q in l {                 // q her yinelemede elemanın kopyasıdır
    print(q.x, q.y);
}
```

- Liste yine **başvurudur** (`list<Nokta> b = l;` aynı listeyi gösterir), elemanları ise **değerdir**: `Nokta p = l[0];` bir kopya verir, `p.x = 9` listeyi değiştirmez. Listedeki elemanı değiştirmek için indeksle yazılır: `l[0].x = 9`.
- Eleman struct'ı her tipten alan taşıyabilir. Alanlar yukarıdaki tablodaki kurallara uyar:
  - **Matris ve dizi alanı elemana aittir.** `push(l, k)` ve `l[i] = k` bunları kopyalar; `l[i].m[0, 0] = 1.0` ve `l[i].puanlar[2] = 5` listedeki elemanı yerinde değiştirir.
  - **Liste alanı paylaşılır.** `Kayit k = l[i];` sonrasında `k.etiketler` ile `l[i].etiketler` aynı listedir.
- `free(l)` listeyi, elemanlarının matris ve dizilerini bırakır; elemanların tuttuğu stringler de bırakılır. Elemanların içindeki listeler bırakılmaz (başka yerden de gösteriliyor olabilirler); gerekiyorsa önce onlar bırakılır: `free(l[i].etiketler)`.
- Struct'ın alanı da struct listesi olabilir (`struct Sekil { list<Nokta> noktalar; }`), bunların listesi de (`list<Sekil>`): `sekiller[1].noktalar[0].x = 2.0`.
- `for p in l` döngü değişkeni değiştirilemez (`p.x = 1` hatadır).
- `parallel for` içinde her yineleme yalnızca kendi elemanına yazabilir: `l[i].x = l[i].x * 2` geçerlidir, `l[0].x = ...` ve `push(l, ...)` değildir. Elemanların içindeki listelere (`l[i].etiketler[0] = ...`) paralel döngüde yazılamaz: iki eleman aynı listeyi paylaşıyor olabilir.
- Literal yalnızca tipi belli olan yerde yazılabilir: `let l = [Nokta(1, 2)];` hatadır, `list<Nokta> l = [Nokta(1, 2)];` yazılır.
- Bir struct, kendi listesini içeremez (`struct Dugum { list<Dugum> cocuklar; }` hatadır). Bunun sebebi saklama biçimidir: `list<Nokta>` bellekte alan başına bir liste olarak durur (bütün `x`'ler bir arada, bütün `y`'ler bir arada), kendisini içeren bir yapı sonsuza açılırdı.
- Global struct listesi ve `@GPU` fonksiyonlarda struct listesi yoktur.

**Map (`map<K, V>`)**

Anahtardan değere eşleme. Anahtarlar **eklenme sırasını korur**: `for k in m` ve `keys(m)` anahtarları eklendikleri sırayla verir.

```
map<string, int> fiyat = [];          // boş map (ya da: map<string, int> fiyat;)
fiyat["elma"] = 3;                    // anahtar yoksa eklenir
fiyat["elma"] = 5;                    // varsa değeri değişir (sırası değişmez)
print(fiyat["elma"], len(fiyat));     // 5 1
if has(fiyat, "armut") { ... }        // anahtar var mı?
bool silindi = remove(fiyat, "elma"); // vardıysa siler, true döner
for k in fiyat { print(k, fiyat[k]); }
list<string> ks = keys(fiyat);        // anahtarların yeni bir listesi (free ile bırakılır)
free(fiyat);
```

- Anahtar: tamsayılar, `char`, `bool` ya da `string`. Değer: liste elemanı olabilen her şey (sayı, `bool`, `char`, `string`, fonksiyon, görev, struct).
- Olmayan bir anahtarı **okumak** programı mesajla durdurur (`map'te "x" anahtari yok`); önce `has` ile bakılır.
- Değer struct ise alanı yerinde değiştirilebilir: `yer["a"].x = 3.0` (anahtar olmalıdır).
- Map de listeler gibi **başvurudur**: `map<string, int> b = a;` aynı map'i gösterir, fonksiyona verilen map'i fonksiyon değiştirebilir. Belleği kendiliğinden bırakılmaz: `free(m)`.
- Struct alanı olabilir (`struct Depo { map<string, int> stok; }`), fonksiyondan döndürülebilir.
- `remove` sonraki anahtarları bir öne kaydırır (sıra korunsun diye); bu yüzden map'in büyüklüğüyle orantılı sürer. `for k in m` döngüsünün içinde map'ten anahtar silinmemelidir.
- `parallel for` içinde map yalnızca okunur (`m[k]`, `has`, `len`); yazma ve `remove` yasaktır.
- Şimdilik yok: global map, `@GPU` fonksiyonlarda map, map'i göreve (`marl`) verme, `list<map<...>>`, dolu map literali (`["a": 1]`).

**Modüller (`import`)**

Her `.marl` dosyası bir modüldür. Bir dosya, başka bir dosyanın fonksiyonlarını ve global değişkenlerini `import` ile kullanır. `import` satırları dosyanın en başında durur.

```
import araclar.vektor;                         // araclar/vektor.marl
import araclar.vektor as v;                    // kısa ad vererek
from araclar.vektor import norm, ic as nokta;  // isimleri doğrudan alarak

func main() -> void {
    print(araclar.vektor.norm(3.0, 4.0));
    print(v.norm(3.0, 4.0));
    print(norm(3.0, 4.0), nokta(1.0, 2.0));
    v.sayac = 0;                               // modülün global değişkeni
}
```

- **Modül yolu dizin ayırıcısı yerine nokta ile yazılır:** `araclar.vektor`, `araclar/vektor.marl` dosyasıdır. Dosya önce import eden dosyanın dizininde, sonra `-I` ile verilen dizinlerde aranır.
- **Bir modül yalnızca kendi tanımlarını ve import ettiklerini görür.** `a`, `b`'yi; `b` de `c`'yi import ediyorsa `a`, `c`'nin isimlerini göremez (kendisi import etmelidir).
- **Farklı modüllerdeki aynı adlı fonksiyonlar çakışmaz.** `from` ile alınan isim dosyadaki bir tanımla çakışıyorsa `as` ile başka ad verilir.
- **Modüller birbirini import edebilir** (`a` → `b` → `a`).
- **Program derlenen dosyanın `main`inden başlar.** Import edilen dosyalardaki `main` sıradan bir fonksiyondur.
- **Yalnızca `public` tanımlar import edilebilir.** Fonksiyonun ya da global değişkenin başına `public` yazılmazsa tanım private'tır: yalnızca kendi dosyasında kullanılır. `private` yazmak isteğe bağlıdır, yazmamakla aynıdır.

  ```
  public int sayac = 0;                  // başka modüller okuyup yazabilir
  float64 PI = 3.14159;                  // private

  public func alan(float64 r) -> float64 { return PI * r * r; }
  private func yardimci() -> int { return 1; }     // `func yardimci()` ile aynı

  @GPU
  public func olcekle(matrix<float64> a, float64 k) -> matrix<float64> { return a * k; }
  ```

  `@GPU` / `@CPU` en üstte, `public` ondan sonra yazılır. Public bir fonksiyon kendi dosyasındaki private fonksiyonları çağırabilir.
- `@CPU` / `@GPU` ve `parallel for` kuralları modüller arasında da aynen geçerlidir.
- `std` ile başlayan modül adları standart kütüphaneye ayrılmıştır (aşağıda); kendi dosyalarına bu adı veremezsin.
- Yerleşik fonksiyonlar (`print`, `sqrt`, `zeros` ...) import gerektirmez.

**Standart kütüphane (`std`)**

Derleyiciyle birlikte gelen modüller. Diğer modüller gibi `import` ile kullanılır:

```
import std.string as str;
import std.file as file;

func main() -> void {
    file.write_text("sayilar.txt", "3,5,8\n");
    list<string> parcalar = str.split(str.trim(file.read_text("sayilar.txt")), ",");
    int64 toplam = 0;
    for p in parcalar {
        toplam = toplam + to_int(p);
    }
    print(toplam);      // 16
}
```

| Modül | Fonksiyonlar |
|---|---|
| `std.string` | `substr(s, start, length)` `find(s, part)` `find_from(s, part, start)` `contains(s, part)` `starts_with(s, prefix)` `ends_with(s, suffix)` `replace(s, old, new)` `trim(s)` `upper(s)` `lower(s)` `split(s, sep)` `join(parts, sep)` |
| `std.file` | `exists(path)` `read_text(path)` `write_text(path, text)` `append_text(path, text)` `read_lines(path)` `remove(path)`; satır satır: `open(path, mode)` `read_line(f)` `eof(f)` `write(f, text)` `close(f)` |
| `std.io` | `read_line()` `eof()` — klavyeden okuma; `write(text)` — satır sonu eklemeden yazma |
| `std.os` | `args()` — komut satırı argümanları; `env(name)`; `exit(code)` |
| `std.time` | `now()` — 1970'ten beri geçen saniye; `sleep(seconds)` |
| `std.random` | `seed(n)` `random()` — [0, 1); `rand_int(low, high)` — [low, high); `rand_matrix(rows, cols)` |
| `std.math` | `PI` `E` sabitleri |
| `std.postgres` | PostgreSQL: `connect` `close` `query(db, sql, params)` `exec(db, sql)` `begin` `commit` `rollback`; havuz `pool` `pool_query` `pool_exec` `acquire` `release`; parametreler `arg_text` `arg_int` `arg_float` `arg_bool` `arg_null`; sonuç `row_count` `column` `get_text` `get_int` `get_float` `get_bool` `is_null` `affected` `clear`; satırları struct'a: `scan(r.rows)` (aşağıda) |
| `std.sqlite` | SQLite (tek dosya, sunucusuz): `open(path)` (`":memory:"` bellekte); geri kalanı `std.postgres` ile aynı |
| `std.db` | Sürücülerin ortak satır kaynağı tipi `Rows` (kendi satır kaynağını yazmak için) |
| `std.http` | HTTP/1.1 sunucusu: `serve(port, rotalar)`; rotalar `get` `post` `put` `delete` `route(method, path, f)`; yanıtlar `text` `html` `json` `response` `redirect` `add_header`; istekten `header` `query_param` `form_value` `url_decode`; alt seviye `listen` `next` `respond` `handle` `dispatch` `close` (aşağıda) |

Bilinmesi gerekenler:

- **Hatalar iki türlüdür.** Tek adımlı fonksiyonlar işi yapamazsa programı mesajla durdurur: olmayan dosyada `read_text`, sınır dışı `substr`. Önceden denetlemek için `file.exists` var. `file.open` ise durdurmaz; döndürdüğü `File`'ın `ok` alanı açılıp açılmadığını söyler:

  ```
  file.File f = file.open("veri.txt", "r");      // "r" oku, "w" yaz, "a" sonuna ekle
  if f.ok {
      while !file.eof(f) {
          print(file.read_line(f));
      }
      file.close(f);
  }
  ```

- **String fonksiyonları yeni string döndürür**; bellekleri diğer stringler gibi kendiliğinden bırakılır. Konumlar bayt sayar; `find` bulamazsa -1 döner. `upper` / `lower` yalnızca İngilizce harfleri çevirir.
- **Dosyalar metin olarak okunur ve yazılır** (ikili dosya desteği yok). `read_line` satır sonunu atar.
- **`random`**: tohum verilmezse program her çalıştığında aynı sayılar gelir (ölçümler tekrarlanabilsin diye). Farklı sayılar için `random.seed(...)`.
- **`parallel for` içinde** yalnızca `std.string` fonksiyonlarının çoğu çağrılabilir (`split` ve `join` hariç: liste değiştirirler). Dosya, girdi, zaman ve rastgele sayı fonksiyonları yan etkilidir; derleyici paralel döngüde reddeder.
- **`@GPU` fonksiyonlarda kullanılamaz** (sabitler dahil): hepsi CPU'da çalışır.
- Süre ölçmek için `std.time.now()` değil, dile gömülü `time()` kullan (saati geri gitmez).

**HTTP sunucusu (`std.http`)**

```
import std.http as http;

func anasayfa(http.Request req) -> http.Response {
    return http.html(200, "<h1>merhaba</h1>\n");
}

func selam(http.Request req) -> http.Response {
    return http.text(200, "merhaba " + http.query_param(req, "ad") + "\n");   // /selam?ad=Ayse
}

func kaydet(http.Request req) -> http.Response {
    http.Response res = http.json(201, "{\"uzunluk\": " + str(len(req.body)) + "}");
    http.add_header(res, "X-Sunucu", "marlin");
    return res;
}

func main() -> void {
    list<http.Route> rotalar = [];
    push(rotalar, http.get("/", anasayfa));
    push(rotalar, http.get("/selam", selam));
    push(rotalar, http.post("/kayit", kaydet));
    push(rotalar, http.get("/dosyalar/*", (http.Request req) -> http.Response | http.text(200, req.path)));
    http.serve(8080, rotalar);       // hiç dönmez; Ctrl+C ile durur
}
```

Tarayıcıdan `http://localhost:8080/` ya da `curl -i localhost:8080/selam?ad=Ayse` ile denenebilir.

- **İstek** (`http.Request`): `method`, `path`, `query` (`?` sonrası), `headers` (`list<http.Header>`, her biri `name` / `value`), `body`. `path` ve `query` yüzde kodlu ham haldedir; değerleri `query_param` çözer, yol için `url_decode` var. `header(req, ad)` büyük/küçük harf ayrımı yapmaz.
- **Yanıt** (`http.Response`): `status`, `headers`, `body`. `text` / `html` / `json` uygun `Content-Type` ile yanıt kurar. `Content-Length` ve `Connection` otomatik eklenir; elle verilemez. Başlık değerinde satır sonu varsa program durur (yanıta başka başlık sokulmasın diye).
- **Yönlendirme:** istek, yolu ve yöntemi uyan ilk rotaya gider. Yol birebir karşılaştırılır; `/dosyalar/*` önektir. Yöntemi `""` olan rota (`http.route("", yol, f)`) her yönteme uyar; GET rotası HEAD'e de yanıt verir. Yol uyup yöntem uymazsa 405, hiçbiri uymazsa 404.
- **Geçersiz istekler işleyiciye ulaşmaz:** sunucu kendisi 400 (bozuk istek, HTTP/1.1'de `Host` yok), 408 (istemci 10 saniyede isteği bitirmedi), 413 (gövde 8 MB'tan büyük), 431 (başlık 64 KB'tan büyük ya da 100'den fazla satır), 501 (`Transfer-Encoding` / chunked gövde), 505 (HTTP/1.x değil) döner.
- **Kendi döngün:** `serve` port dinlenemezse programı durdurur. Durumu kendin denetlemek ya da belli sayıda istekten sonra durmak için:

  ```
  http.Server s = http.listen(8080);
  if !s.ok { print(s.error); return; }
  for i in 0 .. 100 {
      http.handle(s, rotalar);                 // bir istek: next + dispatch + respond
  }
  http.close(s);
  ```

  `next(s)` sıradaki isteği bekler, `respond(req, res)` yanıtı gönderir ve yanıtın başlık listesini bırakır. İsteğin başlık listesi isteği alanındır: işin bitince `free(req.headers)` (`handle` ve `serve` bunu kendileri yapar).
- **Eşzamanlı sunucu:** `serve` istekleri sırayla işler. `http.serve_concurrent(port, rotalar)` her isteği ayrı bir görevde (`marl`, aşağıda) işler: yavaş bir işleyici diğer istekleri bekletmez, istekler çekirdeklere dağılır. Görev kuralları geçerlidir: işleyiciler global değişkene yazamaz ve paralel döngü içeremez (derleyici denetler). Kendi döngünde de aynısı yapılabilir:

  ```
  func cevapla(http.Request req, list<http.Route> rotalar) -> void {
      http.respond(req, http.dispatch(rotalar, req));
  }
  ...
  while true {
      http.Request req = http.next(s);
      marl cevapla(req, rotalar);     // istek ve rotalar göreve kopyalanır
      free(req.headers);
  }
  ```
- **Şimdilik:** her yanıttan sonra bağlantı kapanır (keep-alive yok). Ağır bir işi hızlandırmak için işleyicinin içinde `parallel for` ya da `@GPU` fonksiyon kullanılabilir (sıralı `serve` ile). HTTPS yok; gerekirse sunucunun önüne nginx / caddy konur. HTTP istemcisi ve JSON ayrıştırma da henüz yok.
- Ayrıştırıcının kaynağı `runtime/http_parser.c`'dir: soket kullanmaz, her kuralın yanında ilgili RFC maddesi yazar.

**Veritabanı (`std.postgres`, `std.sqlite`)**

```
import std.postgres as pg;

struct Kullanici {
    int64 id;
    string ad;
    int yas;
}

func main() -> void {
    pg.Conn db = pg.connect("host=localhost dbname=uygulama user=marlin password=gizli");
    if !db.ok { print(db.error); return; }

    pg.Result r = pg.query(db, "INSERT INTO kullanici (ad, yas) VALUES ($1, $2) RETURNING id",
                           [pg.arg_text("Ayse"), pg.arg_int(31)]);
    if !r.ok { print(r.error, r.code); }        // code: SQLSTATE, ör. 23505
    else { print("yeni id:", pg.get_int(r, 0, 0)); }
    pg.clear(r);

    r = pg.query(db, "SELECT id, ad, yas FROM kullanici WHERE yas > $1", [pg.arg_int(18)]);
    list<Kullanici> l = scan(r.rows);           // her satır bir Kullanici
    pg.clear(r);
    for k in l { print(k.id, k.ad, k.yas); }
    free(l);

    pg.close(db);
}
```

`std.sqlite` aynı biçimdedir; yalnızca bağlantı `sql.open("veri.db")` ile açılır (`":memory:"` bellekte, geçici).

- **Parametreler** `$1`, `$2` ... ile verilir. Değerler SQL metnine hiç eklenmez, bu yüzden SQL injection olmaz. Değerleri SQL metnine `+` ile ekleme. NULL için `arg_null()`.
- **`scan(r.rows)`** satırları, adları sütun adlarıyla aynı olan struct alanlarına doldurur. Doldurulacak tip bağlamdan anlaşılır: `list<Kullanici> l = scan(r.rows);` bütün satırlar, `Kullanici k = scan(r.rows);` ilk satır (sonuç boşsa program durur). Sütun sırası önemli değildir, fazla sütunlar yok sayılır; eksik sütun programı durdurur (`AS` ile ad verilebilir). Alanlar sayı, `bool` ya da string olabilir. NULL değer alanın sıfır değeri olur (0, `false`, `""`); ayırt etmek gerekiyorsa `is_null`.
- **Satır / sütun numarasıyla da okunur:** `get_int(r, satir, sutun)`, `get_text`, `get_float`, `get_bool`, `is_null`, `column(r, "ad")`. NULL bir değere `get_int` programı durdurur.
- **Hatalar:** bağlanılamazsa `connect` durmaz (`ok` / `error`). Sorgu hataları da durdurmaz (`r.ok`, `r.error`, `r.code`). Programı yalnızca kullanım hataları durdurur (sınır dışı satır / sütun gibi).
- **Bellek:** her sonuç işi bitince `pg.clear(r)` ile bırakılır, bağlantı `pg.close(db)` ile kapatılır.
- **İşlem:** `pg.begin(db)`, `pg.commit(db)`, `pg.rollback(db)`. Başarısızsa `false` döner, sebebi `pg.last_error(db)`.
- **Görevler ve sunucular:** bir bağlantı aynı anda tek sorgu çalıştırır; aynı bağlantıyı kullanan görevlerin sorguları sıraya girer (bozulmaz). Sorguların aynı anda çalışması için havuz kullan ve onu global yap; görevler ve `serve_concurrent` işleyicileri global'i okuyabilir:

  ```
  pg.Pool havuz;

  func urunler(http.Request req) -> http.Response {
      pg.Result r = pg.pool_query(havuz, "SELECT ad, adet FROM urun WHERE adet >= $1",
                                  [pg.arg_int(to_int(http.query_param(req, "en_az")))]);
      list<Urun> l = scan(r.rows);
      pg.clear(r);
      ...
  }

  func main() -> void {
      havuz = pg.pool("host=localhost dbname=uygulama user=marlin password=gizli", 8);
      ...
      http.serve_concurrent(8080, rotalar);
  }
  ```

  İşlem (transaction) için havuzdan bir bağlantı ayrılır: `pg.Conn c = pg.acquire(havuz); ... pg.release(havuz, c);`
- Sürücüler PostgreSQL'in resmi C kütüphanesi **libpq**'yu ve **SQLite**'ı kullanır (kurulum: `sudo apt install libpq-dev libsqlite3-dev`). Derleyici bu kütüphaneleri bulamadan derlenmişse `import std.postgres` açık bir hata verir. Programa yalnızca kullandığı sürücü bağlanır.

Kütüphanenin kaynağı `std/` dizinindedir ve okunabilir. Her modül, C ile yazılmış küçük bir çekirdeğin (`runtime/std_*.c`) üzerine marlin ile yazılmıştır; C fonksiyonları `extern func` ile bildirilir. `extern` yalnızca bu dizindeki dosyalarda yazılabilir.

**Paralel döngü**

```
parallel for i in 0 .. n {
    sonuc[i] = agir_hesap(veri[i]);
}
```

`parallel for`, yinelemeleri işlemcinin çekirdeklerine dağıtır; ağır bir hesabı çok çekirdekli makinede birkaç kat hızlandırır. Yinelemeler aynı anda çalıştığı için iki yinelemenin aynı veriye yazmaması gerekir. Derleyici bunu denetler ve uygun olmayan döngüyü derlemez:

- **Döngü dışındaki bir değişkene atama yapılamaz.** `toplam = toplam + i` olmaz; toplamak için sonuçları bir diziye/matrise yazıp sonra `sum` kullan.
- **Dışarıdaki dizi, liste ya da matrisin elemanına yalnızca döngü değişkeniyle indekslenerek yazılabilir:** `a[i] = ...`, `m[i, j] = ...` olur; `a[0] = ...` ya da `a[i + 1] = ...` olmaz.
- **Döngüde yazılan bir dizi, aynı döngüde yalnızca aynı indeksle okunabilir:** `a[i] = a[i] + 1` olur; `a[i] = a[i - 1]` olmaz (başka bir yinelemenin yazdığı eleman).
- **`push`, `pop`, `free`, `break` ve `return` kullanılamaz.** `continue` serbesttir.
- **Çağrılan fonksiyonlar paylaşılan veriyi değiştirmemelidir:** global değişkene yazan, parametre olarak aldığı diziyi/listeyi değiştiren ya da böyle bir fonksiyonu çağıran fonksiyonlar paralel döngüden çağrılamaz. Lambda ve fonksiyon değişkenleri de çağrılamaz.

Döngünün içinde tanımlanan değişkenler her yinelemeye özeldir; onlarla her şey yapılabilir. Yazılmayan dış veriler serbestçe okunabilir. Yalnızca aralık döngüsü paralel olabilir (`parallel for x in liste` yok). Paralel döngüler iç içe yazılabilir.

Bu kurallar katıdır: aslında güvenli olan bazı döngüleri de reddeder. Karşılığında, derlenen her paralel döngünün doğru sonuç vereceği garantidir. Paralel döngüde `print` çağrılabilir: her satır bütün olarak yazılır, ama satırların sırası belirsizdir ve her çalıştırmada değişebilir. Döngünün gerçekten paralel çalıştığını görmenin en kolay yolu da budur: `parallel for i in 0 .. 20 { print(i); }` sayıları karışık sırada yazar.

Küçük ve hafif döngülerde paralellik fayda sağlamaz, hatta yavaşlatabilir (işi dağıtmanın da bir maliyeti var); ağır hesaplarda kullan.

**Görevler (`marl`)**

```
func parca(int bas, int son) -> int64 { ... }

func fib(int n) -> int64 {
    if n < 20 { return fib_sirali(n); }
    task<int64> a = marl fib(n - 1);     // yeni görev
    int64 b = fib(n - 2);                // bu görevde devam
    return await(a) + b;                 // a'nın sonucunu bekle
}

func main() -> void {
    task<int64> t = marl fib(35);
    marl logla("basladi");               // başlat ve bırak

    list<task<int64>> isler = [];
    for k in 0 .. 8 { push(isler, marl parca(k * 1000, (k + 1) * 1000)); }
    int64 toplam = 0;
    for g in isler { toplam = toplam + await(g); }
    free(isler);

    print(await(t), toplam);
}
```

- `marl f(a, b)` f'yi yeni bir görevde başlatır ve hemen bir `task<T>` döner (T, f'nin dönüş tipi). Görevler işlemcinin çekirdeklerine dağılır. `await(t)` görev bitene kadar bekler ve sonucunu verir; aynı görev birden çok kez beklenebilir. Deyim olarak yazılan `marl f();` görevi başlatıp bırakır.
- **`main` bitince program biter;** hâlâ çalışan görevler beklenmez. Sonucu gereken görevi `await` ile bekle.
- **Argümanlar kopyalanır.** Görev başlamadan sayılar, stringler, matrisler, diziler ve listeler kopyalanır: görev ile onu başlatan aynı listeyi asla görmez. Kopyalar görev bitince kendiliğinden bırakılır (görev listeyi sonuç olarak döndürürse o liste artık sonucundur).
- **Görev global değişkene yazamaz;** sonucu yalnızca `await` ile verir. Görevin çağırdığı fonksiyonlar da yazamaz. Görevin içinden fonksiyon değeriyle (lambda, fonksiyon değişkeni) çağrılabilecek fonksiyonlar da bu kurala uymalı ve paralel döngü içermemelidir. Derleyici denetler.
- **`await` içeren bir fonksiyon değer olarak kullanılamaz** (yalnızca adıyla çağrılır). `parallel for` içinde `marl` ve `await` kullanılamaz.
- `task<T>`'nin sonucu olarak şimdilik sayı, `bool`, `char`, string, fonksiyon, `void` ya da bunların listesi olabilir; struct ve matris sonuçlar sonra. Görev, struct alanında tutulamaz; listede tutulabilir.
- Görevler MLIR'in `async` dialect'i ile üretilir ve iş parçacığı havuzunda çalışır (paralel döngülerle aynı havuz). Bir görevde başka bir görevi `await` ile beklemek ya da paralel döngü çalıştırmak iş parçacığını tutmaz: derleyici bu fonksiyonları coroutine olarak üretir, bekleme sırasında iş parçacığı başka işe geçer. Bu yüzden havuzdakinden çok daha fazla görev aynı anda bekleyebilir, program kilitlenmez.

**GPU'da çalışan fonksiyonlar (`@GPU`):**

Bir fonksiyonun nerede çalışacağını başına yazılan işaret belirler: `@CPU` ya da `@GPU`. İşaret yazılmazsa `@CPU` sayılır.

```
@GPU
func adim(matrix<float64> a, matrix<float64> b, int n) -> matrix<float64> {
    matrix<float64> x = a;
    for k in 0 .. n {          // bu döngü CPU'da döner, her turda GPU'ya iş verir
        x = x * 0.5 + b;       // GPU'da çalışır
    }
    return x;
}

func main() -> void {
    matrix<float64> a = ones(2000, 2000);
    matrix<float64> b = ones(2000, 2000);
    matrix<float64> c = adim(a, b, 100);   // a ve b GPU'ya, sonuç CPU'ya kopyalanır
    print(sum(c));
}
```

`@GPU` fonksiyonda:

- **Matrisler ekran kartının belleğinde yaşar.** Fonksiyon `@CPU` tarafından çağrıldığında matris parametreleri GPU'ya kopyalanır, dönen matris CPU'ya geri kopyalanır. Çağıran, sonuç hazır olana kadar bekler. Bir `@GPU` fonksiyon başka bir `@GPU` fonksiyonu çağırdığında kopyalama olmaz.
- **`parallel for` ve matris işlemleri GPU'da çalışır** (`a + b`, `matmul`, `transpose`, `zeros`, `sum` ...). `parallel for`un her yinelemesi ayrı bir GPU iş parçacığıdır.
- **`matmul` GPU'da kendi döşemeli çekirdeğiyle çalışır:** matrisler parça parça GPU'nun hızlı paylaşımlı belleğine alınır, her iş parçacığı sonucun 4x4'lük bir parçasını hesaplar. Aynı çarpımı `parallel for` ile elle yazmaktan çok daha hızlıdır (RTX 4060'ta 4096x4096 `float` çarpımı: `matmul` 35 ms, elle yazılmış düz döngü 270 ms).
- **Geri kalan sıralı kod CPU'da çalışır** ve yalnızca GPU'ya sırayla iş verir. Yukarıdaki `for k` döngüsü böyledir: veri 100 tur boyunca GPU'da kalır.

Kurallar (derleyici denetler):

- Yalnızca sayısal / `bool` / `char` değerler ve matrisler kullanılabilir. Dizi, liste, string, lambda, global değişken ve `print` kullanılamaz.
- Yalnızca `@GPU` fonksiyonlar çağrılabilir. `@GPU` fonksiyon bir değişkene konamaz; adıyla çağrılır.
- Matris elemanına (`m[i, j]`) yalnızca `parallel for` içinde erişilebilir. Döngünün dışındaki kod CPU'da çalışır, veri ise GPU'dadır.
- `parallel for` gövdesinde matris işlemi (`a + b`, `zeros`, `sum` ...), iç içe `parallel for` ve bunları yapan fonksiyonlara çağrı olamaz. Gövdeden çağrılan fonksiyonlar yalnızca sayılarla çalışmalıdır (`@GPU func kare(float64 v) -> float64` gibi).
- `main` `@GPU` olamaz.

Bilinmesi gerekenler:

- **GPU'da çalışan `parallel for` içinde indeks sınırı denetlenmez.** CPU'da sınır dışı indeks programı mesajla durdurur; GPU'da durdurmaz, sonuç tanımsızdır. Emin olmak için programı önce `--gpu=off` ile dene: orada denetim vardır.
- **Kayan noktalı toplamlar son basamaklarda farklı çıkabilir.** `sum` GPU'da elemanları CPU'dakinden farklı sırayla toplar. Özellikle `float` (32 bit) ile milyonlarca eleman toplanınca fark belirginleşir. GPU'da `a * b + c` ayrıca tek komutla (FMA, ara yuvarlama yapmadan) hesaplanabilir; CUDA derleyicisinin de varsayılanı budur.
- **GPU her işte hızlı değildir.** Program GPU'yu ilk kullandığında yarım saniye kadar bir başlatma süresi vardır ve her kopyalamanın bir maliyeti olur. Küçük işlerde CPU daha hızlıdır; kazanç büyük matrislerde ve veri GPU'da kalırken yapılan çok sayıda işlemde gelir.
- Aynı program `--gpu=off` ile derlenirse tümüyle CPU'da çalışır ve aynı sonucu verir (yukarıdaki toplama farkı dışında).
- `--emit=gpu-mlir` ile hangi kısımların GPU çekirdeği olduğunu görebilirsin.

**Yerleşik fonksiyonlar:**

| | |
|---|---|
| Çıktı | `print(a, b, ...)` — değerleri boşlukla ayırıp yazar, satır sonu ekler |
| Zaman | `time()` — saniye cinsinden zaman (`float64`). Tek başına anlamı yoktur; iki çağrının farkı aradaki süreyi verir. |
| Matematik | `sqrt sin cos tan atan atan2 tanh exp log log2 log10 pow` |
| Yuvarlama | `floor ceil round trunc` (sonuç kayan noktadır: `floor(2.7)` → `2.0`) |
| Diğer | `abs min max` (tamsayıda tamsayı, kayan noktada kayan nokta döner) |
| Listeler | `push(l, x)` `pop(l)` `free(l)` |
| Stringler | `str(x)` — değeri metne çevirir; `to_int(s)` (`int64`), `to_float(s)` (`float64`) |
| Uzunluk | `len(x)` — liste, dizi ya da string; sonuç `int64` |
| Matris oluşturma | `zeros(d1, ..., dn)` `ones(d1, ..., dn)` `fill(d1, ..., dn, deger)` — her boyut için bir uzunluk |
| Matris işlemleri | `sum(a)` `shape(a, i)` — her boyut sayısında |
| Yalnızca iki boyutta | `matmul(a, b)` `transpose(a)` `rows(a)` `cols(a)` |

**Süre ölçmek:**

```
float64 once = time();
matrix<float64> c = matmul(a, b);
print("sure:", time() - once, "saniye");
```

`matmul` CPU'da işlemcinin çekirdeklerine dağıtılır (`parallel for` ile aynı yoldan); diğer matris işlemleri şimdilik tek çekirdekte çalışır. `@GPU` fonksiyonda `time()` yalnızca `parallel for` dışında kullanılabilir. Bir programda GPU ilk kullanıldığında yarım saniye kadar başlatma süresi olur; GPU'daki bir işi ölçerken ilk çağrıyı değil sonrakileri ölç.

**Bilinmesi gereken kurallar:**

- Her programda parametresiz bir `main` olmalı; `void` ya da `int` döndürmeli.
- Sayısal tipler birbirine kendiliğinden dönüşür (`int h = 7.5;` → 7).
- Koşullar `bool` olmalı; `if x { }` yerine `if x != 0 { }` yazılır.
- `for` döngüsünün değişkeni döngü içinde değiştirilemez; `step` pozitif olmalı.
- Lambdalar dışarıdaki yerel değişkenleri kullanamaz; gerekeni parametre olarak ver.

**Henüz olmayanlar:** farklı boyut sayıları arasında işlem (matrise vektör eklemek gibi), dilimleme (`m[0]` ile satır almak), eksen boyunca toplam, matris karşılaştırma, global matris, iç içe listeler, global liste, başlangıç değeri verilen global string, dizi döndüren fonksiyonlar, dizi ve listelerin doğrudan `print` ile yazdırılması, azalan `for` aralıkları, ikili dosyalar.

---

## 9. Sık karşılaşılan sorunlar

| Belirti | Sebep ve çözüm |
|---|---|
| `marlin_lang: No such file or directory` | Derleyici henüz derlenmemiş ya da yanlış dizindesin. Proje dizinine geç ve 2. bölümdeki derleme komutunu çalıştır. |
| `bash: ./deneme: cannot execute` ya da Windows'ta açılmıyor | Dosya bir Linux programı. WSL terminalinde `./deneme` ile çalıştır. |
| `run.sh: $'\r': command not found` | Dosya Windows satır sonlarıyla kaydedilmiş. Düzelt: `sed -i 's/\r$//' tests/run.sh` |
| `Calisma zamani kutuphanesi (libmarlinrt.a) bulunamadi` | Derleyiciyi `cmake-build-debug` dışına kopyaladıysan kütüphaneyi bulamaz. Yolunu ver: `export MARLIN_RUNTIME=/mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace/cmake-build-debug/libmarlinrt.a` |
| `error while loading shared libraries: libmlir_async_runtime.so` | `parallel for` içeren programlar LLVM derlemendeki bir kütüphaneyi kullanır ve yolunu içlerinde taşır. LLVM dizinini taşıdıysan ya da programı başka bir makineye kopyaladıysan bulunamaz; derleyiciyi yeniden derleyip programı yeniden üret. |
| `error while loading shared libraries: libmlir_cuda_runtime.so` | `@GPU` içeren programlar için aynı durum: kütüphane LLVM derlemendedir (`build-gpu/lib`). |
| Program çalışırken `... failed with 'CUDA_ERROR_...'` yazıyor | GPU'ya ulaşılamıyor ya da GPU'da hata oldu. `nvidia-smi` ekran kartını gösteriyor mu bak. GPU belleği yetmiyor olabilir; ya da `parallel for` içinde sınır dışı indeks vardır. `--gpu=off` ile derleyip dene. |
| `@GPU` programı derlenirken `ptxas` bulunamadı hatası | CUDA araç takımı bulunamıyor. `export PATH=/usr/local/cuda-12.9/bin:$PATH` |
| `... hata: sozdizimi hatasi: ...` | Yazım hatası. Mesajın başında dosya, satır ve sütun yazar (`deneme.marl:3:6`); açıklaması ANTLR'dan geldiği için İngilizcedir. |
| `'std.x' adinda bir standart modul yok` | Modül adı yanlış ya da derleyici `std/` dizinini bulamıyor. Derleyiciyi başka yere kopyaladıysan yolunu ver: `export MARLIN_STD=/mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace/std` |
| `modul bulunamadi: 'a.b'` | `a/b.marl` dosyası import eden dosyanın dizininde yok. Yolu denetle ya da dizini `-I` ile ver. |
| Gramerde (`marlin.g4`) yaptığın değişiklik etkisiz | Derleyiciyi yeniden derle; ayrıştırıcı dosyaları gramerden otomatik üretilir. |
| `git` komutu PowerShell'de bulunamıyor | git yalnızca WSL içinde kurulu. WSL terminalinde kullan. |

---

## 10. Proje yapısı

```
marlin.g4            Dilin grameri (ANTLR)
main.cpp            Derleyicinin giriş noktası: aşamaları sırayla çağırır

AST/                Ağaç yapısı ve semantik analiz
  ASTBuilder.h        Ayrıştırma ağacından AST kurar
  ModuleLoader.*      import edilen dosyaları bulur, ayrıştırır, tek programda birleştirir
  Sema.*              Tip denetimi, isim çözümleme, akış analizi
  SemaParallel.cpp    Paralel döngülerin güvenlik denetimi
  SemaGpu.cpp         @GPU fonksiyonların kuralları
  SemaStruct.cpp      Struct tipleri, kurucu çağrıları, alan erişimi
  SemaTask.cpp        Görevler (marl, await): kurallar ve coroutine renklendirmesi
  SemaBorrow.cpp      Fonksiyona verilen liste literalinin ödünç alınıp alınmadığı
  SemType.h           Çözülmüş tipler
  Builtins.h          Yerleşik fonksiyonların listesi

MLIRBackend/        AST → MLIR
  MLIRGenerator*      Ana üretici (program, ifadeler, deyimler)
  MLIRControlFlowManager.*   if ve döngüler (scf ve cf)
  MLIRArithManager.*         Aritmetik ve tip dönüşümleri
  MLIRBuiltinManager.*       print ve matematik
  MLIRListManager.*          list<T>: oluşturma, erişim, push / pop
  MLIRMatrixManager.*        matrix<T>: tensor üzerinde işlemler
  MLIRGeneratorMatrix.cpp    matris ifadeleri, füzyon, bellek sahipliği
  MLIRGeneratorStruct.cpp    struct değerleri (alanlarına ayrılarak üretilir)
  MLIRGeneratorList.cpp      liste elemanları ve struct listeleri (alan başına bir liste)
  MLIRGeneratorString.cpp    stringlerin ve görevlerin belleği (başvuru sayımı)
  MLIRGeneratorTask.cpp      görevler: marl → async.execute, await, coroutine fonksiyonlar
  MLIRGeneratorScan.cpp      scan: sorgu satırlarını struct'lara doldurma
  MLIRRuntime.h              runtime kütüphanesine çağrılar

LLVMBackend/        MLIR → çalıştırılabilir dosya
  LLVMLowering.*      MLIR'i LLVM IR'a indirir
  GpuOffload.*        @GPU fonksiyonlardaki paralel işleri GPU çekirdeğine çevirir
  ParallelMatmul.*    CPU'da matris çarpımını çekirdeklere dağıtır
  NativeCodegen.*     Optimizasyon, nesne dosyası, bağlama

Lsp/                Dil sunucusu (marlin_lang --lsp; editör eklentisi kullanır)
  MarlinLsp.cpp       LSP mesajları (MLIR'in LSP altyapısı üzerinde)
  MarlinCheck.cpp     belgeyi derleyicinin ön ucuyla (ModuleLoader + Sema) denetler

editors/vscode/     VS Code eklentisi: gramer (syntaxes/), snippet'ler, dil sunucusu istemcisi (src/)

runtime/            Derlenen programlara eklenen C kütüphanesi (libmarlinrt.a)
  marlin_rt.c          print, stringler, hata mesajları
  std_*.c             standart kütüphane modüllerinin C ile yazılmış kısmı
  http_parser.*       HTTP/1.1 istek ayrıştırıcısı (soket kullanmaz; std_http.c kullanır)
  marlin_task.c       görev handle'ı (task<T>): MLIR async çalışma zamanının değerini sarar
  std_postgres.c      PostgreSQL sürücüsü (libpq), bağlantı havuzu
  std_sqlite.c        SQLite sürücüsü
std/                Standart kütüphane modülleri (import std.file → std/file.marl)
tests/              Test programları ve run.sh
```

**Gereksinimler** (WSL içinde kurulu olanlar): LLVM/MLIR 19.1.7 (`/home/emin/llvm-project/build-gpu`; X86 ve NVPTX hedefleriyle derlenmiş), ANTLR 4.10 (`/home/emin/antlr-4.10-complete.jar` ve `libantlr4-runtime-dev`), Java, CMake, bir C/C++ derleyicisi. `@GPU` için ayrıca bir NVIDIA ekran kartı ve CUDA araç takımı (`/usr/local/cuda-12.9`). Veritabanı sürücüleri için `libpq-dev` ve `libsqlite3-dev` (isteğe bağlı; yoksa CMake bunları `~/marlin-deps` altında da arar, bkz. `tests/pg_test_sunucu.sh`). Bu yollar `CMakeLists.txt` içinde sabit yazılıdır; başka bir makinede derlemek için orada değiştirmek gerekir.



mkaymaz : 

Dört adım. Hepsini WSL terminalinde yapacaksın.

1. WSL terminalini aç ve proje dizinine geç

Başlat menüsünden "Ubuntu"yu aç (ya da PowerShell'de wsl yaz), sonra:

cd /mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace

2. Bir .marl dosyası oluştur

Proje dizininde deneme.marl adında bir dosya oluştur (CLion'da ya da Not Defteri'nde, fark etmez) ve içine şunu yaz:

func main() -> void {
    int toplam = 0;
    for i in 0 .. 5 {
        toplam = toplam + i;
    }
    print("toplam =", toplam);
}

3. Derle (ELF dosyasına çevir)

cmake-build-debug/marlin_lang deneme.marl -o deneme

Hata yoksa ekrana hiçbir şey yazmaz ve dizinde deneme adında bir dosya oluşur. Bu senin çalıştırılabilir ELF dosyan.

4. Çalıştır

./deneme

Ekranda şunu görürsün:

toplam = 10

Programı değiştirdiğinde 3. ve 4. adımı tekrarlarsın. İkisini tek satırda yapmak istersen:

cmake-build-debug/marlin_lang deneme.marl -o deneme && ./deneme

3. adımda No such file or directory hatası alırsan derleyicinin kendisi henüz derlenmemiş demektir; önce CLion'da Build'e bas ya da şunu çalıştır: