/**
 * @file    executor.h
 * @author  liugang(liugang@ict.ac.cn)
 * @version 0.1
 *
 * @section DESCRIPTION
 *  
 * definition of executor
 *
 */

#ifndef _EXECUTOR_H
#define _EXECUTOR_H

#include "catalog.h"
#include "mymemory.h"

#include <string>
#include <unordered_map>
#include <vector>

class Operator;

/** aggrerate method. */
enum AggrerateMethod {
    NONE_AM = 0, /**< none */
    COUNT,       /**< count of rows */
    SUM,         /**< sum of data */
    AVG,         /**< average of data */
    MAX,         /**< maximum of data */
    MIN,         /**< minimum of data */
    MAX_AM
};

/** compare method. */
enum CompareMethod {
    NONE_CM = 0,
    LT,        /**< less than */
    LE,        /**< less than or equal to */
    EQ,        /**< equal to */
    NE,        /**< not equal than */
    GT,        /**< greater than */
    GE,        /**< greater than or equal to */
    LINK,      /**< join */
    MAX_CM
};

  /**
   * SchemaCol:
   * - describes a packed binary tuple column (name/type/offset/len)
   * - `aggr` disambiguates derived aggregation outputs (e.g., SUM(col) vs MAX(col))
   */
  struct SchemaCol {
    char name[128];
    BasicType *type;
    int64_t offset;
    int64_t len;
    AggrerateMethod aggr;
  };

/** definition of request column. */
struct RequestColumn {
    char name[128];    /**< name of column */
    AggrerateMethod aggrerate_method;  /** aggrerate method, could be NONE_AM  */
};

/** definition of request table. */
struct RequestTable {
    char name[128];    /** name of table */
};

/** definition of compare condition. */
struct Condition {
    RequestColumn column;   /**< which column */
    CompareMethod compare;  /**< which method */
    char value[128];        /**< the value to compare with, if compare==LINK,value is another column's name; else it's the column's value*/
};

/** definition of conditions. */
struct Conditions {
    int condition_num;      /**< number of condition in use */
    Condition condition[4]; /**< support maximum 4 & conditions */
};

/** definition of selectquery.  */
class SelectQuery {
  public:
    int64_t database_id;           /**< database to execute */
    int select_number;             /**< number of column to select */
    RequestColumn select_column[4];/**< columns to select, maximum 4 */
    int from_number;               /**< number of tables to select from */
    RequestTable from_table[4];    /**< tables to select from, maximum 4 */
    Conditions where;              /**< where meets conditions, maximum 4 & conditions */
    int groupby_number;            /**< number of columns to groupby */
    RequestColumn groupby[4];      /**< columns to groupby */
    Conditions having;             /**< groupby conditions */
    int orderby_number;            /**< number of columns to orderby */
    RequestColumn orderby[4];      /**< columns to orderby */
};  // class SelectQuery

/** definition of result table.  */
class ResultTable {
  public:
    int column_number;       /**< columns number that a result row consist of */
    BasicType **column_type; /**< each column data type */
    char *buffer;         /**< pointer of buffer alloced from g_memory */
    int64_t buffer_size;  /**< size of buffer, power of 2 */
    int row_length;       /**< length per result row */
    int row_number;       /**< current usage of rows */
    int row_capicity;     /**< maximum capicity of rows according to buffer size and length of row */
    int *offset;
    int offset_size;

    /**
     * init alloc memory and set initial value
     * @col_types array of column type pointers
     * @col_num   number of columns in this ResultTable
     * @param  capicity buffer_size, power of 2
     * @retval >0  success
     * @retval <=0  failure
     */
    int init(BasicType *col_types[],int col_num,int64_t capicity = 1024);
    /**
     * calculate the char pointer of data spcified by row and column id
     * you should set up column_type,then call init function
     * @param row    row id in result table
     * @param column column id in result table
     * @retval !=NULL pointer of a column
     * @retval ==NULL error
     */
    char* getRC(int row, int column);
    /**
     * write data to position row,column
     * @param row    row id in result table
     * @param column column id in result table
     * @data data pointer of a column
     * @retval !=NULL pointer of a column
     * @retval ==NULL error
     */
    int writeRC(int row, int column, void *data);
    /**
     * print result table, split by '\t', output a line per row 
     * @retval the number of rows printed
     */
    int print(void);
    /**
     * write to file with FILE *fp
     */
    int dump(FILE *fp);
    /**
     * free memory of this result table to g_memory
     */
    int shut(void);
};  // class ResultTable

