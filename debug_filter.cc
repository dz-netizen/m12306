/**
 * @file    debug_filter.cc
 * @section DESCRIPTION
 * debug FilterOperator (WHERE, AND only, up to 4 conditions)
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

static void fill_reqcol(RequestColumn &rc, const char *name)
{
    std::memset(&rc, 0, sizeof(rc));
    std::strncpy(rc.name, name, sizeof(rc.name) - 1);
    rc.aggrerate_method = NONE_AM;
}

static void fill_cond_col_const(Condition &c, const char *col, CompareMethod cmp, const char *val)
{
    std::memset(&c, 0, sizeof(c));
    fill_reqcol(c.column, col);
    c.compare = cmp;
    std::strncpy(c.value, val, sizeof(c.value) - 1);
}

static void fill_cond_col_col(Condition &c, const char *col1, CompareMethod cmp, const char *col2)
{
    std::memset(&c, 0, sizeof(c));
    fill_reqcol(c.column, col1);
    c.compare = cmp;
    std::strncpy(c.value, col2, sizeof(c.value) - 1);
}

static bool expect_int32(const char *buf, int32_t expected)
{
    int32_t got;
    std::memcpy(&got, buf, sizeof(got));
    if (got != expected) {
        printf("[debug_filter][ERROR] INT32 mismatch: got=%d expected=%d\n", got, expected);
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
        printf("[debug_filter][ERROR] CHARN mismatch: got='%s' expected='%s'\n", tmp, expected);
        return false;
    }
    return true;
}

static int test_const_and(void)
{
    // Table: (INT32 a, FLOAT32 b, CHARN(8) c, INT32 d)
    int64_t db_id = -1;
    g_catalog.createDatabase("db-filter-1", db_id);
    Database *db = (Database *) g_catalog.getObjById(db_id);

    int64_t table_id = -1;
    g_catalog.createTable("tb-filter-1", ROWTABLE, table_id);
    Table *tb = (Table *) g_catalog.getObjById(table_id);
    db->addTable(table_id);

    int64_t c_a = -1, c_b = -1, c_c = -1, c_d = -1;
    g_catalog.createColumn("a", INT32, 4, c_a);
    g_catalog.createColumn("b", FLOAT32, 4, c_b);
    g_catalog.createColumn("c", CHARN, 8, c_c);
    g_catalog.createColumn("d", INT32, 4, c_d);
    tb->addColumn(c_a);
    tb->addColumn(c_b);
    tb->addColumn(c_c);
    tb->addColumn(c_d);

    g_catalog.print();
    g_catalog.initDatabase(db_id);

    // Insert rows
    int32_t a1 = 10, d1 = 11;
    float b1 = 1.25f;
    char c1[9];
    copy_cstr(c1, sizeof(c1), "row0001");
    char *row1[4] = { (char *) &a1, (char *) &b1, c1, (char *) &d1 };
    tb->insert(row1);

    int32_t a2 = 20, d2 = 19;
    float b2 = 2.5f;
    char c2[9];
    copy_cstr(c2, sizeof(c2), "row0002");
    char *row2[4] = { (char *) &a2, (char *) &b2, c2, (char *) &d2 };
    tb->insert(row2);

    int32_t a3 = 30, d3 = 31;
    float b3 = 3.75f;
    char c3[9];
    copy_cstr(c3, sizeof(c3), "row0003");
    char *row3[4] = { (char *) &a3, (char *) &b3, c3, (char *) &d3 };
    tb->insert(row3);

    // WHERE a >= 20 AND c == "row0003"
    Conditions conds;
    std::memset(&conds, 0, sizeof(conds));
    conds.condition_num = 2;
    fill_cond_col_const(conds.condition[0], "a", GE, "20");
    fill_cond_col_const(conds.condition[1], "c", EQ, "row0003");

    // projection: a, c
    RequestColumn proj[2];
    fill_reqcol(proj[0], "a");
    fill_reqcol(proj[1], "c");

    TableScanOperator scan(tb);
    FilterProjectOperator filter(&scan, tb, conds, proj, 2);

    if (!filter.init()) {
        printf("[debug_filter][ERROR] filter.init failed\n");
        return 1;
    }

    int seen = 0;
    while (filter.getNext()) {
        const char *out = filter.getOutput();
        if (out == NULL) {
            printf("[debug_filter][ERROR] output is NULL\n");
            return 1;
        }
        // projected tuple: [a:int32][c:8]
        if (!expect_int32(out + 0, 30)) return 1;
        if (!expect_charn(out + 4, 8, "row0003")) return 1;
        seen++;
    }
    if (seen != 1) {
        printf("[debug_filter][ERROR] expected 1 tuple, got=%d\n", seen);
        return 1;
    }
    filter.close();
    return 0;
}

static int test_col_col_and_link(void)
{
    // reuse a new small table: (INT32 a, INT32 d)
    int64_t db_id = -1;
    g_catalog.createDatabase("db-filter-2", db_id);
    Database *db = (Database *) g_catalog.getObjById(db_id);

    int64_t table_id = -1;
    g_catalog.createTable("tb-filter-2", ROWTABLE, table_id);
    Table *tb = (Table *) g_catalog.getObjById(table_id);
    db->addTable(table_id);

    // Column names in this system are globally unique in catalog; use unique names per test.
    int64_t c_a = -1, c_d = -1;
    g_catalog.createColumn("a2", INT32, 4, c_a);
    g_catalog.createColumn("d2", INT32, 4, c_d);
    tb->addColumn(c_a);
    tb->addColumn(c_d);

    g_catalog.print();
    g_catalog.initDatabase(db_id);

    // rows: (10,10) (20,21) (30,29)
    int32_t a1 = 10, d1 = 10;
    char *r1[2] = { (char *) &a1, (char *) &d1 };
    tb->insert(r1);

    int32_t a2 = 20, d2 = 21;
    char *r2[2] = { (char *) &a2, (char *) &d2 };
    tb->insert(r2);

    int32_t a3 = 30, d3 = 29;
    char *r3[2] = { (char *) &a3, (char *) &d3 };
    tb->insert(r3);

    // WHERE a2 < d2 AND a2 >= 20
    Conditions conds;
    std::memset(&conds, 0, sizeof(conds));
    conds.condition_num = 2;
    fill_cond_col_col(conds.condition[0], "a2", LT, "d2");
    fill_cond_col_const(conds.condition[1], "a2", GE, "20");

    // projection (reordered): d2, a2
    RequestColumn proj[2];
    fill_reqcol(proj[0], "d2");
    fill_reqcol(proj[1], "a2");

    TableScanOperator scan(tb);
    FilterProjectOperator filter(&scan, tb, conds, proj, 2);
    if (!filter.init()) return 1;

    int seen = 0;
    while (filter.getNext()) {
        const char *out = filter.getOutput();
        // projected tuple: [d2:int32][a2:int32]
        if (!expect_int32(out + 0, 21)) return 1;
        if (!expect_int32(out + 4, 20)) return 1;
        seen++;
    }
    if (seen != 1) {
        printf("[debug_filter][ERROR] expected 1 tuple, got=%d\n", seen);
        return 1;
    }
    filter.close();

    // WHERE a2 LINK d2 (normalized to a2 == d2)
    Conditions conds2;
    std::memset(&conds2, 0, sizeof(conds2));
    conds2.condition_num = 1;
    fill_cond_col_col(conds2.condition[0], "a2", LINK, "d2");

    TableScanOperator scan2(tb);
    FilterProjectOperator filter2(&scan2, tb, conds2, proj, 2);
    if (!filter2.init()) return 1;

    seen = 0;
    while (filter2.getNext()) {
        const char *out = filter2.getOutput();
        // projected tuple: [d2:int32][a2:int32]
        if (!expect_int32(out + 0, 10)) return 1;
        if (!expect_int32(out + 4, 10)) return 1;
        seen++;
    }
    if (seen != 1) {
        printf("[debug_filter][ERROR] expected 1 tuple (LINK), got=%d\n", seen);
        return 1;
    }
    filter2.close();

    return 0;
}

int main()
{
    printf("------filter operator test------\n");

    g_memory.init(1 << 30, 1 << 3);
    g_catalog.init();

    int rc = 0;
    rc |= test_const_and();
    rc |= test_col_col_and_link();

    g_catalog.shut();
    g_memory.shut();

    if (rc == 0) {
        printf("test pass!(checked by author)\n");
    }
    return rc;
}
