/**
 * @file    executor.h
 * @author  liugang(liugang@ict.ac.cn)
 * @version 0.1
 *
 * @section DESCRIPTION
 *
 * Definitions of the executor framework for AIMDB Lab3.
 *
 * The executor implements a Volcano/iterator-style query evaluation model.
 * A query plan is represented as a tree of Operator nodes; the root operator
 * is driven by Executor::exec(), which repeatedly calls getNext() until all
 * result rows have been produced or the ResultTable is full.
 *
 * Operator hierarchy:
 *   - TableScanOperator        : sequential full-table scan
 *   - FilterProjectOperator    : predicate evaluation + column projection
 *   - HashJoinOperator         : build-probe equi-join (no index)
 *   - IndexNestedLoopJoinOperator : index-accelerated equi-join
 *   - GroupByAggrOperator      : hash-based group-by + aggregation
 *   - OrderByOperator          : materialize-then-quicksort
 */

#ifndef _EXECUTOR_H
#define _EXECUTOR_H

#include "catalog.h"
#include "mymemory.h"

#include <string>
#include <unordered_map>
#include <vector>

class Operator;

/**
 * @brief Aggregation function type applied to a column.
 *
 * Used in SelectQuery::select_column and by GroupByAggrOperator to
 * identify how each output column is derived.
 */
enum AggregateMethod {
    NONE_AM = 0, /**< No aggregation; plain column reference. */
    COUNT,       /**< COUNT: number of rows in the group. */
    SUM,         /**< SUM: arithmetic sum of column values. */
    AVG,         /**< AVG: arithmetic mean of column values. */
    MAX,         /**< MAX: maximum column value in the group. */
    MIN,         /**< MIN: minimum column value in the group. */
    MAX_AM
};

/**
 * @brief Comparison operator used in WHERE / HAVING conditions.
 *
 * LINK is a legacy alias meaning column-column equality (same as EQ
 * when the right-hand side names another column instead of a literal).
 */
enum CompareMethod {
    NONE_CM = 0,
    LT,        /**< Strictly less than:    left <  right */
    LE,        /**< Less than or equal:    left <= right */
    EQ,        /**< Equal:                 left == right */
    NE,        /**< Not equal:             left != right */
    GT,        /**< Strictly greater than: left >  right */
    GE,        /**< Greater than or equal: left >= right */
    LINK,      /**< Column-column equality join condition (legacy). */
    MAX_CM
};

/**
 * @brief Describes one column in an intermediate packed-binary tuple.
 *
 * Used to build and navigate the schema of tuples that flow between
 * operators.  The @p aggr field distinguishes aggregation outputs that
 * share the same base column name (e.g. SUM(price) vs MAX(price)).
 */
struct SchemaCol {
    char name[128];     /**< Short column name (without table qualifier). */
    BasicType *type;    /**< Pointer to the column's runtime type object. */
    int64_t offset;     /**< Byte offset of this column within one tuple. */
    int64_t len;        /**< Byte length of this column (== type->getTypeSize()). */
    AggregateMethod aggr; /**< Aggregation method, or NONE_AM for plain columns. */
};

/**
 * @brief Identifies one column in a user-level query request.
 *
 * Combines a column name (possibly table-qualified, e.g. "orders.orderkey")
 * with an optional aggregation method for SELECT / GROUP BY / ORDER BY lists.
 */
struct RequestColumn {
    char name[128];                  /**< Column name, possibly table-qualified. */
    AggregateMethod aggregate_method; /**< Aggregation to apply, or NONE_AM. */
};

/**
 * @brief Identifies one table in the FROM clause.
 */
struct RequestTable {
    char name[128]; /**< Table name as stored in the catalog. */
};

/**
 * @brief One comparison predicate in a WHERE or HAVING clause.
 *
 * When @p compare == LINK, @p value names another column (column-column join).
 * Otherwise @p value is a string literal that will be converted to binary via
 * BasicType::formatBin().
 */
struct Condition {
    RequestColumn column;  /**< Left-hand-side column reference. */
    CompareMethod compare; /**< Comparison operator. */
    char value[128];       /**< Right-hand side: column name if LINK, else literal string. */
};

/**
 * @brief A conjunction (AND) of up to four conditions.
 *
 * All conditions in the array are combined with logical AND.
 * Only the first @p condition_num entries are valid.
 */
struct Conditions {
    int condition_num;       /**< Number of active conditions (0..4). */
    Condition condition[4];  /**< The active conditions, in order. */
};

/**
 * @brief Fully parsed representation of a SELECT statement.
 *
 * Filled in by the SQL parser before being handed to Executor::exec().
 * All arrays have a maximum capacity of 4; only the first *_number entries
 * are valid.
 */
