#pragma once

#include "foundations/Database.hpp"
#include "foundations/QueryContext.hpp"
#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"
#include "utils/optimistic_lock.hpp"

#include <boost/dynamic_bitset.hpp>

class Table;

// similar to VersionedTupleStorage; used by the current 'master' branch entry
struct VersionEntry {
    void * first = nullptr;
    void * next = nullptr;
    VersionedTupleStorage * next_in_branch = nullptr;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation (same as the length of the branch bitvector)
    opt_lock::lock_t lock;
    boost::dynamic_bitset<> branch_visibility;
};

struct VersionedTupleStorage {
    const void * next = nullptr;
    const void * next_in_branch = nullptr;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation
    uint8_t data[0];
};

branch_id_t create_branch(std::string name);

// FIXME tuple has to exist in the master branch!
tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

// the entire version chain has to be deleted
// the tuple has to be relocated iff branch==master
tid_t delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

tid_t merge_tuple(branch_id_t src_branch, branch_id_t dst_branch, tid_t tid, QueryContext ctx);

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx);


template<typename Consumer, typename... Ts>
inline void produce_current_master(tid_t tid, Consumer consumer, const std::tuple<Ts...> & scan_items) {
    std::apply([&tid] (const auto &... item) {
        ((item->reg.load_from(item->column.at(tid)), ...);
    },
    scan_items);
    consumer(std::forward(scan_items));
}

template<typename Consumer, typename... Ts>
inline void produce(const VersionedTupleStorage * storage, Consumer consumer, const std::tuple<Ts...> & scan_items) {
    uint8_t * tuple_ptr = storage->data;
    std::apply([] (const auto &... item) {
        ((item->reg.load_from(ptr + item->offset), ...);
    },
    scan_items);
    consumer(std::forward(scan_items));
}

template<typename Consumer, typename... Ts>
void produce_latest_tuple(QueryContext & ctx, tid_t tid, Table & table, Consumer consumer, const std::tuple<Ts...> & scan_items) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        produce_current_master(tid, table, consumer, std::forward(scan_items));
        return;
    }

    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_latest_chain_element(version_entry, table, ctx);

    if (element == nullptr) {
        throw std::runtime_error("no such tuple in the given branch");
    } else if (element == version_entry) { // current master
        produce_current_master(tid, table, consumer, std::forward(scan_items));
    } else {
        auto storage = static_cast<const VersionedTupleStorage *>(element);
        produce(storage, consumer, std::forward(scan_items));
    }
}


template<typename Consumer, typename... Ts>
void produce_earliest_tuple(QueryContext & ctx, tid_t tid, Table & table, Consumer consumer, const std::tuple<Ts...> & scan_items) {
    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_earliest_chain_element(version_entry, table, ctx);

    if (element == nullptr) {
        throw std::runtime_error("no such tuple in the given branch");
    } else if (element == version_entry) { // current master
        produce_current_master(tid, table, consumer, std::forward(scan_items));
    } else {
        auto storage = static_cast<const VersionedTupleStorage *>(element);
        produce(storage, consumer, std::forward(scan_items));
    }
}


// revision_offset == 0 => latest revision
std::unique_ptr<Native::Sql::SqlTuple> get_tuple(tid_t tid, unsigned revision_offset, Table & table, QueryContext & ctx);

template<typename Consumer, typename... Ts>
void produce_tuple(QueryContext & ctx, tid_t tid, unsigned revision_offset, Table & table, Consumer consumer, const std::tuple<Ts...> & scan_items) {
    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_chain_element(version_entry, revision_offset, table, ctx);

    if (element == nullptr) {
        throw std::runtime_error("no such tuple in the given branch");
    } else if (element == version_entry) { // current master
        produce_current_master(tid, table, consumer, std::forward(scan_items));
    } else {
        auto storage = static_cast<const VersionedTupleStorage *>(element);
        produce(storage, consumer, std::forward(scan_items));
    }
}


template<typename Consumer, typename... Ts>
void scan_relation(QueryContext & ctx, Table & table, Consumer consumer, const std::tuple<Ts...> & scan_items) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        auto size = table._version_mgmt_column->size();
        for (tid_t tid = 0; tid < size; ++tid) {
            produce_current_master(tid, consumer, std::forward(scan_items));
        }
    } else {
        auto size = table._version_mgmt_column->size();
        for (tid_t tid = 0; tid < size; ++tid) {
            produce_latest_tuple(ctx, tid, consumer, std::forward(scan_items));
        }
        size = table._dangling_version_mgmt_column->size();
        for (tid_t tid = 0; tid < size; ++tid) {
            tid_t marked = mark_as_dangling_tid(tid);
            produce_latest_tuple(ctx, marked, consumer, std::forward(scan_items));
        }
    }
}


namespace CppCodeGen {

}
