/*
 * main.c — Entry point for semantic-memory-mcp.
 *
 * Modes:
 *   (default)       Run as MCP server on stdin/stdout (JSON-RPC 2.0)
 *   cli <tool> <json>  Run a single tool call and print result
 *   --version       Print version and exit
 *   --help          Print usage and exit
 *   --ui=true/false Enable/disable HTTP UI server (persisted)
 *   --port=N        Set HTTP UI port (persisted, default 9749)
 *
 * Signal handling: SIGTERM/SIGINT trigger graceful shutdown.
 * Watcher runs in a background thread, polling for git changes.
 * HTTP UI server (optional) runs in a background thread on localhost.
 */
#include "cbm.h" // cbm_alloc_init — bind 3rd-party allocators to mimalloc before any sqlite/git init
#include "mcp/mcp.h"
#include "watcher/watcher.h"
#include "pipeline/pipeline.h"
#include "store/store.h"
#include "cli/cli.h"
#include "cli/progress_sink.h"
#include "foundation/constants.h"

enum {
    MAIN_MIN_ARGC = 1,
    MAIN_CLI_ARGC = 2,
    MAIN_FLAG_OFF = 5, /* strlen("--ui=") */
    MAIN_PORT_OFF = 7, /* strlen("--port=") */
    MAIN_MAX_PORT = 65536,
    MAIN_CLI_FLAG_LEN = 2,                       /* strlen("--") — CLI flag prefix */
    PARENT_WATCHDOG_STACK_SIZE = 64 * CBM_SZ_1K, /* watchdog only polls — tiny stack suffices */
};
#define MAIN_RAM_FRACTION 0.5

#define SLEN(s) (sizeof(s) - 1)
#include "foundation/log.h"
#include "foundation/diagnostics.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/mem.h"
#include "foundation/profile.h"
#include "ui/config.h"
#include "ui/http_server.h"
#include "ui/embedded_assets.h"
#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <io.h> /* _isatty, _fileno */
#endif

/* True when stdin is an interactive terminal (vs a pipe / redirected file).
 * Used by the CLI arg-input dispatcher to decide whether to read JSON from
 * stdin when no explicit args are given (smoke-test Phase 3h, B4). */
#ifdef _WIN32
#define CBM_CLI_STDIN_IS_TTY() _isatty(cbm_fileno(stdin))
#else
#define CBM_CLI_STDIN_IS_TTY() isatty(cbm_fileno(stdin))
#endif

#ifndef CBM_VERSION
#define CBM_VERSION "v0.8.1-adr"
#endif

#ifdef _WIN32

enum {
    STAGE14_PATH_CAP = 32768,
    STAGE14_VERSION_CAP = 128,
    STAGE14_SHA256_HEX_CAP = 65,
    STAGE14_JSON_MAX_BYTES = 4 * 1024 * 1024,
    STAGE14_HASH_BUFFER_BYTES = 1024 * 1024,
    STAGE14_MAX_PAYLOAD_FILES = 64,
};

typedef enum {
    STAGE14_ROLE_NONE = 0,
    STAGE14_ROLE_MCP,
    STAGE14_ROLE_HOOK,
    STAGE14_ROLE_MANAGER,
} stage14_role_t;

typedef struct {
    BCRYPT_ALG_HANDLE algorithm;
    ULONG object_bytes;
    ULONG hash_bytes;
} stage14_sha256_t;

typedef struct {
    ULONGLONG volume_serial;
    unsigned char file_id[16];
    DWORD link_count;
    bool cacheable;
} stage14_file_identity_t;

typedef struct {
    stage14_file_identity_t identity;
    uint64_t bytes;
    char sha256[STAGE14_SHA256_HEX_CAP];
    HANDLE handle;
} stage14_verified_file_t;

typedef struct {
    stage14_verified_file_t *files;
    size_t file_count;
    HANDLE launcher;
    HANDLE *directories;
    size_t directory_count;
} stage14_launch_guards_t;

typedef struct {
    wchar_t install_root[STAGE14_PATH_CAP];
    wchar_t bin_root[STAGE14_PATH_CAP];
    wchar_t payload_root[STAGE14_PATH_CAP];
} stage14_final_paths_t;

typedef struct {
    int argc;
    LPWSTR *wide_argv;
    char **utf8_argv;
} stage14_windows_argv_t;

static int stage14_integrity_failure(void) {
    (void)fprintf(stderr, "RED_PAYLOAD_INTEGRITY_FAILURE\n");
    return 1;
}

static bool stage14_copy_wide(wchar_t *out, size_t out_cap, const wchar_t *value) {
    size_t length = value ? wcslen(value) : 0;
    if (!out || out_cap == 0 || !value || length >= out_cap) {
        return false;
    }
    memcpy(out, value, (length + 1) * sizeof(*out));
    return true;
}

static bool stage14_join_wide(wchar_t *out, size_t out_cap, const wchar_t *root,
                              const wchar_t *relative) {
    size_t root_len = root ? wcslen(root) : 0;
    size_t rel_len = relative ? wcslen(relative) : 0;
    bool needs_slash = root_len > 0 && root[root_len - 1] != L'\\' && root[root_len - 1] != L'/';
    if (!out || !root || !relative || root_len + (needs_slash ? 1 : 0) + rel_len >= out_cap) {
        return false;
    }
    memcpy(out, root, root_len * sizeof(*out));
    size_t offset = root_len;
    if (needs_slash) {
        out[offset++] = L'\\';
    }
    memcpy(out + offset, relative, (rel_len + 1) * sizeof(*out));
    return true;
}

static wchar_t *stage14_wide_basename(wchar_t *path) {
    wchar_t *slash = wcsrchr(path, L'\\');
    wchar_t *forward = wcsrchr(path, L'/');
    if (!slash || (forward && forward > slash)) {
        slash = forward;
    }
    return slash ? slash + 1 : path;
}

static bool stage14_wide_parent_in_place(wchar_t *path) {
    wchar_t *base = stage14_wide_basename(path);
    if (base == path) {
        return false;
    }
    base[-1] = L'\0';
    return true;
}

static bool stage14_utf8_to_wide(const char *value, wchar_t *out, size_t out_cap) {
    if (!value || !out || out_cap == 0) {
        return false;
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (needed <= 0 || (size_t)needed > out_cap) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, out, needed) == needed;
}

static bool stage14_free_wide_argv(LPWSTR **wide_argv) {
    if (!wide_argv || !*wide_argv) {
        return false;
    }
    LPWSTR *allocated = *wide_argv;
    *wide_argv = NULL;
    return LocalFree(allocated) == NULL;
}

static bool stage14_free_utf8_argv(char ***utf8_argv) {
    if (!utf8_argv || !*utf8_argv) {
        return false;
    }
    char **allocated = *utf8_argv;
    *utf8_argv = NULL;
    return HeapFree(GetProcessHeap(), 0, allocated) != 0;
}

static bool stage14_normalize_windows_argv(stage14_windows_argv_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    LPWSTR command_line = GetCommandLineW();
    if (!command_line || !*command_line) {
        return false;
    }

    /* The CRT's narrow argv follows the active code page and cannot preserve
     * every Windows path. Parse the authoritative UTF-16 command line once,
     * then derive the UTF-8 argv used by the payload-facing C code. */
    int wide_argc = 0;
    LPWSTR *wide_argv = CommandLineToArgvW(command_line, &wide_argc);
    char **utf8_argv = NULL;
    if (!wide_argv || wide_argc < 1 || wide_argc > STAGE14_PATH_CAP) {
        if (wide_argv) {
            (void)stage14_free_wide_argv(&wide_argv);
        }
        return false;
    }

    size_t pointer_count = (size_t)wide_argc + 1u;
    if (pointer_count > SIZE_MAX / sizeof(*utf8_argv)) {
        (void)stage14_free_wide_argv(&wide_argv);
        return false;
    }
    size_t pointer_bytes = pointer_count * sizeof(*utf8_argv);
    size_t total_bytes = pointer_bytes;
    for (int i = 0; i < wide_argc; i++) {
        if (!wide_argv[i]) {
            (void)stage14_free_wide_argv(&wide_argv);
            return false;
        }
        int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0,
                                         NULL, NULL);
        if (needed <= 0 || (size_t)needed > SIZE_MAX - total_bytes) {
            (void)stage14_free_wide_argv(&wide_argv);
            return false;
        }
        total_bytes += (size_t)needed;
    }

    utf8_argv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_bytes);
    if (!utf8_argv) {
        (void)stage14_free_wide_argv(&wide_argv);
        return false;
    }
    char *cursor = (char *)utf8_argv + pointer_bytes;
    for (int i = 0; i < wide_argc; i++) {
        int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0,
                                         NULL, NULL);
        if (needed <= 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                               cursor, needed, NULL, NULL) != needed) {
            (void)stage14_free_utf8_argv(&utf8_argv);
            (void)stage14_free_wide_argv(&wide_argv);
            return false;
        }
        utf8_argv[i] = cursor;
        cursor += needed;
    }
    utf8_argv[wide_argc] = NULL;
    out->argc = wide_argc;
    out->wide_argv = wide_argv;
    out->utf8_argv = utf8_argv;
    return true;
}

