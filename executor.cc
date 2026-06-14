/**
 * @file    executor.cc
 * @author  liugang(liugang@ict.ac.cn)
 * @version 0.1
 *
 * @section DESCRIPTION
 *
 * definition of executor
 *
 */
#include "executor.h"

#include "datatype.h"
#include <algorithm>

static int64_t roundUpPow2(int64_t x) {
    if (x <= 8)
        return 8;
    int64_t v = 8;
    while (v < x)
        v = v << 1;
    return v;
}

static CompareMethod normalizeCompare(CompareMethod cm) {
    // LINK is used by legacy code to represent column-column equality.
    // For filtering, we normalize it to EQ.
    if (cm == LINK)
        return EQ;
    return cm;
}

static bool isQualifiedSuffixMatch(const char *name, const char *candidate) {
    if (name == NULL || candidate == NULL)
        return false;
    if (strcmp(name, candidate) == 0)
        return true;
    const char *dot = strrchr(name, '.');
    if (dot != NULL && dot[1] != '\0') {
        return strcmp(dot + 1, candidate) == 0;
    }
    return false;
}

static bool schemaColMatch(const RequestColumn &rc, const SchemaCol &sc) {
    if (!isQualifiedSuffixMatch(rc.name, sc.name))
        return false;
    // Require aggregate method match to disambiguate duplicates (e.g., SUM(col) vs MAX(col)).
    return sc.aggr == rc.aggregate_method;
}

static int findSchemaIndexByReq(const std::vector<SchemaCol> &schema, const RequestColumn &rc) {
    for (size_t i = 0; i < schema.size(); i++) {
        if (schemaColMatch(rc, schema[i]))
            return (int)i;
    }
    // fallback: allow matching by name only when request has NONE_AM and schema has NONE_AM
    if (rc.aggregate_method == NONE_AM) {
        for (size_t i = 0; i < schema.size(); i++) {
            if (schema[i].aggr != NONE_AM)
                continue;
            if (isQualifiedSuffixMatch(rc.name, schema[i].name))
                return (int)i;
        }
    }
    return -1;
}

static BasicType *aggOutType(AggregateMethod m, BasicType *in) {
    static TypeInt64 s_int64;
    static TypeFloat64 s_f64;
    if (m == COUNT)
        return &s_int64;
    if (m == AVG)
        return &s_f64;
    if (in == NULL)
        return NULL;
    TypeCode tc = in->getTypeCode();
    if (m == SUM) {
        if (tc == FLOAT32_TC || tc == FLOAT64_TC)
            return &s_f64;
        // treat all integer/date/time types as int64 for SUM
        return &s_int64;
    }
    if (m == MAX || m == MIN)
        return in;
    return in;
}

static bool typeIsFloat(BasicType *t) {
    if (t == NULL)
        return false;
    TypeCode tc = t->getTypeCode();
    return (tc == FLOAT32_TC || tc == FLOAT64_TC);
}

static int64_t readAsInt64(BasicType *t, const char *ptr) {
    if (t == NULL || ptr == NULL)
        return 0;
    switch (t->getTypeCode()) {
    case INT8_TC: {
        int8_t v;
        memcpy(&v, ptr, sizeof(v));
        return (int64_t)v;
    }
    case INT16_TC: {
        int16_t v;
        memcpy(&v, ptr, sizeof(v));
        return (int64_t)v;
    }
    case INT32_TC: {
        int32_t v;
        memcpy(&v, ptr, sizeof(v));
        return (int64_t)v;
    }
    case INT64_TC: {
        int64_t v;
        memcpy(&v, ptr, sizeof(v));
        return (int64_t)v;
    }
    case DATE_TC:
    case TIME_TC:
    case DATETIME_TC: {
        int64_t v;
        memcpy(&v, ptr, sizeof(v));
        return (int64_t)v;
    }
    default:
        return 0;
    }
}

static double readAsDouble(BasicType *t, const char *ptr) {
    if (t == NULL || ptr == NULL)
        return 0.0;
    switch (t->getTypeCode()) {
    case FLOAT32_TC: {
        float v;
        memcpy(&v, ptr, sizeof(v));
        return (double)v;
    }
    case FLOAT64_TC: {
        double v;
        memcpy(&v, ptr, sizeof(v));
        return (double)v;
    }
    default:
        return (double)readAsInt64(t, ptr);
    }
}

//============================================================ BEGIN OWNED BY A (scan operators) ============================================================

//----------------------------------------------------------------------
// TableScanOperator
//----------------------------------------------------------------------

TableScanOperator::TableScanOperator(Table *table) {
    ts_table = table;  // scan 对应的表
    ts_next_rank = 0;  // scan下一行的rank，初始为0
    ts_total_rank = 0; // scan总行数，初始为0
    ts_out = NULL;     // scan输出缓冲区，初始为NULL
    ts_out_cap = 0;    // scan输出缓冲区容量，初始为0
    ts_out_len = 0;    // scan输出长度，初始为0
    ts_end = true;     // scan结束标志，初始为true
}

TableScanOperator::~TableScanOperator() {
    close();
}

//============================================================================
// TableScanOperator初始化函数，负责为TableScanOperator分配输出缓冲区，并计算输出长度（即每行数据的字节数）。
//============================================================================
bool TableScanOperator::init() {
    if (ts_table == NULL) {
        printf("[TableScanOperator][ERROR][init]: table is NULL\n");
        return false;
    }

    ts_total_rank = ts_table->getRecordNum();
    ts_next_rank = 0;
    ts_end = false;

    //---------------------- 计算输出长度和分配输出缓冲区 ----------------------
    ts_out_len = 0;
    std::vector<int64_t> &cols = ts_table->getColumns();
    for (unsigned int ii = 0; ii < cols.size(); ii++) {
        Column *col = (Column *)g_catalog.getObjById(cols[ii]);
        if (col == NULL || col->getOtype() != COLUMN) {
            printf("[TableScanOperator][ERROR][init]: column type error\n");
            return false;
        }
        BasicType *dt = col->getDataType();
        if (dt == NULL) {
            printf("[TableScanOperator][ERROR][init]: column datatype not initialized\n");
            return false;
        }
        ts_out_len += dt->getTypeSize();
    }

    int64_t new_cap = roundUpPow2(ts_out_len);
    if (ts_out != NULL && ts_out_cap > 0) {
        g_memory.free(ts_out, ts_out_cap);
        ts_out = NULL;
    }
    ts_out_cap = new_cap;
    int64_t got = g_memory.alloc(ts_out, ts_out_cap);
    if (got != ts_out_cap) {
        printf("[TableScanOperator][ERROR][init]: alloc output buffer error\n");
        ts_out = NULL;
        ts_out_cap = 0;
        return false;
    }
    memset(ts_out, 0, (size_t)ts_out_cap);
    return true;
}

//============================================================================
// TableScanOperator getNext函数，负责从表中获取下一行数据，并将其存储在输出缓冲区中。
//============================================================================
bool TableScanOperator::getNext() {
    if (ts_end)
        return false;
    if (ts_table == NULL || ts_out == NULL) {
        printf("[TableScanOperator][ERROR][getNext]: not initialized\n");
        ts_end = true;
        return false;
    }

    while (ts_next_rank < ts_total_rank) {
        memset(ts_out, 0, (size_t)ts_out_cap);
        bool ok = ts_table->select(ts_next_rank, ts_out); // 根据rank获取一行数据，存到ts_out里
        ts_next_rank++;
        if (ok)
            return true;
    }
    ts_end = true;
    return false;
}

bool TableScanOperator::isEnd() {
    return ts_end;
}

char *TableScanOperator::getOutput() {
    return ts_out;
}

int64_t TableScanOperator::getOutputLen() {
    return ts_out_len;
}

bool TableScanOperator::close() {
    if (ts_out != NULL && ts_out_cap > 0) {
        g_memory.free(ts_out, ts_out_cap);
    }
    ts_out = NULL;
    ts_out_cap = 0;
    ts_out_len = 0;
    ts_end = true;
    return true;
}

//============================================================ END OWNED BY A (scan operators) =================================================================

//============================================================ BEGIN OWNED BY C (GroupBy / OrderBy) ============================================================

//----------------------------------------------------------------------
// GroupByAggrOperator
//----------------------------------------------------------------------

GroupByAggrOperator::GroupByAggrOperator(Operator *child,
                                         const std::vector<SchemaCol> &input_schema,
                                         const RequestColumn *groupby,
                                         int groupby_num,
                                         const RequestColumn *select,
                                         int select_num) {
    gb_child = child;
    gb_in_schema = input_schema;
    gb_out_schema.clear();
    gb_out_len = 0;
    gb_keys.clear();
    gb_aggs.clear();
    gb_groups.clear();
    gb_group_index.clear();
    gb_emit_pos = 0;
    gb_end = true;
    gb_out = NULL;
    gb_out_cap = 0;

    buildOutputSchema(groupby, groupby_num, select, select_num);
}

GroupByAggrOperator::~GroupByAggrOperator() {
    close();
}

//----------------------------------------------------------------------
// OrderByOperator
//----------------------------------------------------------------------

OrderByOperator::OrderByOperator(Operator *child,
                                 const std::vector<SchemaCol> &schema,
                                 const RequestColumn *orderby,
                                 int orderby_num) {
    ob_child = child;
    ob_schema = schema;
    ob_keys.clear();
    ob_rows.clear();
    ob_chunks.clear();
    ob_chunk_used = 0;
    ob_pos = 0;
    ob_end = true;
    compileKeys(orderby, orderby_num);
}

OrderByOperator::~OrderByOperator() {
    close();
}

int OrderByOperator::findSchemaIndex(const RequestColumn &rc) const {
    return findSchemaIndexByReq(ob_schema, rc);
}

bool OrderByOperator::compileKeys(const RequestColumn *orderby, int orderby_num) {
    ob_keys.clear();
    if (orderby_num < 0)
        orderby_num = 0;
    if (orderby_num > 4)
        orderby_num = 4;
    if (orderby == NULL || orderby_num == 0)
        return true;

    for (int i = 0; i < orderby_num; i++) {
        int idx = findSchemaIndex(orderby[i]);
        if (idx < 0) {
            // best effort: for NONE_AM, match name-only among NONE_AM columns
            RequestColumn tmp = orderby[i];
            tmp.aggregate_method = NONE_AM;
            idx = findSchemaIndex(tmp);
        }
        if (idx < 0) {
            printf("[OrderByOperator][ERROR][compileKeys]: orderby column not found: %s\n", orderby[i].name);
            return false;
        }
        Key k;
        k.idx = idx;
        k.off = ob_schema[idx].offset;
        k.len = ob_schema[idx].len;
        k.type = ob_schema[idx].type;
        if (k.type == NULL)
            return false;
        ob_keys.push_back(k);
    }
    return true;
}

bool OrderByOperator::lessTuple(const TupleBuf &a, const TupleBuf &b) const {
    const char *pa = a.buf;
    const char *pb = b.buf;
    for (size_t i = 0; i < ob_keys.size(); i++) {
        const Key &k = ob_keys[i];
        const char *va = pa + k.off;
        const char *vb = pb + k.off;
        if (k.type->cmpLT((void *)va, (void *)vb))
            return true;
        if (k.type->cmpGT((void *)va, (void *)vb))
            return false;
    }
    return false;
}

bool OrderByOperator::init() {
    if (ob_child == NULL) {
        printf("[OrderByOperator][ERROR][init]: child is NULL\n");
        return false;
    }
    if (!ob_child->init()) {
        printf("[OrderByOperator][ERROR][init]: child init failed\n");
        return false;
    }

    ob_rows.clear();
    // Free any chunks from a previous (re-)init.
    for (size_t i = 0; i < ob_chunks.size(); i++) {
        if (ob_chunks[i].buf != NULL && ob_chunks[i].cap > 0)
            g_memory.free(ob_chunks[i].buf, ob_chunks[i].cap);
    }
    ob_chunks.clear();
    ob_chunk_used = 0;
    ob_pos = 0;
    ob_end = false;

    // IMPORTANT: do NOT call ob_child->getOutputLen() before the first
    // successful getNext(). Pass-through operators (e.g. FilterProjectOperator
    // in residual-filter mode, used right below an ORDER BY) only report a
    // valid current-tuple length AFTER getNext() returns true; querying it
    // earlier yields 0. Trusting that 0 here made OrderBy materialize an empty
    // result and drop every row (the TQ21 bug). We therefore learn the tuple
    // size lazily from the first row that actually arrives.
    int64_t tlen = 0;
    int64_t tlen_aligned = 0;
    int64_t chunk_sz = 0;
    // Allocate tuples in large chunks rather than one g_memory.alloc per row.
    // chunk_sz = max(4 MB, 256 * tlen_aligned) balances small and large tuples.
    static const int64_t OB_MIN_CHUNK = (int64_t)(4 << 20); // 4 MB

    while (ob_child->getNext()) {
        const char *t = ob_child->getOutput();
        if (t == NULL)
            continue;

        if (tlen == 0) {
            // First real tuple: now the child's length is meaningful.
            tlen = ob_child->getOutputLen();
            if (tlen <= 0) {
                // Cannot determine tuple size; skip this tuple rather than
                // mis-allocating a chunk.
                tlen = 0;
                continue;
            }
            // Round tuple size up to power-of-2 alignment so each slot in a
            // chunk is naturally aligned and sizeof easily computable.
            tlen_aligned = roundUpPow2(tlen);
            chunk_sz = OB_MIN_CHUNK;
            if (chunk_sz < tlen_aligned * 256)
                chunk_sz = tlen_aligned * 256;
        }

        if (ob_chunks.empty() || ob_chunk_used + tlen_aligned > ob_chunks.back().cap) {
            char *newbuf = NULL;
            int64_t got = g_memory.alloc(newbuf, chunk_sz);
            if (got != chunk_sz) {
                printf("[OrderByOperator][ERROR][init]: alloc chunk failed\n");
                return false;
            }
            TupleChunk tc;
            tc.buf = newbuf;
            tc.cap = chunk_sz;
            ob_chunks.push_back(tc);
            ob_chunk_used = 0;
        }

        char *slot = ob_chunks.back().buf + ob_chunk_used;
        memcpy(slot, t, (size_t)tlen);
        ob_chunk_used += tlen_aligned;

        TupleBuf tb;
        tb.buf = slot;
        tb.len = tlen;
        ob_rows.push_back(tb);
    }

    // Use std::sort instead of a hand-rolled recursive quicksort.
    // std::sort uses introsort (hybrid quicksort + heapsort) which guarantees
    // O(N log N) worst-case depth, eliminating the stack-overflow risk that the
    // recursive implementation had on sorted or nearly-sorted input.
    if (!ob_rows.empty() && !ob_keys.empty()) {
        std::sort(ob_rows.begin(), ob_rows.end(),
                  [this](const TupleBuf &a, const TupleBuf &b) {
                      return lessTuple(a, b);
                  });
    }
    return true;
}