/** definition of class executor.  */
class Executor {
  private:
    SelectQuery *current_query;  /**< selectquery to iterately execute */

    // execution plan state
    Operator *e_root;                 /**< root operator of current plan */
    bool e_inited;                    /**< whether root operator is initialized */
    std::vector<Operator *> e_ops;    /**< owned operators to delete on close */

    // compiled output schema (for locating columns in tuples)
    std::vector<SchemaCol> e_schema;
    int64_t e_tuple_len;

    // projection mapping (indices into e_schema)
    std::vector<int> e_select_idx;

    // compiled selection predicates (non-join conditions)
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

    // whether Executor should evaluate e_pred in the exec() output loop
    bool e_eval_pred;

    // ResultTable column_type storage must outlive ResultTable usage
    BasicType **e_result_col_types;
    int64_t e_result_col_types_cap;

  public:
    /**
     * exec function.
     * @param  query to execute, if NULL, execute query at last time 
     * @param result table generated by an execution, store result in pattern defined by the result table
     * @retval >0  number of result rows stored in result
     * @retval <=0 no more result
     */
    virtual int exec(SelectQuery *query, ResultTable *result);
    //--------------------------------------
    //  ... 
    //  ...
    /**
     * close function.
     * @param None
     * @retval ==0 succeed to close
     * @retval !=0 fail to close
     */
    virtual int close();

    Executor();
    virtual ~Executor();
};

//----------------------------------------------------------------------
// Operator framework (Lab extension)
//----------------------------------------------------------------------

/**
 * Operator: iterator-style interface for executing query plans.
 *
 * Convention:
 * - call init() once before consuming tuples
 * - repeatedly call getNext(); when it returns false, no more tuples
 * - after each successful getNext(), the current tuple is available via getOutput()
 * - call close() to release resources
 */
class Operator {
  public:
    virtual ~Operator() {}
    virtual bool init() = 0;
    virtual bool getNext() = 0;
    virtual bool isEnd() = 0;
    virtual char *getOutput() = 0;
    virtual int64_t getOutputLen() = 0;
    virtual bool close() = 0;
};

/**
 * TableScanOperator:
 * - sequentially scans all tuples in a given Table
 * - only returns valid tuples (for RowTable, invalid rows are skipped)
 * - each output tuple is in binary format (all columns packed), compatible with Table::select
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
    explicit TableScanOperator(Table *table);
    virtual ~TableScanOperator();

    bool init() override;
    bool getNext() override;
    bool isEnd() override;
    char *getOutput() override;
    int64_t getOutputLen() override;
    bool close() override;
};

//----------------------------------------------------------------------
// GroupByAggrOperator (hash-based group by + aggregation)
//----------------------------------------------------------------------

/**
 * GroupByAggrOperator:
 * - consumes all input tuples from child
 * - builds hash groups on up to 4 group-by keys
 * - computes up to 4 aggregates (COUNT/SUM/AVG/MAX/MIN)
 * - output tuple layout is described by `getOutputSchema()`
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
        AggrerateMethod method;
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
    GroupByAggrOperator(Operator *child,
                        const std::vector<SchemaCol> &input_schema,
                        const RequestColumn *groupby,
                        int groupby_num,
                        const RequestColumn *select,
                        int select_num);
    virtual ~GroupByAggrOperator();

    const std::vector<SchemaCol> &getOutputSchema() const { return gb_out_schema; }

    bool init() override;
    bool getNext() override;
    bool isEnd() override;
    char *getOutput() override;
    int64_t getOutputLen() override;
    bool close() override;
};

//----------------------------------------------------------------------
// OrderByOperator (materialize + quicksort)
//----------------------------------------------------------------------

/**
 * OrderByOperator:
 * - materializes all tuples from child, then sorts them by up to 4 keys
 * - all keys are ascending
 * - uses quick sort algorithm
 * - does not change tuple layout
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
        int64_t cap;
        int64_t len;
    };
    std::vector<TupleBuf> ob_rows;

    size_t ob_pos;
    bool ob_end;

  private:
    int findSchemaIndex(const RequestColumn &rc) const;
    bool compileKeys(const RequestColumn *orderby, int orderby_num);
    bool lessTuple(const TupleBuf &a, const TupleBuf &b) const;
    void quickSort(int l, int r);

  public:
    OrderByOperator(Operator *child,
                    const std::vector<SchemaCol> &schema,
                    const RequestColumn *orderby,
                    int orderby_num);
    virtual ~OrderByOperator();

    bool init() override;
    bool getNext() override;
    bool isEnd() override;
    char *getOutput() override;
    int64_t getOutputLen() override;
    bool close() override;
};



/**
 * FilterProjectOperator:
 * - applies WHERE conditions (AND only, up to 4 conditions) on input tuples
 * - projects specified columns into a new output tuple (packed binary)
 * - output tuple contains only projected columns in the given order
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

    // projection list (indices into fp_cols)
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
    FilterProjectOperator(Operator *child, Table *table, const Conditions &conds,
                          const RequestColumn *proj, int proj_num);
    // schema-driven: filter only (pass-through), does not change tuple layout
    FilterProjectOperator(Operator *child, const std::vector<SchemaCol> &schema, const Conditions &conds);
    // schema-driven: filter + projection
    FilterProjectOperator(Operator *child, const std::vector<SchemaCol> &schema, const Conditions &conds,
                const RequestColumn *proj, int proj_num);
    virtual ~FilterProjectOperator();

    bool init() override;
    bool getNext() override;
    bool isEnd() override;
    char *getOutput() override;
    int64_t getOutputLen() override;
    bool close() override;
};

//----------------------------------------------------------------------
// Join operators (Lab extension)
//----------------------------------------------------------------------

/**
 * HashJoinOperator (simple equi-join):
 * - joins two child tuple streams on equality of join keys
 * - used when no hash index available on join key
 * - output tuple = [left tuple][right tuple]
 */
