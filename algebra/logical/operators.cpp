
#include "operators.hpp"

#include "exceptions.hpp"
#include "sql/SqlType.hpp"

namespace Algebra {
namespace Logical {

//-----------------------------------------------------------------------------
// Operator

Operator * Operator::getRoot()
{
    Operator * root = this;
    while (root->getParent() != nullptr) {
        root = root->getParent();
    }
    return root;
}

const iu_set_t & Operator::getProduced()
{
    if (!producedUpToDate) {
        // it is most likely that the whole tree is not up to date
        updateProducedSets();
    }
    return produced;
}

const iu_set_t & Operator::getRequired()
{
    if (!requiredUpToDate) {
        // it is most likely that the whole tree is not up to date
        updateRequiredSets();
    }
    return required;
}

void Operator::updateProducedSets()
{
    Operator * root = getRoot();
    root->updateProducedSetsTraverser();
}

void Operator::updateRequiredSets()
{
    Operator * root = getRoot();
    root->updateRequiredSetsTraverser();
}

//-----------------------------------------------------------------------------
// NullaryOperator

NullaryOperator::NullaryOperator(QueryContext & context) :
        Operator(context)
{ }

NullaryOperator::~NullaryOperator()
{ }

size_t NullaryOperator::arity() const
{
    return 0;
}

void NullaryOperator::updateProducedSetsTraverser()
{
    computeProduced();
    producedUpToDate = true;
}

void NullaryOperator::updateRequiredSetsTraverser()
{
    computeRequired();
    requiredUpToDate = true;
}

//-----------------------------------------------------------------------------
// UnaryOperator
UnaryOperator::UnaryOperator(std::unique_ptr<Operator> input) :
        Operator(input->getContext()),
        child(std::move(input))
{
    child->parent = this;
}

UnaryOperator::~UnaryOperator()
{ }

size_t UnaryOperator::arity() const
{
    return 1;
}

void UnaryOperator::updateProducedSetsTraverser()
{
    child->updateProducedSetsTraverser();
    computeProduced();
    producedUpToDate = true;
}

void UnaryOperator::updateRequiredSetsTraverser()
{
    computeRequired();
    requiredUpToDate = true;
    child->updateRequiredSetsTraverser();
}

//-----------------------------------------------------------------------------
// BinaryOperator

BinaryOperator::BinaryOperator(std::unique_ptr<Operator> leftInput, std::unique_ptr<Operator> rightInput) :
        Operator(leftInput->getContext()),
        leftChild(std::move(leftInput)),
        rightChild(std::move(rightInput))
{
    leftChild->parent = this;
    rightChild->parent = this;
}

BinaryOperator::~BinaryOperator()
{ }

size_t BinaryOperator::arity() const
{
    return 2;
}

void BinaryOperator::updateProducedSetsTraverser()
{
    leftChild->updateProducedSetsTraverser();
    rightChild->updateProducedSetsTraverser();
    computeProduced();
    producedUpToDate = true;
}

void BinaryOperator::updateRequiredSetsTraverser()
{
    computeRequired();
    leftChild->updateRequiredSetsTraverser();
    rightChild->updateRequiredSetsTraverser();
    requiredUpToDate = true;
}

//-----------------------------------------------------------------------------
// GroupBy operator

namespace Aggregations {

iu_p_t Aggregator::getProduced()
{
    if (!_producedUpToDate) {
        computeProduced();
        _producedUpToDate = true;
    }
    return _produced;
}

const iu_set_t & Aggregator::getRequired()
{
    if (!_requiredUpToDate) {
        computeRequired();
        _requiredUpToDate = true;
    }
    return _required;
}

void Aggregator::computeProduced()
{
    _produced = _context.iuFactory.createIU(getResultType());
}

void Keep::computeRequired()
{
    _required.clear();
    _required.insert(_keep);
}

void Sum::computeRequired()
{
    _required = collectRequired(*_expression);
}

Avg::Avg(QueryContext & context, logical_exp_op_t exp) :
        Aggregator(context),
        _expression(std::move(exp))
{
    Sql::SqlType type = _expression->getType();
    if (type.typeID == Sql::SqlType::TypeID::NumericID) {
        // TODO change precision?
    } else {
        type = Sql::getNumericFullLengthTy(4);
        auto cast = std::make_unique<Expressions::Cast>(std::move(_expression), type);
        _expression = std::move(cast);
    }
}

void Avg::computeRequired()
{
    _required = collectRequired(*_expression);
}

void CountAll::computeRequired()
{
    _required.clear();
}

void Min::computeRequired()
{
    _required = collectRequired(*_expression);
}

} // end namespace Aggregations

GroupBy::GroupBy(std::unique_ptr<Operator> input, std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations) :
        UnaryOperator(std::move(input)),
        _aggregations(std::move(aggregations))
{
    for (auto & aggregation : _aggregations) {
        aggregation->parent = this;
    }
}

void GroupBy::accept(OperatorVisitor & visitor)
{
    visitor.visit(*this);
}

void GroupBy::computeProduced()
{
    produced.clear();

    // add aggregated values
    for (auto & aggregation : _aggregations) {
        produced.insert( aggregation->getProduced() );
    }
}

void GroupBy::computeRequired()
{
    required.clear();

    // the group by operator doesn't pass up IUs of its child

    // add the input ius of all aggregator functions
    for (auto & aggregation : _aggregations) {
        iu_set_t funcRequired = aggregation->getRequired();
        required.insert(funcRequired.begin(), funcRequired.end());
    }
}

//-----------------------------------------------------------------------------
// Join operator

void Join::accept(OperatorVisitor & visitor)
{
    visitor.visit(*this);
}

void Join::computeProduced()
{
    produced.clear();

    const auto & left_ui_set = leftChild->getProduced();
    const auto & right_ui_set = rightChild->getProduced();

    produced.insert(left_ui_set.begin(), left_ui_set.end());
    produced.insert(right_ui_set.begin(), right_ui_set.end());
}

void Join::computeRequired()
{
    required.clear();

    // the join operator just merges two tuple streams, therefore it requires every IU of its parent
    iu_set_t expected = computeExpected(parent, this);
    required.insert(expected.begin(), expected.end());

    // add the join attributes (they might not be needed by the parent operator)
    iu_set_t expRequired = collectRequired(*_joinExp);
    required.insert(expRequired.begin(), expRequired.end());
}

//-----------------------------------------------------------------------------
// Select operator

void Select::accept(OperatorVisitor & visitor)
{
    visitor.visit(*this);
}

void Select::computeProduced()
{
    produced.clear();

    const iu_set_t & childProduced = child->getProduced();
    produced.insert(childProduced.begin(), childProduced.end());
}

void Select::computeRequired()
{
    required.clear();

    // parent IUs + attr IU
    iu_set_t expected = computeExpected(parent, this);
    required.insert(expected.begin(), expected.end());
    iu_set_t expRequired = collectRequired(*_exp);
    required.insert(expRequired.begin(), expRequired.end());
}

//-----------------------------------------------------------------------------
// TableScan operator

void TableScan::accept(OperatorVisitor & visitor)
{
    visitor.visit(*this);
}

void TableScan::computeProduced()
{
    if (!produced.empty()) {
        // the produced set of this operator stays always the same
        return;
    }

    // collect the produced attributes
    for (const std::string & columnName : _table.getColumnNames()) {
        auto columnInformation = _table.getCI(columnName);
        auto iu =  _context.iuFactory.createIU(*this, columnInformation);
        produced.insert(iu);
    }
}

void TableScan::computeRequired()
{
    required.clear();

    iu_set_t expected = computeExpected(parent, this);
    required.insert(expected.begin(), expected.end());
}

//-----------------------------------------------------------------------------
// Result operator

Result::Result(std::unique_ptr<Operator> child, const std::vector<iu_p_t> & selection) :
        UnaryOperator(std::move(child)),
        selection(selection)
{ }

Result::~Result()
{ }

void Result::accept(OperatorVisitor & visitor)
{
    visitor.visit(*this);
}

void Result::computeProduced()
{
    produced.clear();
}

void Result::computeRequired()
{
    // "selection" represents the required attributes of the Result operator
    for (iu_p_t iu : selection) {
        required.insert(iu);
    }
}

//-----------------------------------------------------------------------------
// Utils

iu_set_t computeExpected(Operator * parent, Operator * child)
{
    assert(child != nullptr);

    if (parent == nullptr) {
        return iu_set_t();
    }

    // parentRequired contains the union of all branches
    iu_set_t parentRequired = parent->getRequired();

    if (parent->arity() > 1) {
        iu_set_t expected;
        const iu_set_t & produced = child->getProduced();

        // filter out the other branches
        std::set_intersection(parentRequired.begin(), parentRequired.end(),
                              produced.begin(), produced.end(),
                              std::inserter(expected, expected.end()));
        return expected;
    } else {
        return parentRequired;
    }
}

struct ExpressionIUCollector : public Expressions::ExpressionVisitor {
    iu_set_t & _collected;

