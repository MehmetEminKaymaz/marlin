// JSON ayrıştırıcısı (bkz. json_parser.h). Kurallar RFC 8259'dan; her
// kontrolün yanında ilgili madde yazar.

#include "json_parser.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* text;
    size_t length;
    size_t pos;
    JsonDoc* doc;
    int64_t* stack;  // açık dizi / nesnelerin henüz yerleşmemiş elemanları
    int64_t stack_count, stack_capacity;
    int failed;
} Parser;

// ---------------------------------------------------------------------------
// Yardımcılar
// ---------------------------------------------------------------------------

// İlk hata kaydedilir; sonrakiler (geri dönüş zinciri) yok sayılır.
static int fail(Parser* p, size_t at, const char* format, ...) {
    if (p->failed) return 0;
    p->failed = 1;
    size_t line = 1, column = 1;
    for (size_t i = 0; i < at && i < p->length; ++i) {
        if (p->text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    int n = snprintf(p->doc->error, sizeof p->doc->error, "satir %zu, sutun %zu: ", line, column);
    va_list args;
    va_start(args, format);
    vsnprintf(p->doc->error + n, sizeof p->doc->error - (size_t)n, format, args);
    va_end(args);
    return 0;
}

// `*items` dizisinde `count + extra` eleman için yer açar.
static int reserve(Parser* p, void** items, int64_t* capacity, int64_t count, int64_t extra,
                   size_t itemSize) {
    if (count + extra <= *capacity) return 1;
    int64_t newCapacity = *capacity ? *capacity : 16;
    while (newCapacity < count + extra) newCapacity *= 2;
    void* grown = realloc(*items, (size_t)newCapacity * itemSize);
    if (!grown) return fail(p, p->pos, "bellek yetersiz");
    *items = grown;
    *capacity = newCapacity;
    return 1;
}

static int64_t new_node(Parser* p, JsonKind kind) {
    JsonDoc* d = p->doc;
    if (!reserve(p, (void**)&d->nodes, &d->node_capacity, d->node_count, 1, sizeof(JsonNode)))
        return -1;
    JsonNode* node = &d->nodes[d->node_count];
    memset(node, 0, sizeof *node);
    node->kind = kind;
    node->key = -1;
    return d->node_count++;
}

static int pool_append(Parser* p, const char* bytes, size_t n) {
    if (n == 0) return 1;
    JsonDoc* d = p->doc;
    if (!reserve(p, (void**)&d->pool, &d->pool_capacity, d->pool_length, (int64_t)n, 1)) return 0;
    memcpy(d->pool + d->pool_length, bytes, n);
    d->pool_length += (int64_t)n;
    return 1;
}

// Boşluk: SP, HT, LF, CR (RFC 8259 §2).
static void skip_space(Parser* p) {
    while (p->pos < p->length) {
        const char c = p->text[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++p->pos;
    }
}

static int peek(const Parser* p) { return p->pos < p->length ? (unsigned char)p->text[p->pos] : -1; }

// "X bekleniyordu, 'y' bulundu"  ya da  "X bekleniyordu, girdi bitti"
static int unexpected(Parser* p, const char* expected) {
    const int c = peek(p);
    if (c < 0) return fail(p, p->pos, "%s bekleniyordu, girdi bitti", expected);
    if (c >= 0x20 && c < 0x7F) return fail(p, p->pos, "%s bekleniyordu, '%c' bulundu", expected, c);
    return fail(p, p->pos, "%s bekleniyordu, 0x%02X bayti bulundu", expected, c);
}

// ---------------------------------------------------------------------------
// String (RFC 8259 §7)
// ---------------------------------------------------------------------------

static int hex4(Parser* p, unsigned* out) {
    if (p->pos + 4 > p->length) return fail(p, p->pos, "\\u kacisinda 4 onaltilik rakam bekleniyordu");
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = p->text[p->pos + (size_t)i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return fail(p, p->pos + (size_t)i, "\\u kacisinda gecersiz onaltilik rakam");
        v = v * 16 + d;
    }
    p->pos += 4;
    *out = v;
    return 1;
}

static int append_code_point(Parser* p, unsigned cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return pool_append(p, b, n);
}

// Kaçış dizisi (ters bölüden sonrası).
static int parse_escape(Parser* p) {
    const size_t at = p->pos - 1;
    const int c = peek(p);
    ++p->pos;
    switch (c) {
        case '"': return pool_append(p, "\"", 1);
        case '\\': return pool_append(p, "\\", 1);
        case '/': return pool_append(p, "/", 1);
        case 'b': return pool_append(p, "\b", 1);
        case 'f': return pool_append(p, "\f", 1);
        case 'n': return pool_append(p, "\n", 1);
        case 'r': return pool_append(p, "\r", 1);
        case 't': return pool_append(p, "\t", 1);
        case 'u': {
            unsigned cp;
            if (!hex4(p, &cp)) return 0;
            // UTF-16 vekil çifti: 😀 → U+1F600
            if (cp >= 0xDC00 && cp <= 0xDFFF) return fail(p, at, "eslesmeyen ikinci vekil (\\u%04X)", cp);
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (p->pos + 2 > p->length || p->text[p->pos] != '\\' || p->text[p->pos + 1] != 'u')
                    return fail(p, at, "vekil cifti eksik (\\u%04X'ten sonra \\uDC00-\\uDFFF bekleniyordu)", cp);
                p->pos += 2;
                unsigned low;
                if (!hex4(p, &low)) return 0;
                if (low < 0xDC00 || low > 0xDFFF)
                    return fail(p, at, "vekil cifti eksik (\\u%04X'ten sonra \\uDC00-\\uDFFF bekleniyordu)", cp);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            return append_code_point(p, cp);
        }
        default:
            p->pos = at + 1;
            return unexpected(p, "kacis dizisi (\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\uXXXX)");
    }
}

// Geçerli bir UTF-8 dizisinin bayt sayısı; geçersizse 0 (RFC 3629 §4: fazla
// uzun kodlamalar ve vekiller geçersizdir).
static size_t utf8_sequence(const unsigned char* s, size_t available) {
    const unsigned char c = s[0];
    unsigned char lo = 0x80, hi = 0xBF;
    size_t n;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c == 0xE0) { n = 3; lo = 0xA0; }
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) n = 3;
    else if (c == 0xED) { n = 3; hi = 0x9F; }
    else if (c == 0xF0) { n = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3) n = 4;
    else if (c == 0xF4) { n = 4; hi = 0x8F; }
    else return 0;
    if (available < n) return 0;
    if (s[1] < lo || s[1] > hi) return 0;
    for (size_t i = 2; i < n; ++i)
        if (s[i] < 0x80 || s[i] > 0xBF) return 0;
    return n;
}

// Açılış tırnağındayken çağrılır; çözülmüş metni pool'a yazar.
static int parse_string(Parser* p, int64_t* offset, int64_t* length) {
    ++p->pos; // "
    *offset = p->doc->pool_length;
    for (;;) {
        if (p->pos >= p->length) return fail(p, p->pos, "string kapanmadan girdi bitti");
        const unsigned char c = (unsigned char)p->text[p->pos];
        if (c == '"') {
            ++p->pos;
            break;
        }
        if (c == '\\') {
            ++p->pos;
            if (!parse_escape(p)) return 0;
            continue;
        }
        if (c < 0x20) return fail(p, p->pos, "stringte kacissiz denetim karakteri (0x%02X)", c);
        if (c < 0x80) {
            // Kaçışsız ASCII bölümü bir seferde eklenir.
            size_t end = p->pos + 1;
            while (end < p->length) {
                const unsigned char e = (unsigned char)p->text[end];
                if (e == '"' || e == '\\' || e < 0x20 || e >= 0x80) break;
                ++end;
            }
            if (!pool_append(p, p->text + p->pos, end - p->pos)) return 0;
            p->pos = end;
            continue;
        }
        const size_t n = utf8_sequence((const unsigned char*)p->text + p->pos, p->length - p->pos);
        if (!n) return fail(p, p->pos, "gecersiz UTF-8");
        if (!pool_append(p, p->text + p->pos, n)) return 0;
        p->pos += n;
    }
    *length = p->doc->pool_length - *offset;
    return 1;
}

// ---------------------------------------------------------------------------
// Sayı (RFC 8259 §6)
// ---------------------------------------------------------------------------

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int64_t parse_number(Parser* p) {
    const size_t start = p->pos;
    int negative = 0, isInteger = 1;
    if (peek(p) == '-') {
        negative = 1;
        ++p->pos;
    }
    if (peek(p) == '0') {
        ++p->pos;
        if (is_digit(peek(p))) return fail(p, p->pos - 1, "sayi 0 ile baslayip devam edemez (or. 07)"), -1;
    } else if (is_digit(peek(p))) {
        while (is_digit(peek(p))) ++p->pos;
    } else {
        return unexpected(p, "rakam"), -1;
    }
    const size_t integerEnd = p->pos;
    if (peek(p) == '.') {
        isInteger = 0;
        ++p->pos;
        if (!is_digit(peek(p))) return unexpected(p, "noktadan sonra rakam"), -1;
        while (is_digit(peek(p))) ++p->pos;
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        isInteger = 0;
        ++p->pos;
        if (peek(p) == '+' || peek(p) == '-') ++p->pos;
        if (!is_digit(peek(p))) return unexpected(p, "us icin rakam"), -1;
        while (is_digit(peek(p))) ++p->pos;
    }

    const int64_t index = new_node(p, JSON_NUMBER);
    if (index < 0) return -1;
    JsonNode* node = &p->doc->nodes[index];

    // Tamsayı: büyüklüğü işaretsiz biriktirilir; int64'e sığmazsa float64 olur.
    if (isInteger) {
        uint64_t magnitude = 0;
        int overflow = 0;
        for (size_t i = start + (size_t)negative; i < integerEnd; ++i) {
            const unsigned d = (unsigned)(p->text[i] - '0');
            if (magnitude > (UINT64_MAX - d) / 10) { overflow = 1; break; }
            magnitude = magnitude * 10 + d;
        }
        const uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
        if (!overflow && magnitude <= limit) {
            node->is_integer = 1;
            node->integer = negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
            node->number = (double)node->integer;
            return index;
        }
    }

    // strtod sondaki NUL'a güvenir: sayının kopyası üzerinde çalışılır. (Dil
    // ayarı yerelleştirilmediği için ondalık ayıracı her zaman noktadır.)
    const size_t n = p->pos - start;
    char small[64];
    char* copy = n < sizeof small ? small : malloc(n + 1);
    if (!copy) return fail(p, start, "bellek yetersiz"), -1;
    memcpy(copy, p->text + start, n);
    copy[n] = '\0';
    const double value = strtod(copy, NULL);
    if (copy != small) free(copy);
    if (isinf(value)) return fail(p, start, "sayi float64'e sigmiyor"), -1;
    p->doc->nodes[index].number = value;
    return index;
}

// ---------------------------------------------------------------------------
// Değerler (RFC 8259 §3, §4, §5)
// ---------------------------------------------------------------------------

static int64_t parse_value(Parser* p, int depth);

static int push_child(Parser* p, int64_t child) {
    if (!reserve(p, (void**)&p->stack, &p->stack_capacity, p->stack_count, 1, sizeof(int64_t))) return 0;
    p->stack[p->stack_count++] = child;
    return 1;
}

// Yığıttaki `first` sonrası elemanları belgenin children dizisine taşır.
static int settle_children(Parser* p, int64_t node, int64_t first) {
    JsonDoc* d = p->doc;
    const int64_t count = p->stack_count - first;
    d->nodes[node].offset = d->child_count;
    d->nodes[node].length = count;
    if (count == 0) return 1; // (boş dizi: henüz ayrılmamış belleğe dokunulmaz)
    if (!reserve(p, (void**)&d->children, &d->child_capacity, d->child_count, count, sizeof(int64_t)))
        return 0;
    memcpy(d->children + d->child_count, p->stack + first, (size_t)count * sizeof(int64_t));
    d->nodes[node].offset = d->child_count;
    d->nodes[node].length = count;
    d->child_count += count;
    p->stack_count = first;
    return 1;
}

static int64_t parse_array(Parser* p, int depth) {
    const int64_t node = new_node(p, JSON_ARRAY);
    if (node < 0) return -1;
    ++p->pos; // [
    const int64_t first = p->stack_count;
    skip_space(p);
    if (peek(p) == ']') {
        ++p->pos;
    } else {
        for (;;) {
            const int64_t child = parse_value(p, depth + 1);
            if (child < 0 || !push_child(p, child)) return -1;
            skip_space(p);
            if (peek(p) == ',') {
                ++p->pos;
                skip_space(p);
                continue;
            }
            if (peek(p) == ']') {
                ++p->pos;
                break;
            }
            return unexpected(p, "',' ya da ']'"), -1;
        }
    }
    return settle_children(p, node, first) ? node : -1;
}

static int64_t parse_object(Parser* p, int depth) {
    const int64_t node = new_node(p, JSON_OBJECT);
    if (node < 0) return -1;
    ++p->pos; // {
    const int64_t first = p->stack_count;
    skip_space(p);
    if (peek(p) == '}') {
        ++p->pos;
    } else {
        for (;;) {
            if (peek(p) != '"') return unexpected(p, "anahtar (tirnakli string)"), -1;
            int64_t keyOffset, keyLength;
            if (!parse_string(p, &keyOffset, &keyLength)) return -1;
            skip_space(p);
            if (peek(p) != ':') return unexpected(p, "':'"), -1;
            ++p->pos;
            skip_space(p);
            const int64_t child = parse_value(p, depth + 1);
            if (child < 0 || !push_child(p, child)) return -1;
            p->doc->nodes[child].key = keyOffset;
            p->doc->nodes[child].key_length = keyLength;
            skip_space(p);
            if (peek(p) == ',') {
                ++p->pos;
                skip_space(p);
                continue;
            }
            if (peek(p) == '}') {
                ++p->pos;
                break;
            }
            return unexpected(p, "',' ya da '}'"), -1;
        }
    }
    return settle_children(p, node, first) ? node : -1;
}

static int64_t parse_literal(Parser* p, const char* word, JsonKind kind, int64_t value) {
    const size_t n = strlen(word);
    if (p->length - p->pos < n || memcmp(p->text + p->pos, word, n) != 0)
        return fail(p, p->pos, "gecersiz deger (true, false, null, sayi, string, dizi ya da nesne bekleniyordu)"), -1;
    p->pos += n;
    const int64_t node = new_node(p, kind);
    if (node >= 0) p->doc->nodes[node].integer = value;
    return node;
}

static int64_t parse_value(Parser* p, int depth) {
    if (depth > JSON_MAX_DEPTH) return fail(p, p->pos, "ic ice gecme cok derin (en fazla %d)", JSON_MAX_DEPTH), -1;
    switch (peek(p)) {
        case '{': return parse_object(p, depth);
        case '[': return parse_array(p, depth);
        case '"': {
            const int64_t node = new_node(p, JSON_STRING);
            if (node < 0) return -1;
            int64_t offset, length;
            if (!parse_string(p, &offset, &length)) return -1;
            p->doc->nodes[node].offset = offset;
            p->doc->nodes[node].length = length;
            return node;
        }
        case 't': return parse_literal(p, "true", JSON_BOOL, 1);
        case 'f': return parse_literal(p, "false", JSON_BOOL, 0);
        case 'n': return parse_literal(p, "null", JSON_NULL, 0);
        default:
            if (peek(p) == '-' || is_digit(peek(p))) return parse_number(p);
            return unexpected(p, "deger (true, false, null, sayi, string, dizi ya da nesne)"), -1;
    }
}

// ---------------------------------------------------------------------------
// Belge
// ---------------------------------------------------------------------------

static void release_nodes(JsonDoc* doc) {
    free(doc->nodes);
    free(doc->children);
    free(doc->pool);
    doc->nodes = NULL;
    doc->children = NULL;
    doc->pool = NULL;
    doc->node_count = doc->child_count = doc->pool_length = 0;
}

JsonDoc* json_parse(const char* text, size_t length) {
    JsonDoc* doc = calloc(1, sizeof *doc);
    if (!doc) return NULL;
    Parser p = {text, length, 0, doc, NULL, 0, 0, 0};
    skip_space(&p);
    if (p.pos == length) {
        fail(&p, p.pos, "bos girdi (bir JSON degeri bekleniyordu)");
    } else if (parse_value(&p, 1) >= 0) {
        skip_space(&p);
        if (p.pos != length) fail(&p, p.pos, "degerden sonra fazladan karakter var");
    }
    free(p.stack);
    doc->ok = !p.failed;
    if (!doc->ok) release_nodes(doc);
    return doc;
}

void json_free(JsonDoc* doc) {
    if (!doc) return;
    release_nodes(doc);
    free(doc);
}

JsonKind json_kind(const JsonDoc* doc, int64_t node) {
    if (node < 0 || node >= doc->node_count) return JSON_MISSING;
    return (JsonKind)doc->nodes[node].kind;
}

// Aynı anahtar birden çok kez varsa sonuncusu geçerlidir: sondan aranır.
int64_t json_member(const JsonDoc* doc, int64_t object, const char* key, size_t keyLength) {
    if (json_kind(doc, object) != JSON_OBJECT) return -1;
    const JsonNode* o = &doc->nodes[object];
    for (int64_t i = o->length; i-- > 0;) {
        const int64_t child = doc->children[o->offset + i];
        const JsonNode* c = &doc->nodes[child];
        if ((size_t)c->key_length == keyLength && memcmp(doc->pool + c->key, key, keyLength) == 0)
            return child;
    }
    return -1;
}

int64_t json_child(const JsonDoc* doc, int64_t node, int64_t index) {
    const JsonKind kind = json_kind(doc, node);
    if (kind != JSON_ARRAY && kind != JSON_OBJECT) return -1;
    const JsonNode* n = &doc->nodes[node];
    if (index < 0 || index >= n->length) return -1;
    return doc->children[n->offset + index];
}

size_t json_quote(const char* text, size_t length, char* out) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
#define PUT(c) do { if (out) out[n] = (c); ++n; } while (0)
    PUT('"');
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)text[i];
        switch (c) {
            case '"': PUT('\\'); PUT('"'); break;
            case '\\': PUT('\\'); PUT('\\'); break;
            case '\b': PUT('\\'); PUT('b'); break;
            case '\f': PUT('\\'); PUT('f'); break;
            case '\n': PUT('\\'); PUT('n'); break;
            case '\r': PUT('\\'); PUT('r'); break;
            case '\t': PUT('\\'); PUT('t'); break;
            default:
                if (c < 0x20) {
                    PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hex[c >> 4]); PUT(hex[c & 15]);
                } else {
                    PUT((char)c); // UTF-8 baytları olduğu gibi
                }
        }
    }
    PUT('"');
#undef PUT
    return n;
}