class SelectQuery {
  public:
    int64_t database_id;             /**< ID of the target database in the catalog. */
    int select_number;               /**< Number of columns in the SELECT list. */
    RequestColumn select_column[4];  /**< SELECT list columns (up to 4). */
    int from_number;                 /**< Number of tables in the FROM clause. */
    RequestTable from_table[4];      /**< FROM tables (up to 4). */
    Conditions where;                /**< WHERE clause conditions (up to 4, ANDed). */
    int groupby_number;              /**< Number of GROUP BY keys. */
    RequestColumn groupby[4];        /**< GROUP BY key columns (up to 4). */
    Conditions having;               /**< HAVING clause conditions (up to 4, ANDed). */
    int orderby_number;              /**< Number of ORDER BY keys. */
    RequestColumn orderby[4];        /**< ORDER BY key columns (up to 4). */
};  // class SelectQuery

/**
 * @brief In-memory result table produced by Executor::exec().
 *
 * Rows are stored in a flat binary buffer (@p buffer) allocated from
 * @p g_memory.  Each row contains @p column_number fields packed
 * consecutively according to their types.  The @p offset array gives the
 * byte offset of each column within one row.
 *
 * Typical usage:
 * @code
 *   ResultTable rt;
 *   int n = executor.exec(&query, &rt);
 *   if (n > 0) rt.print();
 *   rt.shut();
 * @endcode
 */
class ResultTable {
  public:
    int column_number;       /**< Number of columns per row. */
    BasicType **column_type; /**< Type pointer for each column (not owned). */
    char *buffer;            /**< Flat row storage allocated from g_memory. */
    int64_t buffer_size;     /**< Allocated size of @p buffer in bytes (power of 2). */
    int row_length;          /**< Byte length of one packed row. */
    int row_number;          /**< Number of rows currently stored. */
    int row_capacity;        /**< Maximum number of rows that fit in @p buffer. */
    int *offset;             /**< Per-column byte offset within one row. */
    int offset_size;         /**< Allocated size of the @p offset array. */

    /**
     * @brief Initialize the ResultTable and allocate storage.
     *
     * Computes per-column offsets from @p col_types, allocates a buffer of
     * @p capacity bytes from @p g_memory, and sets @p row_capacity accordingly.
     * Safe to call multiple times; previous buffers are freed first.
     *
     * @param col_types Array of pointers to column type objects (not copied; must
     *                  remain valid until shut() is called).
     * @param col_num   Number of columns.
     * @param capacity  Buffer size in bytes (must be a power of 2; default 1024).
     * @retval  0  Success.
     * @retval -1  Memory allocation failed.
     */
    int init(BasicType *col_types[], int col_num, int64_t capacity = 1024);

    /**
     * @brief Return a pointer to the cell at (row, column).
     *
     * @param row    Zero-based row index (must be < row_number).
     * @param column Zero-based column index (must be < column_number).
     * @retval non-NULL Pointer to the cell's binary data.
     * @retval NULL     Invalid row or column index.
     */
    char* getRC(int row, int column);

    /**
     * @brief Write binary data into the cell at (row, column).
     *
     * Calls column_type[column]->copy() to perform the write, so the
     * source data must be in the column's native binary format.
     *
     * @param row    Zero-based row index.
     * @param column Zero-based column index.
     * @param data   Pointer to the source binary data to copy.
     * @retval >0  Number of bytes copied (success).
     * @retval  0  Failed (NULL destination or copy error).
     */
    int writeRC(int row, int column, void *data);

    /**
     * @brief Print all rows to stdout, columns separated by tabs.
     *
     * Each row is terminated with a newline.  Values are formatted as
     * human-readable text via BasicType::formatTxt().
     *
     * @retval Number of rows printed.
     */
    int print(void);

    /**
     * @brief Write all rows to a file in tab-separated text format.
     *
     * Identical layout to print(), but output goes to @p fp.
     *
     * @param fp  Open writable FILE pointer.
     * @retval Number of rows written.
     */
    int dump(FILE *fp);

    /**
     * @brief Free all memory owned by this ResultTable and reset fields.
     *
     * Returns @p buffer and @p offset memory to @p g_memory.
     * After shut(), the object may be re-initialised with init().
     *
     * @retval 0 Always succeeds.
     */
    int shut(void);
};  // class ResultTable

