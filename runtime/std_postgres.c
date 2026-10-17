// std.postgres — PostgreSQL istemcisi (bkz. std/postgres.marl).
//
// PostgreSQL'in resmi C kütüphanesi libpq'yu sarar. marlin tarafına üç tür
// nesne adres olarak (int64) verilir:
//
//   Conn    bir bağlantı (PGconn) + kilidi
//   Pool    sabit sayıda bağlantı; sorgu boş olanı alır, iş bitince geri verir
//   Result  bir sorgunun sonucu (PGresult); değişmez, aynı anda okunabilir
//
// İŞ PARÇACIKLARI
//
// libpq'da bir bağlantı aynı anda yalnızca bir iş parçacığından kullanılabilir.
// Bağlantı handle'ı sıradan bir sayı olduğundan iki göreve (marl) verilebilir;
// bu yüzden her bağlantının bir kilidi vardır: aynı bağlantıdaki sorgular
// sıraya girer, bozulmaz. Görevlerin sorguları gerçekten aynı anda çalışsın
// diye havuz kullanılır.
//
// Hatalar: bağlanılamazsa 0 döner, sebebi c_error'dadır (iş parçacığına
// özel). Sorgu hataları (sözdizimi, kısıt ihlali ...) programı durdurmaz;
// sonucun ok / error / code (SQLSTATE) alanlarındadır. Programı yalnızca
// kullanım hataları durdurur (sınır dışı satır / sütun).

#include "marlin_rt.h"

#include <libpq-fe.h>
#include "platform.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    PGconn* pg;
    mr_mutex lock;
} Conn;

typedef struct {
    Conn** conns;
    int size;
    int* free_list;       // boş bağlantıların sırası (yığıt)
    int free_count;
    mr_mutex lock;
    mr_cond available;
} Pool;

typedef struct {
    PGresult* res;
} Result;

// Sorgunun parametreleri: marlin tarafı tek tek ekler, sorgu bırakır.
typedef struct {
    char** values;        // NULL parametre için NULL
    int count, capacity;
} Params;

static _Thread_local char last_error[512];

static void set_error(const char* message) {
    snprintf(last_error, sizeof last_error, "%s", message ? message : "");
    // libpq mesajları satır sonuyla biter.
    size_t n = strlen(last_error);
    while (n > 0 && (last_error[n - 1] == '\n' || last_error[n - 1] == ' ')) last_error[--n] = '\0';
}

int64_t marlin_postgres_error(void) { return marlin_rt_str_from_text(last_error); }

static Conn* conn_of(int64_t handle, const char* operation) {
    if (!handle) marlin_rt_panic("postgres.%s: baglanti acik degil (connect basarisiz olmus; once 'ok' alanina bakin)", operation);
    return (Conn*)(intptr_t)handle;
}

static Pool* pool_of(int64_t handle, const char* operation) {
    if (!handle) marlin_rt_panic("postgres.%s: havuz acik degil (pool basarisiz olmus; once 'ok' alanina bakin)", operation);
    return (Pool*)(intptr_t)handle;
}

static Result* result_of(int64_t handle) {
    if (!handle) marlin_rt_panic("postgres: sonuc gecersiz (clear ile birakilmis olabilir)");
    return (Result*)(intptr_t)handle;
}

// ---------------------------------------------------------------------------
// Bağlantı
// ---------------------------------------------------------------------------

// Sunucunun bilgi mesajları (NOTICE: "tablo yok, atlaniyor" gibi) libpq'da
// varsayılan olarak stderr'e yazılır; programın çıktısını kirletmesin.
static void ignore_notice(void* argument, const char* message) {
    (void)argument;
    (void)message;
}

static Conn* open_conn(const char* conninfo) {
    PGconn* pg = PQconnectdb(conninfo);
    if (!pg || PQstatus(pg) != CONNECTION_OK) {
        set_error(pg ? PQerrorMessage(pg) : "bellek yetersiz");
        PQfinish(pg);
        return NULL;
    }
    PQsetNoticeProcessor(pg, ignore_notice, NULL);
    Conn* conn = malloc(sizeof *conn);
    conn->pg = pg;
    mr_mutex_init(&conn->lock);
    return conn;
}

static void close_conn(Conn* conn) {
    PQfinish(conn->pg);
    mr_mutex_destroy(&conn->lock);
    free(conn);
}

// conninfo: "host=localhost port=5432 dbname=uygulama user=ad password=..."
// ya da "postgresql://ad:sifre@localhost/uygulama". Bağlanılamazsa 0.
int64_t marlin_postgres_connect(int64_t conninfo) {
    last_error[0] = '\0';
    return (int64_t)(intptr_t)open_conn(marlin_rt_str_data(conninfo));
}