bool OrderByOperator::getNext() {
    if (ob_end)
        return false;
    if (ob_pos >= ob_rows.size()) {
        ob_end = true;
        return false;
    }
    ob_pos++;
    return true;
}

bool OrderByOperator::isEnd() {
    return ob_end;
}

char *OrderByOperator::getOutput() {
    if (ob_pos == 0 || ob_pos > ob_rows.size())
        return NULL;
    return ob_rows[ob_pos - 1].buf;
}

int64_t OrderByOperator::getOutputLen() {
    if (ob_child == NULL)
        return 0;
    return ob_child->getOutputLen();
}

bool OrderByOperator::close() {
    for (size_t i = 0; i < ob_chunks.size(); i++) {
        if (ob_chunks[i].buf != NULL && ob_chunks[i].cap > 0)
            g_memory.free(ob_chunks[i].buf, ob_chunks[i].cap);
    }
    ob_chunks.clear();
    ob_chunk_used = 0;
    ob_rows.clear();
    ob_pos = 0;
    ob_end = true;
    if (ob_child != NULL) {
        ob_child->close();
        ob_child = NULL;
    }
    return true;
}

int GroupByAggrOperator::findInSchemaIndex(const RequestColumn &rc) const {
    return findSchemaIndexByReq(gb_in_schema, rc);
}

bool GroupByAggrOperator::buildOutputSchema(const RequestColumn *groupby, int groupby_num,
                                            const RequestColumn *select, int select_num) {
    gb_out_schema.clear();
    gb_out_len = 0;
    gb_keys.clear();
    gb_aggs.clear();

    if (groupby_num < 0)
        groupby_num = 0;
    if (groupby_num > 4)
        groupby_num = 4;
    if (select_num < 0)
        select_num = 0;
    if (select_num > 4)
        select_num = 4;

    // group-by keys first
    for (int i = 0; i < groupby_num; i++) {
        int inIdx = findInSchemaIndex(groupby[i]);
        if (inIdx < 0) {
            printf("[GroupByAggrOperator][ERROR][buildOutputSchema]: groupby column not found: %s\n", groupby[i].name);
            return false;
        }
        SchemaCol sc;
        memset(&sc, 0, sizeof(sc));
        strncpy(sc.name, gb_in_schema[inIdx].name, sizeof(sc.name) - 1);
        sc.type = gb_in_schema[inIdx].type;
        sc.offset = gb_out_len;
        sc.len = sc.type->getTypeSize();
        sc.aggr = NONE_AM;
        gb_out_schema.push_back(sc);
        gb_out_len += sc.len;

        KeyRef kr;
        kr.in_idx = inIdx;
        kr.off = gb_in_schema[inIdx].offset;
        kr.len = gb_in_schema[inIdx].len;
        kr.type = gb_in_schema[inIdx].type;
        gb_keys.push_back(kr);
    }

    // aggregations (in select order, keep duplicates distinguished by method)
    for (int i = 0; i < select_num; i++) {
        if (select[i].aggregate_method == NONE_AM)
            continue;
        int inIdx = findInSchemaIndex(select[i]);
        if (inIdx < 0) {
            // for aggregates, match just by name when input schema has NONE_AM
            RequestColumn tmp = select[i];
            tmp.aggregate_method = NONE_AM;
            inIdx = findInSchemaIndex(tmp);
        }
        if (inIdx < 0) {
            printf("[GroupByAggrOperator][ERROR][buildOutputSchema]: aggregate column not found: %s\n", select[i].name);
            return false;
        }

        BasicType *inType = gb_in_schema[inIdx].type;
        BasicType *outType = aggOutType(select[i].aggregate_method, inType);
        if (outType == NULL)
            return false;

        SchemaCol sc;
        memset(&sc, 0, sizeof(sc));
        strncpy(sc.name, select[i].name, sizeof(sc.name) - 1);
        sc.type = outType;
        sc.offset = gb_out_len;
        sc.len = outType->getTypeSize();
        sc.aggr = select[i].aggregate_method;
        gb_out_schema.push_back(sc);
        gb_out_len += sc.len;

        AggSpec as;
        as.method = select[i].aggregate_method;
        as.in_idx = inIdx;
        as.off = gb_in_schema[inIdx].offset;
        as.len = gb_in_schema[inIdx].len;
        as.in_type = inType;
        as.out_type = outType;
        gb_aggs.push_back(as);
        if ((int)gb_aggs.size() >= 4)
            break;
    }
    return true;
}

bool GroupByAggrOperator::buildStates() {
    gb_groups.clear();
    gb_group_index.clear();
    gb_emit_pos = 0;

    if (gb_child == NULL)
        return false;

    while (gb_child->getNext()) {
        const char *t = gb_child->getOutput();
        if (t == NULL)
            continue;

        std::string key;
        key.reserve(64);
        for (size_t ki = 0; ki < gb_keys.size(); ki++) {
            const KeyRef &kr = gb_keys[ki];
            key.append(t + kr.off, t + kr.off + kr.len);
        }

        int idx = -1;
        std::unordered_map<std::string, int>::iterator it = gb_group_index.find(key);
        if (it == gb_group_index.end()) {
            idx = (int)gb_groups.size();
            gb_group_index[key] = idx;
            GroupState gs;
            gs.key_bytes = key;
            gs.aggs.resize(gb_aggs.size());
            gb_groups.push_back(gs);
        } else {
            idx = it->second;
        }

        GroupState &gs = gb_groups[idx];
        for (size_t ai = 0; ai < gb_aggs.size(); ai++) {
            const AggSpec &as = gb_aggs[ai];
            AggState &st = gs.aggs[ai];

            const char *valPtr = t + as.off;
            st.count++;

            if (as.method == COUNT) {
                continue;
            } else if (as.method == SUM || as.method == AVG) {
                if (typeIsFloat(as.in_type) || as.out_type->getTypeCode() == FLOAT64_TC) {
                    st.sum_d += readAsDouble(as.in_type, valPtr);
                } else {
                    st.sum_i += readAsInt64(as.in_type, valPtr);
                }
            } else if (as.method == MAX || as.method == MIN) {
                if (!st.has_minmax) {
                    st.has_minmax = true;
                    st.minmax_bytes.assign(valPtr, valPtr + as.len);
                } else {
                    const char *cur = st.minmax_bytes.data();
                    bool take = false;
                    if (as.method == MAX) {
                        take = as.in_type->cmpGT((void *)valPtr, (void *)cur);
                    } else {
                        take = as.in_type->cmpLT((void *)valPtr, (void *)cur);
                    }
                    if (take) {
                        st.minmax_bytes.assign(valPtr, valPtr + as.len);
                    }
                }
            }
        }
    }
    return true;
}

bool GroupByAggrOperator::ensureOutBuf() {
    int64_t need_cap = roundUpPow2(gb_out_len);
    if (gb_out == NULL || gb_out_cap < need_cap) {
        if (gb_out != NULL && gb_out_cap > 0) {
            g_memory.free(gb_out, gb_out_cap);
        }
        gb_out = NULL;
        gb_out_cap = need_cap;
        char *buf = NULL;
        int64_t got = g_memory.alloc(buf, gb_out_cap);
        if (got != gb_out_cap) {
            printf("[GroupByAggrOperator][ERROR][ensureOutBuf]: alloc output buffer failed\n");
            gb_out = NULL;
            gb_out_cap = 0;
            return false;
        }
        gb_out = buf;
    }
    memset(gb_out, 0, (size_t)gb_out_cap);
    return true;
}

bool GroupByAggrOperator::init() {
    if (gb_child == NULL) {
        printf("[GroupByAggrOperator][ERROR][init]: child is NULL\n");
        return false;
    }
    if (!gb_child->init()) {
        printf("[GroupByAggrOperator][ERROR][init]: child init failed\n");
        return false;
    }
    gb_end = false;
    gb_emit_pos = 0;

    if (!buildStates())
        return false;
    return true;
}

bool GroupByAggrOperator::getNext() {
    if (gb_end)
        return false;
    if (gb_emit_pos >= gb_groups.size()) {
        gb_end = true;
        return false;
    }
    if (!ensureOutBuf()) {
        gb_end = true;
        return false;
    }

    const GroupState &gs = gb_groups[gb_emit_pos++];

    int64_t pos = 0;
    // write keys (already concatenated in key_bytes)
    int64_t keyPos = 0;
    for (size_t ki = 0; ki < gb_keys.size(); ki++) {
        int64_t len = gb_keys[ki].len;
        memcpy(gb_out + pos, gs.key_bytes.data() + keyPos, (size_t)len);
        pos += len;
        keyPos += len;
    }

    // write aggregates (in gb_aggs order)
    for (size_t ai = 0; ai < gb_aggs.size(); ai++) {
        const AggSpec &as = gb_aggs[ai];
        const AggState &st = gs.aggs[ai];

        if (as.method == COUNT) {
            int64_t v = st.count;
            memcpy(gb_out + pos, &v, sizeof(v));
            pos += sizeof(v);
            continue;
        }
        if (as.method == SUM) {
            if (as.out_type->getTypeCode() == FLOAT64_TC) {
                double v = st.sum_d;
                memcpy(gb_out + pos, &v, sizeof(v));
                pos += sizeof(v);
            } else {
                int64_t v = st.sum_i;
                memcpy(gb_out + pos, &v, sizeof(v));
                pos += sizeof(v);
            }
            continue;
        }
        if (as.method == AVG) {
            double v = 0.0;
            if (st.count > 0) {
                // AVG is always float64 output
                if (as.out_type->getTypeCode() == FLOAT64_TC) {
                    v = st.sum_d / (double)st.count;
                } else {
                    v = (double)st.sum_i / (double)st.count;
                }
            }
            memcpy(gb_out + pos, &v, sizeof(v));
            pos += sizeof(v);
            continue;
        }
        if (as.method == MAX || as.method == MIN) {
            if (!st.has_minmax || (int64_t)st.minmax_bytes.size() < as.len) {
                memset(gb_out + pos, 0, (size_t)as.out_type->getTypeSize());
            } else {
                memcpy(gb_out + pos, st.minmax_bytes.data(), (size_t)as.len);
            }
            pos += as.out_type->getTypeSize();
            continue;
        }
    }

    return true;
}

bool GroupByAggrOperator::isEnd() {
    return gb_end;
}

char *GroupByAggrOperator::getOutput() {
    return gb_out;
}

int64_t GroupByAggrOperator::getOutputLen() {
    return gb_out_len;
}

bool GroupByAggrOperator::close() {
    if (gb_out != NULL && gb_out_cap > 0) {
        g_memory.free(gb_out, gb_out_cap);
    }
    gb_out = NULL;
    gb_out_cap = 0;
    gb_out_len = 0;
    gb_groups.clear();
    gb_group_index.clear();
    gb_emit_pos = 0;
    gb_end = true;

    if (gb_child != NULL) {
        gb_child->close();
        gb_child = NULL;
    }
    return true;
}

//============================================================ BEGIN OWNED BY B (FilterProject/Join) ============================================================

//----------------------------------------------------------------------
// FilterProjectOperator
//----------------------------------------------------------------------

/**
 * @brief Create a filter/project operator whose input layout is described by a base table.
 *
 * @details This constructor is used for tuples produced directly from a table scan.  The
 * table metadata is stored here, while schema expansion, predicate compilation, and output
 * buffer allocation are deferred to init().  Conditions are interpreted as a logical AND.
 * If @p proj is null or @p proj_num is zero, init() later chooses the default projection
 * of all columns, subject to the lab limit of four output columns.
 *
 * @param child Upstream operator that produces packed binary tuples.
 * @param table Base table whose catalog columns define names, types, and packed offsets.
 * @param conds WHERE-style conditions to evaluate on each input tuple.
 * @param proj Optional projection list in output order; must contain @p proj_num entries when non-null.
 * @param proj_num Number of projection entries; values outside the supported range [0,4] are clamped.
 */
FilterProjectOperator::FilterProjectOperator(Operator *child, Table *table, const Conditions &conds,
                                             const RequestColumn *proj, int proj_num) {
    // 对原始表：过滤或投影
    // 创建一个输入布局由“基础表”决定的过滤/投影算子。常用于表扫描之后
    fp_child = child;
    fp_table = table;
    fp_use_schema = false; // 是否使用外部传进来的执行期 schema（输入格式）。false 表示后续通过 table 构造 schema。
    fp_passthrough = false;
    fp_in_schema.clear();
    fp_conds = conds;
    fp_col_num = 0;
    fp_pred_num = 0;
    fp_proj_num = 0;
    fp_proj_req_num = 0;
    fp_out_len = 0;
    fp_out = NULL;
    fp_out_cap = 0;
    fp_end = true;

    // 初始化为安全值，等待编译
    for (int i = 0; i < 4; i++) {
        fp_pred[i].right_const = NULL;
        fp_pred[i].right_const_cap = 0;
        fp_pred[i].right_const_len = 0;
        fp_pred[i].right_is_col = false;
        fp_pred[i].left_col = -1;
        fp_pred[i].right_col = -1;
        fp_pred[i].cmp = NONE_CM;
        fp_proj_col[i] = -1;
    }

    // Store projection request for later compilation
    if (proj_num < 0)
        proj_num = 0;
    if (proj_num > 4)
        proj_num = 4;
    fp_proj_req_num = proj_num;
    for (int i = 0; i < proj_num; i++) {
        memset(&fp_proj_req[i], 0, sizeof(RequestColumn));
        strncpy(fp_proj_req[i].name, proj[i].name, sizeof(fp_proj_req[i].name) - 1);
        fp_proj_req[i].aggregate_method = proj[i].aggregate_method;
    }
}