/**
 * @brief SQL query executor: translates a SelectQuery into an operator
 *        plan and drives its evaluation.
 *
 * Executor::exec() performs the following steps on first call with a new
 * query:
 *  1. Resolve table names via @p g_catalog.
 *  2. Build a left-deep join tree (IndexNestedLoopJoin if an inner-side
 *     hash index exists, otherwise HashJoin).
 *  3. Attach a GroupByAggrOperator when GROUP BY / aggregation is present,
 *     preceded by a WHERE FilterProjectOperator and followed by a HAVING
 *     FilterProjectOperator.
 *  4. Attach an OrderByOperator when ORDER BY is present.
 *  5. Resolve SELECT projection indices into the final schema.
 *
 * Subsequent calls with @p query == NULL re-use the existing plan to
 * fetch the next batch of rows (not currently used in the lab).
 */
class Executor {
  private:
    SelectQuery *current_query;

    Operator *e_root;
    bool e_inited;
    std::vector<Operator *> e_ops;

    std::vector<SchemaCol> e_schema;
    int64_t e_tuple_len;

    std::vector<int> e_select_idx;

    struct ExecPred {
        int left_idx;
        CompareMethod cmp;
        bool right_is_col;
        int right_idx;
        char *right_const;
        int64_t right_const_cap;
        int64_t right_const_len;
    };
    std::vector<ExecPred> e_pred;

    bool e_eval_pred;

    BasicType **e_result_col_types;
    int64_t e_result_col_types_cap;

  public:
    /**
     * @brief Execute a SELECT query and populate a ResultTable.
     *
     * On the first call, @p query must be non-NULL: the executor builds a
     * new operator plan, initialises it, and fills @p result up to its
     * row capacity.  On subsequent calls with @p query == NULL the existing
     * plan is reused to produce additional rows (pagination).
     *
     * @param query  Pointer to the parsed SelectQuery, or NULL to continue
     *               a previous query.
     * @param result Pre-allocated ResultTable that will receive the rows.
     *               The caller must call ResultTable::shut() when done.
     * @retval >0   Number of rows written to @p result.
     * @retval  0   Query produced no (more) rows.
     * @retval <0   Error; the error code indicates the failure stage.
     */
    virtual int exec(SelectQuery *query, ResultTable *result);

    /**
     * @brief Tear down the current operator plan and release all resources.
     *
     * Closes the root operator (which recursively closes children), deletes
     * all operator objects, frees compiled predicates and result-type arrays.
     * After close(), the executor is ready to accept a new query.
     *
     * @retval 0  Always succeeds.
     */
    virtual int close();

    /** @brief Construct an idle Executor with no active query. */
    Executor();

    /** @brief Destroy the Executor, implicitly calling close(). */
    virtual ~Executor();
};

//----------------------------------------------------------------------
// Operator base class
//----------------------------------------------------------------------

/**
 * @brief Abstract base class for all iterator-style query operators.
 *
 * Follows the Volcano/iterator model:
 *  -# Call init() once before consuming any tuples.
 *  -# Repeatedly call getNext(); it returns @p true when a new tuple is
 *     available and @p false when the stream is exhausted.
 *  -# After a successful getNext(), retrieve the current tuple with
 *     getOutput() / getOutputLen().
 *  -# Call close() to release resources when done.
 *
 * The pointer returned by getOutput() is owned by the operator and is
 * valid only until the next call to getNext() or close().
 */
class Operator {
  public:
    /** @brief Virtual destructor for safe polymorphic deletion. */
    virtual ~Operator() {}

    /**
     * @brief Initialise the operator and its children.
     *
     * Must be called exactly once before the first getNext().
     *
     * @retval true  Initialisation succeeded.
     * @retval false Initialisation failed (resources released internally).
     */
    virtual bool init() = 0;

    /**
     * @brief Advance to the next output tuple.
     *
     * After a successful return, getOutput() yields the current tuple.
     *
     * @retval true  A new tuple is available.
     * @retval false The stream is exhausted; no more tuples will be produced.
     */
    virtual bool getNext() = 0;

    /**
     * @brief Check whether the operator is exhausted without advancing it.
     *
     * @retval true  No more tuples are available.
     * @retval false At least one more getNext() call may succeed.
     */
    virtual bool isEnd() = 0;

    /**
     * @brief Return a pointer to the current output tuple.
     *
     * Valid only after a successful getNext() and until the next
     * getNext() or close() call.
     *
     * @retval non-NULL Pointer to the packed binary tuple.
     * @retval NULL     No current tuple (not yet advanced, or exhausted).
     */
    virtual char *getOutput() = 0;

    /**
     * @brief Return the byte length of one output tuple.
     *
     * This value is constant for the lifetime of the operator (i.e. all
     * tuples have the same fixed length).
     *
     * @retval >0 Byte length of one tuple.
     * @retval 0  Not yet initialised or no columns.
     */
    virtual int64_t getOutputLen() = 0;

