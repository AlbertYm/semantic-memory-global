#include "memory/project_resolver.h"
#include "memory/memory_store.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <wchar.h>
#include <wctype.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

typedef struct {
    uint32_t h[5];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} stage14_sha1_t;

static uint32_t sha1_rotl(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32U - bits));
}

static void sha1_transform(stage14_sha1_t *ctx, const unsigned char block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }
        uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotl(b, 30);
        b = a;
        a = temp;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_init(stage14_sha1_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->h[0] = 0x67452301U;
    ctx->h[1] = 0xefcdab89U;
    ctx->h[2] = 0x98badcfeU;
    ctx->h[3] = 0x10325476U;
    ctx->h[4] = 0xc3d2e1f0U;
}

static void sha1_update(stage14_sha1_t *ctx, const unsigned char *data, size_t size) {
    ctx->bits += (uint64_t)size * 8U;
    while (size > 0) {
        size_t take = 64U - ctx->used;
        if (take > size)
            take = size;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        size -= take;
        if (ctx->used == 64U) {
            sha1_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha1_final(stage14_sha1_t *ctx, unsigned char out[20]) {
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56U) {
        while (ctx->used < 64U)
            ctx->block[ctx->used++] = 0;
        sha1_transform(ctx, ctx->block);
        ctx->used = 0;
    }
    while (ctx->used < 56U)
        ctx->block[ctx->used++] = 0;
    for (int i = 7; i >= 0; i--)
        ctx->block[ctx->used++] = (unsigned char)(ctx->bits >> (i * 8));
    sha1_transform(ctx, ctx->block);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)ctx->h[i];
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int uuid_parse(const char *text, unsigned char out[16]) {
    int n = 0;
    for (const char *p = text; p && *p; p++) {
        if (*p == '-')
            continue;
        int hi = hex_nibble(*p++);
        int lo = *p ? hex_nibble(*p) : -1;
        if (hi < 0 || lo < 0 || n >= 16)
            return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
    }
    return n == 16 ? 0 : -1;
}

int cbm_project_uuid_v5(const char *canonical_path, char out[CBM_PROJECT_UUID_SIZE]) {
    unsigned char ns[16], digest[20];
    if (!canonical_path || !canonical_path[0] || !out ||
        uuid_parse(CBM_STAGE14_NAMESPACE_UUID, ns) != 0)
        return -1;
    stage14_sha1_t sha;
    sha1_init(&sha);
    sha1_update(&sha, ns, sizeof(ns));
    sha1_update(&sha, (const unsigned char *)canonical_path, strlen(canonical_path));
    sha1_final(&sha, digest);
    digest[6] = (unsigned char)((digest[6] & 0x0fU) | 0x50U);
    digest[8] = (unsigned char)((digest[8] & 0x3fU) | 0x80U);
    snprintf(out, CBM_PROJECT_UUID_SIZE,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", digest[0],
             digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7], digest[8],
             digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]);
    return 0;
}

static int decode_file_uri(const char *input, char *out, size_t out_size) {
    const char *p = input;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        if (*p == '/' && isalpha((unsigned char)p[1]) && p[2] == ':')
            p++;
    }
    size_t w = 0;
    while (*p && w + 1 < out_size) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            int hi = hex_nibble(p[1]), lo = hex_nibble(p[2]);
            out[w++] = (char)((hi << 4) | lo);
            p += 3;
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
    return *p == '\0' && w > 0 ? 0 : -1;
}

int cbm_project_canonicalize_path(const char *path_or_file_uri, char *out, size_t out_size) {
    char decoded[4096];
    if (!path_or_file_uri || !out || out_size == 0 ||
        decode_file_uri(path_or_file_uri, decoded, sizeof(decoded)) != 0)
        return -1;
#ifdef _WIN32
    wchar_t input_w[4096], full_w[4096];
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, decoded, -1, input_w, 4096);
    if (wn <= 0 || !_wfullpath(full_w, input_w, 4096))
        return -1;
    HANDLE handle = CreateFileW(full_w, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle != INVALID_HANDLE_VALUE) {
        wchar_t final_w[4096];
        DWORD n = GetFinalPathNameByHandleW(handle, final_w, 4096, FILE_NAME_NORMALIZED);
        CloseHandle(handle);
        if (n > 0 && n < 4096) {
            const wchar_t *resolved = final_w;
            if (wcsncmp(resolved, L"\\\\?\\UNC\\", 8) == 0) {
                swprintf(full_w, 4096, L"\\\\%ls", resolved + 8);
            } else if (wcsncmp(resolved, L"\\\\?\\", 4) == 0)
                swprintf(full_w, 4096, L"%ls", resolved + 4);
            else
                swprintf(full_w, 4096, L"%ls", resolved);
        }
    }
    for (wchar_t *p = full_w; *p; p++) {
        if (*p == L'/')
            *p = L'\\';
        *p = (wchar_t)towlower(*p);
    }
    size_t len = wcslen(full_w);
    while (len > 3 && (full_w[len - 1] == L'\\' || full_w[len - 1] == L'/'))
        full_w[--len] = 0;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, full_w, -1, out, (int)out_size, NULL, NULL);
    return bytes > 0 ? 0 : -1;