static bool stage14_safe_relative_utf8(const char *value) {
    if (!value || !*value || value[0] == '/' || value[0] == '\\' || (value[0] && value[1] == ':')) {
        return false;
    }
    const char *segment = value;
    for (const char *cursor = value;; cursor++) {
        if (*cursor != '/' && *cursor != '\\' && *cursor != '\0') {
            if (*cursor == ':') {
                return false;
            }
            continue;
        }
        size_t length = (size_t)(cursor - segment);
        if (length == 0 || (length == 1 && segment[0] == '.') ||
            (length == 2 && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        if (*cursor == '\0') {
            return true;
        }
        segment = cursor + 1;
    }
}

static bool stage14_resolve_child(const wchar_t *root, const char *relative, wchar_t *out,
                                  size_t out_cap) {
    wchar_t wide_relative[STAGE14_PATH_CAP];
    if (!stage14_safe_relative_utf8(relative) ||
        !stage14_utf8_to_wide(relative, wide_relative, STAGE14_PATH_CAP)) {
        return false;
    }
    for (wchar_t *cursor = wide_relative; *cursor; cursor++) {
        if (*cursor == L'/') {
            *cursor = L'\\';
        }
    }
    return stage14_join_wide(out, out_cap, root, wide_relative);
}

static bool stage14_valid_sha256(const char *value) {
    if (!value || strlen(value) != STAGE14_SHA256_HEX_CAP - 1) {
        return false;
    }
    for (size_t i = 0; i < STAGE14_SHA256_HEX_CAP - 1; i++) {
        char ch = value[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool stage14_valid_version_id(const char *value) {
    if (!value || !*value || strlen(value) >= STAGE14_VERSION_CAP) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        unsigned char ch = *cursor;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '+' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static bool stage14_sha256_init(stage14_sha256_t *ctx) {
    ULONG written = 0;
    memset(ctx, 0, sizeof(*ctx));
    if (BCryptOpenAlgorithmProvider(&ctx->algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptGetProperty(ctx->algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&ctx->object_bytes,
                          sizeof(ctx->object_bytes), &written, 0) != 0 ||
        BCryptGetProperty(ctx->algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&ctx->hash_bytes,
                          sizeof(ctx->hash_bytes), &written, 0) != 0 ||
        ctx->hash_bytes != 32 || ctx->object_bytes == 0) {
        if (ctx->algorithm) {
            BCryptCloseAlgorithmProvider(ctx->algorithm, 0);
        }
        memset(ctx, 0, sizeof(*ctx));
        return false;
    }
    return true;
}

static void stage14_sha256_close(stage14_sha256_t *ctx) {
    if (ctx && ctx->algorithm) {
        BCryptCloseAlgorithmProvider(ctx->algorithm, 0);
        memset(ctx, 0, sizeof(*ctx));
    }
}

static bool stage14_file_identity_equal(const stage14_file_identity_t *left,
                                        const stage14_file_identity_t *right) {
    return left->cacheable && right->cacheable && left->volume_serial == right->volume_serial &&
           memcmp(left->file_id, right->file_id, sizeof(left->file_id)) == 0;
}

static bool stage14_file_id_nonzero(const unsigned char file_id[16]) {
    unsigned char combined = 0;
    for (size_t i = 0; i < 16; i++) {
        combined |= file_id[i];
    }
    return combined != 0;
}

static bool stage14_open_regular_file(const wchar_t *path, HANDLE *out_handle,
                                      stage14_file_identity_t *identity, uint64_t *size_out) {
    *out_handle = INVALID_HANDLE_VALUE;
    memset(identity, 0, sizeof(*identity));
    HANDLE file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag;
    BY_HANDLE_FILE_INFORMATION legacy;
    FILE_ID_INFO file_id;
    LARGE_INTEGER size;
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !GetFileInformationByHandle(file, &legacy) || !GetFileSizeEx(file, &size) ||
        size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    identity->link_count = legacy.nNumberOfLinks;
    if (GetFileInformationByHandleEx(file, FileIdInfo, &file_id, sizeof(file_id))) {
        identity->volume_serial = file_id.VolumeSerialNumber;
        memcpy(identity->file_id, file_id.FileId.Identifier, sizeof(identity->file_id));
        identity->cacheable =
            identity->link_count > 1 && stage14_file_id_nonzero(identity->file_id);
    }
    *size_out = (uint64_t)size.QuadPart;
    *out_handle = file;
    return true;
}

static HANDLE stage14_open_directory(const wchar_t *path, wchar_t final_path[STAGE14_PATH_CAP]) {
    HANDLE directory = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (directory == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (directory != INVALID_HANDLE_VALUE) {
            CloseHandle(directory);
        }
        return INVALID_HANDLE_VALUE;
    }
    DWORD length = GetFinalPathNameByHandleW(directory, final_path, STAGE14_PATH_CAP,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= STAGE14_PATH_CAP) {
        CloseHandle(directory);
        return INVALID_HANDLE_VALUE;
    }
    while (length > 0 && (final_path[length - 1] == L'\\' || final_path[length - 1] == L'/')) {
        final_path[--length] = L'\0';
    }
    return directory;
}

static bool stage14_handle_within_root(HANDLE file, const wchar_t *root_final_path) {
    wchar_t file_final_path[STAGE14_PATH_CAP];
    DWORD length = GetFinalPathNameByHandleW(file, file_final_path, STAGE14_PATH_CAP,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    size_t root_length = wcslen(root_final_path);
    return length > root_length && length < STAGE14_PATH_CAP &&
           _wcsnicmp(file_final_path, root_final_path, root_length) == 0 &&
           (file_final_path[root_length] == L'\\' || file_final_path[root_length] == L'/');
}

static void stage14_digest_hex(const UCHAR digest[32], char out[STAGE14_SHA256_HEX_CAP]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[STAGE14_SHA256_HEX_CAP - 1] = '\0';
}

static bool stage14_sha256_bytes(stage14_sha256_t *ctx, const void *data, ULONG bytes,
                                 char out[STAGE14_SHA256_HEX_CAP]) {
    bool ok = false;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    UCHAR digest[32];
    object = HeapAlloc(GetProcessHeap(), 0, ctx->object_bytes);
    if (!object ||
        BCryptCreateHash(ctx->algorithm, &hash, object, ctx->object_bytes, NULL, 0, 0) != 0) {
        goto done;
    }
    if (bytes > 0 && BCryptHashData(hash, (PUCHAR)data, bytes, 0) != 0) {
        goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        goto done;
    }
    stage14_digest_hex(digest, out);
    ok = true;

done:
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (object) {
        HeapFree(GetProcessHeap(), 0, object);
    }
    memset(digest, 0, sizeof(digest));
    return ok;
}

static bool stage14_sha256_handle(stage14_sha256_t *ctx, HANDLE file, uint64_t expected_bytes,
                                  char out[STAGE14_SHA256_HEX_CAP]) {
    bool ok = false;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    PUCHAR buffer = NULL;
    UCHAR digest[32];
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(file, zero, NULL, FILE_BEGIN)) {
        goto done;
    }
    object = HeapAlloc(GetProcessHeap(), 0, ctx->object_bytes);
    buffer = HeapAlloc(GetProcessHeap(), 0, STAGE14_HASH_BUFFER_BYTES);
    if (!object || !buffer ||
        BCryptCreateHash(ctx->algorithm, &hash, object, ctx->object_bytes, NULL, 0, 0) != 0) {
        goto done;
    }
    uint64_t total = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, STAGE14_HASH_BUFFER_BYTES, &read, NULL)) {
            goto done;
        }
        if (read == 0) {
            break;
        }
        total += read;
        if (total > expected_bytes) {
            goto done;
        }
        if (BCryptHashData(hash, buffer, read, 0) != 0) {
            goto done;
        }
    }
    if (total != expected_bytes || BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        goto done;
    }
    stage14_digest_hex(digest, out);
    ok = true;

done:
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (buffer) {
        HeapFree(GetProcessHeap(), 0, buffer);
    }
    if (object) {
        HeapFree(GetProcessHeap(), 0, object);
    }
    memset(digest, 0, sizeof(digest));
    return ok;
}

static yyjson_doc *stage14_read_json_internal(stage14_sha256_t *ctx, const wchar_t *path,
                                              const wchar_t *root_final_path,
                                              char out_sha[STAGE14_SHA256_HEX_CAP]) {
    HANDLE file = INVALID_HANDLE_VALUE;
    stage14_file_identity_t identity;
    uint64_t file_bytes = 0;
    if (!stage14_open_regular_file(path, &file, &identity, &file_bytes) ||
        (root_final_path && !stage14_handle_within_root(file, root_final_path))) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        return NULL;
    }
    if (file_bytes == 0 || file_bytes > STAGE14_JSON_MAX_BYTES) {
        CloseHandle(file);
        return NULL;
    }
    size_t bytes = (size_t)file_bytes;
    char *data = HeapAlloc(GetProcessHeap(), 0, bytes + 1);
    if (!data) {
        CloseHandle(file);
        return NULL;
    }
    size_t offset = 0;
    bool read_ok = true;
    while (offset < bytes) {
        DWORD chunk = 0;
        DWORD wanted = (DWORD)(bytes - offset);
        if (!ReadFile(file, data + offset, wanted, &chunk, NULL) || chunk == 0) {
            read_ok = false;
            break;
        }
        offset += chunk;
    }
    CloseHandle(file);
    data[bytes] = '\0';
    bool hash_ok = !ctx || (out_sha && stage14_sha256_bytes(ctx, data, (ULONG)bytes, out_sha));
    yyjson_doc *doc = read_ok && hash_ok ? yyjson_read(data, bytes, 0) : NULL;
    memset(data, 0, bytes + 1);
    HeapFree(GetProcessHeap(), 0, data);
    return doc;
}

static yyjson_doc *stage14_read_json_hashed(stage14_sha256_t *ctx, const wchar_t *path,
                                            const wchar_t *root_final_path,
                                            char out_sha[STAGE14_SHA256_HEX_CAP]) {
    return stage14_read_json_internal(ctx, path, root_final_path, out_sha);
}

static void stage14_close_verified_files(stage14_verified_file_t *files, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (files[i].handle != INVALID_HANDLE_VALUE) {
            CloseHandle(files[i].handle);
            files[i].handle = INVALID_HANDLE_VALUE;
        }
    }
}

static void stage14_release_launch_guards(stage14_launch_guards_t *guards) {
    if (!guards) {
        return;
    }
    stage14_close_verified_files(guards->files, guards->file_count);
    guards->file_count = 0;
    if (guards->launcher != INVALID_HANDLE_VALUE) {
        CloseHandle(guards->launcher);
        guards->launcher = INVALID_HANDLE_VALUE;
    }
    for (size_t i = 0; i < guards->directory_count; i++) {
        if (guards->directories[i] != INVALID_HANDLE_VALUE) {
            CloseHandle(guards->directories[i]);
            guards->directories[i] = INVALID_HANDLE_VALUE;
        }
    }
    guards->directory_count = 0;
}

static const char *stage14_json_string(yyjson_val *object, const char *name) {
    yyjson_val *value = object ? yyjson_obj_get(object, name) : NULL;
    return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static bool stage14_json_u64(yyjson_val *object, const char *name, uint64_t *out) {
    yyjson_val *value = object ? yyjson_obj_get(object, name) : NULL;
    if (yyjson_is_uint(value)) {
        *out = yyjson_get_uint(value);
        return true;
    }
    if (yyjson_is_sint(value) && yyjson_get_sint(value) >= 0) {
        *out = (uint64_t)yyjson_get_sint(value);
        return true;
    }
    return false;
}

static bool stage14_file_record_equal(yyjson_val *manifest_record, yyjson_val *receipt_record) {
    const char *manifest_path = stage14_json_string(manifest_record, "path");
    const char *receipt_path = stage14_json_string(receipt_record, "path");
    const char *manifest_sha = stage14_json_string(manifest_record, "sha256");
    const char *receipt_sha = stage14_json_string(receipt_record, "sha256");
    uint64_t manifest_bytes = 0;
    uint64_t receipt_bytes = 0;
    return manifest_path && receipt_path && manifest_sha && receipt_sha &&
           stage14_json_u64(manifest_record, "bytes", &manifest_bytes) &&
           stage14_json_u64(receipt_record, "bytes", &receipt_bytes) &&
           strcmp(manifest_path, receipt_path) == 0 && strcmp(manifest_sha, receipt_sha) == 0 &&
           manifest_bytes == receipt_bytes;
}

static bool stage14_set_runtime_environment(const wchar_t *install_root) {
    wchar_t data_root[STAGE14_PATH_CAP];
    wchar_t artifact_root[STAGE14_PATH_CAP];
    if (!stage14_join_wide(data_root, STAGE14_PATH_CAP, install_root, L"data") ||
        !stage14_join_wide(artifact_root, STAGE14_PATH_CAP, data_root, L"artifacts") ||
        !SetEnvironmentVariableW(L"CBM_DATA_ROOT", data_root) ||
        !SetEnvironmentVariableW(L"CBM_CACHE_DIR", data_root) ||
        !SetEnvironmentVariableW(L"CBM_ARTIFACT_DIR", artifact_root) ||
        !SetEnvironmentVariableW(L"CBM_MEMORY_EMBED_BACKEND", L"static")) {
        return false;
    }
    (void)SetEnvironmentVariableW(L"CBM_MEMORY_NO_GLOBAL_UNION", NULL);
    if (!SetEnvironmentVariableW(L"CBM_MEMORY_AUTO_MAINTAIN", L"0")) {
        return false;
    }
    return true;
}

static bool stage14_append_command_argument(wchar_t *out, size_t out_cap, size_t *offset,
                                            const wchar_t *argument) {
    if (!out || out_cap == 0 || !offset || !argument || *offset >= out_cap) {
        return false;
    }
    if (*offset != 0) {
        if (*offset + 1 >= out_cap) {
            return false;
        }
        out[(*offset)++] = L' ';
    }
    if (*offset + 1 >= out_cap) {
        return false;
    }
    out[(*offset)++] = L'"';

    const wchar_t *cursor = argument;
    for (;;) {
        size_t backslashes = 0;
        while (*cursor == L'\\') {
            backslashes++;
            cursor++;
        }
        bool at_end = *cursor == L'\0';
        bool at_quote = *cursor == L'"';
        size_t required_backslashes = backslashes;
        if (at_end || at_quote) {
            if (backslashes > (SIZE_MAX - (at_quote ? 1u : 0u)) / 2u) {
                return false;
            }
            required_backslashes = backslashes * 2u + (at_quote ? 1u : 0u);
        }
        if (required_backslashes > out_cap - *offset - 1u) {
            return false;
        }
        for (size_t i = 0; i < required_backslashes; i++) {
            out[(*offset)++] = L'\\';
        }
        if (at_end) {
            if (*offset + 1 >= out_cap) {
                return false;
            }
            out[(*offset)++] = L'"';
            out[*offset] = L'\0';
            return true;
        }
        if (*offset + 1 >= out_cap) {
            return false;
        }
        out[(*offset)++] = *cursor++;
    }
}

static bool stage14_build_mcp_arguments(int argc, LPWSTR *argv, wchar_t *out, size_t out_cap) {
    if (!out || out_cap == 0) {
        return false;
    }
    size_t offset = 0;
    out[0] = L'\0';
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || !stage14_append_command_argument(out, out_cap, &offset, argv[i])) {
            return false;
        }
    }
    return true;
}

static int stage14_launch_payload(const wchar_t *executable, const wchar_t *payload_argument,
                                  DWORD timeout_ms, stage14_launch_guards_t *guards) {
    wchar_t command[STAGE14_PATH_CAP];
    size_t exe_len = wcslen(executable);
    size_t arg_len = payload_argument ? wcslen(payload_argument) : 0;
    if (exe_len + arg_len + 5 >= STAGE14_PATH_CAP) {
        stage14_release_launch_guards(guards);
        return stage14_integrity_failure();
    }
    command[0] = L'"';
    memcpy(command + 1, executable, exe_len * sizeof(*command));
    command[exe_len + 1] = L'"';
    size_t offset = exe_len + 2;
    if (payload_argument && *payload_argument) {
        command[offset++] = L' ';
        memcpy(command + offset, payload_argument, arg_len * sizeof(*command));
        offset += arg_len;
    }
    command[offset] = L'\0';

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        stage14_release_launch_guards(guards);
        return stage14_integrity_failure();
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        stage14_release_launch_guards(guards);
        return stage14_integrity_failure();
    }

    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &startup,
                        &process)) {
        CloseHandle(job);
        stage14_release_launch_guards(guards);
        return stage14_integrity_failure();
    }
    stage14_release_launch_guards(guards);
    if (!AssignProcessToJobObject(job, process.hProcess) ||
        ResumeThread(process.hThread) == (DWORD)-1) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        return stage14_integrity_failure();
    }
    CloseHandle(process.hThread);
    DWORD wait_result = WaitForSingleObject(process.hProcess, timeout_ms);
    if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_FAILED) {
        (void)TerminateJobObject(job, 1);
        (void)WaitForSingleObject(process.hProcess, 100);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        (void)fprintf(stderr, "STAGE14_HOOK_TIMEOUT\n");
        return 1;
    }
    DWORD exit_code = 1;
    (void)GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    return (int)exit_code;
}