    /**
     * @brief Release all resources held by this operator and its children.
     *
     * After close(), init() may be called again to restart the scan.
     *
     * @retval true  Resources released successfully.
     * @retval false Release encountered an error (rarely used).
     */
    virtual bool close() = 0;
};

//----------------------------------------------------------------------
// TableScanOperator
//----------------------------------------------------------------------

/**
 * @brief Sequential full-table scan operator.
 *
 * Iterates over all valid rows of a Table in rank order, producing one
 * packed binary tuple per row.  The tuple layout mirrors the table's
 * column order: all columns are concatenated without padding.
 *
 * Invalid or deleted rows (as reported by Table::select()) are silently
 * skipped.
 */
class TableScanOperator : public Operator {
  private:
    Table *ts_table;
    int64_t ts_next_rank;
    int64_t ts_total_rank;
    char *ts_out;
    int64_t ts_out_cap;
    int64_t ts_out_len;
    bool ts_end;

  public:
    /**
     * @brief Construct a scan operator over the given table.
     *
     * @param table  Pointer to the Table to scan (must remain valid for the
     *               lifetime of this operator).
     */
    explicit TableScanOperator(Table *table);

    /** @brief Destroy the operator, freeing the output buffer. */
    virtual ~TableScanOperator();

    /**
     * @brief Allocate the output buffer and reset the scan position.
     *
     * Computes the tuple byte length from the table's column types and
     * allocates a matching buffer from g_memory.
     *
     * @retval true  Initialisation succeeded.
     * @retval false Table pointer is NULL or memory allocation failed.
     */
    bool init() override;

    /**
     * @brief Advance to the next valid row.
     *
     * Calls Table::select() for each rank until a valid row is found or
     * the table is exhausted.
     *
     * @retval true  A new tuple is available in the output buffer.
     * @retval false All rows have been scanned.
     */
    bool getNext() override;

    /**
     * @brief Return true if the scan is exhausted.
     * @retval true  No more rows to scan.
     * @retval false More rows remain.
     */
    bool isEnd() override;

    /**
     * @brief Return a pointer to the current packed-binary tuple.
     * @retval non-NULL Current row in the output buffer.
     * @retval NULL     Not initialised or exhausted.
     */
    char *getOutput() override;

    /**
     * @brief Return the fixed byte length of one output tuple.
     * @retval >0 Sum of all column type sizes.
     * @retval 0  Not yet initialised.
     */
    int64_t getOutputLen() override;

    /**
     * @brief Free the output buffer and reset state.
     * @retval true Always.
     */
    bool close() override;
};

//----------------------------------------------------------------------
// GroupByAggrOperator
//----------------------------------------------------------------------

/**
 * @brief Hash-based GROUP BY with aggregation operator.
 *
 * Materialises all input tuples from its child, groups them by up to
 * four key columns using a hash map, and computes up to four aggregation
 * functions (COUNT, SUM, AVG, MAX, MIN) over numeric columns.
 *
 * Output tuple layout (fixed per instance):
 *   [ key_0 | key_1 | ... | agg_0 | agg_1 | ... ]
 *
 * The output schema is available via getOutputSchema() after construction.
 * SUM/COUNT output type is int64; AVG output type is float64.
 */
class GroupByAggrOperator : public Operator {
  private:
    Operator *gb_child;
    std::vector<SchemaCol> gb_in_schema;
    std::vector<SchemaCol> gb_out_schema;
    int64_t gb_out_len;

    struct KeyRef {
        int in_idx;
        int64_t off;
        int64_t len;
        BasicType *type;
    };
    std::vector<KeyRef> gb_keys;

    struct AggSpec {
        AggregateMethod method;
        int in_idx;
        int64_t off;
        int64_t len;
        BasicType *in_type;
        BasicType *out_type;
    };
    std::vector<AggSpec> gb_aggs;

    struct AggState {
        int64_t count;
        int64_t sum_i;
        double sum_d;
        bool has_minmax;
        std::string minmax_bytes;
        AggState() : count(0), sum_i(0), sum_d(0.0), has_minmax(false), minmax_bytes() {}
    };

    struct GroupState {
        std::string key_bytes;
        std::vector<AggState> aggs;
    };

    std::vector<GroupState> gb_groups;
    std::unordered_map<std::string, int> gb_group_index;
    size_t gb_emit_pos;

    bool gb_end;
    char *gb_out;
    int64_t gb_out_cap;

  private:
    int findInSchemaIndex(const RequestColumn &rc) const;
    bool buildOutputSchema(const RequestColumn *groupby, int groupby_num,
                           const RequestColumn *select, int select_num);
    bool buildStates();
    bool ensureOutBuf();

