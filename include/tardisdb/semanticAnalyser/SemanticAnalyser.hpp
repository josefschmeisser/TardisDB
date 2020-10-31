#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "semanticAnalyser/AnalyzingContext.hpp"
#include "semanticAnalyser/ParserResult.hpp"
#include "semanticAnalyser/JoinGraph.hpp"

using namespace Algebra::Logical;

namespace semanticalAnalysis {
    struct semantic_sql_error : std::runtime_error {
        //semantic or syntactic errors
        using std::runtime_error::runtime_error;
    };

    struct QueryPlan {
        JoinGraph graph;

        std::unordered_map<std::string,std::unordered_map<std::string, iu_p_t>> ius;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;
    };

    class SemanticAnalyser {
    public:
        SemanticAnalyser(AnalyzingContext &context) : _context(context) {}
        virtual ~SemanticAnalyser() {};

        virtual void verify() = 0;
        virtual std::unique_ptr<Operator> constructTree() = 0;

    protected:
        AnalyzingContext &_context;

    public:
        static std::unique_ptr<SemanticAnalyser> getSemanticAnalyser(AnalyzingContext &context);

    protected:
        static void construct_scans(AnalyzingContext& context, QueryPlan & plan, std::vector<Relation> &relations);
        static void construct_selects(QueryPlan & plan, std::vector<std::pair<Column,std::string>> &selections);
    };

    //
    // StatementTypeAnalyser
    //

    class SelectAnalyser : public SemanticAnalyser {
    public:
        SelectAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        std::unique_ptr<Operator> constructTree() override;
    private:
        static void construct_joins(AnalyzingContext & context, QueryPlan & plan, SQLParserResult &parserResult);
        static void construct_join_graph(AnalyzingContext & context, QueryPlan & plan, SelectStatement *stmt);
        static void construct_join(std::string &vertexName, AnalyzingContext &context, QueryPlan &plan);
    };

    class InsertAnalyser : public SemanticAnalyser {
    public:
        InsertAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        std::unique_ptr<Operator> constructTree() override;
    };

    class UpdateAnalyser : public SemanticAnalyser {
    public:
        UpdateAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        std::unique_ptr<Operator> constructTree() override;
    };

    class DeleteAnalyser : public SemanticAnalyser {
    public:
        DeleteAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        std::unique_ptr<Operator> constructTree() override;
    };

    class CreateTableAnalyser : public SemanticAnalyser {
    public:
        CreateTableAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        std::unique_ptr<Operator> constructTree() override;
    };

    class CreateBranchAnalyser : public SemanticAnalyser {
    public:
        CreateBranchAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        std::unique_ptr<Operator> constructTree() override;
    };
}