static int stage14_stable_launcher(stage14_windows_argv_t *arguments, bool *handled) {
    if (!handled) {
        return stage14_integrity_failure();
    }
    *handled = false;
    ULONGLONG launcher_started_ms = GetTickCount64();
    wchar_t module_path[STAGE14_PATH_CAP];
    DWORD module_len = GetModuleFileNameW(NULL, module_path, STAGE14_PATH_CAP);
    if (module_len == 0 || module_len >= STAGE14_PATH_CAP) {
        return 1;
    }
    wchar_t module_copy[STAGE14_PATH_CAP];
    wchar_t bin_dir[STAGE14_PATH_CAP];
    if (!stage14_copy_wide(module_copy, STAGE14_PATH_CAP, module_path) ||
        !stage14_copy_wide(bin_dir, STAGE14_PATH_CAP, module_path) ||
        !stage14_wide_parent_in_place(bin_dir)) {
        return 1;
    }
    wchar_t bin_copy[STAGE14_PATH_CAP];
    if (!stage14_copy_wide(bin_copy, STAGE14_PATH_CAP, bin_dir)) {
        return 1;
    }
    if (_wcsicmp(stage14_wide_basename(bin_copy), L"bin") != 0) {
        return 0;
    }
    *handled = true;

    const wchar_t *module_name = stage14_wide_basename(module_copy);
    stage14_role_t role = STAGE14_ROLE_NONE;
    const char *role_key = NULL;
    const char *expected_entry_relative = NULL;
    const wchar_t *expected_payload_name = NULL;
    if (_wcsicmp(module_name, L"semantic-memory-mcp.exe") == 0) {
        role = STAGE14_ROLE_MCP;
        role_key = "mcp";
        expected_entry_relative = "semantic-memory-mcp.exe";
        expected_payload_name = L"semantic-memory-mcp.exe";
    } else if (_wcsicmp(module_name, L"semantic-memory-hook.exe") == 0) {
        role = STAGE14_ROLE_HOOK;
        role_key = "hook";
        expected_entry_relative = "semantic-memory-hook.exe";
        expected_payload_name = L"semantic-memory-hook.exe";
    } else if (_wcsicmp(module_name, L"semantic-memory-manager.exe") == 0) {
        role = STAGE14_ROLE_MANAGER;
        role_key = "manager";
        expected_entry_relative = "semantic-memory-manager.exe";
        expected_payload_name = L"semantic-memory-manager.exe";
    } else {
        if (arguments && arguments->wide_argv) {
            (void)stage14_free_wide_argv(&arguments->wide_argv);
        }
        return stage14_integrity_failure();
    }

    if (!arguments || !arguments->wide_argv || !arguments->utf8_argv || arguments->argc < 1) {
        if (arguments && arguments->wide_argv) {
            (void)stage14_free_wide_argv(&arguments->wide_argv);
        }
        return stage14_integrity_failure();
    }
    int wide_argc = arguments->argc;
    LPWSTR *wide_argv = arguments->wide_argv;
    bool arguments_valid = true;
    for (int i = 0; i < wide_argc; i++) {
        if (!wide_argv[i]) {
            arguments_valid = false;
        }
    }

    bool verify_only = false;
    const wchar_t *hook_action = NULL;
    wchar_t mcp_arguments[STAGE14_PATH_CAP];
    mcp_arguments[0] = L'\0';
    if (arguments_valid && role == STAGE14_ROLE_MCP) {
        if (wide_argc == 2 && wcscmp(wide_argv[1], L"--verify-only") == 0) {
            verify_only = true;
        } else {
            for (int i = 1; i < wide_argc; i++) {
                if (wcscmp(wide_argv[i], L"--verify-only") == 0) {
                    arguments_valid = false;
                }
            }
            if (arguments_valid && !stage14_build_mcp_arguments(wide_argc, wide_argv, mcp_arguments,
                                                                STAGE14_PATH_CAP)) {
                arguments_valid = false;
            }
        }
    } else if (arguments_valid) {
        for (int i = 1; i < wide_argc; i++) {
            if (wcscmp(wide_argv[i], L"--verify-only") == 0 && !verify_only) {
                verify_only = true;
            } else if (role == STAGE14_ROLE_HOOK && !hook_action) {
                hook_action = wide_argv[i];
            } else {
                arguments_valid = false;
            }
        }
    }
    const wchar_t *payload_argument = NULL;
    if (arguments_valid && role == STAGE14_ROLE_MCP) {
        payload_argument = mcp_arguments[0] ? mcp_arguments : NULL;
    } else if (arguments_valid && role == STAGE14_ROLE_HOOK) {
        if (!hook_action) {
            arguments_valid = false;
        } else if (wcscmp(hook_action, L"recall") == 0) {
            payload_argument = L"memory-recall";
        } else if (wcscmp(hook_action, L"post-tool") == 0) {
            payload_argument = L"memory-post-tool";
        } else if (wcscmp(hook_action, L"stop") == 0) {
            payload_argument = L"memory-stop";
        } else {
            arguments_valid = false;
        }
    } else if (arguments_valid && role == STAGE14_ROLE_MANAGER) {
        payload_argument = L"manager";
    }
    bool wide_freed = stage14_free_wide_argv(&arguments->wide_argv);
    wide_argv = NULL;
    if (!arguments_valid || !wide_freed) {
        return stage14_integrity_failure();
    }

    wchar_t install_root[STAGE14_PATH_CAP];
    wchar_t pointer_path[STAGE14_PATH_CAP];
    if (!stage14_copy_wide(install_root, STAGE14_PATH_CAP, bin_dir) ||
        !stage14_wide_parent_in_place(install_root) ||
        !stage14_join_wide(pointer_path, STAGE14_PATH_CAP, install_root, L"state\\current.json")) {
        return stage14_integrity_failure();
    }

    stage14_final_paths_t *final_paths =
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*final_paths));
    if (!final_paths) {
        return stage14_integrity_failure();
    }
    HANDLE directory_handles[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
                                   INVALID_HANDLE_VALUE};
    stage14_verified_file_t verified_files[STAGE14_MAX_PAYLOAD_FILES];
    memset(verified_files, 0, sizeof(verified_files));
    stage14_launch_guards_t guards = {
        .files = verified_files,
        .file_count = 0,
        .launcher = INVALID_HANDLE_VALUE,
        .directories = directory_handles,
        .directory_count = 0,
    };
    stage14_sha256_t sha_ctx;
    memset(&sha_ctx, 0, sizeof(sha_ctx));
    bool sha_ready = false;
    bool emit_verify_pass = false;
    yyjson_doc *manifest_doc = NULL;
    yyjson_doc *receipt_doc = NULL;

    HANDLE install_handle = stage14_open_directory(install_root, final_paths->install_root);
    if (install_handle == INVALID_HANDLE_VALUE) {
        goto launcher_cleanup;
    }
    directory_handles[guards.directory_count++] = install_handle;
    HANDLE bin_handle = stage14_open_directory(bin_dir, final_paths->bin_root);
    if (bin_handle == INVALID_HANDLE_VALUE ||
        !stage14_handle_within_root(bin_handle, final_paths->install_root)) {
        if (bin_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(bin_handle);
        }
        goto launcher_cleanup;
    }
    directory_handles[guards.directory_count++] = bin_handle;

    yyjson_doc *pointer_doc =
        stage14_read_json_internal(NULL, pointer_path, final_paths->install_root, NULL);
    yyjson_val *pointer = pointer_doc ? yyjson_doc_get_root(pointer_doc) : NULL;
    const char *pointer_schema = stage14_json_string(pointer, "schema");
    const char *pointer_version = stage14_json_string(pointer, "version_id");
    const char *pointer_manifest_sha = stage14_json_string(pointer, "manifest_sha256");
    const char *pointer_receipt_sha = stage14_json_string(pointer, "receipt_sha256");
    char version_id[STAGE14_VERSION_CAP];
    char manifest_expected[STAGE14_SHA256_HEX_CAP];
    char receipt_expected[STAGE14_SHA256_HEX_CAP];
    if (!pointer_doc || !yyjson_is_obj(pointer) || !pointer_schema ||
        strcmp(pointer_schema, "stage14-current-pointer/v1") != 0 ||
        !stage14_valid_version_id(pointer_version) || !stage14_valid_sha256(pointer_manifest_sha) ||
        !stage14_valid_sha256(pointer_receipt_sha)) {
        yyjson_doc_free(pointer_doc);
        goto launcher_cleanup;
    }
    memcpy(version_id, pointer_version, strlen(pointer_version) + 1);
    memcpy(manifest_expected, pointer_manifest_sha, sizeof(manifest_expected));
    memcpy(receipt_expected, pointer_receipt_sha, sizeof(receipt_expected));
    yyjson_doc_free(pointer_doc);

    wchar_t versions_root[STAGE14_PATH_CAP];
    wchar_t wide_version[STAGE14_VERSION_CAP];
    wchar_t payload_root[STAGE14_PATH_CAP];
    wchar_t manifest_path[STAGE14_PATH_CAP];
    wchar_t receipt_path[STAGE14_PATH_CAP];
    if (!stage14_join_wide(versions_root, STAGE14_PATH_CAP, install_root, L"app\\versions") ||
        !stage14_utf8_to_wide(version_id, wide_version, STAGE14_VERSION_CAP) ||
        !stage14_join_wide(payload_root, STAGE14_PATH_CAP, versions_root, wide_version) ||
        !stage14_join_wide(manifest_path, STAGE14_PATH_CAP, payload_root,
                           L"payload-manifest.json") ||
        !stage14_join_wide(receipt_path, STAGE14_PATH_CAP, payload_root,
                           L"verification-receipt.json")) {
        goto launcher_cleanup;
    }
    HANDLE payload_handle = stage14_open_directory(payload_root, final_paths->payload_root);
    if (payload_handle == INVALID_HANDLE_VALUE ||
        !stage14_handle_within_root(payload_handle, final_paths->install_root)) {
        if (payload_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(payload_handle);
        }
        goto launcher_cleanup;
    }
    directory_handles[guards.directory_count++] = payload_handle;

    char manifest_actual[STAGE14_SHA256_HEX_CAP];
    char receipt_actual[STAGE14_SHA256_HEX_CAP];
    if (!stage14_sha256_init(&sha_ctx)) {
        goto launcher_cleanup;
    }
    sha_ready = true;
    manifest_doc = stage14_read_json_hashed(&sha_ctx, manifest_path, final_paths->payload_root,
                                            manifest_actual);
    receipt_doc =
        stage14_read_json_hashed(&sha_ctx, receipt_path, final_paths->payload_root, receipt_actual);
    if (!manifest_doc || !receipt_doc || strcmp(manifest_actual, manifest_expected) != 0 ||
        strcmp(receipt_actual, receipt_expected) != 0) {
        goto launcher_cleanup;
    }

    yyjson_val *manifest = yyjson_doc_get_root(manifest_doc);
    yyjson_val *receipt = yyjson_doc_get_root(receipt_doc);
    yyjson_val *manifest_files = manifest ? yyjson_obj_get(manifest, "files") : NULL;
    yyjson_val *receipt_files = receipt ? yyjson_obj_get(receipt, "files") : NULL;
    yyjson_val *entrypoints = manifest ? yyjson_obj_get(manifest, "entrypoints") : NULL;
    const char *manifest_schema = stage14_json_string(manifest, "schema");
    const char *manifest_version = stage14_json_string(manifest, "version_id");
    const char *receipt_schema = stage14_json_string(receipt, "schema");
    const char *receipt_version = stage14_json_string(receipt, "version_id");
    const char *receipt_manifest_sha = stage14_json_string(receipt, "manifest_sha256");
    const char *mcp_entry = stage14_json_string(entrypoints, "mcp");
    const char *hook_entry = stage14_json_string(entrypoints, "hook");
    const char *manager_entry = stage14_json_string(entrypoints, "manager");
    const char *entry_relative = stage14_json_string(entrypoints, role_key);
    size_t file_count = yyjson_is_arr(manifest_files) ? yyjson_arr_size(manifest_files) : 0;
    if (!yyjson_is_obj(manifest) || !yyjson_is_obj(receipt) || !yyjson_is_obj(entrypoints) ||
        !manifest_schema || strcmp(manifest_schema, "stage14-payload-manifest/v1") != 0 ||
        !manifest_version || strcmp(manifest_version, version_id) != 0 || !receipt_schema ||
        strcmp(receipt_schema, "stage14-verification-receipt/v1") != 0 || !receipt_version ||
        strcmp(receipt_version, version_id) != 0 || !receipt_manifest_sha ||
        strcmp(receipt_manifest_sha, manifest_actual) != 0 || !yyjson_is_arr(receipt_files) ||
        yyjson_arr_size(receipt_files) != file_count || file_count == 0 ||
        file_count > STAGE14_MAX_PAYLOAD_FILES || !mcp_entry || !hook_entry || !manager_entry ||
        strcmp(mcp_entry, "semantic-memory-mcp.exe") != 0 ||
        strcmp(hook_entry, "semantic-memory-hook.exe") != 0 ||
        strcmp(manager_entry, "semantic-memory-manager.exe") != 0 || !entry_relative ||
        strcmp(entry_relative, expected_entry_relative) != 0) {
        goto launcher_cleanup;
    }

    bool entry_seen = false;
    bool mcp_seen = false;
    bool hook_seen = false;
    bool manager_seen = false;
    char entry_sha[STAGE14_SHA256_HEX_CAP] = {0};
    uint64_t entry_bytes = 0;
    stage14_file_identity_t entry_identity = {0};
    wchar_t payload_executable[STAGE14_PATH_CAP];
    memset(payload_executable, 0, sizeof(payload_executable));
    for (size_t i = 0; i < file_count; i++) {
        yyjson_val *record = yyjson_arr_get(manifest_files, i);
        yyjson_val *receipt_record = yyjson_arr_get(receipt_files, i);
        const char *relative = stage14_json_string(record, "path");
        const char *expected_sha = stage14_json_string(record, "sha256");
        uint64_t expected_bytes = 0;
        wchar_t file_path[STAGE14_PATH_CAP];
        char actual_sha[STAGE14_SHA256_HEX_CAP];
        uint64_t actual_bytes = 0;
        stage14_file_identity_t actual_identity;
        HANDLE actual_handle = INVALID_HANDLE_VALUE;
        if (!yyjson_is_obj(record) || !yyjson_is_obj(receipt_record) ||
            !stage14_file_record_equal(record, receipt_record) ||
            !stage14_json_u64(record, "bytes", &expected_bytes) ||
            !stage14_valid_sha256(expected_sha) ||
            !stage14_resolve_child(payload_root, relative, file_path, STAGE14_PATH_CAP) ||
            !stage14_open_regular_file(file_path, &actual_handle, &actual_identity,
                                       &actual_bytes) ||
            !stage14_handle_within_root(actual_handle, final_paths->payload_root)) {
            if (actual_handle != INVALID_HANDLE_VALUE) {
                CloseHandle(actual_handle);
            }
            goto launcher_cleanup;
        }
        for (size_t prior = 0; prior < i; prior++) {
            const char *prior_path =
                stage14_json_string(yyjson_arr_get(manifest_files, prior), "path");
            if (prior_path && _stricmp(prior_path, relative) == 0) {
                CloseHandle(actual_handle);
                goto launcher_cleanup;
            }
        }

        bool hash_cached = false;
        for (size_t verified = 0; verified < guards.file_count; verified++) {
            if (stage14_file_identity_equal(&verified_files[verified].identity, &actual_identity)) {
                if (verified_files[verified].bytes != actual_bytes) {
                    CloseHandle(actual_handle);
                    goto launcher_cleanup;
                }
                memcpy(actual_sha, verified_files[verified].sha256, sizeof(actual_sha));
                hash_cached = true;
                break;
            }
        }
        if (hash_cached) {
            CloseHandle(actual_handle);
            actual_handle = INVALID_HANDLE_VALUE;
        } else {
            if (guards.file_count >= STAGE14_MAX_PAYLOAD_FILES ||
                !stage14_sha256_handle(&sha_ctx, actual_handle, actual_bytes, actual_sha)) {
                CloseHandle(actual_handle);
                goto launcher_cleanup;
            }
            verified_files[guards.file_count].identity = actual_identity;
            verified_files[guards.file_count].bytes = actual_bytes;
            memcpy(verified_files[guards.file_count].sha256, actual_sha, sizeof(actual_sha));
            verified_files[guards.file_count].handle = actual_handle;
            guards.file_count++;
            actual_handle = INVALID_HANDLE_VALUE;
        }
        if (actual_bytes != expected_bytes || strcmp(actual_sha, expected_sha) != 0) {
            goto launcher_cleanup;
        }
        if (strcmp(relative, "semantic-memory-mcp.exe") == 0) {
            mcp_seen = true;
        } else if (strcmp(relative, "semantic-memory-hook.exe") == 0) {
            hook_seen = true;
        } else if (strcmp(relative, "semantic-memory-manager.exe") == 0) {
            manager_seen = true;
        }
        if (strcmp(relative, entry_relative) == 0) {
            wchar_t entry_copy[STAGE14_PATH_CAP];
            if (entry_seen || !stage14_copy_wide(entry_copy, STAGE14_PATH_CAP, file_path) ||
                _wcsicmp(stage14_wide_basename(entry_copy), expected_payload_name) != 0 ||
                !stage14_copy_wide(payload_executable, STAGE14_PATH_CAP, file_path)) {
                goto launcher_cleanup;
            }
            memcpy(entry_sha, actual_sha, sizeof(entry_sha));
            entry_bytes = actual_bytes;
            entry_identity = actual_identity;
            entry_seen = true;
        }
    }
    if (!entry_seen || !mcp_seen || !hook_seen || !manager_seen) {
        goto launcher_cleanup;
    }

    char launcher_sha[STAGE14_SHA256_HEX_CAP];
    uint64_t launcher_bytes = 0;
    stage14_file_identity_t launcher_identity;
    if (!stage14_open_regular_file(module_path, &guards.launcher, &launcher_identity,
                                   &launcher_bytes) ||
        !stage14_handle_within_root(guards.launcher, final_paths->bin_root) ||
        launcher_bytes != entry_bytes) {
        goto launcher_cleanup;
    }
    if (stage14_file_identity_equal(&launcher_identity, &entry_identity)) {
        memcpy(launcher_sha, entry_sha, sizeof(launcher_sha));
    } else if (!stage14_sha256_handle(&sha_ctx, guards.launcher, launcher_bytes, launcher_sha)) {
        goto launcher_cleanup;
    }
    if (strcmp(launcher_sha, entry_sha) != 0) {
        goto launcher_cleanup;
    }

    yyjson_doc_free(manifest_doc);
    manifest_doc = NULL;
    yyjson_doc_free(receipt_doc);
    receipt_doc = NULL;
    stage14_sha256_close(&sha_ctx);
    sha_ready = false;
    if (verify_only) {
        emit_verify_pass = true;
        goto launcher_cleanup;
    }
    if (!stage14_set_runtime_environment(install_root)) {
        goto launcher_cleanup;
    }
    DWORD payload_timeout = role == STAGE14_ROLE_HOOK ? 2000 : INFINITE;
    if (role == STAGE14_ROLE_HOOK) {
        const DWORD cleanup_reserve_ms = 100;
        ULONGLONG elapsed_ms = GetTickCount64() - launcher_started_ms;
        if (payload_timeout <= cleanup_reserve_ms ||
            elapsed_ms >= (ULONGLONG)(payload_timeout - cleanup_reserve_ms)) {
            stage14_release_launch_guards(&guards);
            HeapFree(GetProcessHeap(), 0, final_paths);
            (void)fprintf(stderr, "STAGE14_HOOK_TIMEOUT\n");
            return 1;
        }
        payload_timeout -= (DWORD)elapsed_ms + cleanup_reserve_ms;
    }
    HeapFree(GetProcessHeap(), 0, final_paths);
    final_paths = NULL;
    return stage14_launch_payload(payload_executable, payload_argument, payload_timeout, &guards);