  public:
    /**
     * @brief Construct a GroupByAggr operator.
     *
     * @param child        Child operator supplying input tuples.
     * @param input_schema Schema describing the child's output tuple layout.
     * @param groupby      Array of GROUP BY key column references.
     * @param groupby_num  Length of @p groupby (0..4).
     * @param select       Array of SELECT column references (may include
     *                     aggregation methods such as SUM/COUNT).
     * @param select_num   Length of @p select (0..4).
     */
    GroupByAggrOperator(Operator *child,
                        const std::vector<SchemaCol> &input_schema,
                        const RequestColumn *groupby,
                        int groupby_num,
                        const RequestColumn *select,
                        int select_num);

    /** @brief Destroy the operator, freeing the output buffer and child. */
    virtual ~GroupByAggrOperator();

    /**
     * @brief Return the output schema produced by this operator.
     *
     * Valid after construction; does not require init() to have been called.
     *
     * @return Const reference to the vector of SchemaCol descriptors.
     */
    const std::vector<SchemaCol> &getOutputSchema() const { return gb_out_schema; }

    /**
     * @brief Initialise child and materialise all groups.
     *
     * Calls child->init(), then consumes all child tuples to build the
     * internal hash-group table.  Expensive for large inputs.
     *
     * @retval true  All groups built; ready to emit via getNext().
     * @retval false Child init failed or memory allocation error.
     */
    bool init() override;

    /**
     * @brief Emit the next group's aggregated tuple.
     *
     * Groups are emitted in arbitrary (hash) order.
     *
     * @retval true  A new aggregated tuple is available.
     * @retval false All groups have been emitted.
     */
    bool getNext() override;

    /**
     * @brief Return true if all groups have been emitted.
     * @retval true  No more groups.
     * @retval false More groups remain.
     */
    bool isEnd() override;

    /**
     * @brief Return a pointer to the current aggregated output tuple.
     * @retval non-NULL Packed binary tuple in output schema layout.
     * @retval NULL     Not initialised or exhausted.
     */
    char *getOutput() override;

    /**
     * @brief Return the fixed byte length of one output tuple.
     * @retval >0 Total size of all output columns.
     * @retval 0  No output columns defined.
     */
    int64_t getOutputLen() override;

    /**
     * @brief Free the output buffer, hash tables, and child operator.
     * @retval true Always.
     */
    bool close() override;
};

//----------------------------------------------------------------------
// OrderByOperator
//----------------------------------------------------------------------

/**
 * @brief Materialise-then-sort ORDER BY operator.
 *
 * Consumes all tuples from its child into an in-memory vector, then sorts
 * them using a quicksort keyed on up to four columns (all ascending).
 * The tuple layout is unchanged from the child's output.
 *
 * Memory usage is proportional to the total input size; suitable for
 * result sets that fit in g_memory.
 */
class OrderByOperator : public Operator {
  private:
    Operator *ob_child;
    std::vector<SchemaCol> ob_schema;

    struct Key {
        int idx;
        int64_t off;
        int64_t len;
        BasicType *type;
    };
    std::vector<Key> ob_keys;

    struct TupleBuf {
        char *buf;
        int64_t len;
    };
    std::vector<TupleBuf> ob_rows;

    // Chunk-based allocation: all tuple data lives in large blocks to reduce
    // g_memory call count and fragmentation on large result sets.
    struct TupleChunk {
        char *buf;
        int64_t cap;
    };
    std::vector<TupleChunk> ob_chunks;
    int64_t ob_chunk_used;

    size_t ob_pos;
    bool ob_end;

  private:
    int findSchemaIndex(const RequestColumn &rc) const;
    bool compileKeys(const RequestColumn *orderby, int orderby_num);
    bool lessTuple(const TupleBuf &a, const TupleBuf &b) const;

  public:
    /**
     * @brief Construct an OrderBy operator.
     *
     * @param child       Child operator supplying input tuples.
     * @param schema      Schema describing the child's output tuple layout;
     *                    used to locate sort-key columns.
     * @param orderby     Array of ORDER BY column references.
     * @param orderby_num Length of @p orderby (0..4).
     */
    OrderByOperator(Operator *child,
                    const std::vector<SchemaCol> &schema,
                    const RequestColumn *orderby,
                    int orderby_num);

    /** @brief Destroy the operator, freeing all materialised tuple buffers. */
    virtual ~OrderByOperator();

