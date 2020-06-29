#include "semanticAnalyser/SemanticAnalyser.hpp"

#include "foundations/Database.hpp"
#include "native/sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include "semanticAnalyser/SemanticalVerifier.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>
#include <native/sql/SqlTuple.hpp>

namespace semanticalAnalysis {

    void SemanticAnalyser::construct_scans(QueryContext& context, QueryPlan & plan) {
        auto& db = context.db;

        if (plan.parser_result.versions.size() != plan.parser_result.relations.size()) {
            return;
        }

        size_t i = 0;
        for (auto& relation : plan.parser_result.relations) {
            std::string tableName = relation.first;
            std::string tableAlias = relation.second;
            if (tableAlias.length() == 0) tableAlias = tableName;
            std::string &branchName = plan.parser_result.versions[i];

            Table* table = db.getTable(tableName);

            // Recognize version
            if (branchName.compare("master") != 0) {
                context.executionContext.branchIds.insert({tableAlias,db._branchMapping[branchName]});
            } else {
                context.executionContext.branchIds.insert({tableAlias,0});
            }


            //Construct the logical TableScan operator
            std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context, *table, new std::string(tableAlias));

            //Store the ius produced by this TableScan
            const iu_set_t& produced = scan->getProduced();
            for (iu_p_t iu : produced) {
                plan.ius[tableAlias][iu->columnInformation->columnName] = iu;
                plan.iuNameToTable[iu->columnInformation->columnName] = table;
            }

            //Add a new production with TableScan as root node
            plan.dangling_productions.insert({tableAlias, std::move(scan)});
            i++;
        }
    }

    void SemanticAnalyser::construct_selects(QueryContext& context, QueryPlan& plan) {
        auto & db = context.db;
        for (auto & selection : plan.parser_result.selections) {
            std::string& productionName = selection.first.first;
            std::string& productionIUName = selection.first.second;
            if (productionIUName.length() == 0) productionIUName = productionName;
            std::string& valueString = selection.second;

            iu_p_t iu = plan.ius[productionName][productionIUName];

            if (iu->columnInformation->type.nullable) {
                throw NotImplementedException();
            }

            //Construct Expression
            auto constExp = std::make_unique<Expressions::Constant>(valueString, iu->columnInformation->type);
            auto identifier = std::make_unique<Expressions::Identifier>(iu);
            std::unique_ptr<Expressions::Comparison> exp = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq,
                    std::move(identifier),
                    std::move(constExp)
            );

            //Construct the logical Select operator
            std::unique_ptr<Select> select = std::make_unique<Select>(std::move(plan.dangling_productions[productionName]), std::move(exp));

            //Update corresponding production by setting the Select operator as new root node
            plan.dangling_productions[productionName] = std::move(select);
        }
    }

    void SemanticAnalyser::construct_join_graph(QueryContext & context, QueryPlan & plan) {
        JoinGraph &graph = plan.graph;

        // create and add vertices to join graph
        for (auto & rel : plan.parser_result.relations) {
            JoinGraph::Vertex v = JoinGraph::Vertex(plan.dangling_productions[rel.second]);
            graph.addVertex(rel.second,v);
        }

        // create edges
        for (auto & join_condition : plan.parser_result.joinConditions) {
            std::string &vName = join_condition.first.first;
            std::string &uName = join_condition.second.first;
            std::string &vColumn = join_condition.first.second;
            std::string &uColumn = join_condition.second.second;

            //If edge does not already exist add it
            if (!graph.hasEdge(vName,uName)) {
                std::vector<Expressions::exp_op_t> expressions;
                JoinGraph::Edge edge = JoinGraph::Edge(vName,uName,expressions);
                graph.addEdge(edge);
            }

            //Get InformationUnits for both attributes
            iu_p_t iuV = plan.ius[vName][vColumn];
            iu_p_t iuU = plan.ius[uName][uColumn];

            //Create new compare expression as join condition
            std::vector<Expressions::exp_op_t> joinExprVec;
            auto joinExpr = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq, // equijoin
                    std::make_unique<Expressions::Identifier>(iuV),
                    std::make_unique<Expressions::Identifier>(iuU)
            );
            //Add join condition to edge
            graph.getEdge(vName,uName)->expressions.push_back(std::move(joinExpr));
        }
    }

    void SemanticAnalyser::construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan) {
        // Get the vertex struct from the join graph
        JoinGraph::Vertex *vertex = plan.graph.getVertex(vertexName);

        // Get edges connected to the vertex
        std::vector<JoinGraph::Edge*> edges(0);
        plan.graph.getConnectedEdges(vertexName,edges);

        // Mark vertex as visited
        vertex->visited = true;

        // If the vertex is the first join component add it as new root node of the join graph
        if (plan.joinedTree == nullptr) {
            plan.joinedTree = std::move(vertex->production);
        }

        for (auto& edge : edges) {

            // Get struct of neighboring vertex
            std::string &neighboringVertexName = edge->uID;
            if (vertexName.compare(edge->vID) != 0) {
                neighboringVertexName = edge->vID;
            }
            JoinGraph::Vertex *neighboringVertex = plan.graph.getVertex(neighboringVertexName);

            // If neighboring vertex has already been visited => discard edge
            if (neighboringVertex->visited) continue;

            // If the edge is directed from the neighboring node to the current node also change the order of the join leafs
            if (vertexName.compare(edge->vID) != 0) {
                plan.joinedTree = std::make_unique<Join>(
                        std::move(neighboringVertex->production),
                        std::move(plan.joinedTree),
                        std::move(edge->expressions),
                        Join::Method::Hash
                );
            } else {
                plan.joinedTree = std::make_unique<Join>(
                        std::move(plan.joinedTree),
                        std::move(neighboringVertex->production),
                        std::move(edge->expressions),
                        Join::Method::Hash
                );
            }

            //Construct join for the neighboring node
            construct_join(neighboringVertexName, context, plan);
        }
    }

    void SemanticAnalyser::construct_joins(QueryContext & context, QueryPlan & plan) {
        // Construct the join graph
        construct_join_graph(context,plan);

        //Start with the first vertex in the vector of vertices of the join graph
        std::string firstVertexName = plan.graph.getFirstVertexName();

        //Construct join the first vertex
        construct_join(firstVertexName, context, plan);
    }

//TODO: Make projections available to every node in the tree
//TODO: Check physical tree projections do not work after joining
    void SemanticAnalyser::construct_projection(QueryContext& context, QueryPlan & plan) {
        auto & db = context.db;

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (const std::string& projectedIUName : plan.parser_result.projections) {
            if (plan.iuNameToTable.find(projectedIUName) == plan.iuNameToTable.end()) {
                throw std::runtime_error("column " + projectedIUName + " not in scope");
            }
            for (auto& production : plan.ius) {
                for (auto& iu : production.second) {
                    if (iu.first.compare(projectedIUName) == 0) {
                        projectedIUs.push_back( iu.second );
                    }
                }
            }

        }

        if (plan.joinedTree != nullptr) {
            // Construct Result and store it in the query plan struct
            plan.tree = std::make_unique<Result>( std::move(plan.joinedTree), projectedIUs );
        } else {
            throw std::runtime_error("no or more than one root found: Table joining has failed");
        }
    }

    void SemanticAnalyser::construct_update(QueryContext& context, QueryPlan & plan) {
        if (plan.dangling_productions.size() != 1 || plan.parser_result.relations.size() != 1) {
            throw std::runtime_error("no or more than one root found: Table joining has failed");
        }

        auto &relationName = plan.parser_result.relations[0].second;
        if (relationName.length() == 0) relationName = plan.parser_result.relations[0].first;
        Table* table = context.db.getTable(plan.parser_result.relations[0].first);

        std::vector<std::pair<iu_p_t,std::string>> updateIUs;

        //Get all ius of the tuple to update
        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                updateIUs.emplace_back( iu.second, "" );
            }
        }

        //Map values to be updated to the corresponding ius
        for (auto columnValuePairs : plan.parser_result.columnToValue) {
            const std::string &valueString = columnValuePairs.second;

            for (auto &iuPair : updateIUs) {
                if (iuPair.first->columnInformation->columnName.compare(columnValuePairs.first) == 0) {
                    iuPair.second = valueString;
                }
            }
        }

        auto &production = plan.dangling_productions[relationName];
        plan.tree = std::make_unique<Update>( std::move(production), updateIUs, *table, new std::string(relationName));
    }

    void SemanticAnalyser::construct_delete(QueryContext& context, QueryPlan & plan) {
        if (plan.dangling_productions.size() != 1 || plan.parser_result.relations.size() != 1) {
            throw std::runtime_error("no or more than one root found: Table joining has failed");
        }

        auto &relationName = plan.parser_result.relations[0].second;
        if (relationName.length() == 0) relationName = plan.parser_result.relations[0].first;
        Table* table = context.db.getTable(plan.parser_result.relations[0].first);

        iu_p_t tidIU;

        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                if (iu.first.compare("tid") == 0) {
                    tidIU = iu.second;
                    break;
                }
            }
        }

        auto &production = plan.dangling_productions[relationName];
        plan.tree = std::make_unique<Delete>( std::move(production), tidIU, *table);
    }

    std::unique_ptr<SemanticAnalyser> SemanticAnalyser::getSemanticAnalyser(QueryContext &context,
            tardisParser::SQLParserResult &parserResult) {
        switch (parserResult.opType) {
            case tardisParser::SQLParserResult::OpType::Select:
                return std::make_unique<SelectAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::Insert:
                return std::make_unique<InsertAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::Update:
                return std::make_unique<UpdateAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::Delete:
                return std::make_unique<DeleteAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::CreateTable:
                return std::make_unique<CreateTableAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::CreateBranch:
                return std::make_unique<CreateBranchAnalyser>(context,parserResult);
        }

        return nullptr;
    }

    std::unique_ptr<Operator> SemanticAnalyser::analyseQuery(QueryContext& context, std::string sql) {
        tardisParser::SQLParserResult parserResult = tardisParser::SQLParser::parse_sql_statement(sql);

        std::unique_ptr<SemanticAnalyser> analyser = getSemanticAnalyser(context,parserResult);
        analyser->verify();

        return std::move(analyser->constructTree());
    }
}