/**
 * @brief Create a schema-driven pass-through filter.
 *
 * @details The input tuple layout is supplied explicitly through @p schema instead of being
 * read from a base table.  Because no projection list is supplied, matching tuples are not
 * copied into a new layout; getNext() returns the child tuple pointer and length unchanged.
 * This form is useful for residual WHERE or HAVING filters placed above joins or aggregation.
 *
 * @param child Upstream operator that produces tuples matching @p schema.
 * @param schema Column descriptors for the child tuple layout.
 * @param conds Conditions to evaluate as a logical AND.
 */
FilterProjectOperator::FilterProjectOperator(Operator *child, const std::vector<SchemaCol> &schema,
                                             const Conditions &conds)
    : FilterProjectOperator(child, schema, conds, NULL, 0) {
    // 对中间结果：只过滤不投影
    // 满足条件的 tuple 会原样传给上层，不重新拷贝列（因为中间结果已经是拷贝了）
    // Join 输出 tuple--->FilterProjectOperator 过滤剩余 WHERE 条件
    // GroupBy 输出 tuple--->FilterProjectOperator 过滤 HAVING 条件
    // 这里必须使用上一个算子的输出 schema 作为输入格式

    // pass-through filter
    fp_passthrough = true;
}

/**
 * @brief Create a schema-driven filter/project operator.
 *
 * @details This constructor handles intermediate tuples whose layout is already known from
 * an execution-plan schema, such as the output of a join or aggregation.  The constructor
 * only records configuration; init() later validates the schema, compiles conditions and
 * projection names into column indexes, and allocates the output tuple buffer.
 *
 * @param child Upstream operator that produces tuples matching @p schema.
 * @param schema Column descriptors for the child tuple layout.
 * @param conds Conditions to evaluate as a logical AND.
 * @param proj Optional projection list in output order; must contain @p proj_num entries when non-null.
 * @param proj_num Number of projection entries; values outside the supported range [0,4] are clamped.
 */
FilterProjectOperator::FilterProjectOperator(Operator *child, const std::vector<SchemaCol> &schema,
                                             const Conditions &conds,
                                             const RequestColumn *proj, int proj_num) {
    // 对中间结果：过滤 + 投影
    // 基于的 SchemaCol	从执行计划传入，用于 Join / GroupBy / OrderBy 之后
    fp_child = child;
    fp_table = NULL;
    fp_use_schema = true; // 使用 执行期 schema
    fp_passthrough = false;
    fp_in_schema = schema;
    fp_conds = conds;
    fp_col_num = 0;
    fp_pred_num = 0;
    fp_proj_num = 0;
    fp_proj_req_num = 0;
    fp_out_len = 0;
    fp_out = NULL;
    fp_out_cap = 0;
    fp_end = true;

    for (int i = 0; i < 4; i++) {
        fp_pred[i].right_const = NULL;
        fp_pred[i].right_const_cap = 0;
        fp_pred[i].right_const_len = 0;
        fp_pred[i].right_is_col = false;
        fp_pred[i].left_col = -1;
        fp_pred[i].right_col = -1;
        fp_pred[i].cmp = NONE_CM;
        fp_proj_col[i] = -1;
    }

    if (proj_num < 0)
        proj_num = 0;
    if (proj_num > 4)
        proj_num = 4;
    fp_proj_req_num = proj_num;
    for (int i = 0; i < proj_num; i++) {
        memset(&fp_proj_req[i], 0, sizeof(RequestColumn));
        strncpy(fp_proj_req[i].name, proj[i].name, sizeof(fp_proj_req[i].name) - 1);
        fp_proj_req[i].aggregate_method = proj[i].aggregate_method;
    }
}

/**
 * @brief Destroy the operator and release owned runtime buffers.
 *
 * @details close() is idempotent, so invoking it from the destructor safely frees any
 * compiled constant buffers, output buffers, and the child operator chain that this object
 * still owns.
 */
FilterProjectOperator::~FilterProjectOperator() {
    // 析构函数直接调用 close()，这样即使上层忘记手动关闭算子，也可以在对象销毁时尽量释放已经申请的内存资源。
    close();
}

/**
 * @brief Resolve a requested column name to an index in the compiled input schema.
 *
 * @details The function first tries an exact name match.  If the requested name is qualified
 * (for example, "table.column"), it then tries the suffix after the final dot so that simple
 * catalog column names can still match qualified query names.  The first matching column wins.
 *
 * @param name Column name or qualified column name to resolve.
 * @return Zero-based index in fp_cols on success, or -1 when the name is empty or unknown.
 */
int FilterProjectOperator::findColIndexByName(const char *name) {
    // 根据列名，在 fp_cols 中找到对应列的下标。
    // 列名匹配 age
    if (name == NULL || name[0] == '\0')
        return -1;
    for (int i = 0; i < fp_col_num; i++) {
        if (strcmp(fp_cols[i].name, name) == 0)
            return i;
    }
    // 带表名前缀的后缀匹配 student.age
    const char *dot = strrchr(name, '.');
    if (dot != NULL && dot[1] != '\0') {
        const char *shortName = dot + 1;
        for (int i = 0; i < fp_col_num; i++) {
            if (strcmp(fp_cols[i].name, shortName) == 0)
                return i;
        }
    }
    return -1;
}

/**
 * @brief Build the internal column descriptor array for the input tuples.
 *
 * @details When fp_use_schema is true, descriptors are copied from the supplied SchemaCol
 * vector.  Otherwise the descriptors are derived from the base table's catalog columns and
 * offsets are computed by packing all visible column values consecutively.  The resulting
 * fp_cols array is the common source used by predicate and projection compilation.
 *
 * @return true if the schema was built successfully; false if metadata is missing, invalid,
 *         or exceeds the fixed lab array size.
 */
bool FilterProjectOperator::buildSchema() {
    // 构建 FilterProjectOperator 内部使用的输入列描述数组 fp_cols。列名 + 数据类型 + tuple 中的偏移
    fp_col_num = 0;

    // 输入来自中间结果
    if (fp_use_schema) {
        if (fp_in_schema.size() > (sizeof(fp_cols) / sizeof(fp_cols[0]))) {
            printf("[FilterProjectOperator][ERROR][buildSchema]: too many schema columns\n");
            return false;
        }
        for (size_t i = 0; i < fp_in_schema.size(); i++) {
            const SchemaCol &sc = fp_in_schema[i];
            if (sc.type == NULL) {
                printf("[FilterProjectOperator][ERROR][buildSchema]: schema datatype not initialized\n");
                return false;
            }
            memset(fp_cols[fp_col_num].name, 0, sizeof(fp_cols[fp_col_num].name));
            strncpy(fp_cols[fp_col_num].name, sc.name, sizeof(fp_cols[fp_col_num].name) - 1);
            fp_cols[fp_col_num].type = sc.type;
            fp_cols[fp_col_num].offset = sc.offset;
            fp_col_num++;
        }
        return true;
    }

    // 输入来自基础表
    if (fp_table == NULL) {
        printf("[FilterProjectOperator][ERROR][buildSchema]: table is NULL\n");
        return false;
    }
    std::vector<int64_t> &cols = fp_table->getColumns(); // 表的每一列的 OID
    if (cols.size() > (sizeof(fp_cols) / sizeof(fp_cols[0]))) {
        printf("[FilterProjectOperator][ERROR][buildSchema]: too many columns\n");
        return false;
    }

    int64_t offset = 0; // 需要累计offset，因为 Column 对象中不含偏移
    for (unsigned int i = 0; i < cols.size(); i++) {
        Column *col = (Column *)g_catalog.getObjById(cols[i]); // 根据 OID 获取对应的 Column 对象
        if (col == NULL || col->getOtype() != COLUMN) {
            printf("[FilterProjectOperator][ERROR][buildSchema]: column type error\n");
            return false;
        }
        BasicType *dt = col->getDataType();
        if (dt == NULL) {
            printf("[FilterProjectOperator][ERROR][buildSchema]: column datatype not initialized\n");
            return false;
        }
        memset(fp_cols[fp_col_num].name, 0, sizeof(fp_cols[fp_col_num].name));
        strncpy(fp_cols[fp_col_num].name, col->getOname(), sizeof(fp_cols[fp_col_num].name) - 1);
        fp_cols[fp_col_num].type = dt;
        fp_cols[fp_col_num].offset = offset;
        offset += dt->getTypeSize();
        fp_col_num++;
    }
    return true;
}

/**
 * @brief Compile query conditions into executable predicate descriptors.
 *
 * @details Each Condition is converted into a Pred containing the left column index, normalized
 * comparison operator, and either a right column index or a binary constant buffer.  A right
 * operand is treated as a column when the condition uses LINK or when its text can be resolved
 * as a column name; otherwise it is parsed through the left column's BasicType::formatBin().
 * At execution time all compiled predicates are evaluated with AND semantics.
 *
 * @return true if every condition is valid and all constant buffers are allocated; false on
 *         unknown columns, invalid comparison methods, allocation failure, or parse failure.
 */
bool FilterProjectOperator::compilePredicates() {
    fp_pred_num = 0; // 清空当前谓词数量
    int n = fp_conds.condition_num;
    if (n < 0)
        n = 0;
    if (n > 4)
        n = 4;

    for (int i = 0; i < n; i++) {
        Condition &c = fp_conds.condition[i];
        int leftIdx = findColIndexByName(c.column.name); // 由左列名 得 左列下标（在输入schema fp_cols 中的下标）
        if (leftIdx < 0) {
            printf("[FilterProjectOperator][ERROR][compilePredicates]: left column not found: %s\n", c.column.name);
            return false;
        }
        CompareMethod cm = normalizeCompare(c.compare); // 规范化比较符：LINK 会被当作 EQ
        if (cm <= NONE_CM || cm >= MAX_CM) {
            printf("[FilterProjectOperator][ERROR][compilePredicates]: invalid compare method\n");
            return false;
        }

        bool rhsIsCol = (c.compare == LINK); // 判断是否为连接操作
        int rightIdx = -1;
        if (!rhsIsCol) { // 不是连接操作
            // 先当列名处理，找到就是列名，找不到就自动降级为常量
            rightIdx = findColIndexByName(c.value);
            if (rightIdx >= 0)
                rhsIsCol = true;
        } else { // 是连接操作
            // 右侧一定是列名，找不到报错
            rightIdx = findColIndexByName(c.value);
            if (rightIdx < 0) {
                printf("[FilterProjectOperator][ERROR][compilePredicates]: right column not found: %s\n", c.value);
                return false;
            }
        }

        fp_pred[fp_pred_num].left_col = leftIdx;
        fp_pred[fp_pred_num].cmp = cm;
        fp_pred[fp_pred_num].right_is_col = rhsIsCol;
        fp_pred[fp_pred_num].right_col = rightIdx;
        fp_pred[fp_pred_num].right_const = NULL;
        fp_pred[fp_pred_num].right_const_cap = 0;
        fp_pred[fp_pred_num].right_const_len = 0;

        // 按常量处理
        if (!rhsIsCol) {
            // 根据左侧列类型为右值分配一块缓冲区。
            BasicType *dt = fp_cols[leftIdx].type;
            int64_t len = dt->getTypeSize();
            int64_t cap = roundUpPow2(len);
            char *buf = NULL;
            int64_t got = g_memory.alloc(buf, cap);
            if (got != cap) {
                printf("[FilterProjectOperator][ERROR][compilePredicates]: alloc const buffer error\n");
                return false;
            }
            memset(buf, 0, (size_t)cap);

            // 用左侧类型的解析函数 BasicType::formatBin() 把右侧字符串常量转成二进制
            if (dt->formatBin(buf, (void *)c.value) < 0) {
                printf("[FilterProjectOperator][ERROR][compilePredicates]: constant formatBin error, col=%s val=%s\n",
                       fp_cols[leftIdx].name, c.value);
                g_memory.free(buf, cap);
                return false;
            }
            fp_pred[fp_pred_num].right_const = buf; // 指向右值缓冲区
            fp_pred[fp_pred_num].right_const_cap = cap;
            fp_pred[fp_pred_num].right_const_len = len;
        }
        fp_pred_num++;
    }
    return true;
}

/**
 * @brief Compile the projection list and compute output tuple width.
 *
 * @details Projection names are resolved against fp_cols and stored as column indexes in
 * fp_proj_col.  fp_out_len becomes the sum of the selected column type sizes.  If no explicit
 * projection is provided, all input columns are projected in input order, but only when the
 * number of columns fits the lab's four-column projection limit.
 *
 * @param proj Projection list requested by the query, or NULL for project-all behavior.
 * @param proj_num Number of entries in @p proj; values outside [0,4] are clamped.
 * @return true when the projection can be compiled; false when a requested column is unknown
 *         or project-all would exceed the fixed output-column limit.
 */
