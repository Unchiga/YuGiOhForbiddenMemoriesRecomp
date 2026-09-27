#include "psx_textfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <dirent.h>
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

int psx_remove_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])) > 0)
        return _wremove(wpath);
#endif
    return remove(path);
}

int psx_path_exists_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])) > 0)
        return GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
    return 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int psx_stat_utf8(const char *path, long *out_mtime, long *out_size)
{
#ifdef _WIN32
    wchar_t wpath[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])) <= 0)
        return 0;
    struct _stat64 st;
    if (_wstat64(wpath, &st) != 0) return 0;
    if (out_mtime) *out_mtime = (long)st.st_mtime;
    if (out_size)  *out_size  = (long)st.st_size;
    return 1;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (out_mtime) *out_mtime = (long)st.st_mtime;
    if (out_size)  *out_size  = (long)st.st_size;
    return 1;
#endif
}

int psx_mkdir_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t wpath[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])) > 0)
        return _wmkdir(wpath);
    return -1;
#else
    return mkdir(path, 0755);
#endif
}

int psx_dir_list_utf8(const char *dir, void (*cb)(const char *name, int is_dir, void *ctx), void *ctx)
{
#ifdef _WIN32
    wchar_t wglob[2048];
    char glob8[2048];
    snprintf(glob8, sizeof glob8, "%s/*", dir);
    if (MultiByteToWideChar(CP_UTF8, 0, glob8, -1, wglob, (int)(sizeof wglob / sizeof wglob[0])) <= 0)
        return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wglob, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        char name[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof name, NULL, NULL) <= 0)
            continue;
        cb(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
#else
    DIR *dp = opendir(dir);
    if (!dp)
        return 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, ".."))
            continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", dir, nm);
        struct stat st;
        const int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        cb(nm, is_dir, ctx);
    }
    closedir(dp);
    return 1;
#endif
}

int psx_rename_utf8(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t wfrom[2048], wto[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, from, -1, wfrom, (int)(sizeof wfrom / sizeof wfrom[0])) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, to, -1, wto, (int)(sizeof wto / sizeof wto[0])) > 0)
        return _wrename(wfrom, wto);
#endif
    return rename(from, to);
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