launcher_cleanup:
    if (manifest_doc) {
        yyjson_doc_free(manifest_doc);
    }
    if (receipt_doc) {
        yyjson_doc_free(receipt_doc);
    }
    if (sha_ready) {
        stage14_sha256_close(&sha_ctx);
    }
    stage14_release_launch_guards(&guards);
    if (final_paths) {
        HeapFree(GetProcessHeap(), 0, final_paths);
    }
    if (emit_verify_pass) {
        static const char pass_seal[] = "PASS_FULL_SHA256\n";
        HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (!stdout_handle || stdout_handle == INVALID_HANDLE_VALUE ||
            !WriteFile(stdout_handle, pass_seal, (DWORD)(sizeof(pass_seal) - 1u), &written, NULL) ||
            written != sizeof(pass_seal) - 1u) {
            return stage14_integrity_failure();
        }
        return 0;
    }
    return stage14_integrity_failure();
}

#endif

/* ── Globals for signal handling ────────────────────────────────── */

static cbm_watcher_t *g_watcher = NULL;
static cbm_mcp_server_t *g_server = NULL;
static cbm_http_server_t *g_http_server = NULL;
static atomic_int g_shutdown = 0;

/* Idempotent shutdown: cancels the active pipeline, stops background servers,
 * and closes stdin to unblock the MCP read loop. Invoked from the signal
 * handler and from the parent-death watchdog, hence the atomic_exchange guard
 * so the body runs at most once. Body is async-signal-safe (only atomic stores
 * and stop calls that themselves only set atomics). */