bool FilterProjectOperator::compileProjection(const RequestColumn *proj, int proj_num) {
    // 把 SELECT 投影列编译成列下标数组，并计算输出 tuple 的长度
    // 记录到 fp_proj_col[] 数组中
    fp_proj_num = 0; // 输出列数
    fp_out_len = 0;  // 输出 tuple 大小

    if (proj_num < 0)
        proj_num = 0;
    if (proj_num > 4)
        proj_num = 4;

    // If projection is empty, default to projecting all columns.
    // 没有显示投影，默认输出所有输入列 fp_cols
    if (proj == NULL || proj_num == 0) {
        if (fp_col_num > 4) {
            printf("[FilterProjectOperator][ERROR][compileProjection]: project-all exceeds 4 columns (lab limit)\n");
            return false;
        }
        for (int i = 0; i < fp_col_num; i++) {
            fp_proj_col[fp_proj_num++] = i;
            fp_out_len += fp_cols[i].type->getTypeSize();
        }
        return true;
    }

    // 有显式投影：逐个解析列名
    for (int i = 0; i < proj_num; i++) {
        int idx = findColIndexByName(proj[i].name);
        if (idx < 0) {
            printf("[FilterProjectOperator][ERROR][compileProjection]: projection column not found: %s\n", proj[i].name);
            return false;
        }
        fp_proj_col[fp_proj_num++] = idx;
        fp_out_len += fp_cols[idx].type->getTypeSize();
    }
    return true;
}

/**
 * @brief Evaluate one compiled predicate on a packed input tuple.
 *
 * @details The left value is read from the tuple using the compiled column offset.  The right
 * value is either another tuple column or a preformatted constant buffer.  Column-to-column
 * comparisons require identical BasicType codes and byte widths.  The actual comparison is
 * delegated to BasicType comparison helpers so integer, string, date, and other supported
 * types keep their own ordering rules.
 *
 * @param p Compiled predicate descriptor to evaluate.
 * @param tuple Packed binary tuple produced by the child operator.
 * @return true if the tuple satisfies the predicate; false for failed checks, type mismatch,
 *         invalid metadata, or an unsupported comparison method.
 */
bool FilterProjectOperator::evalOne(const Pred &p, const char *tuple) {
    // 对一条 tuple 判断一个条件是否成立
    if (tuple == NULL)
        return false;
    if (p.left_col < 0 || p.left_col >= fp_col_num)
        return false;
    const ColInfo &lhs = fp_cols[p.left_col];
    const char *lhsPtr = tuple + lhs.offset; // 定位左值

    const char *rhsPtr = NULL;
    BasicType *cmpType = lhs.type;
    // 右侧是列
    if (p.right_is_col) {
        if (p.right_col < 0 || p.right_col >= fp_col_num)
            return false;
        const ColInfo &rhs = fp_cols[p.right_col];
        if (rhs.type == NULL || cmpType == NULL) // 左右列类型一致检查
            return false;
        if (rhs.type->getTypeCode() != cmpType->getTypeCode() ||
            rhs.type->getTypeSize() != cmpType->getTypeSize()) {
            printf("[FilterProjectOperator][WARN][evalOne]: type mismatch between %s and %s\n",
                   lhs.name, rhs.name);
            return false;
        }
        rhsPtr = tuple + rhs.offset; // 定位右值
    } else {
        rhsPtr = p.right_const; // 直接使用常量缓冲区指针
    }

    switch (p.cmp) {
    case LT:
        return cmpType->cmpLT((void *)lhsPtr, (void *)rhsPtr);
    case LE:
        return cmpType->cmpLE((void *)lhsPtr, (void *)rhsPtr);
    case EQ:
        return cmpType->cmpEQ((void *)lhsPtr, (void *)rhsPtr);
    case NE:
        return !cmpType->cmpEQ((void *)lhsPtr, (void *)rhsPtr);
    case GT:
        return cmpType->cmpGT((void *)lhsPtr, (void *)rhsPtr);
    case GE:
        return cmpType->cmpGE((void *)lhsPtr, (void *)rhsPtr);
    default:
        return false;
    }
}

/**
 * @brief Initialize the child operator and prepare filtering/projection state.
 *
 * @details init() follows the iterator protocol by initializing the child first.  It then
 * builds the input schema and compiles all predicates.  For pass-through filters, no output
 * buffer is allocated because accepted tuples are forwarded directly.  For projecting filters,
 * the projection list is compiled and a reusable g_memory-managed output buffer is allocated
 * with power-of-two capacity.
 *
 * @return true when the operator is ready for getNext(); false when child initialization,
 *         schema compilation, predicate compilation, projection compilation, or allocation fails.
 */
bool FilterProjectOperator::init() {
    if (fp_child == NULL) {
        printf("[FilterProjectOperator][ERROR][init]: child is NULL\n");
        return false;
    }
    // 初始化子算子
    if (!fp_child->init()) {
        printf("[FilterProjectOperator][ERROR][init]: child init failed\n");
        return false;
    }
    fp_end = false; // 打开算子

    if (!buildSchema()) // 构建输入列信息
        return false;
    if (!compilePredicates()) // 编译过滤条件
        return false;

    if (fp_passthrough) {
        fp_out = NULL;
        fp_out_cap = 0;
        fp_out_len = 0;
        fp_proj_num = 0;
        return true;
    }

    // 非 pass-through 模式：编译投影列
    if (!compileProjection(fp_proj_req_num > 0 ? fp_proj_req : NULL, fp_proj_req_num))
        return false;
    // 根据 fp_out_len 计算输出缓冲区容量
    int64_t new_cap = roundUpPow2(fp_out_len);
    if (fp_out != NULL && fp_out_cap > 0) { // 清空历史缓冲区
        g_memory.free(fp_out, fp_out_cap);
        fp_out = NULL;
    }
    // 分配输出缓冲区
    fp_out_cap = new_cap;
    char *buf = NULL;
    int64_t got = g_memory.alloc(buf, fp_out_cap);
    if (got != fp_out_cap) {
        printf("[FilterProjectOperator][ERROR][init]: alloc output buffer error\n");
        fp_out = NULL;
        fp_out_cap = 0;
        return false;
    }
    fp_out = buf;
    memset(fp_out, 0, (size_t)fp_out_cap);
    return true;
}

/**
 * @brief Produce the next tuple that satisfies all filters.
 *
 * @details The method repeatedly pulls tuples from the child until one passes every compiled
 * predicate.  In pass-through mode it simply exposes the accepted child tuple and length.  In
 * projection mode it copies the requested columns, in order, into fp_out and returns that packed
 * projected tuple.  When the child is exhausted or a fatal metadata/buffer error is detected,
 * fp_end is set and false is returned.
 *
 * @return true when getOutput() points to a valid accepted tuple; false when no more tuples are available.
 */
bool FilterProjectOperator::getNext() {
    // 不断从子算子取数据，直到找到一条满足条件的 tuple，并把它作为当前输出

    if (fp_end)
        return false; // 已结束，返回 false
    if (fp_child == NULL) {
        fp_end = true;
        return false;
    }

    while (fp_child->getNext()) {
        const char *in = fp_child->getOutput();
        bool ok = true;
        for (int i = 0; i < fp_pred_num; i++) {
            if (!evalOne(fp_pred[i], in)) {
                ok = false;
                break;
            }
        }
        if (!ok) // 某个过滤条件不成立
            continue;

        if (fp_passthrough) {
            fp_out = (char *)in;
            fp_out_len = fp_child->getOutputLen();
            return true;
        }

        if (fp_out == NULL) {
            fp_end = true;
            return false;
        }

        // Project into fp_out
        memset(fp_out, 0, (size_t)fp_out_cap);
        int64_t outPos = 0;
        // 按投影列顺序复制数据
        for (int i = 0; i < fp_proj_num; i++) {
            int colIdx = fp_proj_col[i];
            if (colIdx < 0 || colIdx >= fp_col_num) {
                fp_end = true;
                return false;
            }
            int64_t sz = fp_cols[colIdx].type->getTypeSize();
            memcpy(fp_out + outPos, in + fp_cols[colIdx].offset, (size_t)sz);
            outPos += sz;
        }
        return true;
    }
    fp_end = true; // 能执行到这里说明子算子没有数据了
    return false;
}

/**
 * @brief Report whether this operator has reached end-of-stream.
 *
 * @return true after initialization failure, close(), or child exhaustion; otherwise false.
 */
bool FilterProjectOperator::isEnd() { // 给上层算子使用？？
    return fp_end;                    // 查询算子关闭状态
}

/**
 * @brief Return the current output tuple buffer.
 *
 * @details After a successful getNext(), the pointer either references fp_out in projection
 * mode or the child's current output buffer in pass-through mode.  The pointer is owned by the
 * operator tree and remains valid only until the next getNext() or close().
 *
 * @return Pointer to the current packed tuple, or NULL if no tuple is available.
 */
char *FilterProjectOperator::getOutput() {
    return fp_out;
}

/**
 * @brief Return the byte length of the current output tuple.
 *
 * @return Packed tuple length in bytes for the tuple returned by getOutput().
 */
int64_t FilterProjectOperator::getOutputLen() {
    return fp_out_len;
}

/**
 * @brief Release all resources owned by the filter/project operator.
 *
 * @details Frees binary constant buffers compiled for predicates, frees the reusable projection
 * output buffer when this operator owns one, resets iterator state, and closes the child chain.
 * The method is safe to call multiple times.
 *
 * @return Always true.
 */
bool FilterProjectOperator::close() {
    // 释放 FilterProjectOperator 占用的资源，并关闭子算子

    // 遍历并释放条件谓词
    for (int i = 0; i < 4; i++) {
        // 释放右侧常量缓冲区
        if (fp_pred[i].right_const != NULL && fp_pred[i].right_const_cap > 0) {
            g_memory.free(fp_pred[i].right_const, fp_pred[i].right_const_cap);
        }
        fp_pred[i].right_const = NULL;
        fp_pred[i].right_const_cap = 0;
        fp_pred[i].right_const_len = 0;
    }
    fp_pred_num = 0;
    fp_proj_num = 0;
    fp_out_len = 0;
    fp_end = true; // 设置算子关闭状态
    // 如果不是 pass-through，释放输出缓冲区
    if (!fp_passthrough && fp_out != NULL && fp_out_cap > 0) {
        g_memory.free(fp_out, fp_out_cap);
    }
    fp_out = NULL;
    fp_out_cap = 0;
    if (fp_child != NULL) {
        fp_child->close();
        fp_child = NULL;
    }
    return true;
}

//================================== END OWNED BY C (GroupBy / OrderBy / Filter) ====================================================

//=============================== OWNED BY B (Join) =================================================================================

//----------------------------------------------------------------------
// HashJoinOperator
//----------------------------------------------------------------------
// 1. 构建右表哈希表
// 2. 扫描左表，输出 join tuple

// hj_build：保存右侧所有 tuple 的拷贝。
// hj_map：哈希表，从 key 映射到 hj_build 中的 tuple 下标列表。

/**
 * @brief Create a hash join operator for an equality join between two child streams.
 *
 * @details The right child is used as the build side and the left child as the probe side.
 * Join keys are read directly from packed tuples at the supplied byte offsets and compared as
 * raw byte strings with length derived from @p key_type.  Each output tuple is the concatenation
 * [left tuple][right tuple].
 *
 * @param left Probe-side child operator.
 * @param right Build-side child operator to materialize into an in-memory hash table.
 * @param left_key_off Byte offset of the join key inside left tuples.
 * @param right_key_off Byte offset of the join key inside right tuples.
 * @param key_type Type descriptor of the equality key; determines key byte length.
 */
HashJoinOperator::HashJoinOperator(Operator *left, Operator *right,
                                   int64_t left_key_off, int64_t right_key_off,
                                   BasicType *key_type) {
    hj_left = left;
    hj_right = right;
    hj_left_key_off = left_key_off;   // 左 tuple 中 join key 的偏移
    hj_right_key_off = right_key_off; // 右 tuple 中 join key 的偏移
    hj_key_type = key_type;
    hj_key_len = (hj_key_type != NULL ? hj_key_type->getTypeSize() : 0);

    hj_end = true;
    hj_out = NULL;
    hj_out_len = 0;
    hj_out_cap = 0;

    hj_chunks.clear();
    hj_chunk_used = 0;

    hj_cur_left = NULL;
    hj_cur_left_len = 0;
    hj_cur_match_idx.clear();
    hj_cur_match_pos = 0;
}

/**
 * @brief Destroy the hash join operator.
 *
 * @details Resource cleanup is intentionally not performed here because the Executor owns the
 * full operator list and closes it in a controlled order.  This avoids double-closing shared
 * child pointers when the plan is torn down.
 */
HashJoinOperator::~HashJoinOperator() {
    // HashJoin 的析构函数故意不释放资源，因为整个算子树由 Executor 统一关闭。这样可以避免多个算子之间因为父子关系造成重复 close。
    //  Do not call close() here. The plan owner (Executor) manages close order.
}

/**
 * @brief Extract a fixed-width binary join key from a packed tuple.
 *
 * @details The key is stored in std::string so it can safely contain '\0' bytes from binary
 * integer, date, or fixed-width character values.  No type conversion is performed here; the
 * join relies on both sides having already been type-checked by the planner.
 *
 * @param key Output byte string that receives the key bytes.
 * @param tuple Packed tuple containing the key.
 * @param key_off Byte offset where the key begins in @p tuple.
 * @return true when a key of hj_key_len bytes was copied; false if tuple or key length is invalid.
 */
bool HashJoinOperator::extractKey(std::string &key, const char *tuple, int64_t key_off) {
    // 从 packed tuple 中取出连接键的二进制字节，放进 std::string
    if (tuple == NULL)
        return false;
    if (hj_key_len <= 0)
        return false;
    key.assign(tuple + key_off, tuple + key_off + hj_key_len);
    return true;
}

/**
 * @brief Materialize the right child and build the hash table used by the join.
 *
 * @details Every tuple produced by the right child is copied into g_memory-managed storage so
 * that it remains valid after the child advances.  The binary join key is extracted from that
 * copy and inserted into hj_map, whose value is a list of indexes into hj_build.  Storing a list
 * supports duplicate join keys and therefore one-to-many or many-to-many join results.
 *
 * @return true when the build side is fully materialized; false on missing input, invalid tuple
 *         length, allocation failure, or key extraction failure.
 */