#else
    char full[PATH_MAX];
    if (!realpath(decoded, full)) {
        if (decoded[0] == '/')
            snprintf(full, sizeof(full), "%s", decoded);
        else {
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof(cwd)))
                return -1;
            snprintf(full, sizeof(full), "%s/%s", cwd, decoded);
        }
    }
    size_t len = strlen(full);
    while (len > 1 && full[len - 1] == '/')
        full[--len] = 0;
    if (len + 1 > out_size)
        return -1;
    for (size_t i = 0; i <= len; i++)
        out[i] = (char)tolower((unsigned char)full[i]);
    return 0;
#endif
}

static const char *path_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    return base[0] ? base : path;
}

static int project_filesystem_identity(const char *canonical_path, char volume_id[128],
                                       char fingerprint[CBM_PROJECT_PATH_HASH_SIZE]) {
    char seed[256];
#ifdef _WIN32
    wchar_t path_w[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, canonical_path, -1, path_w, 4096) <= 0)
        return -1;
    HANDLE handle = CreateFileW(path_w, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(handle, &info) != 0;
    CloseHandle(handle);
    if (!ok)
        return -1;
    snprintf(volume_id, 128, "win-volume-%08lx", (unsigned long)info.dwVolumeSerialNumber);
    snprintf(seed, sizeof(seed), "stage14-filesystem-identity/v1\n%08lx\n%08lx%08lx",
             (unsigned long)info.dwVolumeSerialNumber, (unsigned long)info.nFileIndexHigh,
             (unsigned long)info.nFileIndexLow);
#else
    struct stat st;
    if (stat(canonical_path, &st) != 0)
        return -1;
    snprintf(volume_id, 128, "posix-device-%llu", (unsigned long long)st.st_dev);
    snprintf(seed, sizeof(seed), "stage14-filesystem-identity/v1\n%llu\n%llu",
             (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
#endif
    return cbm_stage7_sha256_hex(seed, strlen(seed), fingerprint) == 0 ? 0 : -1;
}

int cbm_project_resolve(const char *path_or_file_uri, const char *display_name,
                        const char *source_fingerprint, cbm_project_resolution_t *out) {
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (cbm_project_canonicalize_path(path_or_file_uri, out->canonical_path,
                                      sizeof(out->canonical_path)) != 0 ||
        cbm_stage7_sha256_hex(out->canonical_path, strlen(out->canonical_path), out->path_hash) !=
            0 ||
        cbm_project_uuid_v5(out->canonical_path, out->project_uuid) != 0)
        return -1;
    snprintf(out->display_name, sizeof(out->display_name), "%s",
             display_name && display_name[0] ? display_name : path_basename(out->canonical_path));
    if (source_fingerprint && source_fingerprint[0]) {
        if (strlen(source_fingerprint) != 64)
            return -1;
        for (size_t i = 0; i < 64; i++)
            if (hex_nibble(source_fingerprint[i]) < 0)
                return -1;
        snprintf(out->source_fingerprint, sizeof(out->source_fingerprint), "%s",
                 source_fingerprint);
    }
#ifdef _WIN32
    wchar_t canonical_w[4096];
    int wide_ok = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out->canonical_path, -1,
                                      canonical_w, 4096);
    out->path_exists = wide_ok > 0 && GetFileAttributesW(canonical_w) != INVALID_FILE_ATTRIBUTES;
    out->path_writable = out->path_exists && wide_ok > 0 && _waccess(canonical_w, 2) == 0;
    if (isalpha((unsigned char)out->canonical_path[0]) && out->canonical_path[1] == ':')
        snprintf(out->volume_id, sizeof(out->volume_id), "%c:", out->canonical_path[0]);
#else
    struct stat st;
    out->path_exists = stat(out->canonical_path, &st) == 0;
    out->path_writable = out->path_exists && access(out->canonical_path, W_OK) == 0;
    snprintf(out->volume_id, sizeof(out->volume_id), "/");
#endif
    if (out->path_exists && !out->source_fingerprint[0])
        (void)project_filesystem_identity(out->canonical_path, out->volume_id,
                                          out->source_fingerprint);
    return 0;
}
