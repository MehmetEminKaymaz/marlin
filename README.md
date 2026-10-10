# minik

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

"Derleyici" dediğimiz şey `minik_lang` adlı programdır. Kaynak kodda (C++ dosyalarında ya da `minik.g4`'te) bir değişiklik yaptığında onu yeniden derlemen gerekir.

**CLion ile:** Her zamanki gibi Build (çekiç simgesi). CLion bunu WSL üzerinden yapar.

**Terminalden:**

```bash
cmake --build cmake-build-debug -j 6
```

İlk derleme birkaç dakika sürebilir, sonrakiler yalnızca değişen dosyaları derler. Sonunda iki dosya oluşur:

- `cmake-build-debug/minik_lang` — derleyici
- `cmake-build-debug/libminikrt.a` — derlenen programlara eklenen yardımcı kütüphane (`print` bunun içinde)

> Ekrana LLVM başlıklarından gelen çok uzun `TypeRange ... operator==` uyarıları basılır. Bunlar zararsızdır; bizim kodumuzla ilgili değildir. Asıl bakman gereken satır içinde `error` geçenlerdir.

`cmake-build-debug` dizinini silersen ya da hiç yoksa önce şunu çalıştır:

```bash
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
```

---

## 3. İlk programını çalıştırmak

Bir dosya oluştur, örneğin `deneme.minik`:

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
cmake-build-debug/minik_lang deneme.minik -o deneme
./deneme
```

Çıktı:

```
toplam = 10
karekok: 3.1622776601683795
```

`-o deneme` çıktı dosyasının adını belirler. Vermezsen dosya `a.out` adıyla oluşur.

Hiç dosya vermeden `cmake-build-debug/minik_lang` çalıştırırsan `main.cpp` içine gömülü örnek program derlenir.

### Derleme hatası alırsan

Hatalar dosya, satır ve sütun numarasıyla gösterilir:

```
deneme.minik:3:13: hata: tip uyusmazligi: beklenen int, bulunan bool
```

Bu "deneme.minik dosyasının 3. satırının 13. sütununda" demektir.

---

## 4. Derleyicinin seçenekleri

```
minik_lang [kaynak.minik] [secenekler]
```

| Seçenek | Ne yapar |
|---|---|
| `-o dosya` | Çıktı dosyasının adı |
| `-O0` `-O1` `-O2` `-O3` | Optimizasyon seviyesi. Varsayılan `-O0` (optimizasyon yok). |
| `--emit=...` | Derlemeyi hangi aşamada durduracağını seçer (aşağıya bak) |
| `--print-ast` | Programın ağaç yapısını (AST) ekrana yazar |
| `--help` | Kısa yardım |

### Ara aşamaları görmek: `--emit`

Derleyici programı adım adım dönüştürür. `--emit` ile istediğin adımda durup o andaki hali görebilirsin. Bir şey yanlış çalışıyorsa sorunun hangi adımda başladığını bulmak için çok işe yarar.

```
.minik → AST → MLIR → LLVM'e indirilmiş MLIR → LLVM IR → nesne dosyası → çalıştırılabilir
```

| Komut | Gösterdiği |
|---|---|
| `--emit=mlir` | Üst seviye MLIR. Döngülerin `scf.for` mu yoksa `cf` bloğu mu olduğunu burada görürsün. |
| `--emit=llvm-mlir` | LLVM dialect'ine indirilmiş MLIR |
| `--emit=llvm-ir` | LLVM IR (`.ll` dosyalarındaki biçim) |
| `--emit=obj` | Nesne dosyası (`.o`), henüz bağlanmamış |
| `--emit=exe` | Çalıştırılabilir dosya (varsayılan) |

Metin çıktıları `-o` verilmezse ekrana yazılır. Örnekler:

```bash
# Üretilen MLIR'e bak
cmake-build-debug/minik_lang deneme.minik --emit=mlir

# Optimizasyondan sonra LLVM IR'ı dosyaya yaz
cmake-build-debug/minik_lang deneme.minik --emit=llvm-ir -O2 -o deneme.ll

# AST'yi gör
cmake-build-debug/minik_lang deneme.minik --print-ast --emit=mlir
```

---

## 5. Hazır testleri çalıştırmak

`tests/` dizininde hazır test programları var. Hepsini tek komutla çalıştır:

```bash
bash tests/run.sh
```

Örnek çıktı:

```
  GECTI  break_continue.minik -O0
  GECTI  break_continue.minik -O2
  GECTI  hatalar.minik
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
| `yapisal_donguler.minik` | Dışarı atlamayan `for` ve `while` döngüleri |
| `break_continue.minik` | `break` ve `continue`, iç içe döngüler |
| `erken_return.minik` | Döngü ve `if` içinden `return` |
| `tipler.minik` | Tip dönüşümleri, işaretsiz sayılar, `&&` ve `\|\|` |
| `fonksiyonlar.minik` | Global değişkenler, özyineleme, lambdalar |
| `print.minik` | `print` ile her tipin yazdırılması |
| `matematik.minik` | `sqrt`, `pow`, `min`, `max` ve diğerleri |
| `hatalar.minik` | Derleyicinin yakalaması gereken hatalar |
| `hatalar_yerlesik.minik` | `print` ve matematik fonksiyonlarının hatalı kullanımı |
| `listeler.minik` | `list<T>`: `push`, `pop`, `len`, başvuru davranışı, büyüme |
| `liste_sinir_disi.minik` | Sınır dışı liste indeksinde programın mesajla durması |
| `liste_bos_pop.minik` | Boş listeden `pop` yapılınca programın mesajla durması |
| `hatalar_liste.minik` | Listelerin hatalı kullanımı |
| `matrisler.minik` | `matrix<T>`: oluşturma, eleman eleman işlemler, `matmul`, değer davranışı |
| `matris_boyut_uyusmazligi.minik` | Farklı boyutta matrisler toplanınca programın mesajla durması |
| `matris_sinir_disi.minik` | Sınır dışı matris indeksinde programın mesajla durması |
| `matris_matmul_boyut.minik` | Boyutları uygun olmayan `matmul` |
| `hatalar_matris.minik` | Matrislerin hatalı kullanımı |
| `cok_boyutlu.minik` | `matrix<T, N>`: vektör, üç ve dört boyut, `shape`, çok seviyeli literal |
| `cok_boyutlu_calisma_hatalari.minik` | Üç boyutta sınır dışı indeks |
| `cok_boyutlu_sekil_uyusmazligi.minik` | Üç boyutta uzunlukları farklı matrislerle işlem |
| `hatalar_cok_boyutlu.minik` | Boyut sayısıyla ilgili derleme hataları |
| `paralel.minik` | `parallel for`: liste, dizi ve matrise paralel yazma, iç içe paralel döngü |
| `hatalar_paralel.minik` | Paralel döngüde yarışa yol açabilecek kullanımların reddedilmesi |

---

## 6. Kendi testini yazmak

Yeni bir test eklemek için `tests/` dizinine `.minik` uzantılı bir dosya koyman yeterli; `run.sh` onu kendiliğinden bulur. Testin ne beklediğini dosyanın başındaki özel yorum satırlarıyla söylersin.

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

### Bir test kaldığında

`run.sh` neyin farklı olduğunu gösterir:

```
  KALDI  deneme.minik -O0: cikti farkli:
           -10
           +11
```

`-` ile başlayan satır beklenen, `+` ile başlayan satır programın gerçekte yazdığıdır.

Sorunu aramak için o tek dosyayı elle derleyip çalıştır:

```bash
cmake-build-debug/minik_lang tests/deneme.minik -o /tmp/deneme && /tmp/deneme
echo "cikis kodu: $?"
```

Sonuç yalnızca `-O2` ile yanlışsa (ya da yalnızca `-O0` ile), `--emit=mlir` ve `--emit=llvm-ir` çıktılarına bakmak iyi bir başlangıçtır.

---

## 7. Başka neler denenebilir

**Üretilen MLIR'i incelemek.** Bir döngünün yapısal (`scf.for`, `scf.while`) mı yoksa bloklu (`cf.br`) mı üretildiğini görmek için:

```bash
cmake-build-debug/minik_lang tests/break_continue.minik --emit=mlir | grep -E 'scf\.|cf\.'
```

Kural: gövdesinde `break` ya da `return` olmayan döngüler `scf` olarak üretilir. Bu, ileride GPU'ya göndereceğimiz döngülerin tanınabilmesi için önemlidir.

**MLIR araçlarıyla oynamak.** LLVM derlemenle birlikte gelen araçlar `/home/emin/llvm-project/build/bin` dizinindedir. Örneğin ürettiğimiz MLIR'i `mlir-opt` ile kendin dönüştürebilirsin:

```bash
cmake-build-debug/minik_lang deneme.minik --emit=mlir -o deneme.mlir
/home/emin/llvm-project/build/bin/mlir-opt deneme.mlir --convert-scf-to-cf
```

Yeni bir MLIR pass'ini derleyiciye eklemeden önce etkisini görmek için kullanışlıdır.

**Optimizasyonun etkisini görmek.** Aynı programın `-O0` ve `-O2` LLVM IR'ını karşılaştır:

```bash
cmake-build-debug/minik_lang deneme.minik --emit=llvm-ir -O0 -o o0.ll
cmake-build-debug/minik_lang deneme.minik --emit=llvm-ir -O2 -o o2.ll
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

**Tipler:** `int8` `int16` `int` `int64`, `uint8` `uint16` `uint` `uint64`, `float` (32 bit) `float64`, `bool`, `char`, `string`, sabit boyutlu diziler (`int[5]`), listeler (`list<int>`), matrisler (`matrix<float>`), fonksiyon tipleri (`(int) -> int`).

**Dizi mi, liste mi?**

| | Dizi `int[5]` | Liste `list<int>` |
|---|---|---|
| Boyut | Kaynak kodda sabit | Çalışırken değişir (`push`, `pop`) |
| Fonksiyona verme | Boyut tipin parçası: `int[5]` alan fonksiyon yalnızca 5 elemanlı dizi alır | Her uzunlukta liste alır |
| Fonksiyondan döndürme | Yok | Var |
| Sınır denetimi | Yalnızca sabit indekste, derlerken | Her erişimde, çalışırken |
| `[1, 2, 3]` yazınca | `let a = [1, 2, 3];` dizi oluşturur | Tipi yazarsan liste olur: `list<int> l = [1, 2, 3];` |

Listeler **başvuru** olarak dolaşır: `list<int> b = a;` listeyi kopyalamaz, `a` ve `b` aynı listeyi gösterir; birine eklenen eleman diğerinde de görünür. Fonksiyona verilen liste de aynı listedir, fonksiyon onu değiştirebilir.

Listelerin belleği kendiliğinden geri verilmez; program bitene kadar yaşar. Uzun çalışan bir programda işi biten listeyi `free(l)` ile bırakabilirsin (bıraktıktan sonra o listeyi kullanma). Liste elemanları şimdilik yalnızca sayı, `bool` ya da `char` olabilir.

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

**Yerleşik fonksiyonlar:**

| | |
|---|---|
| Çıktı | `print(a, b, ...)` — değerleri boşlukla ayırıp yazar, satır sonu ekler |
| Matematik | `sqrt sin cos tan atan atan2 tanh exp log log2 log10 pow` |
| Yuvarlama | `floor ceil round trunc` (sonuç kayan noktadır: `floor(2.7)` → `2.0`) |
| Diğer | `abs min max` (tamsayıda tamsayı, kayan noktada kayan nokta döner) |
| Listeler | `push(l, x)` `pop(l)` `free(l)` |
| Uzunluk | `len(x)` — liste, dizi ya da string; sonuç `int64` |
| Matris oluşturma | `zeros(d1, ..., dn)` `ones(d1, ..., dn)` `fill(d1, ..., dn, deger)` — her boyut için bir uzunluk |
| Matris işlemleri | `sum(a)` `shape(a, i)` — her boyut sayısında |
| Yalnızca iki boyutta | `matmul(a, b)` `transpose(a)` `rows(a)` `cols(a)` |

**Bilinmesi gereken kurallar:**

- Her programda parametresiz bir `main` olmalı; `void` ya da `int` döndürmeli.
- Sayısal tipler birbirine kendiliğinden dönüşür (`int h = 7.5;` → 7).
- Koşullar `bool` olmalı; `if x { }` yerine `if x != 0 { }` yazılır.
- `for` döngüsünün değişkeni döngü içinde değiştirilemez; `step` pozitif olmalı.
- Lambdalar dışarıdaki yerel değişkenleri kullanamaz; gerekeni parametre olarak ver.

**Henüz olmayanlar:** farklı boyut sayıları arasında işlem (matrise vektör eklemek gibi), dilimleme (`m[0]` ile satır almak), eksen boyunca toplam, matris karşılaştırma, global matris, iç içe listeler, string listesi, global liste, dizi döndüren fonksiyonlar, dizi ve listelerin doğrudan `print` ile yazdırılması, string işlemleri (birleştirme, karşılaştırma), azalan `for` aralıkları, kullanıcıdan girdi okuma.

---

## 9. Sık karşılaşılan sorunlar

| Belirti | Sebep ve çözüm |
|---|---|
| `minik_lang: No such file or directory` | Derleyici henüz derlenmemiş ya da yanlış dizindesin. Proje dizinine geç ve 2. bölümdeki derleme komutunu çalıştır. |
| `bash: ./deneme: cannot execute` ya da Windows'ta açılmıyor | Dosya bir Linux programı. WSL terminalinde `./deneme` ile çalıştır. |
| `run.sh: $'\r': command not found` | Dosya Windows satır sonlarıyla kaydedilmiş. Düzelt: `sed -i 's/\r$//' tests/run.sh` |
| `Calisma zamani kutuphanesi (libminikrt.a) bulunamadi` | Derleyiciyi `cmake-build-debug` dışına kopyaladıysan kütüphaneyi bulamaz. Yolunu ver: `export MINIK_RUNTIME=/mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace/cmake-build-debug/libminikrt.a` |
| `error while loading shared libraries: libmlir_async_runtime.so` | `parallel for` içeren programlar LLVM derlemendeki bir kütüphaneyi kullanır ve yolunu içlerinde taşır. LLVM dizinini taşıdıysan ya da programı başka bir makineye kopyaladıysan bulunamaz; derleyiciyi yeniden derleyip programı yeniden üret. |
| `Syntax hatasi bulundu!` | Yazım hatası. Hemen üstünde satır ve sütunu gösteren bir `line 3:5 ...` mesajı vardır. |
| Gramerde (`minik.g4`) yaptığın değişiklik etkisiz | Derleyiciyi yeniden derle; ayrıştırıcı dosyaları gramerden otomatik üretilir. |
| `git` komutu PowerShell'de bulunamıyor | git yalnızca WSL içinde kurulu. WSL terminalinde kullan. |

---

## 10. Proje yapısı

```
minik.g4            Dilin grameri (ANTLR)
main.cpp            Derleyicinin giriş noktası: aşamaları sırayla çağırır

AST/                Ağaç yapısı ve semantik analiz
  ASTBuilder.h        Ayrıştırma ağacından AST kurar
  Sema.*              Tip denetimi, isim çözümleme, akış analizi
  SemaParallel.cpp    Paralel döngülerin güvenlik denetimi
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
  MLIRRuntime.h              runtime kütüphanesine çağrılar

LLVMBackend/        MLIR → çalıştırılabilir dosya
  LLVMLowering.*      MLIR'i LLVM IR'a indirir
  NativeCodegen.*     Optimizasyon, nesne dosyası, bağlama

runtime/minik_rt.c  Derlenen programlara eklenen C kütüphanesi (print, hata mesajları)
tests/              Test programları ve run.sh
```

**Gereksinimler** (WSL içinde kurulu olanlar): LLVM/MLIR 19.1.7 (`/home/emin/llvm-project/build`), ANTLR 4.10 (`/home/emin/antlr-4.10-complete.jar` ve `libantlr4-runtime-dev`), Java, CMake, bir C/C++ derleyicisi. Bu yollar `CMakeLists.txt` içinde sabit yazılıdır; başka bir makinede derlemek için orada değiştirmek gerekir.



mkaymaz : 

Dört adım. Hepsini WSL terminalinde yapacaksın.

1. WSL terminalini aç ve proje dizinine geç

Başlat menüsünden "Ubuntu"yu aç (ya da PowerShell'de wsl yaz), sonra:

cd /mnt/c/Users/Eminf/OneDrive/Desktop/MLIRWorkspace

2. Bir .minik dosyası oluştur

Proje dizininde deneme.minik adında bir dosya oluştur (CLion'da ya da Not Defteri'nde, fark etmez) ve içine şunu yaz:

func main() -> void {
    int toplam = 0;
    for i in 0 .. 5 {
        toplam = toplam + i;
    }
    print("toplam =", toplam);
}

3. Derle (ELF dosyasına çevir)

cmake-build-debug/minik_lang deneme.minik -o deneme

Hata yoksa ekrana hiçbir şey yazmaz ve dizinde deneme adında bir dosya oluşur. Bu senin çalıştırılabilir ELF dosyan.

4. Çalıştır

./deneme

Ekranda şunu görürsün:

toplam = 10

Programı değiştirdiğinde 3. ve 4. adımı tekrarlarsın. İkisini tek satırda yapmak istersen:

cmake-build-debug/minik_lang deneme.minik -o deneme && ./deneme

3. adımda No such file or directory hatası alırsan derleyicinin kendisi henüz derlenmemiş demektir; önce CLion'da Build'e bas ya da şunu çalıştır: