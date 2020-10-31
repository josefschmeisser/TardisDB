//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void CreateTableAnalyser::verify() {
        Database &db = _context.db;
        CreateTableStatement* stmt = _context.parserResult.createTableStmt;

        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");
        if (db.hasTable(stmt->tableName)) throw semantic_sql_error("table '" + stmt->tableName + "' already exists");
        std::vector<std::string> definedColumnNames;
        std::vector<std::string> typeNames = {"bool","date","integer","longinteger","numeric","char","varchar","timestamp","text"};
        for (auto &column : stmt->columns) {
            if (std::find(definedColumnNames.begin(),definedColumnNames.end(),column.name) != definedColumnNames.end())
                throw semantic_sql_error("column '" + column.name + "' already exists");
            definedColumnNames.push_back(column.name);
            if (std::find(typeNames.begin(),typeNames.end(),column.type) == typeNames.end())
                throw semantic_sql_error("type '" + column.type + "' does not exist");
        }

        // Table already exists?
        // For each columnspec
        // // name already exists?
        // // type exists?
    }

    void CreateTableAnalyser::constructTree() {
        auto & createdTable = _context.db.createTable(_context.parserResult.createTableStmt->tableName);

        for (auto &columnSpec : _context.parserResult.createTableStmt->columns) {
            Sql::SqlType sqlType;
            if (columnSpec.type.compare("bool") == 0) {
                sqlType = Sql::getBoolTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("date") == 0) {
                sqlType = Sql::getDateTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("integer") == 0) {
                sqlType = Sql::getIntegerTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("longinteger") == 0) {
                sqlType = Sql::getLongIntegerTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("numeric") == 0) {
                sqlType = Sql::getNumericTy(columnSpec.length,columnSpec.precision,columnSpec.nullable);
            } else if (columnSpec.type.compare("char") == 0) {
                sqlType = Sql::getCharTy(columnSpec.length, columnSpec.nullable);
            } else if (columnSpec.type.compare("varchar") == 0) {
                sqlType = Sql::getVarcharTy(columnSpec.length, columnSpec.nullable);
            } else if (columnSpec.type.compare("timestamp") == 0) {
                sqlType = Sql::getTimestampTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("text") == 0) {
                sqlType = Sql::getTextTy(columnSpec.nullable);
            }

            createdTable.addColumn(columnSpec.name, sqlType);
        }

        _context.joinedTree = nullptr;
    }

}