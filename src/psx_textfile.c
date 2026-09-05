#include "psx_textfile.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

FILE *psx_fopen_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wpath[2048], wmode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, (int)(sizeof wmode / sizeof wmode[0])) > 0) {
        FILE *f = _wfopen(wpath, wmode);
        if (f) return f;
    }
#endif
    return fopen(path, mode);
}

/* one UTF-16 code unit stream -> UTF-8 */
static char *utf16_to_utf8(const uint8_t *b, size_t n, int big_endian, size_t *out_len)
{
    char *out = (char *)malloc(n * 2 + 4);
    size_t o = 0;
    for (size_t i = 0; i + 1 < n;) {
        uint32_t c = big_endian ? ((uint32_t)b[i] << 8 | b[i + 1]) : ((uint32_t)b[i + 1] << 8 | b[i]);
        i += 2;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            const uint32_t lo = big_endian ? ((uint32_t)b[i] << 8 | b[i + 1]) : ((uint32_t)b[i + 1] << 8 | b[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

char *psx_read_text_utf8(const char *path, size_t *len, size_t max_bytes)
{
    FILE *f = psx_fopen_utf8(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || (max_bytes && (size_t)sz > max_bytes)) { fclose(f); return NULL; }
    uint8_t *raw = (uint8_t *)malloc((size_t)sz + 2);
    const size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[got] = 0; raw[got + 1] = 0;
    char *out; size_t n;
    if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) out = utf16_to_utf8(raw + 2, got - 2, 0, &n);
    else if (got >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) out = utf16_to_utf8(raw + 2, got - 2, 1, &n);
    else {
        size_t skip = (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        n = got - skip;
        out = (char *)malloc(n + 1);
        memcpy(out, raw + skip, n); out[n] = 0;
    }
    free(raw);
    if (len) *len = n;
    return out;
}