bool HashJoinOperator::buildHash() {
    // 扫描右子算子，把右侧所有 tuple 存进内存，并建立哈希表
    // 为什么要复制右 tuple？ 因为子算子的输出缓冲区通常会被下一次 getNext() 覆盖，为了后续 join 时访问对应右 tuple 内容必须进行拷贝

    hj_build.clear();
    hj_map.clear();
    // Free any chunks from a previous init.
    for (size_t i = 0; i < hj_chunks.size(); i++) {
        if (hj_chunks[i].buf != NULL && hj_chunks[i].cap > 0)
            g_memory.free(hj_chunks[i].buf, hj_chunks[i].cap);
    }
    hj_chunks.clear();
    hj_chunk_used = 0;

    if (hj_right == NULL) {
        printf("[HashJoinOperator][ERROR][buildHash]: right is NULL\n");
        return false;
    }
    const int64_t rightLen = hj_right->getOutputLen();
    if (rightLen <= 0) {
        printf("[HashJoinOperator][ERROR][buildHash]: right tuple len invalid\n");
        return false;
    }
    int64_t rightLen_aligned = roundUpPow2(rightLen);

    // Allocate build-side tuples in large chunks to reduce g_memory call
    // count and fragmentation when the right (inner) table is large.
    static const int64_t HJ_MIN_CHUNK = (int64_t)(4 << 20); // 4 MB
    int64_t chunk_sz = HJ_MIN_CHUNK;
    if (chunk_sz < rightLen_aligned * 256)
        chunk_sz = rightLen_aligned * 256;

    while (hj_right->getNext()) {
        char *tup = hj_right->getOutput();
        if (tup == NULL)
            continue;

        if (hj_chunks.empty() || hj_chunk_used + rightLen_aligned > hj_chunks.back().cap) {
            char *newbuf = NULL;
            int64_t got = g_memory.alloc(newbuf, chunk_sz);
            if (got != chunk_sz) {
                printf("[HashJoinOperator][ERROR][buildHash]: alloc chunk failed\n");
                return false;
            }
            BuildChunk bc;
            bc.buf = newbuf;
            bc.cap = chunk_sz;
            hj_chunks.push_back(bc);
            hj_chunk_used = 0;
        }

        char *slot = hj_chunks.back().buf + hj_chunk_used;
        memcpy(slot, tup, (size_t)rightLen);
        hj_chunk_used += rightLen_aligned;

        BuildTuple bt;
        bt.buf = slot;
        bt.len = rightLen;
        int idx = (int)hj_build.size();
        hj_build.push_back(bt);

        std::string k;
        if (!extractKey(k, bt.buf, hj_right_key_off)) {
            printf("[HashJoinOperator][ERROR][buildHash]: extract key failed\n");
            return false;
        }
        hj_map[k].push_back(idx);
    }
    return true;
}

/**
 * @brief Assemble one joined tuple from a left tuple and a stored right tuple.
 *
 * @details The reusable output buffer is grown when necessary using power-of-two allocation.
 * The tuple layout is a simple concatenation: the complete left tuple is copied first, followed
 * immediately by the complete right tuple.  The output length is updated before allocation so
 * callers can read it through getOutputLen() after success.
 *
 * @param left Pointer to the current left/probe tuple.
 * @param leftLen Byte length of @p left.
 * @param right Build-side tuple to append.
 * @return true when hj_out contains the joined tuple; false if output buffer allocation fails.
 */
bool HashJoinOperator::emitJoin(const char *left, int64_t leftLen, const BuildTuple &right) {
    // 把一条左 tuple 和一条右 tuple 拼接成一条 join 输出
    hj_out_len = leftLen + right.len;
    int64_t need_cap = roundUpPow2(hj_out_len);
    if (hj_out == NULL || hj_out_cap < need_cap) {
        if (hj_out != NULL && hj_out_cap > 0) {
            g_memory.free(hj_out, hj_out_cap);
        }
        hj_out = NULL;
        hj_out_cap = need_cap;
        char *buf = NULL;
        int64_t got = g_memory.alloc(buf, hj_out_cap);
        if (got != hj_out_cap) {
            printf("[HashJoinOperator][ERROR][emitJoin]: alloc output buffer failed\n");
            hj_out = NULL;
            hj_out_cap = 0;
            return false;
        }
        hj_out = buf;
    }
    memcpy(hj_out, left, (size_t)leftLen);
    memcpy(hj_out + leftLen, right.buf, (size_t)right.len);
    return true;
}

/**
 * @brief Initialize both children and build the in-memory hash table.
 *
 * @details The method validates child pointers and key metadata, initializes left and right
 * child streams, records the current left tuple length, resets match-iteration state, and then
 * calls buildHash() to consume the right side before probing starts.
 *
 * @return true when the hash join is ready to produce joined tuples; false on invalid inputs,
 *         child initialization failure, or build-side materialization failure.
 */
bool HashJoinOperator::init() {
    // 初始化 Hash Join 算子，并构建右侧哈希表

    // 检查左右子算子是否为空
    if (hj_left == NULL || hj_right == NULL) {
        printf("[HashJoinOperator][ERROR][init]: child is NULL\n");
        return false;
    }
    // 检查连接键类型是否有效
    if (hj_key_type == NULL || hj_key_len <= 0) {
        printf("[HashJoinOperator][ERROR][init]: key type invalid\n");
        return false;
    }
    // 调用左子算子的 init()
    if (!hj_left->init()) {
        printf("[HashJoinOperator][ERROR][init]: left init failed\n");
        return false;
    }
    // 调用右子算子的 init()
    if (!hj_right->init()) {
        printf("[HashJoinOperator][ERROR][init]: right init failed\n");
        return false;
    }

    hj_end = false; // 开启算子
    hj_cur_left = NULL;
    hj_cur_left_len = hj_left->getOutputLen();
    hj_cur_match_idx.clear();
    hj_cur_match_pos = 0;

    if (!buildHash()) // 调用 buildHash() 构建右侧哈希表
        return false;
    return true;
}

/**
 * @brief Produce the next equality-join result.
 *
 * @details If the previous left tuple matched multiple right tuples, this method first emits
 * the next pending match.  Otherwise it advances the left child until it finds a key present in
 * hj_map, stores that match list in hj_cur_match_idx, and emits the first joined row.  Left
 * tuples with no matching build-side key are skipped.  When the left side is exhausted, hj_end
 * is set to true.
 *
 * @return true when getOutput() contains a joined tuple; false when no further matches exist
 *         or a fatal key/buffer error occurs.
 */
bool HashJoinOperator::getNext() {
    if (hj_end)
        return false;
    if (hj_left == NULL) {
        hj_end = true;
        return false;
    }

    // If we still have pending matches for current left tuple, emit next.
    // 若 当前左 tuple 还有未输出的匹配，继续输出
    if (hj_cur_left != NULL && hj_cur_match_pos < (int)hj_cur_match_idx.size()) {
        int ridx = hj_cur_match_idx[hj_cur_match_pos++];
        if (ridx < 0 || ridx >= (int)hj_build.size()) {
            hj_end = true;
            return false;
        }
        return emitJoin(hj_cur_left, hj_cur_left_len, hj_build[ridx]);
    }

    // Otherwise advance left tuples until we find a match.
    // 否则需要继续从左子算子取下一条 左tuple 进行匹配
    hj_cur_left = NULL;
    hj_cur_match_idx.clear();
    hj_cur_match_pos = 0;
    while (hj_left->getNext()) {
        char *lt = hj_left->getOutput();
        if (lt == NULL)
            continue;
        std::string k;
        if (!extractKey(k, lt, hj_left_key_off)) {
            printf("[HashJoinOperator][ERROR][getNext]: extract left key failed\n");
            hj_end = true;
            return false;
        }
        auto it = hj_map.find(k);
        if (it == hj_map.end() || it->second.empty()) {
            continue;
        }
        hj_cur_left = lt;
        hj_cur_left_len = hj_left->getOutputLen();
        hj_cur_match_idx = it->second;
        hj_cur_match_pos = 0;

        int ridx = hj_cur_match_idx[hj_cur_match_pos++];
        return emitJoin(hj_cur_left, hj_cur_left_len, hj_build[ridx]);
    }

    hj_end = true;
    return false;
}

/**
 * @brief Report whether the hash join has reached end-of-stream.
 *
 * @return true after initialization failure, close(), or left-side exhaustion; otherwise false.
 */
bool HashJoinOperator::isEnd() {
    return hj_end;
}

/**
 * @brief Return the current joined tuple buffer.
 *
 * @return Pointer to hj_out after a successful getNext(), or NULL if no tuple is available.
 */
char *HashJoinOperator::getOutput() {
    return hj_out;
}

/**
 * @brief Return the byte length of the current joined tuple.
 *
 * @return Byte length of [left tuple][right tuple].
 */
int64_t HashJoinOperator::getOutputLen() {
    return hj_out_len;
}

/**
 * @brief Release all hash join runtime resources.
 *
 * @details Frees the reusable output buffer, frees every materialized build-side tuple, clears
 * the hash table and pending-match state, marks the operator ended, and closes both children.
 * The method is safe to call multiple times.
 *
 * @return Always true.
 */
bool HashJoinOperator::close() {
    // 释放输出缓冲区 hj_out
    if (hj_out != NULL && hj_out_cap > 0) {
        g_memory.free(hj_out, hj_out_cap);
    }
    hj_out = NULL;
    hj_out_cap = 0;
    hj_out_len = 0;

    // Free chunk-based build-side storage.
    for (size_t i = 0; i < hj_chunks.size(); i++) {
        if (hj_chunks[i].buf != NULL && hj_chunks[i].cap > 0)
            g_memory.free(hj_chunks[i].buf, hj_chunks[i].cap);
    }
    hj_chunks.clear();
    hj_chunk_used = 0;
    hj_build.clear();
    hj_map.clear();

    // 清空当前左 tuple 和匹配状态
    hj_cur_left = NULL;
    hj_cur_left_len = 0;
    hj_cur_match_idx.clear();
    hj_cur_match_pos = 0;
    hj_end = true; // 关闭算子

    // 关闭左右子算子 
    if (hj_left != NULL)
        hj_left->close();
    if (hj_right != NULL)
        hj_right->close();
    return true;
}

//----------------------------------------------------------------------
// IndexNestedLoopJoinOperator
//----------------------------------------------------------------------
// 对外表每一条 tuple:
//     取出 join key
//     用这个 key 去内表 HashIndex 查找匹配记录
//     每找到一条就拼接输出

/**
 * @brief Create an index nested-loop join using a hash index on the inner table.
 *
 * @details The outer input is any operator that produces packed tuples.  For each outer tuple,
 * the operator reads the join key at @p outer_key_off and uses @p inner_index to find matching
 * records in @p inner_table.  Each output tuple is [outer tuple][packed inner tuple].  Key
 * length and inner tuple width are derived from catalog/index metadata during init().
 *
 * @param outer Outer/probe child operator.
 * @param inner_table Base table used as the indexed inner side.
 * @param inner_index Hash index on the inner join key.
 * @param outer_key_off Byte offset of the outer join key in outer tuples.
 */
IndexNestedLoopJoinOperator::IndexNestedLoopJoinOperator(Operator *outer,
                                                         Table *inner_table,
                                                         HashIndex *inner_index,
                                                         int64_t outer_key_off) {
    inlj_outer = outer;
    inlj_inner_table = inner_table;
    inlj_inner_index = inner_index;
    inlj_outer_key_off = outer_key_off;
    inlj_key_len = 0; // derived from index key column dtype in init

    inlj_inner_tuple = NULL; // 把内表物理记录转换成 packed tuple 的缓冲区
    inlj_inner_cap = 0;      // 内表缓冲区容量
    inlj_inner_len = 0;      // 内表 packed tuple 的实际长度

    inlj_out = NULL; // 最终输出 [outer][inner] 的缓冲区
    inlj_out_cap = 0;
    inlj_out_len = 0;

    inlj_end = true;
    inlj_cur_outer = NULL; // 当前外层 tuple
    inlj_cur_outer_len = 0;
    memset(&inlj_info, 0, sizeof(inlj_info)); // HashIndex 查找迭代状态
    inlj_info_valid = false;
    inlj_match_rec = NULL;
}

/**
 * @brief Destroy the index nested-loop join operator.
 *
 * @details The destructor does not call close() because the Executor controls operator-chain
 * shutdown order.  This mirrors HashJoinOperator and prevents double-closing child operators.
 */
IndexNestedLoopJoinOperator::~IndexNestedLoopJoinOperator() {
    // 和 HashJoinOperator 一样，算子树由 Executor 统一关闭。为了让 Executor 按统一顺序关闭整棵执行树，避免父子算子重复释放。
    // Do not call close() here. The plan owner (Executor) manages close order.
}

/**
 * @brief Ensure that the joined output buffer can hold one result for the given outer length.
 *
 * @details The required output size is outerLen plus the already computed packed inner tuple
 * length.  The buffer is allocated or grown using g_memory and power-of-two capacity.  Existing
 * contents may be discarded when the buffer grows.
 *
 * @param outerLen Byte length of the current outer tuple.
 * @return true when inlj_out has enough capacity; false if allocation fails.
 */
