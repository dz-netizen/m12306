/**
 * @file    debug_tablescan.cc
 * @section DESCRIPTION
 * debug TableScanOperator
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "executor.h"

static void copy_cstr(char *dest, int64_t cap, const char *src)
{
    if (cap <= 0) return;
    std::memset(dest, 0, static_cast<size_t>(cap));
    std::strncpy(dest, src, static_cast<size_t>(cap - 1));
}

static bool expect_int32(const char *buf, int32_t expected)
{
    int32_t got;
    std::memcpy(&got, buf, sizeof(got));
    if (got != expected) {
        printf("[debug_tablescan][ERROR] INT32 mismatch: got=%d expected=%d\n", got, expected);
        return false;
    }
    return true;
}

static bool expect_float32(const char *buf, float expected)
{
    float got;
    std::memcpy(&got, buf, sizeof(got));
    // keep it simple: exact match for values we inserted
    if (got != expected) {
        printf("[debug_tablescan][ERROR] FLOAT32 mismatch: got=%f expected=%f\n", got, expected);
        return false;
    }
    return true;
}

static bool expect_charn(const char *buf, int64_t n, const char *expected)
{
    if (std::strncmp(buf, expected, static_cast<size_t>(n)) != 0) {
        char tmp[128];
        std::memset(tmp, 0, sizeof(tmp));
        std::memcpy(tmp, buf, (n < 120 ? static_cast<size_t>(n) : 120));
        printf("[debug_tablescan][ERROR] CHARN mismatch: got='%s' expected='%s'\n", tmp, expected);
        return false;
    }
    return true;
}

static int test1(void)
{
    // Build a simple table: (INT32 a, FLOAT32 b, CHARN(8) c)
    int64_t db_id = -1;
    g_catalog.createDatabase("db-ts", db_id);
    Database *db = (Database *) g_catalog.getObjById(db_id);

    int64_t table_id = -1;
    g_catalog.createTable("tb-ts", ROWTABLE, table_id);
    Table *tb = (Table *) g_catalog.getObjById(table_id);
    db->addTable(table_id);

    int64_t c_a = -1;
    int64_t c_b = -1;
    int64_t c_c = -1;
    g_catalog.createColumn("a", INT32, 4, c_a);
    g_catalog.createColumn("b", FLOAT32, 4, c_b);
    g_catalog.createColumn("c", CHARN, 8, c_c);

    tb->addColumn(c_a);
    tb->addColumn(c_b);
    tb->addColumn(c_c);

    // Match debug_catalog.cc: print first, then initDatabase
    g_catalog.print();
    g_catalog.initDatabase(db_id);

    // Insert 3 rows
    int32_t a1 = 10;
    float b1 = 1.25f;
    char c1[8 + 1];
    copy_cstr(c1, sizeof(c1), "row0001");
    char *row1[3] = { (char *) &a1, (char *) &b1, c1 };
    if (!tb->insert(row1)) {
        printf("[debug_tablescan][ERROR] insert row1 failed\n");
        return 1;
    }

    int32_t a2 = 20;
    float b2 = 2.5f;
    char c2[8 + 1];
    copy_cstr(c2, sizeof(c2), "row0002");
    char *row2[3] = { (char *) &a2, (char *) &b2, c2 };
    if (!tb->insert(row2)) {
        printf("[debug_tablescan][ERROR] insert row2 failed\n");
        return 1;
    }

    int32_t a3 = 30;
    float b3 = 3.75f;
    char c3[8 + 1];
    copy_cstr(c3, sizeof(c3), "row0003");
    char *row3[3] = { (char *) &a3, (char *) &b3, c3 };
    if (!tb->insert(row3)) {
        printf("[debug_tablescan][ERROR] insert row3 failed\n");
        return 1;
    }

    // Delete middle row (rank 1), so scan should return only row1 and row3
    if (!tb->del(1L)) {
        printf("[debug_tablescan][ERROR] del rank 1 failed\n");
        return 1;
    }

    TableScanOperator scan(tb);
    if (!scan.init()) {
        printf("[debug_tablescan][ERROR] scan.init failed\n");
        return 1;
    }

    const int64_t expected_len = sizeof(int32_t) + sizeof(float) + 8;
    if (scan.getOutputLen() != expected_len) {
        printf("[debug_tablescan][ERROR] output length mismatch: got=%ld expected=%ld\n",
               (long) scan.getOutputLen(), (long) expected_len);
        return 1;
    }

    int seen = 0;
    while (scan.getNext()) {
        const char *out = scan.getOutput();
        if (out == NULL) {
            printf("[debug_tablescan][ERROR] output is NULL\n");
            return 1;
        }

        // decode packed tuple: [int32][float][charn(8)]
        if (seen == 0) {
            if (!expect_int32(out + 0, 10)) return 1;
            if (!expect_float32(out + 4, 1.25f)) return 1;
            if (!expect_charn(out + 8, 8, "row0001")) return 1;
        } else if (seen == 1) {
            if (!expect_int32(out + 0, 30)) return 1;
            if (!expect_float32(out + 4, 3.75f)) return 1;
            if (!expect_charn(out + 8, 8, "row0003")) return 1;
        } else {
            printf("[debug_tablescan][ERROR] too many tuples from scan\n");
            return 1;
        }

        seen++;
    }

    if (seen != 2) {
        printf("[debug_tablescan][ERROR] tuple count mismatch: got=%d expected=2\n", seen);
        return 1;
    }

    scan.close();
    return 0;
}

int main()
{
    printf("------tablescan operator test------\n");

    g_memory.init(1 << 30, 1 << 3);
    g_catalog.init();

    int rc = test1();

    g_catalog.shut();
    g_memory.shut();

    if (rc == 0) {
        printf("test pass!(checked by author)\n");
    }
    return rc;
}
