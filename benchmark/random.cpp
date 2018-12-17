#include "foundations/Database.hpp"
#include "foundations/version_management.hpp"

#include <chrono>
#include <random>
#include <iostream>

#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"

static constexpr auto master_factor = 0.7;
static constexpr auto update_factor = 0.2;
static constexpr int seed = 42;
static constexpr size_t new_tuples_per_episode = 1<<18;

static std::mt19937 rd_engine(seed);

using namespace Native::Sql;

void insert_tuples(branch_id_t branch, size_t cnt, Database & db, Table & table) {
    QueryContext ctx(db);
    db.constructBranchLineage(master_branch_id, ctx.executionContext);

    std::vector<value_op_t> values;
    values.push_back(std::make_unique<Integer>(1));
    values.push_back(std::make_unique<Integer>(2));
    values.push_back(std::make_unique<Integer>(3));
    SqlTuple tuple(std::move(values));
    for (size_t i = 0; i < cnt; ++i) {
        insert_tuple(tuple, table, ctx);
    }
}

void update_tuples_once(branch_id_t branch, size_t cnt, Database & db, Table & table) {
    QueryContext ctx(db);
    db.constructBranchLineage(master_branch_id, ctx.executionContext);

    std::vector<tid_t> tids;
    for (size_t tid = 0; tid < table._version_mgmt_column.size(); ++tid) {
        tids.push_back(tid);
    }
    for (size_t tid = 0; tid < table._dangling_version_mgmt_column.size(); ++tid) {
        tids.push_back(mark_as_dangling_tid(tid));
    }

    std::vector<value_op_t> values;
    values.push_back(std::make_unique<Integer>(1));
    values.push_back(std::make_unique<Integer>(2));
    values.push_back(std::make_unique<Integer>(3));
    SqlTuple tuple(std::move(values));

    std::shuffle(tids.begin(), tids.end(), rd_engine);
    size_t updated = 0;
    for (tid_t tid : tids) {
        VersionEntry * version_entry;
        if (is_marked_as_dangling_tid(tid)) {
            tid_t unmarked = unmark_dangling_tid(tid);
            version_entry = table._dangling_version_mgmt_column[unmarked].get();
        } else {
            version_entry = table._version_mgmt_column[tid].get();
        }
        if (!has_lineage_intersection(ctx, version_entry)) {
            continue;
        }

        update_tuple(tid, tuple, table, ctx);
        updated += 1;
    }
    assert(updated == cnt);
}

void update_tuples(branch_id_t branch, size_t cnt, Database & db, Table & table) {
    QueryContext ctx(db);
    db.constructBranchLineage(master_branch_id, ctx.executionContext);

    std::vector<tid_t> tids;
    for (size_t tid = 0; tid < table._version_mgmt_column.size(); ++tid) {
        tids.push_back(tid);
    }
    for (size_t tid = 0; tid < table._dangling_version_mgmt_column.size(); ++tid) {
        tids.push_back(mark_as_dangling_tid(tid));
    }

    std::vector<value_op_t> values;
    values.push_back(std::make_unique<Integer>(1));
    values.push_back(std::make_unique<Integer>(2));
    values.push_back(std::make_unique<Integer>(3));
    SqlTuple tuple(std::move(values));

    std::uniform_int_distribution<size_t> distribution(0, tids.size());
    for (size_t updated = 0; updated < cnt; ++updated) {
        tid_t tid = tids[distribution(rd_engine)];

        VersionEntry * version_entry;
        if (is_marked_as_dangling_tid(tid)) {
            tid_t unmarked = unmark_dangling_tid(tid);
            version_entry = table._dangling_version_mgmt_column[unmarked].get();
        } else {
            version_entry = table._version_mgmt_column[tid].get();
        }
        if (!has_lineage_intersection(ctx, version_entry)) {
            continue;
        }

        update_tuple(tid, tuple, table, ctx);
    }
}

std::vector<branch_id_t> get_branches_dist(Database & db) {
    branch_id_t max_branch = db.getLargestBranchId();
    size_t master_cnt = max_branch/(1.0-master_factor)*master_factor;
    std::vector<branch_id_t> branches_dist;
    for (size_t i = 0; i < master_cnt; ++i) {
        branches_dist.push_back(master_branch_id);
    }
    for (branch_id_t branch = 0; branch <= max_branch; ++branch) {
        if (branch != master_branch_id) {
            branches_dist.push_back(branch);
        }
    }
    std::shuffle(branches_dist.begin(), branches_dist.end(), rd_engine);
    return branches_dist;
}

void perform_bunch_inserts(Database & db, Table & table) {
    auto branches_dist = get_branches_dist(db);
    size_t chunk_size = new_tuples_per_episode/branches_dist.size();
    for (branch_id_t branch : branches_dist) {
        insert_tuples(branch, chunk_size, db, table);
    }
}

void perform_bunch_updates(Database & db, Table & table) {
    auto branches_dist = get_branches_dist(db);
    size_t table_size = table._version_mgmt_column.size() + table._dangling_version_mgmt_column.size();
    size_t total_cnt = table_size*update_factor;
    size_t chunk_size = total_cnt/branches_dist.size();
    for (branch_id_t branch : branches_dist) {
        update_tuples(branch, chunk_size, db, table);
    }
}

inline void print_result(const std::tuple<TmplScanItem<Integer>, TmplScanItem<Integer>, TmplScanItem<Integer>> & scan_items) {
    printf("%d\t%d\t%d\n",
        std::get<0>(scan_items).reg.sql_value.value,
        std::get<1>(scan_items).reg.sql_value.value,
        std::get<2>(scan_items).reg.sql_value.value
    );
}

void measure_master_scan(Database & db, Table & table) {
    QueryContext ctx(db);
    db.constructBranchLineage(master_branch_id, ctx.executionContext);

    const auto & column0 = table.getColumn(0);
    const auto & column1 = table.getColumn(1);
    const auto & column2 = table.getColumn(2);

    auto scan_items = std::make_tuple<
        TmplScanItem<Integer>, TmplScanItem<Integer>, TmplScanItem<Integer>>(
        {column0, 0},
        {column1, 4},
        {column2, 8});

    using namespace std::chrono;
    const auto query_start = high_resolution_clock::now();
    scan_relation_yielding_latest(ctx, table, print_result, scan_items);
    const auto query_duration = high_resolution_clock::now() - query_start;
    printf("Execution time: %lums\n", duration_cast<milliseconds>(query_duration).count());
}

void run_benchmark() {
    auto db = std::make_unique<Database>();

    auto & bench_table = db->createTable("bench_table");
    bench_table.addColumn("a", Sql::getIntegerTy());
    bench_table.addColumn("b", Sql::getIntegerTy());
    bench_table.addColumn("c", Sql::getIntegerTy());

    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch1 = db->createBranch("branch1", master_branch_id);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch2 = db->createBranch("branch2", branch2);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch3 = db->createBranch("branch3", invalid_branch_id);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch4 = db->createBranch("branch4", branch3);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);

    measure_master_scan(*db, bench_table);
}

int main(int argc, char * argv[]) {
    return 0;
}
