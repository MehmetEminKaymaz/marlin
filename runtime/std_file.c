// std.file — dosya işlemlerinin C tarafı (bkz. std/file.minik).
//
// Açık dosya, minik tarafına FILE* adresi olarak (int64) verilir; 0 "açılamadı"
// demektir. Dosyalar metin olarak okunur ve yazılır.

#include "minik_rt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE* stream_of(int64_t handle, const char* operation) {
    if (!handle) minik_rt_panic("%s: dosya acik degil (open basarisiz olmus; once 'ok' alanina bakin)", operation);
    return (FILE*)(intptr_t)handle;
}

int32_t minik_file_exists(int64_t path) { return access(minik_rt_str_data(path), F_OK) == 0; }

// Dosyanın tamamı. Açılamazsa program durur (önceden denetlemek için: exists).
int64_t minik_file_read_text(int64_t path) {
    FILE* file = fopen(minik_rt_str_data(path), "rb");
    if (!file) minik_rt_panic("dosya okunamadi: %s (%s)", minik_rt_str_data(path), strerror(errno));

    size_t size = 0, capacity = 4096;
    char* buffer = malloc(capacity);
    for (;;) {
        if (!buffer) minik_rt_panic("dosya icin bellek ayrilamadi: %s", minik_rt_str_data(path));
        size += fread(buffer + size, 1, capacity - size, file);
        if (size < capacity) break;
        capacity *= 2;
        buffer = realloc(buffer, capacity);
    }
    fclose(file);
    const int64_t text = minik_rt_str_new(buffer, (int64_t)size);
    free(buffer);
    return text;
}

// Metni dosyaya yazar; append ise sonuna ekler, değilse eski içeriğin yerine.
void minik_file_write_text(int64_t path, int64_t text, int32_t append) {
    FILE* file = fopen(minik_rt_str_data(path), append ? "ab" : "wb");
    if (!file) minik_rt_panic("dosya yazilamadi: %s (%s)", minik_rt_str_data(path), strerror(errno));
    fwrite(minik_rt_str_data(text), 1, (size_t)minik_str_len(text), file);
    fclose(file);
}

int32_t minik_file_remove(int64_t path) { return remove(minik_rt_str_data(path)) == 0; }

// mode: "r" (oku), "w" (yaz; eski içerik silinir), "a" (sonuna ekle).
// Açılamazsa 0 döner.
int64_t minik_file_open(int64_t path, int64_t mode) {
    const char* m = minik_rt_str_data(mode);
    if (strcmp(m, "r") != 0 && strcmp(m, "w") != 0 && strcmp(m, "a") != 0)
        minik_rt_panic("open: gecersiz mod \"%s\" (gecerli olanlar: \"r\", \"w\", \"a\")", m);
    return (int64_t)(intptr_t)fopen(minik_rt_str_data(path), m);
}

int64_t minik_file_read_line(int64_t handle) { return minik_rt_read_line(stream_of(handle, "read_line")); }

int32_t minik_file_eof(int64_t handle) { return minik_rt_at_eof(stream_of(handle, "eof")); }

void minik_file_write(int64_t handle, int64_t text) {
    fwrite(minik_rt_str_data(text), 1, (size_t)minik_str_len(text), stream_of(handle, "write"));
}

void minik_file_close(int64_t handle) {
    if (handle) fclose((FILE*)(intptr_t)handle);
}
