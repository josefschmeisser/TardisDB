
#pragma once

#include "algebra/physical/Operator.hpp"
#include "foundations/Database.hpp"

namespace Algebra {
namespace Physical {

/// The table scan operator
class TableScan : public NullaryOperator {
public:
    TableScan(const logical_operator_t & logicalOperator, Table & table, std::string *alias);

    virtual ~TableScan();

    void produce() override;

    void produce(cg_tid_t tid);

private:
    cg_bool_t isVisible(cg_tid_t tid, cg_branch_id_t branchId);

    Table & table;
    std::string *alias;

    using column_t = std::tuple<ci_p_t, llvm::Type *, llvm::Value *, size_t>;
    std::vector<column_t> columns;
};

} // end namespace Physical
} // end namespace Algebra