    /**
     * @brief Materialise all child tuples and sort them.
     *
     * Calls child->init(), drains all tuples into @p ob_rows, then runs
     * quicksort.  Expensive for large inputs.
     *
     * @retval true  All rows materialised and sorted; ready to emit.
     * @retval false Child init failed or memory allocation error.
     */
    bool init() override;

    /**
     * @brief Advance to the next sorted tuple.
     *
     * @retval true  A new tuple is available at the current position.
     * @retval false All sorted tuples have been emitted.
     */
    bool getNext() override;

    /**
     * @brief Return true if all sorted tuples have been emitted.
     * @retval true  No more tuples.
     * @retval false More tuples remain.
     */
    bool isEnd() override;

    /**
     * @brief Return a pointer to the current sorted tuple.
     * @retval non-NULL Pointer into the materialised tuple buffer.
     * @retval NULL     Not initialised or exhausted.
     */
    char *getOutput() override;

    /**
     * @brief Return the fixed byte length of one output tuple (same as child).
     * @retval >0 Tuple length in bytes.
     * @retval 0  Not yet initialised.
     */
    int64_t getOutputLen() override;

    /**
     * @brief Free all materialised tuple buffers and reset state.
     * @retval true Always.
     */
    bool close() override;
};

//----------------------------------------------------------------------
// FilterProjectOperator
//----------------------------------------------------------------------

/**
 * @brief Filter (WHERE predicate) and optional column projection operator.
 *
 * Supports three construction modes:
 *  -# <b>Table mode</b>: schema is derived from a catalog Table; applies
 *     conditions and projects a named subset of columns.
 *  -# <b>Schema pass-through mode</b>: schema is supplied externally;
 *     only filtering is performed (tuple layout unchanged).
 *  -# <b>Schema projection mode</b>: schema is supplied externally;
 *     both filtering and column projection are performed.
 *
 * Conditions are AND-ed together (up to 4).  Each condition may compare
 * a column against a literal constant or against another column.
 *
 * Output tuple layout for modes 1 and 3:
 *   [ proj_col_0 | proj_col_1 | ... ]
 * Output tuple layout for mode 2: identical to child output.
 */
class FilterProjectOperator : public Operator {
  private:
    Operator *fp_child;
    Table *fp_table;
    bool fp_use_schema;
    bool fp_passthrough;
    std::vector<SchemaCol> fp_in_schema;
    Conditions fp_conds;

    struct ColInfo {
        char name[128];
        BasicType *type;
        int64_t offset;
    };
    ColInfo fp_cols[128];
    int fp_col_num;

    struct Pred {
        int left_col;
        CompareMethod cmp;
        bool right_is_col;
        int right_col;
        char *right_const;
        int64_t right_const_cap;
        int64_t right_const_len;
    };
    Pred fp_pred[4];
    int fp_pred_num;

    int fp_proj_col[4];
    int fp_proj_num;
    RequestColumn fp_proj_req[4];
    int fp_proj_req_num;
    int64_t fp_out_len;
    char *fp_out;
    int64_t fp_out_cap;
    bool fp_end;

  private:
    int findColIndexByName(const char *name);
    bool buildSchema();
    bool compilePredicates();
    bool compileProjection(const RequestColumn *proj, int proj_num);
    bool evalOne(const Pred &p, const char *tuple);

  public:
    /**
     * @brief Construct a filter+projection operator from a catalog Table.
     *
     * The schema is derived from the table's column definitions via the
     * catalog.  The output contains only the columns named in @p proj.
     *
     * @param child    Child operator supplying raw table tuples.
     * @param table    Source table (used to build the internal schema).
     * @param conds    WHERE conditions to evaluate on each input tuple.
     * @param proj     Array of column references to include in the output.
     * @param proj_num Number of entries in @p proj (0..4).
     */
    FilterProjectOperator(Operator *child, Table *table, const Conditions &conds,
                          const RequestColumn *proj, int proj_num);

    /**
     * @brief Construct a pass-through filter from an external schema.
     *
     * Only predicate evaluation is performed; the tuple layout is unchanged
     * (fp_passthrough = true, no output buffer allocation).
     *
     * @param child  Child operator.
     * @param schema Schema describing the child's output tuple layout.
     * @param conds  WHERE conditions to evaluate on each input tuple.
     */
    FilterProjectOperator(Operator *child, const std::vector<SchemaCol> &schema,
                          const Conditions &conds);

    /**
     * @brief Construct a filter+projection operator from an external schema.
     *
     * @param child    Child operator.
     * @param schema   Schema describing the child's output tuple layout.
     * @param conds    WHERE / HAVING conditions to evaluate.
     * @param proj     Columns to include in the output, in output order.
     * @param proj_num Number of entries in @p proj (0..4).
     */
    FilterProjectOperator(Operator *child, const std::vector<SchemaCol> &schema,
                          const Conditions &conds,
                          const RequestColumn *proj, int proj_num);

