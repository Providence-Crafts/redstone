/*
 * Tests for the db module.
 *
 * These run against an in-memory database built here, so they do not depend on
 * tests/test.db. The fixture database is for completion tests (Phase 3) and
 * for manual exercise.
 */
#include "db.h"
#include "minunit.h"
#include "suites.h"

#include <stdlib.h>
#include <string.h>

/* Capture db_exec output into a buffer by way of a temporary file. Simpler and
 * more portable under C99 than open_memstream. */
static char *exec_capture(Db *db, const char *sql, bool *ok)
{
    FILE *tmp = tmpfile();
    char *buf;
    long size;

    if (tmp == NULL) {
        return NULL;
    }
    *ok = db_exec(db, sql, tmp, tmp);
    fflush(tmp);

    if (fseek(tmp, 0L, SEEK_END) != 0) {
        fclose(tmp);
        return NULL;
    }
    size = ftell(tmp);
    if (size < 0) {
        fclose(tmp);
        return NULL;
    }
    if (fseek(tmp, 0L, SEEK_SET) != 0) {
        fclose(tmp);
        return NULL;
    }

    buf = malloc((size_t)size + 1u);
    if (buf != NULL) {
        size_t got = fread(buf, 1u, (size_t)size, tmp);
        buf[got] = '\0';
    }
    fclose(tmp);
    return buf;
}

static const char *test_open_close(void)
{
    Db *db = db_open(NULL, stderr);

    mu_assert("in-memory open failed", db != NULL);
    mu_assert("path should be :memory:", strcmp(db_path(db), ":memory:") == 0);
    /* db.c no longer owns a mode of its own: a freshly opened Db formats
     * through db_out(), which out_new() starts in "box" with headers on. */
    mu_assert("default mode should be box", strcmp(out_mode_name(db_out(db)), "box") == 0);
    mu_assert("headers should default on", out_headers(db_out(db)));
    db_close(db);

    db_close(NULL); /* must not crash */
    return NULL;
}

static const char *test_is_complete(void)
{
    mu_assert("terminated statement is complete", db_is_complete("SELECT 1;"));
    mu_assert("unterminated statement is incomplete", !db_is_complete("SELECT 1"));
    mu_assert("open string is incomplete", !db_is_complete("SELECT 'abc;"));
    mu_assert("semicolon inside a string does not terminate", !db_is_complete("SELECT 'a;b'"));
    mu_assert("closed string statement is complete", db_is_complete("SELECT 'a;b';"));
    mu_assert("empty input is not complete", !db_is_complete(""));
    return NULL;
}

static const char *test_exec_and_modes(void)
{
    Db *db = db_open(NULL, stderr);
    char *out;
    bool ok = false;

    mu_assert("open failed", db != NULL);

    out = exec_capture(db, "CREATE TABLE t (a INTEGER, b TEXT);", &ok);
    mu_assert("CREATE should succeed", ok);
    free(out);

    out = exec_capture(db, "INSERT INTO t VALUES (1,'x'),(2,NULL);", &ok);
    mu_assert("INSERT should succeed", ok);
    free(out);

    out_set_mode(db_out(db), "list");
    out_set_headers(db_out(db), false);
    out = exec_capture(db, "SELECT a, b FROM t ORDER BY a;", &ok);
    mu_assert("SELECT should succeed", ok);
    mu_assert("list output wrong", out != NULL && strcmp(out, "1|x\n2|\n") == 0);
    free(out);

    out_set_null_text(db_out(db), "NULL");
    out = exec_capture(db, "SELECT a, b FROM t ORDER BY a;", &ok);
    mu_assert("null text not applied", out != NULL && strcmp(out, "1|x\n2|NULL\n") == 0);
    free(out);

    out_set_colsep(db_out(db), ",");
    out = exec_capture(db, "SELECT a, b FROM t WHERE a = 1;", &ok);
    mu_assert("separator not applied", out != NULL && strcmp(out, "1,x\n") == 0);
    free(out);

    db_close(db);
    return NULL;
}

static const char *test_csv_quoting(void)
{
    Db *db = db_open(NULL, stderr);
    char *out;
    bool ok = false;

    mu_assert("open failed", db != NULL);
    out_set_mode(db_out(db), "csv");
    out_set_headers(db_out(db), false);

    out = exec_capture(db, "SELECT 'a,b', 'say \"hi\"', 'plain';", &ok);
    mu_assert("csv select failed", ok);
    /* csv's row separator is CRLF (out.c's preset table), unlike list's bare
     * "\n", so the expected bytes carry the \r too. */
    mu_assert("csv quoting wrong",
              out != NULL && strcmp(out, "\"a,b\",\"say \"\"hi\"\"\",plain\r\n") == 0);
    free(out);

    db_close(db);
    return NULL;
}

static const char *test_error_reporting(void)
{
    Db *db = db_open(NULL, stderr);
    char *out;
    bool ok = true;

    mu_assert("open failed", db != NULL);

    out = exec_capture(db, "SELECT * FROM no_such_table;", &ok);
    mu_assert("bad SQL should fail", !ok);
    mu_assert("error text should be reported", out != NULL && strstr(out, "no_such_table") != NULL);
    free(out);

    db_close(db);
    return NULL;
}

static const char *test_multi_statement(void)
{
    Db *db = db_open(NULL, stderr);
    char *out;
    bool ok = false;

    mu_assert("open failed", db != NULL);
    out_set_mode(db_out(db), "list");
    out_set_headers(db_out(db), false);

    out = exec_capture(db, "CREATE TABLE m (x); INSERT INTO m VALUES (7); SELECT x FROM m;", &ok);
    mu_assert("multi-statement exec failed", ok);
    mu_assert("multi-statement output wrong", out != NULL && strcmp(out, "7\n") == 0);
    free(out);

    db_close(db);
    return NULL;
}

/* A write failure must be reported, not silently swallowed. Individual fputc
 * returns are unchecked by design, so this exercises the fflush/ferror check
 * in db_exec. /dev/full accepts opens and fails every write with ENOSPC.
 *
 * Every resource is released before the assertion: mu_assert returns early, so
 * nothing may be held live across one. */
static const char *test_write_failure(void)
{
    Db *db = db_open(NULL, stderr);
    FILE *full = fopen("/dev/full", "w");
    FILE *sink = tmpfile();
    bool have_full = db != NULL && full != NULL && sink != NULL;
    bool ok = true;

    if (have_full) {
        out_set_mode(db_out(db), "list");
        ok = db_exec(db, "SELECT 'a long enough row to force a flush';", full, sink);
    }

    if (full != NULL) {
        /* fclose flushes, and on /dev/full that flush fails; the return is
         * deliberately ignored because the failure is the point of the test. */
        (void)fclose(full);
    }
    if (sink != NULL) {
        (void)fclose(sink);
    }
    db_close(db);

    mu_assert("open failed", db != NULL);
    /* No /dev/full on this platform: nothing further to assert. */
    if (!have_full) {
        return NULL;
    }
    mu_assert("write failure should be reported", !ok);
    return NULL;
}

const char *db_suite(void)
{
    mu_run_test(test_open_close);
    mu_run_test(test_is_complete);
    mu_run_test(test_exec_and_modes);
    mu_run_test(test_csv_quoting);
    mu_run_test(test_error_reporting);
    mu_run_test(test_multi_statement);
    mu_run_test(test_write_failure);
    return NULL;
}
