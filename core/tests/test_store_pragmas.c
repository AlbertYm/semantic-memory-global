/*
 * test_store_pragmas.c — Tests for SQLite pragma resolution.
 *
 * Validates that the CBM_SQLITE_MMAP_SIZE env var controls the mmap_size
 * pragma applied to on-disk stores. Default behavior (env unset) must
 * remain 64 MB. Setting the env to 0 disables memory-mapped I/O so
 * concurrent processes that truncate the DB file under a sibling's live
 * mapping return SQLITE_IOERR instead of crashing the process with SIGBUS.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <store/store.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clear_mmap_env(void) {
    cbm_unsetenv("CBM_SQLITE_MMAP_SIZE");
}

TEST(mmap_size_default_when_unset) {
    clear_mmap_env();
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    PASS();
}

TEST(mmap_size_zero_disables_mmap) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_explicit_value) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "1048576", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 1048576LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_negative_clamped_to_zero) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "-1", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "not-a-number", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_partial_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "123abc", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

/* Integration smoke: opening a file-backed store with mmap_size=0 must
 * succeed. Proves the resolver is wired through configure_pragmas(). */
TEST(store_open_with_mmap_disabled) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_pragmas_%d.db", cbm_tmpdir(), (int)getpid());
    unlink(tmp_path);

    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    cbm_store_close(s);

    unlink(tmp_path);
    /* WAL/SHM siblings created by the open */
    char tmp_wal[300];
    char tmp_shm[300];
    snprintf(tmp_wal, sizeof(tmp_wal), "%s-wal", tmp_path);
    snprintf(tmp_shm, sizeof(tmp_shm), "%s-shm", tmp_path);
    unlink(tmp_wal);
    unlink(tmp_shm);

    clear_mmap_env();
    PASS();
}

TEST(store_stage11_wal_and_busy_contract) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_stage11_pragmas_%d.db", cbm_tmpdir(),
             (int)getpid());
    unlink(tmp_path);
    cbm_store_t *first = cbm_store_open_path(tmp_path);
    cbm_store_t *second = cbm_store_open_path(tmp_path);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    sqlite3 *db = cbm_store_get_db(first);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "wal");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1); /* NORMAL */
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA busy_timeout", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 10000);
    sqlite3_finalize(stmt);

    ASSERT_EQ(cbm_store_exec(second, "PRAGMA busy_timeout=1"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_begin(first), CBM_STORE_OK);
    int blocked = cbm_store_begin(second);
    ASSERT_EQ(blocked, CBM_STORE_BUSY);
    ASSERT_STR_EQ(cbm_store_result_code_name(blocked), "DB_BUSY");
    ASSERT_EQ(cbm_store_rollback(first), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_begin(second), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_rollback(second), CBM_STORE_OK);
    ASSERT_STR_EQ(cbm_store_result_code_name(CBM_STORE_TIMEOUT), "DB_TIMEOUT");
    ASSERT_STR_EQ(cbm_store_result_code_name(CBM_STORE_RECOVERY_REQUIRED),
                  "DB_RECOVERY_REQUIRED");
    ASSERT_STR_EQ(cbm_store_result_code_name(CBM_STORE_BACKUP_VERIFY_FAILED),
                  "BACKUP_VERIFY_FAILED");

    cbm_store_close(second);
    cbm_store_close(first);
    unlink(tmp_path);
    char sibling[300];
    snprintf(sibling, sizeof(sibling), "%s-wal", tmp_path);
    unlink(sibling);
    snprintf(sibling, sizeof(sibling), "%s-shm", tmp_path);
    unlink(sibling);
    PASS();
}

SUITE(store_pragmas) {
    RUN_TEST(mmap_size_default_when_unset);
    RUN_TEST(mmap_size_zero_disables_mmap);
    RUN_TEST(mmap_size_explicit_value);
    RUN_TEST(mmap_size_negative_clamped_to_zero);
    RUN_TEST(mmap_size_garbage_falls_back_to_default);
    RUN_TEST(mmap_size_partial_garbage_falls_back_to_default);
    RUN_TEST(store_open_with_mmap_disabled);
    RUN_TEST(store_stage11_wal_and_busy_contract);
}
