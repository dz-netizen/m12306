/**
 * @file    debug_join.cc
 * @section DESCRIPTION
 * debug FROM + join execution (HashJoin / IndexNestedLoopJoin)
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "executor.h"

#define CHECK_TRUE(expr, msg) \
    do { \
        if (!(expr)) { \
            printf("[debug_join][ERROR] %s\n", msg); \
            return 1; \
        } \
    } while (0)

#define CHECK_ID(id, msg) \
    do { \
        if ((id) < 0) { \
            printf("[debug_join][ERROR] %s (id=%ld)\n", msg, (long) (id)); \
            return 1; \
        } \
    } while (0)

static void fill_reqcol(RequestColumn &rc, const char *name)
{
    std::memset(&rc, 0, sizeof(rc));
    std::strncpy(rc.name, name, sizeof(rc.name) - 1);
    rc.aggrerate_method = NONE_AM;
}

static void fill_reqtable(RequestTable &rt, const char *name)
{
    std::memset(&rt, 0, sizeof(rt));
    std::strncpy(rt.name, name, sizeof(rt.name) - 1);
}

static void fill_cond_col_col(Condition &c, const char *col1, CompareMethod cmp, const char *col2)
{
    std::memset(&c, 0, sizeof(c));
    fill_reqcol(c.column, col1);
    c.compare = cmp;
    std::strncpy(c.value, col2, sizeof(c.value) - 1);
}

static bool expect_int32(const char *buf, int32_t expected, const char *msg)
{
    int32_t got;
    std::memcpy(&got, buf, sizeof(got));
    if (got != expected) {
        printf("[debug_join][ERROR] %s: got=%d expected=%d\n", msg, got, expected);
        return false;
    }
    return true;
}

static int test_hash_join(void)
{
    printf("[debug_join] test_hash_join...\n");
    fflush(stdout);
    // db + two tables (no index on inner key)
    int64_t db_id = -1;
    printf("[debug_join] createDatabase...\n");
    fflush(stdout);
    CHECK_TRUE(g_catalog.createDatabase("db-join-1", db_id), "createDatabase db-join-1 failed");
    CHECK_ID(db_id, "db_id invalid");
    printf("[debug_join] createDatabase ok (db_id=%ld)\n", (long)db_id);
    fflush(stdout);
    Database *db = (Database *) g_catalog.getObjById(db_id);
    CHECK_TRUE(db != NULL, "getObjById(db_id) returned NULL");

    int64_t lt_id = -1;
    printf("[debug_join] createTable left...\n");
    fflush(stdout);
    CHECK_TRUE(g_catalog.createTable("tb-join-l1", ROWTABLE, lt_id), "createTable tb-join-l1 failed");
    CHECK_ID(lt_id, "lt_id invalid");
    printf("[debug_join] createTable left ok (lt_id=%ld)\n", (long)lt_id);
    fflush(stdout);
    Table *lt = (Table *) g_catalog.getObjById(lt_id);
    CHECK_TRUE(lt != NULL, "getObjById(lt_id) returned NULL");
    db->addTable(lt_id);

    int64_t rt_id = -1;
    printf("[debug_join] createTable right...\n");
    fflush(stdout);
    CHECK_TRUE(g_catalog.createTable("tb-join-r1", ROWTABLE, rt_id), "createTable tb-join-r1 failed");
    CHECK_ID(rt_id, "rt_id invalid");
    printf("[debug_join] createTable right ok (rt_id=%ld)\n", (long)rt_id);
    fflush(stdout);
    Table *rt = (Table *) g_catalog.getObjById(rt_id);
    CHECK_TRUE(rt != NULL, "getObjById(rt_id) returned NULL");
    db->addTable(rt_id);

    int64_t l_k = -1, l_v = -1, r_k = -1, r_w = -1;
    printf("[debug_join] createColumn...\n");
    fflush(stdout);
    CHECK_TRUE(g_catalog.createColumn("l1_k", INT32, 4, l_k), "createColumn l1_k failed");
    CHECK_TRUE(g_catalog.createColumn("l1_v", INT32, 4, l_v), "createColumn l1_v failed");
    CHECK_TRUE(g_catalog.createColumn("r1_k", INT32, 4, r_k), "createColumn r1_k failed");
    CHECK_TRUE(g_catalog.createColumn("r1_w", INT32, 4, r_w), "createColumn r1_w failed");
    CHECK_ID(l_k, "l_k invalid");
    CHECK_ID(l_v, "l_v invalid");
    CHECK_ID(r_k, "r_k invalid");
    CHECK_ID(r_w, "r_w invalid");

    lt->addColumn(l_k);
    lt->addColumn(l_v);
    rt->addColumn(r_k);
    rt->addColumn(r_w);

    printf("[debug_join] initDatabase...\n");
    fflush(stdout);
    CHECK_TRUE(g_catalog.initDatabase(db_id), "initDatabase db-join-1 failed");
    printf("[debug_join] initDatabase ok\n");
    fflush(stdout);

    printf("[debug_join] insert left rows...\n");
    fflush(stdout);

    // left rows: (1,100) (2,200) (2,201)
    int32_t lk1 = 1, lv1 = 100;
    char *lr1[2] = { (char *) &lk1, (char *) &lv1 };
    lt->insert(lr1);

    int32_t lk2 = 2, lv2 = 200;
    char *lr2[2] = { (char *) &lk2, (char *) &lv2 };
    lt->insert(lr2);

    int32_t lk3 = 2, lv3 = 201;
    char *lr3[2] = { (char *) &lk3, (char *) &lv3 };
    lt->insert(lr3);

    printf("[debug_join] insert right rows...\n");
    fflush(stdout);

    // right rows: (2,500) (3,600) (2,501)
    int32_t rk1 = 2, rw1 = 500;
    char *rr1[2] = { (char *) &rk1, (char *) &rw1 };
    rt->insert(rr1);

    int32_t rk2 = 3, rw2 = 600;
    char *rr2[2] = { (char *) &rk2, (char *) &rw2 };
    rt->insert(rr2);

    int32_t rk3 = 2, rw3 = 501;
    char *rr3[2] = { (char *) &rk3, (char *) &rw3 };
    rt->insert(rr3);

    printf("[debug_join] inserts ok\n");
    fflush(stdout);

    // SELECT l1_v, r1_w FROM lt, rt WHERE l1_k LINK r1_k
    SelectQuery q;
    std::memset(&q, 0, sizeof(q));
    q.database_id = db_id;

    q.select_number = 2;
    fill_reqcol(q.select_column[0], "l1_v");
    fill_reqcol(q.select_column[1], "r1_w");

    q.from_number = 2;
    fill_reqtable(q.from_table[0], "tb-join-l1");
    fill_reqtable(q.from_table[1], "tb-join-r1");

    q.where.condition_num = 1;
    fill_cond_col_col(q.where.condition[0], "l1_k", LINK, "r1_k");

    Executor ex;
    ResultTable res;
    printf("[debug_join] exec query (HashJoin path expected)...\n");
    fflush(stdout);
    int n = ex.exec(&q, &res);
    printf("[debug_join] exec returned n=%d row_number=%d\n", n, res.row_number);
        printf("[debug_join] res.buffer=%p buffer_size=%ld offset=%p offset_size=%d row_length=%d\n",
            (void *)res.buffer, (long)res.buffer_size, (void *)res.offset, res.offset_size, res.row_length);
    fflush(stdout);
    if (n != 4 || res.row_number != 4) {
        printf("[debug_join][ERROR] HashJoin expected 4 rows, got n=%d row_number=%d\n", n, res.row_number);
        ex.close();
        res.shut();
        return 1;
    }

    // Expected order (left scan order, right match order):
    // (200,500) (200,501) (201,500) (201,501)
    const int32_t ev[4] = { 200, 200, 201, 201 };
    const int32_t ew[4] = { 500, 501, 500, 501 };
    printf("[debug_join] validate hash join result...\n");
    fflush(stdout);
    for (int i = 0; i < 4; i++) {
        char *p0 = res.getRC(i, 0);
        char *p1 = res.getRC(i, 1);
        printf("[debug_join] row=%d p0=%p p1=%p\n", i, (void *)p0, (void *)p1);
        fflush(stdout);
        if (!expect_int32(p0, ev[i], "HashJoin l1_v")) return 1;
        if (!expect_int32(p1, ew[i], "HashJoin r1_w")) return 1;
    }

    ex.close();
    res.shut();
    printf("[debug_join] test_hash_join ok\n");
    return 0;
}

static int test_inlj(void)
{
    printf("[debug_join] test_inlj...\n");
    // db + two tables (hash index on inner key)
    int64_t db_id = -1;
    CHECK_TRUE(g_catalog.createDatabase("db-join-2", db_id), "createDatabase db-join-2 failed");
    CHECK_ID(db_id, "db_id invalid");
    Database *db = (Database *) g_catalog.getObjById(db_id);
    CHECK_TRUE(db != NULL, "getObjById(db_id) returned NULL");

    int64_t lt_id = -1;
    CHECK_TRUE(g_catalog.createTable("tb-join-l2", ROWTABLE, lt_id), "createTable tb-join-l2 failed");
    CHECK_ID(lt_id, "lt_id invalid");
    Table *lt = (Table *) g_catalog.getObjById(lt_id);
    CHECK_TRUE(lt != NULL, "getObjById(lt_id) returned NULL");
    db->addTable(lt_id);

    int64_t rt_id = -1;
    CHECK_TRUE(g_catalog.createTable("tb-join-r2", ROWTABLE, rt_id), "createTable tb-join-r2 failed");
    CHECK_ID(rt_id, "rt_id invalid");
    Table *rt = (Table *) g_catalog.getObjById(rt_id);
    CHECK_TRUE(rt != NULL, "getObjById(rt_id) returned NULL");
    db->addTable(rt_id);

    int64_t l_k = -1, l_v = -1, r_k = -1, r_w = -1;
    CHECK_TRUE(g_catalog.createColumn("l2_k", INT32, 4, l_k), "createColumn l2_k failed");
    CHECK_TRUE(g_catalog.createColumn("l2_v", INT32, 4, l_v), "createColumn l2_v failed");
    CHECK_TRUE(g_catalog.createColumn("r2_k", INT32, 4, r_k), "createColumn r2_k failed");
    CHECK_TRUE(g_catalog.createColumn("r2_w", INT32, 4, r_w), "createColumn r2_w failed");
    CHECK_ID(l_k, "l_k invalid");
    CHECK_ID(l_v, "l_v invalid");
    CHECK_ID(r_k, "r_k invalid");
    CHECK_ID(r_w, "r_w invalid");

    lt->addColumn(l_k);
    lt->addColumn(l_v);
    rt->addColumn(r_k);
    rt->addColumn(r_w);

    // create hash index on r2_k and attach to inner table
    std::vector<int64_t> keys;
    keys.push_back(r_k);
    Key k;
    k.set(keys);

    int64_t ix_id = -1;
    CHECK_TRUE(g_catalog.createIndex("ix-r2-k", HASHINDEX, k, ix_id), "createIndex ix-r2-k failed");
    CHECK_ID(ix_id, "ix_id invalid");
    rt->addIndex(ix_id);

    CHECK_TRUE(g_catalog.initDatabase(db_id), "initDatabase db-join-2 failed");

    HashIndex *hix = (HashIndex *) g_catalog.getObjById(ix_id);
    CHECK_TRUE(hix != NULL, "getObjById(ix_id) returned NULL");

    // left rows: (1,100) (2,200) (2,201)
    int32_t lk1 = 1, lv1 = 100;
    char *lr1[2] = { (char *) &lk1, (char *) &lv1 };
    lt->insert(lr1);

    int32_t lk2 = 2, lv2 = 200;
    char *lr2[2] = { (char *) &lk2, (char *) &lv2 };
    lt->insert(lr2);

    int32_t lk3 = 2, lv3 = 201;
    char *lr3[2] = { (char *) &lk3, (char *) &lv3 };
    lt->insert(lr3);

    // right rows: (2,500) (3,600) (2,501)
    int32_t rk1 = 2, rw1 = 500;
    char *rr1[2] = { (char *) &rk1, (char *) &rw1 };
    rt->insert(rr1);
    {
        void *rec = ((RowTable *) rt)->getRecordPtr(((RowTable *) rt)->getRecordNum() - 1);
        CHECK_TRUE(rec != NULL, "inner record ptr is NULL after insert (rk1) ");
        CHECK_TRUE(hix->insert((void *) &rk1, rec), "hash index insert failed (rk1)");
    }

    int32_t rk2 = 3, rw2 = 600;
    char *rr2[2] = { (char *) &rk2, (char *) &rw2 };
    rt->insert(rr2);
    {
        void *rec = ((RowTable *) rt)->getRecordPtr(((RowTable *) rt)->getRecordNum() - 1);
        CHECK_TRUE(rec != NULL, "inner record ptr is NULL after insert (rk2) ");
        CHECK_TRUE(hix->insert((void *) &rk2, rec), "hash index insert failed (rk2)");
    }

    int32_t rk3 = 2, rw3 = 501;
    char *rr3[2] = { (char *) &rk3, (char *) &rw3 };
    rt->insert(rr3);
    {
        void *rec = ((RowTable *) rt)->getRecordPtr(((RowTable *) rt)->getRecordNum() - 1);
        CHECK_TRUE(rec != NULL, "inner record ptr is NULL after insert (rk3) ");
        CHECK_TRUE(hix->insert((void *) &rk3, rec), "hash index insert failed (rk3)");
    }

    // SELECT l2_v, r2_w FROM lt, rt WHERE l2_k LINK r2_k
    SelectQuery q;
    std::memset(&q, 0, sizeof(q));
    q.database_id = db_id;

    q.select_number = 2;
    fill_reqcol(q.select_column[0], "l2_v");
    fill_reqcol(q.select_column[1], "r2_w");

    q.from_number = 2;
    fill_reqtable(q.from_table[0], "tb-join-l2");
    fill_reqtable(q.from_table[1], "tb-join-r2");

    q.where.condition_num = 1;
    fill_cond_col_col(q.where.condition[0], "l2_k", LINK, "r2_k");

    Executor ex;
    ResultTable res;
    int n = ex.exec(&q, &res);
    if (n != 4 || res.row_number != 4) {
        printf("[debug_join][ERROR] INLJ expected 4 rows, got n=%d row_number=%d\n", n, res.row_number);
        ex.close();
        res.shut();
        return 1;
    }

    const int32_t ev[4] = { 200, 200, 201, 201 };
    const int32_t ew[4] = { 500, 501, 500, 501 };
    for (int i = 0; i < 4; i++) {
        if (!expect_int32(res.getRC(i, 0), ev[i], "INLJ l2_v")) return 1;
        if (!expect_int32(res.getRC(i, 1), ew[i], "INLJ r2_w")) return 1;
    }

    ex.close();
    res.shut();
    printf("[debug_join] test_inlj ok\n");
    return 0;
}

int main()
{
    printf("------join operator test------\n");

    printf("[debug_join] init memory...\n");
    fflush(stdout);
    int mrc = g_memory.init(1 << 30, 1 << 3);
    printf("[debug_join] g_memory.init rc=%d\n", mrc);
    fflush(stdout);

    printf("[debug_join] init catalog...\n");
    fflush(stdout);
    g_catalog.init();
    printf("[debug_join] init done\n");
    fflush(stdout);

    int rc = 0;
    rc |= test_hash_join();
    rc |= test_inlj();

    g_catalog.shut();
    g_memory.shut();

    if (rc == 0) {
        printf("test pass!(checked by author)\n");
    }
    return rc;
}