    /** @brief Destroy the operator, freeing predicate constants and output buffer. */
    virtual ~FilterProjectOperator();

    /**
     * @brief Compile the schema, predicates, and projection; initialise child.
     *
     * @retval true  Ready to produce tuples.
     * @retval false Schema build, predicate compilation, or child init failed.
     */
    bool init() override;

    /**
     * @brief Advance to the next tuple that satisfies all predicates.
     *
     * Pulls tuples from the child until one passes all conditions, then
     * (in non-passthrough modes) projects it into the output buffer.
     *
     * @retval true  A qualifying tuple is available.
     * @retval false No more qualifying tuples from the child.
     */
    bool getNext() override;

    /**
     * @brief Return true if the operator is exhausted.
     * @retval true  No more qualifying tuples.
     * @retval false May still produce tuples.
     */
    bool isEnd() override;

    /**
     * @brief Return a pointer to the current (filtered+projected) output tuple.
     *
     * In pass-through mode the pointer aliases the child's output buffer.
     * In projection mode the pointer is the operator's own output buffer.
     *
     * @retval non-NULL Valid tuple pointer.
     * @retval NULL     Not initialised or exhausted.
     */
    char *getOutput() override;

    /**
     * @brief Return the byte length of one output tuple.
     *
     * In projection mode: sum of projected column sizes.
     * In pass-through mode: equals child->getOutputLen().
     *
     * @retval >0 Tuple length in bytes.
     * @retval 0  Not yet initialised.
     */
    int64_t getOutputLen() override;

    /**
     * @brief Free predicate constant buffers, output buffer, and child.
     * @retval true Always.
     */
    bool close() override;
};

//----------------------------------------------------------------------
// HashJoinOperator
//----------------------------------------------------------------------

/**
 * @brief Build-probe equi-join operator (no index required).
 *
 * Implements a classic two-phase hash join:
 *  -# <b>Build phase</b> (during init()): all right-side tuples are read
 *     and stored in an in-memory hash map keyed on the join column.
 *  -# <b>Probe phase</b> (getNext() loop): each left-side tuple is
 *     probed against the hash map; every matching right-side tuple
 *     produces one output tuple.
 *
 * Output tuple layout: [ left_tuple | right_tuple ] (concatenated).
 *
 * Used when the inner table has no hash index on the join key.
 */
class HashJoinOperator : public Operator {
  private:
    Operator *hj_left;
    Operator *hj_right;
    int64_t hj_left_key_off;
    int64_t hj_right_key_off;
    BasicType *hj_key_type;
    int64_t hj_key_len;

    struct BuildTuple {
        char *buf;
        int64_t len;
    };
    std::vector<BuildTuple> hj_build;

    // Chunk-based allocation for build-side tuples.
    struct BuildChunk {
        char *buf;
        int64_t cap;
    };
    std::vector<BuildChunk> hj_chunks;
    int64_t hj_chunk_used;

    bool hj_end;
    char *hj_out;
    int64_t hj_out_len;
    int64_t hj_out_cap;

    char *hj_cur_left;
    int64_t hj_cur_left_len;
    std::vector<int> hj_cur_match_idx;
    int hj_cur_match_pos;

  private:
    bool buildHash();
    bool extractKey(std::string &key, const char *tuple, int64_t key_off);
    bool emitJoin(const char *left, int64_t leftLen, const BuildTuple &right);

    std::unordered_map<std::string, std::vector<int> > hj_map;

  public:
    /**
     * @brief Construct a HashJoin operator.
     *
     * @param left          Left (outer/probe) child operator.
     * @param right         Right (inner/build) child operator.
     * @param left_key_off  Byte offset of the join key within a left tuple.
     * @param right_key_off Byte offset of the join key within a right tuple.
     * @param key_type      Runtime type of the join key column (used for
     *                      binary key extraction; both sides must match).
     */
    HashJoinOperator(Operator *left, Operator *right,
                     int64_t left_key_off, int64_t right_key_off,
                     BasicType *key_type);

    /** @brief Destructor (does not call close(); lifecycle managed by Executor). */
    virtual ~HashJoinOperator();

    /**
     * @brief Initialise both children and build the right-side hash table.
     *
     * After init(), all right-side tuples are held in memory.  The left
     * child is ready for probing.
     *
     * @retval true  Build phase completed; ready to probe.
     * @retval false A child init failed or memory allocation error.
     */
    bool init() override;