static void request_shutdown(void) {
    if (atomic_exchange(&g_shutdown, 1)) {
        return; /* already shutting down */
    }

    /* Cancel any in-progress pipeline (async-signal-safe: only does atomic_store) */
    if (g_server) {
        cbm_pipeline_t *p = cbm_mcp_server_active_pipeline(g_server);
        if (p) {
            cbm_pipeline_cancel(p);
        }
    }
    /* Release pipeline lock to prevent stale lock on restart */
    cbm_pipeline_unlock();

    if (g_watcher) {
        cbm_watcher_stop(g_watcher);
    }
    if (g_http_server) {
        cbm_http_server_stop(g_http_server);
    }
    /* Close stdin to unblock getline in the MCP server loop */
    (void)fclose(stdin);
}

static void signal_handler(int sig) {
    (void)sig;
    request_shutdown();
}

/* ── Parent-process watchdog ────────────────────────────────────── */
/* parent-death watchdog — distilled from #407 (fixes #406, thanks @nvt-pankajsharma).
 *
 * When this stdio MCP server is launched by an agent that later dies without a
 * clean SIGTERM (e.g. the editor is force-killed), the orphaned server would
 * otherwise linger forever blocked on stdin. POSIX has no portable "notify on
 * parent death" primitive (PR_SET_PDEATHSIG is Linux-only), so we poll getppid:
 * once the parent dies the process is reparented (ppid changes, typically to 1)
 * and we shut down. Windows is unaffected (job objects handle this) — #ifndef. */