class HashJoinOperator : public Operator {
  private:
    Operator *hj_left;
    Operator *hj_right;
    int64_t hj_left_key_off;
    int64_t hj_right_key_off;
    BasicType *hj_key_type;
    int64_t hj_key_len;

    // build-side storage
    struct BuildTuple {
        char *buf;
        int64_t cap;
        int64_t len;
    };

    std::vector<BuildTuple> hj_build;

    // internal iteration state
    bool hj_end;
    char *hj_out;
    int64_t hj_out_len;
    int64_t hj_out_cap;

    // match iteration
    char *hj_cur_left;
    int64_t hj_cur_left_len;
    std::vector<int> hj_cur_match_idx;
    int hj_cur_match_pos;

  private:
    bool buildHash();
    bool extractKey(std::string &key, const char *tuple, int64_t key_off);
    bool emitJoin(const char *left, int64_t leftLen, const BuildTuple &right);

    // hash map: key bytes -> indices in hj_build
    std::unordered_map<std::string, std::vector<int> > hj_map;

  public:
    HashJoinOperator(Operator *left, Operator *right,
                     int64_t left_key_off, int64_t right_key_off,
                     BasicType *key_type);
    virtual ~HashJoinOperator();

    bool init() override;
    bool getNext() override;
    bool isEnd() override;
    char *getOutput() override;
    int64_t getOutputLen() override;
    bool close() override;
};

/**
 * IndexNestedLoopJoinOperator (equi-join):
 * - outer input is a tuple stream
 * - inner side is a base table with a HASHINDEX on the join key
 * - output tuple = [outer tuple][inner tuple]
 */
class IndexNestedLoopJoinOperator : public Operator {
  private:
    Operator *inlj_outer;
    Table *inlj_inner_table;
    HashIndex *inlj_inner_index;
    int64_t inlj_outer_key_off;
    int64_t inlj_key_len;

    // buffers
    char *inlj_inner_tuple;
    int64_t inlj_inner_cap;
    int64_t inlj_inner_len;
    char *inlj_out;
    int64_t inlj_out_cap;
    int64_t inlj_out_len;

    // state for current outer tuple's index iteration
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
    IndexNestedLoopJoinOperator(Operator *outer,
                                Table *inner_table,
                                HashIndex *inner_index,
                                int64_t outer_key_off);
    virtual ~IndexNestedLoopJoinOperator();

    bool init() override;
    bool getNext() override;
    bool isEnd() override;
    char *getOutput() override;
    int64_t getOutputLen() override;
    bool close() override;
};
#endif