    /**
     * @brief Advance to the next joined output tuple.
     *
     * Emits all right-side matches for the current left tuple before
     * advancing to the next left tuple.
     *
     * @retval true  A joined tuple is available.
     * @retval false Left side exhausted or no more matches.
     */
    bool getNext() override;

    /**
     * @brief Return true if no more joined tuples will be produced.
     * @retval true  Join is exhausted.
     * @retval false More tuples may be available.
     */
    bool isEnd() override;

    /**
     * @brief Return a pointer to the current joined output tuple.
     *
     * The tuple is laid out as [ left_tuple | right_tuple ].
     *
     * @retval non-NULL Valid joined tuple.
     * @retval NULL     Not initialised or exhausted.
     */
    char *getOutput() override;

    /**
     * @brief Return the byte length of one output tuple (left_len + right_len).
     * @retval >0 Combined tuple length.
     * @retval 0  Not yet initialised.
     */
    int64_t getOutputLen() override;

    /**
     * @brief Free the output buffer, build-side storage, and children.
     * @retval true Always.
     */
    bool close() override;
};

//----------------------------------------------------------------------
// IndexNestedLoopJoinOperator
//----------------------------------------------------------------------

/**
 * @brief Index nested-loop equi-join using a HashIndex on the inner table.
 *
 * For each outer tuple, looks up the inner table's HashIndex with the
 * outer join key to retrieve all matching inner records efficiently.
 * The HashIndex::set_ls() / HashIndex::lookup() iterator API is used to
 * enumerate multiple matches per outer key.
 *
 * Output tuple layout: [ outer_tuple | inner_tuple ] (concatenated).
 *
 * Preferred over HashJoinOperator when the inner join column has a
 * single-column HashIndex, as it avoids materialising the entire inner
 * table.
 */
class IndexNestedLoopJoinOperator : public Operator {
  private:
    Operator *inlj_outer;
    Table *inlj_inner_table;
    HashIndex *inlj_inner_index;
    int64_t inlj_outer_key_off;
    int64_t inlj_key_len;

    char *inlj_inner_tuple;
    int64_t inlj_inner_cap;
    int64_t inlj_inner_len;
    char *inlj_out;
    int64_t inlj_out_cap;
    int64_t inlj_out_len;

    bool inlj_end;
    char *inlj_cur_outer;
    int64_t inlj_cur_outer_len;
    HashInfo inlj_info;
    bool inlj_info_valid;
    void *inlj_match_rec;

  private:
    bool packInnerRecord(void *rec_ptr);
    bool ensureBuffers(int64_t outerLen);

  public:
    /**
     * @brief Construct an IndexNestedLoopJoin operator.
     *
     * @param outer         Outer (probe) child operator.
     * @param inner_table   Inner table to look up via the index.
     * @param inner_index   HashIndex on the inner join key column.
     * @param outer_key_off Byte offset of the join key within an outer tuple.
     */
    IndexNestedLoopJoinOperator(Operator *outer,
                                Table *inner_table,
                                HashIndex *inner_index,
                                int64_t outer_key_off);

    /** @brief Destructor (lifecycle managed by Executor). */
    virtual ~IndexNestedLoopJoinOperator();

    /**
     * @brief Initialise the outer child and allocate inner-tuple buffers.
     *
     * Derives the inner tuple length from the inner table's column schema
     * and the key length from the HashIndex's key descriptor.
     *
     * @retval true  Ready to produce joined tuples.
     * @retval false Outer child init failed, NULL inputs, or alloc error.
     */
    bool init() override;

    /**
     * @brief Advance to the next joined output tuple.
     *
     * Calls HashIndex::lookup() to iterate over all inner matches for the
     * current outer tuple.  When the index iterator is exhausted, advances
     * to the next outer tuple and calls HashIndex::set_ls() to start a
     * new lookup.
     *
     * @retval true  A joined tuple is available.
     * @retval false Outer side exhausted or no matching inner rows.
     */
    bool getNext() override;

    /**
     * @brief Return true if the join is exhausted.
     * @retval true  No more joined tuples.
     * @retval false More tuples may be available.
     */
    bool isEnd() override;

    /**
     * @brief Return a pointer to the current joined output tuple.
     *
     * Layout: [ outer_tuple | inner_packed_tuple ].
     *
     * @retval non-NULL Valid joined tuple.
     * @retval NULL     Not initialised or exhausted.
     */
    char *getOutput() override;

    /**
     * @brief Return the byte length of one output tuple.
     * @retval >0 outer_len + inner_len.
     * @retval 0  Not yet initialised.
     */
    int64_t getOutputLen() override;

    /**
     * @brief Free all buffers and close the outer child.
     * @retval true Always.
     */
    bool close() override;
};
#endif
