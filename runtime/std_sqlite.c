// std.sqlite — SQLite veritabanı (bkz. std/sqlite.marl).
//
// SQLite'ın C kütüphanesini sarar. API std.postgres ile aynı biçimdedir:
//
//   Conn    açık bir veritabanı dosyası (sqlite3*) + kilidi
//   Result  sorgunun sonucu: bütün satırlar sorgu sırasında okunup bellekte
//           tutulur (metin olarak), böylece sonuç değişmezdir ve postgres'teki
//           gibi satır / sütun numarasıyla okunur.
//
// Parametreler $1, $2 ... (postgres ile aynı) ya da ? ile yazılır. $n biçimi
// SQLite'ta adlı parametredir; adına göre bağlanır, sorgudaki sırası önemli
// değildir.
//
// Bir bağlantı aynı anda tek iş parçacığından kullanılır (kilit). Hatalar:
// açılamazsa 0 döner (sebebi c_error); sorgu hataları sonucun ok / error /
// code alanlarındadır.

#include "marlin_rt.h"

#include "platform.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sqlite3* db;
    mr_mutex lock;
} Conn;

typedef struct {
    int ok;
    char* error;
    int code;                // SQLite'ın genişletilmiş hata kodu; başarılıysa 0
    int64_t changes;         // etkilenen satır sayısı
    int rows, cols;
    char** names;            // sütun adları
    char** cells;            // rows * cols değer; NULL değer için NULL
    int* lengths;
} Result;

typedef struct {
    char** values;
    int count, capacity;
} Params;

static _Thread_local char last_error[512];

int64_t marlin_sqlite_error(void) { return marlin_rt_str_from_text(last_error); }

static Conn* conn_of(int64_t handle, const char* operation) {
    if (!handle) marlin_rt_panic("sqlite.%s: veritabani acik degil (open basarisiz olmus; once 'ok' alanina bakin)", operation);
    return (Conn*)(intptr_t)handle;
}

static Result* result_of(int64_t handle) {
    if (!handle) marlin_rt_panic("sqlite: sonuc gecersiz (clear ile birakilmis olabilir)");
    return (Result*)(intptr_t)handle;
}

// path: dosya yolu (yoksa oluşturulur) ya da ":memory:" (bellekte, geçici).
int64_t marlin_sqlite_open(int64_t path) {
    last_error[0] = '\0';
    sqlite3* db = NULL;
    if (sqlite3_open(marlin_rt_str_data(path), &db) != SQLITE_OK) {
        snprintf(last_error, sizeof last_error, "%s acilamadi: %s", marlin_rt_str_data(path),
                 db ? sqlite3_errmsg(db) : "bellek yetersiz");
        sqlite3_close(db);
        return 0;
    }
    Conn* conn = malloc(sizeof *conn);
    conn->db = db;
    mr_mutex_init(&conn->lock);
    return (int64_t)(intptr_t)conn;
}

void marlin_sqlite_close(int64_t handle) {
    if (!handle) return;
    Conn* conn = (Conn*)(intptr_t)handle;
    sqlite3_close(conn->db);
    mr_mutex_destroy(&conn->lock);
    free(conn);
}

int64_t marlin_sqlite_params_new(void) { return (int64_t)(intptr_t)calloc(1, sizeof(Params)); }

void marlin_sqlite_params_add(int64_t handle, int64_t text, int32_t isNull) {
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

static void fail_result(Result* r, sqlite3* db) {
    r->ok = 0;
    r->code = sqlite3_extended_errcode(db);
    r->error = strdup(sqlite3_errmsg(db));
}

// i. parametrenin (1'den başlar) bağlanacağı yer: $i yazıldıysa o, yoksa
// sıradaki ? (ya da ?i).
static int parameter_index(sqlite3_stmt* stmt, int i) {
    char name[24];
    snprintf(name, sizeof name, "$%d", i);
    const int named = sqlite3_bind_parameter_index(stmt, name);
    return named > 0 ? named : i;
}

int64_t marlin_sqlite_query(int64_t handle, int64_t sql, int64_t paramsHandle) {
    Conn* conn = conn_of(handle, "query");
    Params* params = (Params*)(intptr_t)paramsHandle;
    Result* r = calloc(1, sizeof *r);
    r->ok = 1;

    mr_mutex_lock(&conn->lock);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(conn->db, marlin_rt_str_data(sql), -1, &stmt, NULL) != SQLITE_OK) {
        fail_result(r, conn->db);
        goto done;
    }
    if (params->count != sqlite3_bind_parameter_count(stmt)) {
        r->ok = 0;
        r->code = SQLITE_RANGE;
        char message[128];
        snprintf(message, sizeof message, "sorguda %d parametre var, %d deger verildi",
                 sqlite3_bind_parameter_count(stmt), params->count);
        r->error = strdup(message);
        goto done;
    }
    for (int i = 0; i < params->count; ++i) {
        const int at = parameter_index(stmt, i + 1);
        const int rc = params->values[i]
            ? sqlite3_bind_text(stmt, at, params->values[i], -1, SQLITE_TRANSIENT)
            : sqlite3_bind_null(stmt, at);
        if (rc != SQLITE_OK) {
            fail_result(r, conn->db);
            goto done;
        }
    }

    // Bütün satırlar okunur.
    r->cols = sqlite3_column_count(stmt);
    r->names = calloc((size_t)(r->cols ? r->cols : 1), sizeof *r->names);
    for (int c = 0; c < r->cols; ++c) r->names[c] = strdup(sqlite3_column_name(stmt, c));
    int capacity = 0;
    for (;;) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            fail_result(r, conn->db);
            goto done;
        }
        if (r->rows == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            r->cells = realloc(r->cells, (size_t)capacity * r->cols * sizeof *r->cells);
            r->lengths = realloc(r->lengths, (size_t)capacity * r->cols * sizeof *r->lengths);
        }
        for (int c = 0; c < r->cols; ++c) {
            const int at = r->rows * r->cols + c;
            if (sqlite3_column_type(stmt, c) == SQLITE_NULL) {
                r->cells[at] = NULL;
                r->lengths[at] = 0;
                continue;
            }
            const unsigned char* text = sqlite3_column_text(stmt, c);
            const int length = sqlite3_column_bytes(stmt, c);
            r->cells[at] = malloc((size_t)length + 1);
            memcpy(r->cells[at], text, (size_t)length);
            r->cells[at][length] = '\0';
            r->lengths[at] = length;
        }
        ++r->rows;
    }
    r->changes = sqlite3_changes(conn->db);

