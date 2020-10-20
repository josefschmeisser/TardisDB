//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> InsertAnalyser::constructTree() {
        QueryPlan plan;
        InsertStatement *stmt = _parserResult.insertStmt;

        auto& db = _context.analyzingContext.db;
        Table* table = db.getTable(stmt->relation.name);

        std::string &branchName = stmt->relation.version;
        branch_id_t branchId;
        if (branchName.compare("master") != 0) {
            branchId = db._branchMapping[branchName];
        } else {
            branchId = master_branch_id;
        }

        std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;

        for (int i=0; i<stmt->columns.size(); i++) {
            Native::Sql::SqlType type = table->getCI(stmt->columns[i].name)->type;
            std::string &value = stmt->values[i];
            std::unique_ptr<Native::Sql::Value> sqlvalue = Native::Sql::Value::castString(value,type);

            sqlvalues.emplace_back(std::move(sqlvalue));
        }

        Native::Sql::SqlTuple *tuple =  new Native::Sql::SqlTuple(std::move(sqlvalues));

        return std::make_unique<Insert>(_context,*table,tuple,branchId);
    }

}