void marlin_postgres_close(int64_t handle) {
    if (handle) close_conn((Conn*)(intptr_t)handle);
}

// Bağlantının son hatası (begin / commit gibi komutlar için).
int64_t marlin_postgres_conn_error(int64_t handle) {
    Conn* conn = conn_of(handle, "error");
    mr_mutex_lock(&conn->lock);
    set_error(PQerrorMessage(conn->pg));
    mr_mutex_unlock(&conn->lock);
    return marlin_rt_str_from_text(last_error);
}

// ---------------------------------------------------------------------------
// Havuz
// ---------------------------------------------------------------------------

int64_t marlin_postgres_pool(int64_t conninfo, int32_t size) {
    last_error[0] = '\0';
    if (size < 1) {
        set_error("havuz en az bir baglanti icermeli");
        return 0;
    }
    Pool* pool = calloc(1, sizeof *pool);
    pool->conns = calloc((size_t)size, sizeof *pool->conns);
    pool->free_list = malloc((size_t)size * sizeof *pool->free_list);
    pool->size = size;
    for (int i = 0; i < size; ++i) {
        pool->conns[i] = open_conn(marlin_rt_str_data(conninfo));
        if (!pool->conns[i]) {
            for (int k = 0; k < i; ++k) close_conn(pool->conns[k]);
            free(pool->conns);
            free(pool->free_list);
            free(pool);
            return 0;
        }
        pool->free_list[pool->free_count++] = i;
    }
    mr_mutex_init(&pool->lock);
    mr_cond_init(&pool->available);
    return (int64_t)(intptr_t)pool;
}

void marlin_postgres_pool_close(int64_t handle) {
    if (!handle) return;
    Pool* pool = (Pool*)(intptr_t)handle;
    for (int i = 0; i < pool->size; ++i) close_conn(pool->conns[i]);
    mr_mutex_destroy(&pool->lock);
    mr_cond_destroy(&pool->available);
    free(pool->conns);
    free(pool->free_list);
    free(pool);
}

// Boş bir bağlantı alır (yoksa biri geri verilene kadar bekler).
int64_t marlin_postgres_acquire(int64_t handle) {
    Pool* pool = pool_of(handle, "acquire");
    mr_mutex_lock(&pool->lock);
    while (pool->free_count == 0) mr_cond_wait(&pool->available, &pool->lock);
    Conn* conn = pool->conns[pool->free_list[--pool->free_count]];
    mr_mutex_unlock(&pool->lock);
    return (int64_t)(intptr_t)conn;
}

void marlin_postgres_release(int64_t poolHandle, int64_t connHandle) {
    Pool* pool = pool_of(poolHandle, "release");
    Conn* conn = conn_of(connHandle, "release");
    mr_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->size; ++i) {
        if (pool->conns[i] != conn) continue;
        pool->free_list[pool->free_count++] = i;
        mr_cond_signal(&pool->available);
        mr_mutex_unlock(&pool->lock);
        return;
    }
    mr_mutex_unlock(&pool->lock);
    marlin_rt_panic("postgres.release: baglanti bu havuza ait degil");
}

// ---------------------------------------------------------------------------
// Sorgu
// ---------------------------------------------------------------------------

int64_t marlin_postgres_params_new(void) { return (int64_t)(intptr_t)calloc(1, sizeof(Params)); }

void marlin_postgres_params_add(int64_t handle, int64_t text, int32_t isNull) {
    Params* p = (Params*)(intptr_t)handle;
    if (p->count == p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 4;
        p->values = realloc(p->values, (size_t)p->capacity * sizeof *p->values);
    }
    p->values[p->count++] = isNull ? NULL : strdup(marlin_rt_str_data(text));
}

static void params_free(Params* p) {
    for (int i = 0; i < p->count; ++i) free(p->values[i]);
    free(p->values);
    free(p);
}

// $1, $2 ... parametreleriyle sorgu. Parametreler metin olarak gider; tiplerini
// PostgreSQL sorgudan çıkarır. Değerler SQL metnine hiç eklenmediği için
// SQL injection olmaz.
static int64_t run_query(Conn* conn, const char* sql, Params* params) {
    mr_mutex_lock(&conn->lock);
    PGresult* res = PQexecParams(conn->pg, sql, params->count, NULL,
                                 (const char* const*)params->values, NULL, NULL, /*text*/ 0);
    mr_mutex_unlock(&conn->lock);
    params_free(params);
    Result* result = malloc(sizeof *result);
    result->res = res; // bellek yetersizse NULL; ok yanlış görünür
    return (int64_t)(intptr_t)result;
}

int64_t marlin_postgres_query(int64_t connHandle, int64_t sql, int64_t params) {
    return run_query(conn_of(connHandle, "query"), marlin_rt_str_data(sql), (Params*)(intptr_t)params);
}