#ifndef _WIN32
static void *parent_watchdog_thread(void *arg) {
    pid_t initial_ppid = *(pid_t *)arg;
    const unsigned int poll_interval_us = 500000; /* 500ms */

    while (!atomic_load(&g_shutdown)) {
        cbm_usleep(poll_interval_us);
        if (atomic_load(&g_shutdown)) {
            break;
        }
        /* initial_ppid > 1 guards against an already-orphaned start (ppid==1),
         * where a changing ppid carries no signal. */
        if (initial_ppid > 1 && getppid() != initial_ppid) {
            cbm_log_warn("parent.exited", "reason", "ppid_changed");
            request_shutdown();
            exit(0);
        }
    }
    return NULL;
}
#endif

/* ── Watcher background thread ──────────────────────────────────── */

static void *watcher_thread(void *arg) {
    cbm_watcher_t *w = arg;
#define WATCHER_BASE_INTERVAL_MS 5000

    cbm_watcher_run(w, WATCHER_BASE_INTERVAL_MS);
    return NULL;
}

/* ── HTTP UI background thread ──────────────────────────────────── */

static void *http_thread(void *arg) {
    cbm_http_server_t *srv = arg;
    cbm_http_server_run(srv);
    return NULL;
}

/* ── Index callback for watcher ─────────────────────────────────── */

static int watcher_index_fn(const char *project_name, const char *root_path, void *user_data) {
    (void)user_data;

    /* Skip indexing if shutdown is in progress */
    if (atomic_load(&g_shutdown)) {
        return 0;
    }

    /* Non-blocking: skip if another pipeline is already running.
     * Watcher will retry on next poll cycle (5-60s). */
    if (!cbm_pipeline_try_lock()) {
        cbm_log_info("watcher.skip", "project", project_name, "reason", "pipeline_busy");
        return 0;
    }

    cbm_log_info("watcher.reindex", "project", project_name, "path", root_path);

    cbm_pipeline_t *p = cbm_pipeline_new(root_path, NULL, CBM_MODE_FULL);
    if (!p) {
        cbm_pipeline_unlock();
        return CBM_NOT_FOUND;
    }

    int rc = cbm_pipeline_run(p);
    cbm_pipeline_free(p);
    cbm_pipeline_unlock();
    return rc;
}

/* ── CLI mode ───────────────────────────────────────────────────── */

#define CLI_USAGE                                                                          \
    "Usage: semantic-memory-mcp cli [--progress] [--json] <tool_name> [--flag value... | " \
    "--args-file "                                                                         \
    "<path> | '<raw-json>']\n"

/* Extract text content from MCP tool result envelope and print it.
 * MCP results: {"content":[{"type":"text","text":"..."}],"isError":...}
 * Returns 1 if the result was an error, 0 otherwise. */
static int cli_print_mcp_result(const char *result) {
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    if (!doc) {
        printf("%s\n", result);
        return 0;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *err_val = yyjson_obj_get(root, "isError");
    bool is_error = err_val && yyjson_get_bool(err_val);

    const char *text = NULL;
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (yyjson_is_arr(content) && yyjson_arr_size(content) > 0) {
        yyjson_val *tv = yyjson_obj_get(yyjson_arr_get_first(content), "text");
        text = tv ? yyjson_get_str(tv) : NULL;
    }

    if (text) {
        (void)fprintf(is_error ? stderr : stdout, "%s\n", text);
    } else {
        printf("%s\n", result);
    }

    yyjson_doc_free(doc);
    return is_error ? SKIP_ONE : 0;
}

static void setup_signal_handlers(void);

static int run_manager(void) {
    if (CBM_EMBEDDED_FILE_COUNT == 0) {
        (void)fprintf(stderr, "manager unavailable: embedded assets missing\n");
        return 1;
    }
    cbm_mem_init(MAIN_RAM_FRACTION);
    cbm_ui_log_init();
    cbm_log_set_sink(cbm_ui_log_append);
    setup_signal_handlers();
    g_http_server = cbm_http_server_new_manager();
    if (!g_http_server) {
        (void)fprintf(stderr, "manager unavailable\n");
        return 1;
    }
    char url[192];
    if (!cbm_http_server_manager_url(g_http_server, url, sizeof(url))) {
        cbm_http_server_free(g_http_server);
        g_http_server = NULL;
        return 1;
    }
#ifdef _WIN32
    const char *no_browser = getenv("CBM_MANAGER_NO_BROWSER");
    if (!no_browser || strcmp(no_browser, "1") != 0) {
        (void)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    }
#endif
    memset(url, 0, sizeof(url));
    cbm_http_server_run(g_http_server);
    cbm_http_server_free(g_http_server);
    g_http_server = NULL;
    return 0;
}

/* Strip a flag from argv, returning true if found. */
static bool cli_strip_flag(int *argc, char **argv, const char *flag) {
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], flag) != 0) {
            continue;
        }
        for (int j = i; j < *argc - SKIP_ONE; j++) {
            argv[j] = argv[j + SKIP_ONE];
        }
        (*argc)--;
        return true;
    }
    return false;
}

/* Read the whole FILE stream (stdin or an opened file) up to EOF into a
 * NUL-terminated heap buffer. Returns NULL on I/O failure. Caller frees. */
static char *cli_read_stream(FILE *fp) {
    size_t cap = CBM_SZ_4K;
    size_t len = 0;
    char *buf = malloc(cap + SKIP_ONE);
    if (!buf) {
        return NULL;
    }
    buf[0] = '\0';
    for (;;) {
        if (len == cap) {
            cap *= SKIP_ONE + SKIP_ONE;
            char *grown = realloc(buf, cap + SKIP_ONE);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
        size_t n = fread(buf + len, SKIP_ONE, cap - len, fp);
        len += n;
        if (n == 0) {
            break; /* EOF or error; caller checks ferror via empty result */
        }
    }
    buf[len] = '\0';
    return buf;
}

/* Read an entire file into a NUL-terminated heap buffer. NULL on failure. */
static char *cli_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    char *data = cli_read_stream(fp);
    (void)fclose(fp);
    return data;
}

/* Scan argv (starting at index 1 — argv[0] is the tool name) for an
 * `--args-file <path>` or `--args-file=<path>` flag. If found, remove the flag
 * (and its value token) from argv, decrement *argc accordingly, and return the
 * path via *out_path (borrowed from argv, non-owning). Returns true if found. */