bool IndexNestedLoopJoinOperator::ensureBuffers(int64_t outerLen) {
    // 确保输出缓冲区 inlj_out 足够容纳一条连接结果
    inlj_out_len = outerLen + inlj_inner_len;
    int64_t needOut = roundUpPow2(inlj_out_len);
    if (inlj_out == NULL || inlj_out_cap < needOut) {
        if (inlj_out != NULL && inlj_out_cap > 0) {
            g_memory.free(inlj_out, inlj_out_cap);
        }
        inlj_out = NULL;
        inlj_out_cap = needOut;
        char *buf = NULL;
        int64_t got = g_memory.alloc(buf, inlj_out_cap);
        if (got != inlj_out_cap) {
            printf("[IndexNestedLoopJoinOperator][ERROR][ensureBuffers]: alloc out failed\n");
            inlj_out = NULL;
            inlj_out_cap = 0;
            return false;
        }
        inlj_out = buf;
    }
    return true;
}

/**
 * @brief Convert an inner table record pointer into the packed tuple layout used by operators.
 *
 * @details HashIndex lookup returns a pointer to the physical table record.  That record may
 * include internal layout details such as column offsets managed by RowTable.  This method
 * copies only user columns, in catalog order, into inlj_inner_tuple so it can be concatenated
 * with an outer operator tuple and described by the planner's packed schema.
 *
 * @param rec_ptr Physical record pointer returned by the inner hash index.
 * @return true when all inner columns are copied successfully; false on NULL input or invalid catalog metadata.
 */
bool IndexNestedLoopJoinOperator::packInnerRecord(void *rec_ptr) {
    // 把索引查到的“内表物理记录”转换成执行器使用的 packed tuple
    // HashIndex::lookup() 返回的是表内部记录指针
    // 但执行器上层期望的格式是：[col0][col1][col2]...
    if (rec_ptr == NULL || inlj_inner_table == NULL || inlj_inner_tuple == NULL)
        return false;
    memset(inlj_inner_tuple, 0, (size_t)inlj_inner_cap);

    std::vector<int64_t> &cols = inlj_inner_table->getColumns();
    int64_t outPos = 0;
    for (unsigned int i = 0; i < cols.size(); i++) {
        Column *col = (Column *)g_catalog.getObjById(cols[i]);
        if (col == NULL || col->getOtype() != COLUMN)
            return false;
        BasicType *dt = col->getDataType();
        if (dt == NULL)
            return false;
        int64_t sz = dt->getTypeSize();
        memcpy(inlj_inner_tuple + outPos, ((char *)rec_ptr) + col->getCoffset(), (size_t)sz);
        outPos += sz;
    }
    return true;
}

/**
 * @brief Initialize the indexed join and allocate reusable buffers.
 *
 * @details The outer child is initialized first.  The inner tuple length is computed by summing
 * inner table column type sizes, then an inner packing buffer is allocated.  The operator also
 * verifies that the hash index is a single-column index and derives the key byte length from
 * that column.  Finally, outer iteration state and the output buffer are prepared.
 *
 * @return true when the operator is ready to probe the inner index; false on invalid inputs,
 *         child initialization failure, unsupported index shape, bad metadata, or allocation failure.
 */
bool IndexNestedLoopJoinOperator::init() {
    if (inlj_outer == NULL || inlj_inner_table == NULL || inlj_inner_index == NULL) {
        printf("[IndexNestedLoopJoinOperator][ERROR][init]: NULL input\n");
        return false;
    }
    // 初始化外层算子
    if (!inlj_outer->init()) {
        printf("[IndexNestedLoopJoinOperator][ERROR][init]: outer init failed\n");
        return false;
    }

    // inner packed tuple length
    // 计算内表 packed tuple 的长度
    inlj_inner_len = 0;
    std::vector<int64_t> &cols = inlj_inner_table->getColumns();
    for (unsigned int i = 0; i < cols.size(); i++) {
        Column *col = (Column *)g_catalog.getObjById(cols[i]);
        if (col == NULL || col->getOtype() != COLUMN)
            return false;
        BasicType *dt = col->getDataType();
        if (dt == NULL)
            return false;
        inlj_inner_len += dt->getTypeSize();
    }
    // 为 inlj_inner_tuple 分配缓冲区
    inlj_inner_cap = roundUpPow2(inlj_inner_len);
    if (inlj_inner_tuple != NULL && inlj_inner_cap > 0) {
        g_memory.free(inlj_inner_tuple, inlj_inner_cap);
        inlj_inner_tuple = NULL;
    }
    char *buf = NULL;
    int64_t got = g_memory.alloc(buf, inlj_inner_cap);
    if (got != inlj_inner_cap) {
        printf("[IndexNestedLoopJoinOperator][ERROR][init]: alloc inner tuple failed\n");
        inlj_inner_tuple = NULL;
        inlj_inner_cap = 0;
        return false;
    }
    inlj_inner_tuple = buf;
    memset(inlj_inner_tuple, 0, (size_t)inlj_inner_cap);

    // Derive key length from inner index key (assume single-column key)
    // 从内表索引中获取 key 信息
    Key &k = inlj_inner_index->getIKey();
    // 检查索引是否是单列索引（只支持单列 HashIndex）
    if (k.getKey().size() != 1) {
        printf("[IndexNestedLoopJoinOperator][ERROR][init]: only support single-column hash index\n");
        return false;
    }
    Column *keyCol = (Column *)g_catalog.getObjById(k.getKey()[0]);
    if (keyCol == NULL)
        return false;
    BasicType *keyType = keyCol->getDataType();
    if (keyType == NULL)
        return false;
    inlj_key_len = keyType->getTypeSize();

    // 重置当前 outer tuple 和索引查找状态
    inlj_end = false;
    inlj_cur_outer = NULL;
    inlj_cur_outer_len = inlj_outer->getOutputLen();
    inlj_info_valid = false;
    inlj_match_rec = NULL;

    // 调用 ensureBuffers() 准备输出缓冲区
    if (!ensureBuffers(inlj_cur_outer_len))
        return false;
    return true;
}

/**
 * @brief Produce the next result of the index nested-loop join.
 *
 * @details For the current outer tuple, the method continues an existing hash-index lookup
 * cursor and emits every matching inner record.  When no more records match that outer key, it
 * advances the outer child, initializes a fresh lookup state with HashIndex::set_ls(), and loops
 * back to fetch the first match.  The emitted row is [outer tuple][packed inner tuple].
 *
 * @return true when getOutput() contains a joined tuple; false when the outer stream is exhausted
 *         or a packing/allocation error occurs.
 */
bool IndexNestedLoopJoinOperator::getNext() {
    if (inlj_end)
        return false;

    while (1) {
        // 当前 outer tuple 还有索引匹配结果
        if (inlj_info_valid) {
            const char *keyPtr = inlj_cur_outer + inlj_outer_key_off;
            void *rec = NULL;
            if (inlj_inner_index->lookup((void *)keyPtr, &inlj_info, rec)) {
                if (!packInnerRecord(rec)) {
                    printf("[IndexNestedLoopJoinOperator][ERROR][getNext]: pack inner failed\n");
                    inlj_end = true;
                    return false;
                }
                if (!ensureBuffers(inlj_cur_outer_len)) {
                    inlj_end = true;
                    return false;
                }
                memcpy(inlj_out, inlj_cur_outer, (size_t)inlj_cur_outer_len);
                memcpy(inlj_out + inlj_cur_outer_len, inlj_inner_tuple, (size_t)inlj_inner_len);
                return true;
            }
            // no more matches for this outer tuple
            inlj_info_valid = false; // 没有更多匹配，准备换下一条 outer tuple
        }

        // 需要读取新的 outer tuple
        // advance outer
        if (!inlj_outer->getNext()) {
            inlj_end = true;
            return false;
        }
        inlj_cur_outer = inlj_outer->getOutput();
        inlj_cur_outer_len = inlj_outer->getOutputLen();
        if (inlj_cur_outer == NULL)
            continue;
        if (!ensureBuffers(inlj_cur_outer_len)) {
            inlj_end = true;
            return false;
        }

        const char *keyPtr = inlj_cur_outer + inlj_outer_key_off;
        memset(&inlj_info, 0, sizeof(inlj_info));
        // 用 outer key 初始化索引查找状态
        inlj_inner_index->set_ls((void *)keyPtr, NULL, &inlj_info);
        inlj_info_valid = true;
        // loop to fetch first match
    }
}

/**
 * @brief Report whether the indexed nested-loop join has reached end-of-stream.
 *
 * @return true after initialization failure, close(), or outer-stream exhaustion; otherwise false.
 */
bool IndexNestedLoopJoinOperator::isEnd() {
    return inlj_end;
}

/**
 * @brief Return the current joined tuple buffer.
 *
 * @return Pointer to inlj_out after a successful getNext(), or NULL if no tuple is available.
 */
char *IndexNestedLoopJoinOperator::getOutput() {
    return inlj_out;
}

/**
 * @brief Return the byte length of the current joined tuple.
 *
 * @return Byte length of [outer tuple][packed inner tuple].
 */
int64_t IndexNestedLoopJoinOperator::getOutputLen() {
    return inlj_out_len;
}

/**
 * @brief Release resources owned by the index nested-loop join.
 *
 * @details Frees the reusable joined-output buffer and inner packing buffer, resets the current
 * index cursor state, marks the operator ended, and closes the outer child.  The inner table and
 * hash index are catalog-owned objects and are not freed here.
 *
 * @return Always true.
 */
bool IndexNestedLoopJoinOperator::close() {
    if (inlj_out != NULL && inlj_out_cap > 0) {
        g_memory.free(inlj_out, inlj_out_cap);
    }
    inlj_out = NULL;
    inlj_out_cap = 0;
    inlj_out_len = 0;

    if (inlj_inner_tuple != NULL && inlj_inner_cap > 0) {
        g_memory.free(inlj_inner_tuple, inlj_inner_cap);
    }
    inlj_inner_tuple = NULL;
    inlj_inner_cap = 0;
    inlj_inner_len = 0;

    // 清空 HashIndex 查找状态 inlj_info
    inlj_end = true;
    inlj_cur_outer = NULL;
    inlj_cur_outer_len = 0;
    inlj_info_valid = false;
    memset(&inlj_info, 0, sizeof(inlj_info));

    // 关闭外层子算子
    if (inlj_outer != NULL)
        inlj_outer->close();
    return true;
}

//============================================================ END OWNED BY B (FilterProject/Join) ==================================================================

//============================================================ BEGIN OWNED BY A (Executor / ResultTable) ============================================================

//----------------------------------------------------------------------
// Executor
//----------------------------------------------------------------------

Executor::Executor() {
    current_query = NULL;       // 当前正在执行的查询
    e_root = NULL;              // 执行树根节点
    e_inited = false;           // 是否已经初始化执行树
    e_eval_pred = true;         // 是否在执行树中评估谓词
    e_ops.clear();              // 清除执行树节点列表
    e_schema.clear();           // 清除执行树输出 schema
    e_tuple_len = 0;            // 执行树输出 tuple 长度
    e_select_idx.clear();       // 清除 select 列在输出 schema 中的列索引
    e_pred.clear();             // 清除执行树谓词
    e_result_col_types = NULL;  // 执行树输出结果列的数据类型列表
    e_result_col_types_cap = 0; // 执行树输出结果列的数据类型列表容量
}

Executor::~Executor() {
    close();
}

//=========================================================================
// 计算在表 t 中，列名为 colName 的列在该表的 packed tuple 中的 offset（字节偏移）
//==========================================================================
static int64_t computePackedOffsetInTable(Table *t, const char *colName) {
    if (t == NULL || colName == NULL)
        return -1;
    int64_t off = 0;
    std::vector<int64_t> &cols = t->getColumns();
    for (unsigned int i = 0; i < cols.size(); i++) {
        Column *c = (Column *)g_catalog.getObjById(cols[i]);
        if (c == NULL || c->getOtype() != COLUMN)
            return -1;
        BasicType *dt = c->getDataType();
        if (dt == NULL)
            return -1;
        if (isQualifiedSuffixMatch(colName, c->getOname())) {
            return off;
        }
        off += dt->getTypeSize();
    }
    return -1;
}

//=========================================================================
// 在表列表 tables 中，寻找列名为 colName 的列所属的表的索引（table index）。如果找到了，返回该表在 tables 中的索引；
//==========================================================================

static int findTableIdxForColumnName(Table *tables[], int tnum, const char *colName) {
    if (colName == NULL)
        return -1;
    for (int ti = 0; ti < tnum; ti++) {
        std::vector<int64_t> &cols = tables[ti]->getColumns();
        for (unsigned int i = 0; i < cols.size(); i++) {
            Column *c = (Column *)g_catalog.getObjById(cols[i]);
            if (c == NULL || c->getOtype() != COLUMN)
                continue;
            if (isQualifiedSuffixMatch(colName, c->getOname()))
                return ti;
        }
    }
    return -1;
}

// 在指定表 t 中查找“单列哈希索引（single-column hash index）”，并且该索引对应的列 OID 必须等于 colOid.
static HashIndex *findSingleColHashIndex(Table *t, int64_t colOid) {
    if (t == NULL)
        return NULL;
    std::vector<int64_t> &idxs = t->getIndexs();
    for (unsigned int i = 0; i < idxs.size(); i++) {
        Index *ix = (Index *)g_catalog.getObjById(idxs[i]);
        if (ix == NULL || ix->getOtype() != INDEX)
            continue;
        if (ix->getIType() != HASHINDEX)
            continue;
        Key &k = ix->getIKey();
        if (k.getKey().size() != 1)
            continue;
        if (k.getKey()[0] == colOid)
            return (HashIndex *)ix;
    }
    return NULL;
}

//=====================================================================
// SQL执行器+查询计划生成器，负责把SelectQuery翻译成一个Operator树，并执行这个树来得到结果。
//=====================================================================