    ExpressionIUCollector(iu_set_t & collected) :
            _collected(collected)
    { }

    void visit(Expressions::Cast & exp) override
    {
        exp.getChild().accept(*this);
    }

    void visit(Expressions::Addition & exp) override
    {
        exp.getLeftChild().accept(*this);
        exp.getRightChild().accept(*this);
    }

    void visit(Expressions::Subtraction & exp) override
    {
        exp.getLeftChild().accept(*this);
        exp.getRightChild().accept(*this);
    }

    void visit(Expressions::Multiplication & exp) override
    {
        exp.getLeftChild().accept(*this);
        exp.getRightChild().accept(*this);
    }

    void visit(Expressions::Division & exp) override
    {
        exp.getLeftChild().accept(*this);
        exp.getRightChild().accept(*this);
    }

    void visit(Expressions::Comparison & exp) override
    {
        exp.getLeftChild().accept(*this);
        exp.getRightChild().accept(*this);
    }

    void visit(Expressions::Identifier & exp) override
    {
        _collected.insert(exp._iu);
    }

    void visit(Expressions::Constant & exp) override { /* NOP */ }
    void visit(Expressions::NullConstant & exp) override { /* NOP */ }
};

iu_set_t collectRequired(const Expressions::Expression & exp)
{
    auto & current = const_cast<Expressions::Expression &>(exp);
    iu_set_t collected;
    ExpressionIUCollector collector(collected);
    current.accept(collector);
    return collected;
}

struct Verifier : public OperatorVisitor {
    bool _result = true;

    Verifier(Result & root)
    {
        root.accept(*this);
    }

    void test(Operator & parent, Operator & child)
    {
        if (!_result) { return; }

        _result = is_subset(parent.getRequired(), child.getProduced());
    }

    void visit(GroupBy & op) override
    {
        if (!_result) { return; }

        test(op, op.getChild());
        op.getChild().accept(*this);
    }

    void visit(Join & op) override
    {
        if (!_result) { return; }

        test(op, op.getLeftChild());
        test(op, op.getRightChild());
        op.getLeftChild().accept(*this);
        op.getRightChild().accept(*this);
    }

    void visit(Map & op) override
    {
        if (!_result) { return; }

        test(op, op.getChild());
        op.getChild().accept(*this);
    }

    void visit(Result & op) override
    {
        if (!_result) { return; }

        test(op, op.getChild());
        op.getChild().accept(*this);
    }

    void visit(Select & op) override
    {
        if (!_result) { return; }

        test(op, op.getChild());
        op.getChild().accept(*this);
    }

    void visit(TableScan & op) override
    {
        // NOP
    }
};

bool verifyDependencies(Result & root)
{
    Verifier verifier(root);
    return verifier._result;
}

} // end namespace Logical
} // end namespace Algebra