static bool cli_strip_args_file(int *argc, char **argv, const char **out_path) {
    for (int i = SKIP_ONE; i < *argc; i++) {
        if (strcmp(argv[i], "--args-file") == 0) {
            if (i + SKIP_ONE >= *argc) {
                return false; /* --args-file with no value: leave for error path */
            }
            *out_path = argv[i + SKIP_ONE];
            /* Shift left by 2 to drop the flag and its value. */
            for (int j = i; j < *argc - SKIP_ONE - SKIP_ONE; j++) {
                argv[j] = argv[j + SKIP_ONE + SKIP_ONE];
            }
            *argc -= SKIP_ONE + SKIP_ONE;
            return true;
        }
        if (strncmp(argv[i], "--args-file=", sizeof("--args-file=") - 1) == 0) {
            *out_path = argv[i] + sizeof("--args-file=") - SKIP_ONE;
            for (int j = i; j < *argc - SKIP_ONE; j++) {
                argv[j] = argv[j + SKIP_ONE];
            }
            (*argc)--;
            return true;
        }
    }
    return false;
}

/* Whether an argv token (after the tool name) is raw JSON (starts with { or [)
 * rather than a --flag. */
static bool cli_is_raw_json(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return *s == '{' || *s == '[';
}

static int run_cli(int argc, char **argv) {
    if (argc < MAIN_MIN_ARGC) {
        (void)fprintf(stderr, CLI_USAGE);
        return SKIP_ONE;
    }

    bool progress = cli_strip_flag(&argc, argv, "--progress");
    bool raw_json = cli_strip_flag(&argc, argv, "--json");

    if (argc < MAIN_MIN_ARGC) {
        (void)fprintf(stderr, CLI_USAGE);
        if (progress) {
            cbm_progress_sink_fini();
        }
        return SKIP_ONE;
    }

    const char *tool_name = argv[0];

    if (progress) {
        cbm_progress_sink_init(stderr);
    }

    /* ── Input-mode dispatch ───────────────────────────────────────
     * smoke-test Phase 3h (scripts/smoke-test.sh, cases B1–B7) defines the
     * accepted call forms:
     *   cli <tool> --flag value ...            (B1/B2/B3) — schema-typed flags
     *   cli <tool> --help                      (B6)      — per-tool help
     *   cli <tool> --args-file <path>          (B5)      — JSON file
     *   echo '<json>' | cli <tool>             (B4)      — JSON on stdin
     *   cli <tool> '{...}'                     (B7)      — raw JSON (deprecated)
     * With zero args and no piped stdin, it defaults to {} (an empty tool call).
     * Legacy `--progress` / `--json` are stripped above. */

    char *json_owned = NULL; /* heap args JSON we allocate and must free */
    const char *json = NULL; /* effective args JSON passed to the tool */

    /* B6: --help / -h anywhere after the tool name. Must be handled before
     * flag parsing so unknown tools error cleanly (B6c). */
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            int rc = cbm_cli_print_tool_help(tool_name);
            if (progress) {
                cbm_progress_sink_fini();
            }
            if (rc != 0) {
                (void)fprintf(stderr, "error: unknown tool '%s'\n", tool_name);
                return SKIP_ONE;
            }
            return 0;
        }
    }

    /* B5: --args-file <path> — JSON read straight from the file. The flag
     * (and its value) are stripped from argv before any flag-form detection. */
    const char *args_file = NULL;
    if (cli_strip_args_file(&argc, argv, &args_file)) {
        json = cli_read_file(args_file);
        if (!json) {
            if (progress) {
                cbm_progress_sink_fini();
            }
            (void)fprintf(stderr, "error: cannot read --args-file '%s'\n", args_file);
            return SKIP_ONE;
        }
        json_owned = (char *)json;
        goto handled;
    }

    /* argv[1..] after stripping --progress/--json/--args-file. */
    if (argc >= MAIN_CLI_ARGC) {
        if (cli_is_raw_json(argv[SKIP_ONE])) {
            /* B7: raw JSON remains accepted but is deprecated. Emit a warning
             * on stderr — flag form is preferred. */
            (void)fprintf(stderr,
                          "warning: raw-JSON cli argument is deprecated; "
                          "use 'cli %s --flag value' or --args-file instead\n",
                          tool_name);
            json = argv[SKIP_ONE];
            goto handled;
        }
        if (strncmp(argv[SKIP_ONE], "--", MAIN_CLI_FLAG_LEN) == 0) {
            /* B1/B2/B3/B7b: flag form. Convert using the tool's input_schema. */
            char *err = NULL;
            json = cbm_cli_build_args_json(tool_name, argc - SKIP_ONE, argv + SKIP_ONE, &err);
            if (!json) {
                if (progress) {
                    cbm_progress_sink_fini();
                }
                (void)fprintf(stderr, "error: %s\n", err ? err : "invalid arguments");
                free(err);
                return SKIP_ONE;
            }
            json_owned = (char *)json;
            goto handled;
        }
        /* Not JSON and not a flag — malformed. */
        if (progress) {
            cbm_progress_sink_fini();
        }
        (void)fprintf(stderr, "error: unexpected argument '%s'\n", argv[SKIP_ONE]);
        return SKIP_ONE;
    }

    /* B4: no explicit args. If stdin is piped (not a TTY), read JSON from it;
     * an interactive terminal (or empty pipe) degrades to an empty object. */
    if (!CBM_CLI_STDIN_IS_TTY()) {
        char *stdin_json = cli_read_stream(stdin);
        if (stdin_json && stdin_json[0] != '\0') {
            json = stdin_json;
            json_owned = stdin_json;
            goto handled;
        }
        free(stdin_json);
    }
    json = "{}";

handled: {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        (void)fprintf(stderr, "error: failed to create server\n");
        free(json_owned);
        if (progress) {
            cbm_progress_sink_fini();
        }
        return SKIP_ONE;
    }

    char *result = cbm_mcp_handle_tool(srv, tool_name, json);
    int exit_code = 0;

    if (result) {
        if (raw_json) {
            printf("%s\n", result);
        } else {
            exit_code = cli_print_mcp_result(result);
        }
        free(result);
    }

    free(json_owned);
    cbm_mcp_server_free(srv);
    if (progress) {
        cbm_progress_sink_fini();
    }
    return exit_code;
}
}

/* ── Help ───────────────────────────────────────────────────────── */

static void print_help(void) {
    printf("semantic-memory-mcp %s\n\n", CBM_VERSION);
    printf("Usage:\n");
    printf("  semantic-memory-mcp              Run MCP server on stdio\n");
    printf("  semantic-memory-mcp cli <tool> [json]  Run a single tool\n");
    printf("  semantic-memory-mcp install [-y|-n] [--force] [--dry-run]\n");
    printf("  semantic-memory-mcp uninstall [-y|-n] [--dry-run]\n");
    printf("  semantic-memory-mcp update [-y|-n]\n");
    printf("  semantic-memory-mcp config <list|get|set|reset>\n");
    printf("  semantic-memory-mcp manager      Open the local Stage12 manager\n");
    printf("  semantic-memory-mcp --version    Print version\n");
    printf("  semantic-memory-mcp --help       Print this help\n");
    printf("\nUI options:\n");
    printf("  --ui=true    Enable HTTP graph visualization (persisted)\n");
    printf("  --ui=false   Disable HTTP graph visualization (persisted)\n");
    printf("  --port=N     Set UI port (default 9749, persisted)\n");
    printf("\nSupported agents (auto-detected):\n");
    printf("  Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode,\n");
    printf("  Antigravity, Aider, KiloCode, Kiro\n");
    printf("\nTools: index_repository, search_graph, query_graph, trace_path,\n");
    printf("  get_code_snippet, get_graph_schema, get_architecture, search_code,\n");
    printf("  list_projects, delete_project, index_status, detect_changes,\n");
    printf("  events, memories_retrieve, memory_update_status, memory_feedback, admin_consolidate, "
           "admin_decay, memory_health\n");
}

/* ── Main ───────────────────────────────────────────────────────── */

/* Try to handle a subcommand (cli/install/uninstall/update/config/--version/--help).
 * Returns -1 if no subcommand matched, otherwise the exit code. */
static int handle_subcommand(int argc, char **argv) {
    /* First scan: global flags */
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0) {
            cbm_profile_enable();
        }
    }
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("semantic-memory-mcp %s\n", CBM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "cli") == 0) {
            cbm_mem_init(MAIN_RAM_FRACTION);
            return run_cli(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "hook-augment") == 0) {
            cbm_mem_init(MAIN_RAM_FRACTION);
            return cbm_cmd_hook_augment();
        }
        if (strcmp(argv[i], "memory-recall") == 0) {
            cbm_mem_init(MAIN_RAM_FRACTION);
            return cbm_cmd_hook_recall();
        }
        if (strcmp(argv[i], "memory-post-tool") == 0) {
            cbm_mem_init(MAIN_RAM_FRACTION);
            return cbm_cmd_memory_post_tool();
        }
        if (strcmp(argv[i], "memory-stop") == 0) {
            cbm_mem_init(MAIN_RAM_FRACTION);
            return cbm_cmd_memory_stop();
        }
        if (strcmp(argv[i], "global-migrate") == 0) {
            cbm_mem_init(MAIN_RAM_FRACTION);
            return cbm_cmd_global_migrate(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "manager") == 0) {
            return run_manager();
        }
        if (strcmp(argv[i], "install") == 0) {
            return cbm_cmd_install(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "uninstall") == 0) {
            return cbm_cmd_uninstall(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "update") == 0) {
            return cbm_cmd_update(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "config") == 0) {
            return cbm_cmd_config(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
    }
    return CBM_NOT_FOUND;
}