// Havuzdan bir bağlantıyla.
int64_t marlin_postgres_pool_query(int64_t poolHandle, int64_t sql, int64_t params) {
    const int64_t conn = marlin_postgres_acquire(poolHandle);
    const int64_t result = run_query((Conn*)(intptr_t)conn, marlin_rt_str_data(sql),
                                     (Params*)(intptr_t)params);
    marlin_postgres_release(poolHandle, conn);
    return result;
}

// Sonuç döndürmeyen komut (BEGIN, COMMIT ...): başarılı mı?
int32_t marlin_postgres_command(int64_t connHandle, int64_t sql) {
    Conn* conn = conn_of(connHandle, "command");
    mr_mutex_lock(&conn->lock);
    PGresult* res = PQexec(conn->pg, marlin_rt_str_data(sql));
    mr_mutex_unlock(&conn->lock);
    const ExecStatusType status = PQresultStatus(res);
    PQclear(res);
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

// ---------------------------------------------------------------------------
// Sonuç
// ---------------------------------------------------------------------------

int32_t marlin_postgres_result_ok(int64_t r) {
    const ExecStatusType status = PQresultStatus(result_of(r)->res);
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

int64_t marlin_postgres_result_error(int64_t r) {
    Result* result = result_of(r);
    if (!result->res) return marlin_rt_str_from_text("bellek yetersiz");
    set_error(PQresultErrorMessage(result->res));
    return marlin_rt_str_from_text(last_error);
}

// SQLSTATE: "23505" tekrarlanan anahtar, "42P01" tablo yok ... Hata yoksa "".
int64_t marlin_postgres_result_code(int64_t r) {
    const char* code = PQresultErrorField(result_of(r)->res, PG_DIAG_SQLSTATE);
    return marlin_rt_str_from_text(code ? code : "");
}

int64_t marlin_postgres_rows(int64_t r) { return PQntuples(result_of(r)->res); }
int64_t marlin_postgres_cols(int64_t r) { return PQnfields(result_of(r)->res); }

// Etkilenen satır sayısı (INSERT / UPDATE / DELETE); yoksa 0.
int64_t marlin_postgres_affected(int64_t r) {
    const char* n = PQcmdTuples(result_of(r)->res);
    return n && *n ? atoll(n) : 0;
}

// Adı verilen sütunun sırası; yoksa -1. (Tırnaksız ad küçük harfe çevrilir:
// PostgreSQL'in kendi kuralı.)
int64_t marlin_postgres_column(int64_t r, int64_t name) {
    return PQfnumber(result_of(r)->res, marlin_rt_str_data(name));
}

static void check_cell(PGresult* res, int64_t row, int64_t col) {
    if (row < 0 || row >= PQntuples(res))
        marlin_rt_panic("postgres: satir sinir disinda (satir %lld, satir sayisi %d)", (long long)row, PQntuples(res));
    if (col < 0 || col >= PQnfields(res))
        marlin_rt_panic("postgres: sutun sinir disinda (sutun %lld, sutun sayisi %d)", (long long)col, PQnfields(res));
}

int64_t marlin_postgres_column_name(int64_t r, int64_t col) {
    PGresult* res = result_of(r)->res;
    if (col < 0 || col >= PQnfields(res))
        marlin_rt_panic("postgres: sutun sinir disinda (sutun %lld, sutun sayisi %d)", (long long)col, PQnfields(res));
    return marlin_rt_str_from_text(PQfname(res, (int)col));
}

int32_t marlin_postgres_is_null(int64_t r, int64_t row, int64_t col) {
    PGresult* res = result_of(r)->res;
    check_cell(res, row, col);
    return PQgetisnull(res, (int)row, (int)col);
}

// Değerin metni (PostgreSQL'in metin biçimi); NULL ise "".
int64_t marlin_postgres_text(int64_t r, int64_t row, int64_t col) {
    PGresult* res = result_of(r)->res;
    check_cell(res, row, col);
    return marlin_rt_str_new(PQgetvalue(res, (int)row, (int)col), PQgetlength(res, (int)row, (int)col));
}

// NULL bir değer sayı / bool olarak okunmak istendi.
void marlin_postgres_null_value(int64_t r, int64_t row, int64_t col) {
    PGresult* res = result_of(r)->res;
    marlin_rt_panic("postgres: satir %lld, '%s' sutunu NULL; sayi olarak okunamaz (once is_null ile bakin)",
                    (long long)row, PQfname(res, (int)col));
}

void marlin_postgres_clear(int64_t r) {
    if (!r) return;
    Result* result = (Result*)(intptr_t)r;
    PQclear(result->res);
    free(result);
}