int Executor::exec(SelectQuery *query, ResultTable *result) {

    if (result == NULL) {
        printf("[Executor][ERROR][exec]: result is NULL\n");
        return -1;
    }

    if (query != NULL) {
        // 查询初始化
        close();
        current_query = query;

        //==============================================
        // from子句解析
        //==============================================
        int tnum = current_query->from_number;
        if (tnum <= 0) {
            printf("[Executor][ERROR][exec]: empty FROM\n");
            return -2;
        }
        if (tnum > 4)
            tnum = 4;

        Table *tables[4] = {NULL, NULL, NULL, NULL};

        //-------- 根据表名从catalog中获取表对象指针------------
        for (int i = 0; i < tnum; i++) {
            Object *o = g_catalog.getObjByName(current_query->from_table[i].name);
            if (o == NULL || o->getOtype() != TABLE) {
                printf("[Executor][ERROR][exec]: table not found: %s\n", current_query->from_table[i].name);
                return -3;
            }
            tables[i] = (Table *)o;
        }

        //==============================================
        // 初始化执行计划
        //==============================================

        // used_cond[i] 表示某个连接条件（join condition）是否已经被用来连接了一个表到当前的已连接表集合（included set）。
        bool used_cond[4] = {false, false, false, false};

        // 第一张表作为执行树根节点
        // 创建第一张表的 TableScanOperator
        Operator *op = new TableScanOperator(tables[0]);
        e_ops.push_back(op);

        //==============================================
        // 构造当前执行树输出 Schema
        //==============================================

        // ------- 把table[0]的所有列加入执行树输出 schema -----------
        e_schema.clear();
        e_tuple_len = 0;
        {
            std::vector<int64_t> &cols = tables[0]->getColumns();
            for (unsigned int ci = 0; ci < cols.size(); ci++) {
                Column *c = (Column *)g_catalog.getObjById(cols[ci]);
                if (c == NULL || c->getOtype() != COLUMN)
                    return -4;
                BasicType *dt = c->getDataType();
                if (dt == NULL)
                    return -4;
                SchemaCol sc;
                memset(&sc, 0, sizeof(sc));
                strncpy(sc.name, c->getOname(), sizeof(sc.name) - 1);
                sc.type = dt;
                sc.offset = e_tuple_len;
                sc.len = dt->getTypeSize();
                sc.aggr = NONE_AM;
                e_schema.push_back(sc);
                e_tuple_len += sc.len;
            }
        }

        //==============================================
        // 基于连接条件（join conditions）构造连接操作符，并把其他表的列加入执行树输出 schema
        //==============================================

        bool included[4] = {false, false, false, false};
        included[0] = true;

        int included_cnt = 1;
        while (included_cnt < tnum) {

            //---------------  在当前的已连接表集合（included set）和剩余的表之间，寻找一个连接条件（join condition）来连接一张新表到执行树上。---------------
            int best_ti = -1;
            int best_cond = -1;
            const char *best_outer_col = NULL;
            const char *best_inner_col = NULL;
            bool best_has_index = false;

            int whereN = current_query->where.condition_num;
            if (whereN < 0)
                whereN = 0;
            if (whereN > 4)
                whereN = 4;
            for (int ci = 0; ci < whereN; ci++) {
                if (used_cond[ci])
                    continue;
                Condition &cond = current_query->where.condition[ci];
                int lt = findTableIdxForColumnName(tables, tnum, cond.column.name);
                int rt = findTableIdxForColumnName(tables, tnum, cond.value);
                CompareMethod cm = normalizeCompare(cond.compare);
                if (lt < 0 || rt < 0 || lt == rt)
                    continue; // only consider column-column condition that compares two different tables
                if (!(cond.compare == LINK || cm == EQ))
                    continue; // only consider LINK/EQ for join conditions

                int cand_ti = -1;
                const char *outerCol = NULL;
                const char *innerCol = NULL;

                //--------------- 判断是否形成了一个连接边（join edge），即一个表在已连接表集合（included set）中，另一个表不在已连接表集合中。---------------
                if (included[lt] && !included[rt]) {
                    cand_ti = rt;
                    outerCol = cond.column.name;
                    innerCol = cond.value;
                } else if (included[rt] && !included[lt]) {
                    cand_ti = lt;
                    outerCol = cond.value;
                    innerCol = cond.column.name;
                } else {
                    continue;
                }

                // --------------- 判断内表 Join key 是否有单列哈希索引（single-column hash index）---------------

                bool hasIndex = false;
                int64_t innerColOid = -1;
                {
                    std::vector<int64_t> &cols = tables[cand_ti]->getColumns();
                    for (unsigned int k = 0; k < cols.size(); k++) {
                        Column *c = (Column *)g_catalog.getObjById(cols[k]);
                        if (c == NULL || c->getOtype() != COLUMN)
                            continue;
                        if (isQualifiedSuffixMatch(innerCol, c->getOname())) {
                            innerColOid = c->getOid();
                            break;
                        }
                    }
                }
                if (innerColOid >= 0) {
                    hasIndex = (findSingleColHashIndex(tables[cand_ti], innerColOid) != NULL);
                }

                //--------------- 优先选择有索引的 Join ---------------
                if (best_ti < 0 || (hasIndex && !best_has_index)) {
                    best_ti = cand_ti;
                    best_cond = ci;
                    best_outer_col = outerCol;
                    best_inner_col = innerCol;
                    best_has_index = hasIndex;
                }
            }

            if (best_ti < 0 || best_cond < 0) {
                // No join edge available from current joined set.
                for (int ti = 0; ti < tnum; ti++) {
                    if (!included[ti]) {
                        printf("[Executor][ERROR][exec]: cannot find join condition to connect table %s\n", tables[ti]->getOname());
                        break;
                    }
                }
                return -5;
            }

            int ti = best_ti;
            Table *nextT = tables[ti];
            const char *outerCol = best_outer_col;
            const char *innerCol = best_inner_col;
            used_cond[best_cond] = true;

            // --------------- 在当前执行树输出 schema 中找到 outer join key 的 offset 和 type --------------------
            int outerSchemaIdx = -1;
            for (size_t si = 0; si < e_schema.size(); si++) {
                if (isQualifiedSuffixMatch(outerCol, e_schema[si].name)) {
                    outerSchemaIdx = (int)si;
                    break;
                }
            }
            if (outerSchemaIdx < 0) {
                printf("[Executor][ERROR][exec]: outer join column not found in schema: %s\n", outerCol);
                return -6;
            }
            int64_t outerKeyOff = e_schema[outerSchemaIdx].offset;
            BasicType *keyType = e_schema[outerSchemaIdx].type;

            //--------------- 在内表中找到 inner join key 的 offset --------------------
            int64_t innerKeyOff = computePackedOffsetInTable(nextT, innerCol);
            if (innerKeyOff < 0) {
                printf("[Executor][ERROR][exec]: inner join column not found: %s\n", innerCol);
                return -7;
            }

            //--------------- 在内表中找到 inner join key 的 oid 并进行类型检查 --------------------
            int64_t innerColOid = -1;
            {
                std::vector<int64_t> &cols = nextT->getColumns();
                for (unsigned int ci = 0; ci < cols.size(); ci++) {
                    Column *c = (Column *)g_catalog.getObjById(cols[ci]);
                    if (c == NULL || c->getOtype() != COLUMN)
                        continue;
                    if (isQualifiedSuffixMatch(innerCol, c->getOname())) {
                        innerColOid = c->getOid();
                        // also sanity check type
                        BasicType *dt = c->getDataType();
                        if (dt == NULL || keyType == NULL ||
                            dt->getTypeCode() != keyType->getTypeCode() ||
                            dt->getTypeSize() != keyType->getTypeSize()) {
                            printf("[Executor][ERROR][exec]: join key type mismatch\n");
                            return -8;
                        }
                        break;
                    }
                }
            }
            if (innerColOid < 0)
                return -9;

            //-------------------- 构造连接操作符 --------------------
            HashIndex *hix = findSingleColHashIndex(nextT, innerColOid);
            if (hix != NULL) { // 有Hash Index，优先使用 Index Nested Loop Join
                Operator *j = new IndexNestedLoopJoinOperator(op, nextT, hix, outerKeyOff);
                e_ops.push_back(j);
                op = j;
            } else { // 没有Hash Index，使用 Hash Join
                Operator *rightScan = new TableScanOperator(nextT);
                e_ops.push_back(rightScan);
                Operator *j = new HashJoinOperator(op, rightScan, outerKeyOff, innerKeyOff, keyType);
                e_ops.push_back(j);
                op = j;
            }

            included[ti] = true;
            included_cnt++;

            // -------------- Join后新表的列加入执行树输出 schema -------------------
            std::vector<int64_t> &cols = nextT->getColumns();
            for (unsigned int ci = 0; ci < cols.size(); ci++) {
                Column *c = (Column *)g_catalog.getObjById(cols[ci]);
                if (c == NULL || c->getOtype() != COLUMN)
                    return -10;
                BasicType *dt = c->getDataType();
                if (dt == NULL)
                    return -10;
                SchemaCol sc;
                memset(&sc, 0, sizeof(sc));
                strncpy(sc.name, c->getOname(), sizeof(sc.name) - 1);
                sc.type = dt;
                sc.offset = e_tuple_len;
                sc.len = dt->getTypeSize();
                sc.aggr = NONE_AM;
                e_schema.push_back(sc);
                e_tuple_len += sc.len;
            }
        }

        //==============================================
        // 基于GROUP BY和AGGR信息构造聚合操作符，并把聚合结果列加入执行树输出 schema
        //==============================================

        bool has_groupby = (current_query->groupby_number > 0);
        bool has_aggr = false;
        {
            int sel = current_query->select_number;
            if (sel < 0)
                sel = 0;
            if (sel > 4)
                sel = 4;
            for (int i = 0; i < sel; i++) {
                if (current_query->select_column[i].aggregate_method != NONE_AM) {
                    has_aggr = true;
                    break;
                }
            }
        }

        //==============================================
        // 如果有GROUP BY或AGGR，则构造GroupByAggrOperator，并且在它上面加一个FilterProjectOperator来处理WHERE和HAVING中的残余条件（residual conditions）。
        // 否则WHERE中的条件都可以在连接阶段处理掉，直接把当前执行树
        // 的输出作为最终结果输出。
        //==============================================

        if (has_groupby || has_aggr) {
            // ----------- Build WHERE residual conditions (excluding join edges used for connecting plan) ---------------
            Conditions resid;
            memset(&resid, 0, sizeof(resid));
            resid.condition_num = 0;
            int cnum = current_query->where.condition_num;
            if (cnum < 0)
                cnum = 0;
            if (cnum > 4)
                cnum = 4;
            for (int ci = 0; ci < cnum; ci++) {
                if (used_cond[ci])
                    continue; // skip join conditions used for plan construction
                if (resid.condition_num >= 4)
                    break;
                resid.condition[resid.condition_num++] = current_query->where.condition[ci];
            }

            // --------------- WHERE filter before aggregation --------------------------
            Operator *wf = new FilterProjectOperator(op, e_schema, resid);
            e_ops.push_back(wf);
            op = wf;

            // --------------------------- GROUP BY + AGGR ---------------------------------------
            int gnum = current_query->groupby_number;
            if (gnum < 0)
                gnum = 0;
            if (gnum > 4)
                gnum = 4;
            int sel = current_query->select_number;
            if (sel < 0)
                sel = 0;
            if (sel > 4)
                sel = 4;

            GroupByAggrOperator *gb = new GroupByAggrOperator(op, e_schema,
                                                              current_query->groupby, gnum,
                                                              current_query->select_column, sel);
            e_ops.push_back(gb);
            op = gb;

            // -------------------- 更新输出 Schema和tuple长度 --------------------
            e_schema = gb->getOutputSchema();
            e_tuple_len = 0;
            for (size_t si = 0; si < e_schema.size(); si++) {
                e_tuple_len += e_schema[si].len;
            }

            // -------------------- HAVING filter after aggregation --------------------
            int hnum = current_query->having.condition_num;
            if (hnum < 0)
                hnum = 0;
            if (hnum > 4)
                hnum = 4;
            if (hnum > 0) {
                Operator *hf = new FilterProjectOperator(op, e_schema, current_query->having);
                e_ops.push_back(hf);
                op = hf;
            }

            e_root = op;
            e_inited = false;
            e_eval_pred = false; // HAVING条件已经在HAVING filter里处理掉了，ORDER BY里也不需要再处理了，e_eval_pred置false，后续不再编译WHERE残余条件，直接在ORDER BY里用FilterProjectOperator处理WHERE残余条件。
            e_pred.clear();
        }
        //============================================================
        // 如果没有GROUP BY和AGGR，直接在连接阶段处理掉WHERE条件（如果有的话）。
        //=============================================================
        else {
            e_root = op;
            e_inited = false;
            e_eval_pred = true; // 连接阶段没有处理掉的WHERE条件（如果有的话）作为残余条件在执行树最后通过FilterProjectOperator处理掉。

            //------------------- compile residual predicates (including column-column filters) -------------------
            e_pred.clear();
            int cnum = current_query->where.condition_num;
            if (cnum < 0)
                cnum = 0;
            if (cnum > 4)
                cnum = 4;
            for (int ci = 0; ci < cnum; ci++) {
                if (used_cond[ci])
                    continue;
                Condition &cond = current_query->where.condition[ci];

                //--------------- compile left column -------------------
                RequestColumn lhsRc = cond.column;
                lhsRc.aggregate_method = NONE_AM;
                int leftIdx = findSchemaIndexByReq(e_schema, lhsRc);
                if (leftIdx < 0) {
                    printf("[Executor][ERROR][exec]: predicate left column not found: %s\n", cond.column.name);
                    return -11;
                }

                ExecPred ep;
                memset(&ep, 0, sizeof(ep));
                ep.left_idx = leftIdx;
                ep.cmp = normalizeCompare(cond.compare);
                if (ep.cmp <= NONE_CM || ep.cmp >= MAX_CM)
                    return -12;

                // ----------判断右侧是常量还是列，如果是列，继续编译；如果是常量，格式化成二进制并存储在ep里。----------
                RequestColumn rhsRc;
                memset(&rhsRc, 0, sizeof(rhsRc));
                strncpy(rhsRc.name, cond.value, sizeof(rhsRc.name) - 1);
                rhsRc.aggregate_method = NONE_AM;
                int rightIdx = findSchemaIndexByReq(e_schema, rhsRc);
                bool rhsIsCol = (cond.compare == LINK) || (rightIdx >= 0);
                ep.right_is_col = rhsIsCol;
                ep.right_idx = rightIdx;
                ep.right_const = NULL;
                ep.right_const_cap = 0;
                ep.right_const_len = 0;

                if (rhsIsCol) {
                    if (rightIdx < 0) {
                        printf("[Executor][ERROR][exec]: predicate right column not found: %s\n", cond.value);
                        return -13;
                    }
                    if (e_schema[leftIdx].type->getTypeCode() != e_schema[rightIdx].type->getTypeCode() ||
                        e_schema[leftIdx].type->getTypeSize() != e_schema[rightIdx].type->getTypeSize()) {
                        printf("[Executor][ERROR][exec]: predicate type mismatch\n");
                        return -14;
                    }
                } else {
                    BasicType *dt = e_schema[leftIdx].type;
                    int64_t len = dt->getTypeSize();
                    int64_t cap = roundUpPow2(len);
                    char *buf = NULL;
                    int64_t got = g_memory.alloc(buf, cap);
                    if (got != cap)
                        return -15;
                    memset(buf, 0, (size_t)cap);
                    if (dt->formatBin(buf, (void *)cond.value) < 0) {
                        printf("[Executor][ERROR][exec]: formatBin failed for %s = %s\n", e_schema[leftIdx].name, cond.value);
                        g_memory.free(buf, cap);
                        return -16;
                    }
                    ep.right_const = buf;
                    ep.right_const_cap = cap;
                    ep.right_const_len = len;
                }
                e_pred.push_back(ep);
            }
        }

        //===============================================================================
        // 基于ORDER BY信息构造排序操作符，如果之前还有WHERE残余条件没有处理掉，则在它前面加一个FilterProjectOperator来处理WHERE残余条件。
        //===============================================================================
        int onum = current_query->orderby_number;
        if (onum < 0)
            onum = 0;
        if (onum > 4)
            onum = 4;
        if (onum > 0) {
            //--------------- compile residual predicates before order by -------------------
            if (e_eval_pred) {
                Conditions resid;
                memset(&resid, 0, sizeof(resid));
                resid.condition_num = 0;
                int cnum = current_query->where.condition_num;
                if (cnum < 0)
                    cnum = 0;
                if (cnum > 4)
                    cnum = 4;
                for (int ci = 0; ci < cnum; ci++) {
                    if (used_cond[ci])
                        continue;
                    if (resid.condition_num >= 4)
                        break;
                    resid.condition[resid.condition_num++] = current_query->where.condition[ci];
                }
                Operator *wf2 = new FilterProjectOperator(e_root, e_schema, resid); // where filter for residual conditions before order by
                e_ops.push_back(wf2);
                e_root = wf2;
                e_inited = false;
                e_eval_pred = false;
                // free compiled constants and clear
                for (size_t i = 0; i < e_pred.size(); i++) {
                    if (!e_pred[i].right_is_col && e_pred[i].right_const != NULL && e_pred[i].right_const_cap > 0) {
                        g_memory.free(e_pred[i].right_const, e_pred[i].right_const_cap);
                    }
                    e_pred[i].right_const = NULL;
                    e_pred[i].right_const_cap = 0;
                    e_pred[i].right_const_len = 0;
                }
                e_pred.clear();
            }

            Operator *ob = new OrderByOperator(e_root, e_schema, current_query->orderby, onum);
            e_ops.push_back(ob);
            e_root = ob;
            e_inited = false;
        }

        //===============================================================================
        // SELECT投影列解析
        //===============================================================================
        int sel = current_query->select_number;
        if (sel < 0)
            sel = 0;
        if (sel > 4)
            sel = 4;
        e_select_idx.clear();
        if (sel == 0) {
            // default: project first up to 4 columns of schema
            sel = (e_schema.size() > 4 ? 4 : (int)e_schema.size());
            for (int i = 0; i < sel; i++)
                e_select_idx.push_back(i);
        } else {
            for (int i = 0; i < sel; i++) {
                int idx = findSchemaIndexByReq(e_schema, current_query->select_column[i]);
                if (idx < 0) {
                    printf("[Executor][ERROR][exec]: select column not found: %s\n", current_query->select_column[i].name);
                    return -17;
                }
                e_select_idx.push_back(idx);
            }
        }

        //===============================================================================
        // 根据SELECT投影列信息准备结果表的列类型信息，
        // 以便后续ResultTable初始化和结果写入时使用。
        //===============================================================================

        //--------------- 如果之前已经准备过结果表列类型信息了，先free掉 ---------------
        if (e_result_col_types != NULL && e_result_col_types_cap > 0) {
            g_memory.free((char *)e_result_col_types, e_result_col_types_cap);
        }
        e_result_col_types = NULL;
        e_result_col_types_cap = 0;

        int64_t need = (int64_t)(sizeof(BasicType *) * (int)e_select_idx.size());
        int64_t cap = roundUpPow2(need);
        char *buf = NULL;
        int64_t got = g_memory.alloc(buf, cap);
        if (got != cap)
            return -18;
        e_result_col_types = (BasicType **)buf;
        e_result_col_types_cap = cap;
        memset(e_result_col_types, 0, (size_t)cap);

        for (size_t i = 0; i < e_select_idx.size(); i++) { // 填充结果表列类型信息
            e_result_col_types[i] = e_schema[e_select_idx[i]].type;
        }
    }

    // ======================================================================
    // 初始化ResultTable，并执行执行树来产生结果。
    // ======================================================================
    if (e_result_col_types == NULL || e_select_idx.empty()) {
        return 0;
    }
    if (result->init(e_result_col_types, (int)e_select_idx.size(), 1024) < 0) {
        printf("[Executor][ERROR][exec]: result init failed\n");
        return -19;
    }

    if (e_root == NULL)
        return 0;
    if (!e_inited) {
        if (!e_root->init()) {
            printf("[Executor][ERROR][exec]: root init failed\n");
            return -20;
        }
        e_inited = true;
    }

    result->row_number = 0;

    //----------------- helper function ：给定一条 tuple 和一个谓词（Predicate），判断该记录是否满足条件。-----------------------------
    auto evalPred = [&](const ExecPred &p, const char *tuple) -> bool {
        const SchemaCol &lhs = e_schema[p.left_idx];
        const char *lhsPtr = tuple + lhs.offset;
        const char *rhsPtr = NULL;
        if (p.right_is_col) {
            rhsPtr = tuple + e_schema[p.right_idx].offset;
        } else {
            rhsPtr = p.right_const;
        }
        switch (p.cmp) {
        case LT:
            return lhs.type->cmpLT((void *)lhsPtr, (void *)rhsPtr);
        case LE:
            return lhs.type->cmpLE((void *)lhsPtr, (void *)rhsPtr);
        case EQ:
            return lhs.type->cmpEQ((void *)lhsPtr, (void *)rhsPtr);
        case NE:
            return !lhs.type->cmpEQ((void *)lhsPtr, (void *)rhsPtr);
        case GT:
            return lhs.type->cmpGT((void *)lhsPtr, (void *)rhsPtr);
        case GE:
            return lhs.type->cmpGE((void *)lhsPtr, (void *)rhsPtr);
        default:
            return false;
        }
    };

    //=============================================================================
    // Volcano Iterator 执行
    // 通过不断调用 getNext() 来获取执行树的输出 tuple，直到输出 tuple 满足结果表的行容量或者执行树输出 exhausted。
    //=============================================================================

    int produced = 0;
    while (produced < result->row_capacity && e_root->getNext()) {
        char *tuple = e_root->getOutput();
        if (tuple == NULL)
            continue;

        if (e_eval_pred) {
            bool ok = true;
            for (size_t pi = 0; pi < e_pred.size(); pi++) {
                if (!evalPred(e_pred[pi], tuple)) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                continue;
        }

        // project into ResultTable row
        int row = result->row_number;
        for (size_t si = 0; si < e_select_idx.size(); si++) {
            int schemaIdx = e_select_idx[si];
            result->writeRC(row, (int)si, tuple + e_schema[schemaIdx].offset);
        }
        result->row_number++;
        produced++;
    }

    if (produced == 0) {
        // exhausted
        return 0;
    }
    return produced;
}

//==================================================================================
// 销毁当前执行计划（执行树），释放所有相关资源，把 Executor 恢复到“未执行任何查询”的初始状态。
//==================================================================================
int Executor::close() {
    if (e_root != NULL) { // 递归关闭整个执行树，释放所有执行树相关资源
        e_root->close();
    }
    for (size_t i = 0; i < e_ops.size(); i++) { // 删除执行树上所有操作符对象
        delete e_ops[i];
    }
    e_ops.clear();
    e_root = NULL;
    e_inited = false;
    e_eval_pred = true;

    for (size_t i = 0; i < e_pred.size(); i++) { // 释放编译谓词时申请的常量内存，并清空编译谓词信息
        if (!e_pred[i].right_is_col && e_pred[i].right_const != NULL && e_pred[i].right_const_cap > 0) {
            g_memory.free(e_pred[i].right_const, e_pred[i].right_const_cap);
        }
        e_pred[i].right_const = NULL;
        e_pred[i].right_const_cap = 0;
        e_pred[i].right_const_len = 0;
    }
    e_pred.clear();

    e_schema.clear();
    e_tuple_len = 0;
    e_select_idx.clear();

    if (e_result_col_types != NULL && e_result_col_types_cap > 0) { // 释放准备结果表列类型信息时申请的内存，并清空相关信息
        g_memory.free((char *)e_result_col_types, e_result_col_types_cap);
    }
    e_result_col_types = NULL;
    e_result_col_types_cap = 0;

    current_query = NULL;
    return 0;
}

//============================================================================
// ResultTable初始化函数，负责根据执行树输出结果的列类型信息和预设的行容量来分配内存，并准备好相关信息以便后续写入结果。
//============================================================================
int ResultTable::init(BasicType *col_types[], int col_num, int64_t capicity) {
    // allow re-init safely
    if (buffer != NULL && buffer_size > 0) { // free之前的结果表数据内存（如果有的话）
        g_memory.free(buffer, buffer_size);
    }
    if (offset != NULL && offset_size > 0) { // free之前的结果表列offset内存（如果有的话）
        g_memory.free((char *)offset, offset_size);
    }
    buffer = NULL;
    buffer_size = 0;
    offset = NULL;
    offset_size = 0;

    column_type = col_types;
    column_number = col_num;
    row_length = 0;
    buffer_size = g_memory.alloc(buffer, capicity); // 分配结果表数据内存
    if (buffer_size != capicity) {
        printf("[ResultTable][ERROR][init]: buffer allocate error!\n");
        return -1;
    }
    int allocate_size = 1;
    int require_size = sizeof(int) * column_number;
    while (allocate_size < require_size)
        allocate_size = allocate_size << 1;
    while (allocate_size < (int)sizeof(void *))
        allocate_size = allocate_size << 1;
    char *p = NULL;
    offset_size = g_memory.alloc(p, allocate_size); // 分配结果表列offset内存
    offset = (int *)p;
    for (int ii = 0; ii < column_number; ii++) {
        offset[ii] = row_length;
        row_length += column_type[ii]->getTypeSize();
    }
    row_capacity = (int)(capicity / row_length);
    row_number = 0;
    return 0;
}

//============================================================================
// ResultTable打印函数，负责把结果表中的数据按照文本格式输出到控制台，
// 每列之间用制表符分隔，每行一条记录。
//============================================================================
int ResultTable::print(void) {
    int row = 0;
    int ii = 0;
    char buffer[1024];
    char *p = NULL;
    while (row < row_number) {
        for (; ii < column_number - 1; ii++) {
            p = getRC(row, ii);
            column_type[ii]->formatTxt(buffer, p);
            printf("%s\t", buffer);
        }
        p = getRC(row, ii);
        column_type[ii]->formatTxt(buffer, p);
        printf("%s\n", buffer);
        row++;
        ii = 0;
    }
    return row;
}

//============================================================================
// ResultTable dump函数，负责把结果表中的数据按照文本格式输出到指定文件，
// 每列之间用制表符分隔，每行一条记录。
//============================================================================
int ResultTable::dump(FILE *fp) {
    // write to file
    int row = 0;
    int ii = 0;
    char buffer[1024];
    char *p = NULL;
    while (row < row_number) {
        for (; ii < column_number - 1; ii++) {
            p = getRC(row, ii);
            column_type[ii]->formatTxt(buffer, p);
            fprintf(fp, "%s\t", buffer);
        }
        p = getRC(row, ii);
        column_type[ii]->formatTxt(buffer, p);
        fprintf(fp, "%s\n", buffer);
        row++;
        ii = 0;
    }
    return row;
}

// this include checks, may decrease its speed
char *ResultTable::getRC(int row, int column) {
    return buffer + row * row_length + offset[column];
}

int ResultTable::writeRC(int row, int column, void *data) {
    char *p = getRC(row, column);
    if (p == NULL)
        return 0;
    return column_type[column]->copy(p, data);
}

//============================================================================
// ResultTable销毁函数，负责释放结果表相关的内存资源，并把ResultTable恢复到“未初始化”的初始状态。
//============================================================================
int ResultTable::shut(void) {
    // free memory (idempotent)
    if (buffer != NULL && buffer_size > 0)
        g_memory.free(buffer, buffer_size);
    if (offset != NULL && offset_size > 0)
        g_memory.free((char *)offset, offset_size);

    buffer = NULL;
    buffer_size = 0;
    offset = NULL;
    offset_size = 0;
    column_type = NULL;
    column_number = 0;
    row_length = 0;
    row_number = 0;
    row_capacity = 0;
    return 0;
}

//============================================================ END OWNED BY A (Executor / ResultTable) ============================================================