/* Parse --ui= and --port= flags. Returns true if config was modified. */
static bool parse_ui_flags(int argc, char **argv, cbm_ui_config_t *cfg, bool *explicit_enable) {
    bool changed = false;
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strncmp(argv[i], "--ui=", SLEN("--ui=")) == 0) {
            cfg->ui_enabled = (strcmp(argv[i] + MAIN_FLAG_OFF, "true") == 0);
            if (explicit_enable && cfg->ui_enabled) {
                *explicit_enable = true;
            }
            changed = true;
        }
        if (strncmp(argv[i], "--port=", SLEN("--port=")) == 0) {
            int p = (int)strtol(argv[i] + MAIN_PORT_OFF, NULL, CBM_DECIMAL_BASE);
            if (p > 0 && p < MAIN_MAX_PORT) {
                cfg->ui_port = p;
                changed = true;
            }
        }
    }
    return changed;
}

/* Install platform-specific signal handlers. */
static void setup_signal_handlers(void) {
#ifdef _WIN32
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
#else
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
#endif
}

#ifdef _WIN32
static int cbm_main(int argc, char **argv, stage14_windows_argv_t *windows_arguments) {
#else
static int cbm_main(int argc, char **argv) {
#endif
    /* Keep the MCP initialize handshake on the same build-time version source
     * as --version and startup logging. Without this assignment cli.c retains
     * its test-friendly "dev" default in production serverInfo. */
    cbm_cli_set_version(CBM_VERSION);
#ifdef _WIN32
    bool stage14_launcher_handled = false;
    int stage14_launcher_result =
        stage14_stable_launcher(windows_arguments, &stage14_launcher_handled);
    if (stage14_launcher_handled || stage14_launcher_result != 0) {
        return stage14_launcher_result;
    }
#endif
    cbm_profile_init(); /* reads CBM_PROFILE env var, gates all prof macros */
    /* CBM_LOG_LEVEL support — distilled from #414 (closes #413). Apply before
     * the first log statement so the configured level governs all output. */
    cbm_log_init_from_env();
    int subcmd = handle_subcommand(argc, argv);
    if (subcmd >= 0) {
        return subcmd;
    }

    /* parent-death watchdog — distilled from #407 (fixes #406). Start it early so
     * an orphaned server exits even if it dies before reaching the MCP loop. A
     * thread-create failure (or ppid<=1) is non-fatal: the server still runs, it
     * just won't auto-exit on parent death — same policy as the watcher/HTTP
     * threads below. We deliberately do NOT exit at startup when ppid<=1 (the PR's
     * original behaviour): a legitimately-launched server can transiently show
     * ppid==1 (early reparent races, double-fork/container launchers), and the
     * watchdog already no-ops safely in that case via its initial_ppid>1 guard. */
#ifndef _WIN32
    /* main() outlives the watchdog (it joins before returning), so a stack
     * local is a valid lifetime for the thread's argument. */
    pid_t initial_ppid = getppid();
    cbm_thread_t parent_watchdog_tid;
    bool parent_watchdog_started = false;
    if (cbm_thread_create(&parent_watchdog_tid, PARENT_WATCHDOG_STACK_SIZE, parent_watchdog_thread,
                          &initial_ppid) == 0) {
        parent_watchdog_started = true;
    } else {
        cbm_log_warn("parent.watchdog.unavailable", "reason", "thread_create_failed");
    }
#endif

    /* Default: MCP server on stdio */
    cbm_mem_init(MAIN_RAM_FRACTION); /* 50% of RAM — safe now because mimalloc tracks ALL
                                      * memory (C + C++ allocations) via global override.
                                      * No more untracked heap blind spots. */
    /* Store binary path for subprocess spawning + hook log sink */
    cbm_http_server_set_binary_path(argv[0]);
    cbm_log_set_sink(cbm_ui_log_append);
    cbm_log_info("server.start", "version", CBM_VERSION);
    cbm_diag_start(); /* starts if CBM_DIAGNOSTICS=1 */

    /* Parse --ui and --port flags (persisted config) */
    cbm_ui_config_t ui_cfg;
    cbm_ui_config_load(&ui_cfg);
    bool explicit_ui_enable = false;
    if (parse_ui_flags(argc, argv, &ui_cfg, &explicit_ui_enable)) {
        cbm_ui_config_save(&ui_cfg);
    }
    /* If the user explicitly asked for the UI but this binary has no embedded
     * frontend, the HTTP server can never start (see below). The warning that
     * covers this goes to the log sink, which a user running `--ui=true` on a
     * terminal won't see — so tell them plainly on stderr why nothing happens
     * and which build to use (#350). */
    if (explicit_ui_enable && CBM_EMBEDDED_FILE_COUNT == 0) {
        (void)fprintf(stderr,
                      "semantic-memory-mcp: --ui requested, but this binary was built without the "
                      "embedded UI, so the HTTP server will not start.\n"
                      "Use the UI release asset (semantic-memory-mcp-ui) or rebuild with: "
                      "make -f Makefile.cbm cbm-with-ui\n");
    }

    setup_signal_handlers();

    /* Open config store for runtime settings */
    char config_dir[CBM_SZ_1K];
    const char *cfg_home = cbm_get_home_dir();
    cbm_config_t *runtime_config = NULL;
    if (cfg_home) {
        snprintf(config_dir, sizeof(config_dir), "%s", cbm_resolve_cache_dir());
        runtime_config = cbm_config_open(config_dir);
    }

    /* Create MCP server */
    g_server = cbm_mcp_server_new(NULL);
    if (!g_server) {
        cbm_log_error("server.err", "msg", "failed to create server");
        cbm_config_close(runtime_config);
#ifndef _WIN32
        if (parent_watchdog_started) {
            atomic_store(&g_shutdown, 1);
            cbm_thread_join(&parent_watchdog_tid);
        }
#endif
        return SKIP_ONE;
    }

    /* Create and start watcher in background thread */
    /* Initialize log mutex before any threads are created */
    cbm_ui_log_init();

    cbm_store_t *watch_store = cbm_store_open_memory();
    g_watcher = cbm_watcher_new(watch_store, watcher_index_fn, NULL);

    /* Wire watcher + config into MCP server for session auto-index */
    cbm_mcp_server_set_watcher(g_server, g_watcher);
    cbm_mcp_server_set_config(g_server, runtime_config);
    cbm_thread_t watcher_tid;
    bool watcher_started = false;

    if (g_watcher) {
        if (cbm_thread_create(&watcher_tid, 0, watcher_thread, g_watcher) == 0) {
            watcher_started = true;
        }
    }

    /* Optionally start HTTP UI server in background thread */
    cbm_thread_t http_tid;
    bool http_started = false;

    if (ui_cfg.ui_enabled && CBM_EMBEDDED_FILE_COUNT > 0) {
        g_http_server = cbm_http_server_new(ui_cfg.ui_port);
        if (g_http_server) {
            if (cbm_thread_create(&http_tid, 0, http_thread, g_http_server) == 0) {
                http_started = true;
            }
        }
    } else if (ui_cfg.ui_enabled && CBM_EMBEDDED_FILE_COUNT == 0) {
        cbm_log_warn("ui.no_assets", "hint", "rebuild with: make -f Makefile.cbm cbm-with-ui");
    }

    /* Run MCP event loop (blocks until EOF or signal) */
    int rc = cbm_mcp_server_run(g_server, stdin, stdout);
    atomic_store(&g_shutdown, 1); /* unblock the watchdog poll loop */

    /* Shutdown */
    cbm_log_info("server.shutdown");

#ifndef _WIN32
    if (parent_watchdog_started) {
        cbm_thread_join(&parent_watchdog_tid);
    }
#endif

    if (http_started) {
        cbm_http_server_stop(g_http_server);
        cbm_thread_join(&http_tid);
        cbm_http_server_free(g_http_server);
        g_http_server = NULL;
    }

    if (watcher_started) {
        cbm_watcher_stop(g_watcher);
        cbm_thread_join(&watcher_tid);
    }
    cbm_watcher_free(g_watcher);
    cbm_store_close(watch_store);
    cbm_mcp_server_free(g_server);
    cbm_config_close(runtime_config);

    g_watcher = NULL;
    g_server = NULL;
    cbm_diag_stop();

    return rc;
}

int main(int argc, char **argv) {
    /* Defense-in-depth: bind tree-sitter, sqlite3, and libgit2 to mimalloc so a
     * correct binary does not rely on the fragile MI_OVERRIDE symbol override
     * (#424). MUST be the VERY FIRST statement: SQLITE_CONFIG_MALLOC has to run
     * before the first sqlite3_open* (cbm_mcp_server_new → cbm_store_open_memory
     * below opens sqlite early), else sqlite3_config returns SQLITE_MISUSE and
     * the bind is silently ignored. No-op in the test build. */
    cbm_alloc_init();
#ifdef _WIN32
    (void)argc;
    (void)argv;
    stage14_windows_argv_t normalized_arguments;
    if (!stage14_normalize_windows_argv(&normalized_arguments)) {
        return stage14_integrity_failure();
    }
    int result =
        cbm_main(normalized_arguments.argc, normalized_arguments.utf8_argv, &normalized_arguments);
    bool wide_freed = true;
    if (normalized_arguments.wide_argv) {
        wide_freed = stage14_free_wide_argv(&normalized_arguments.wide_argv);
    }
    bool utf8_freed = stage14_free_utf8_argv(&normalized_arguments.utf8_argv);
    if (!wide_freed || !utf8_freed) {
        return stage14_integrity_failure();
    }
    return result;
#else
    return cbm_main(argc, argv);
#endif
}