done:
    sqlite3_finalize(stmt);
    mr_mutex_unlock(&conn->lock);
    params_free(params);
    return (int64_t)(intptr_t)r;
}

int32_t marlin_sqlite_result_ok(int64_t h) { return result_of(h)->ok; }

int64_t marlin_sqlite_result_error(int64_t h) {
    Result* r = result_of(h);
    return marlin_rt_str_from_text(r->error ? r->error : "");
}

// Hata kodu metin olarak (ör. "2067" = SQLITE_CONSTRAINT_UNIQUE); hata yoksa "".
int64_t marlin_sqlite_result_code(int64_t h) {
    Result* r = result_of(h);
    if (r->ok) return marlin_rt_str_from_text("");
    char code[16];
    snprintf(code, sizeof code, "%d", r->code);
    return marlin_rt_str_from_text(code);
}

int64_t marlin_sqlite_rows(int64_t h) { return result_of(h)->rows; }
int64_t marlin_sqlite_cols(int64_t h) { return result_of(h)->cols; }
int64_t marlin_sqlite_affected(int64_t h) { return result_of(h)->changes; }

int64_t marlin_sqlite_column(int64_t h, int64_t name) {
    Result* r = result_of(h);
    const char* wanted = marlin_rt_str_data(name);
    for (int c = 0; c < r->cols; ++c)
        if (sqlite3_stricmp(r->names[c], wanted) == 0) return c;
    return -1;
}

static int cell_index(Result* r, int64_t row, int64_t col) {
    if (row < 0 || row >= r->rows)
        marlin_rt_panic("sqlite: satir sinir disinda (satir %lld, satir sayisi %d)", (long long)row, r->rows);
    if (col < 0 || col >= r->cols)
        marlin_rt_panic("sqlite: sutun sinir disinda (sutun %lld, sutun sayisi %d)", (long long)col, r->cols);
    return (int)row * r->cols + (int)col;
}

int64_t marlin_sqlite_column_name(int64_t h, int64_t col) {
    Result* r = result_of(h);
    if (col < 0 || col >= r->cols)
        marlin_rt_panic("sqlite: sutun sinir disinda (sutun %lld, sutun sayisi %d)", (long long)col, r->cols);
    return marlin_rt_str_from_text(r->names[col]);
}

int32_t marlin_sqlite_is_null(int64_t h, int64_t row, int64_t col) {
    Result* r = result_of(h);
    return r->cells[cell_index(r, row, col)] == NULL;
}

int64_t marlin_sqlite_text(int64_t h, int64_t row, int64_t col) {
    Result* r = result_of(h);
    const int at = cell_index(r, row, col);
    return r->cells[at] ? marlin_rt_str_new(r->cells[at], r->lengths[at]) : marlin_rt_str_from_text("");
}

void marlin_sqlite_null_value(int64_t h, int64_t row, int64_t col) {
    Result* r = result_of(h);
    marlin_rt_panic("sqlite: satir %lld, '%s' sutunu NULL; sayi olarak okunamaz (once is_null ile bakin)",
                    (long long)row, r->names[col]);
}

void marlin_sqlite_clear(int64_t h) {
    if (!h) return;
    Result* r = (Result*)(intptr_t)h;
    for (int i = 0; i < r->rows * r->cols; ++i) free(r->cells[i]);
    for (int c = 0; c < r->cols; ++c) free(r->names[c]);
    free(r->cells);
    free(r->lengths);
    free(r->names);
    free(r->error);
    free(r);
}
