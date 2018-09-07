//
// Created by josef on 30.12.16.
//

#include "HashJoin.hpp"

#include <algorithm>
#include <memory>

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/StaticHashtable.hpp"
#include "foundations/MemoryPool.hpp"
#include "foundations/utils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"

using namespace Sql;

//-----------------------------------------------------------------------------
// join list

/// \returns The join list header type
static llvm::Type * getListHeaderTy()
{
    /*
     * struct ListHeader { void * next; void * last; size_t len; };
     */
    auto & codeGen = getThreadLocalCodeGen();
    auto & typeCache = codeGen.getTypeCache();

    std::string name = "ListHeader";
    llvm::Type * type = typeCache.get(name); // we use the same type for every join

    // create the type signature
    if (type == nullptr) {
        auto & context = codeGen->getContext();

        std::vector<llvm::Type *> members(3);

        // struct members:
        members[0] = llvm::PointerType::getInt8PtrTy(context);
        members[1] = llvm::PointerType::getInt8PtrTy(context);
        members[2] = cg_size_t::getType();

        llvm::StructType * nodeTy = llvm::StructType::create(context, name);
        nodeTy->setBody(members);

        type = nodeTy;
        typeCache.add(name, type);
    }

    return type;
}

/// \brief Allocates a new list header
/// \returns The pointer to the newly allocated header
llvm::Value * genCreateListHeader(cg_voidptr_t memoryPool)
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * headerTy = getListHeaderTy();

    auto & dataLayout = codeGen.getCurrentModuleGen().getDataLayout();
    size_t size = dataLayout.getTypeAllocSize( headerTy );
    cg_size_t cgSize(size);
    cg_voidptr_t mem = genMemoryPoolMallocCall(memoryPool, cgSize);

    // set headerPtr->last = headerPtr
    // => we don't need to check whether the list is empty on every insert

    llvm::Type * headerPtrTy = llvm::PointerType::getUnqual(headerTy);
    llvm::Value * headerPtr = codeGen->CreatePointerCast(mem, headerPtrTy);

//    Functions::genPrintfCall("allocated headerPtr: %p\n", mem);

    llvm::Value * headerLastFieldPtr = codeGen->CreateStructGEP(headerTy, headerPtr, 1);
    codeGen->CreateStore(headerPtr, headerLastFieldPtr);

    return headerPtr;
}

/// \returns The Node type based on the contained data type
llvm::Type * getListNodeTy(llvm::Type * dataTy)
{
    /*
     * struct { void * next; uint64 hash; tuple; };
     */
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen->getContext();

    std::vector<llvm::Type *> members(3);

    // struct members:
    members[0] = llvm::PointerType::getInt8PtrTy(context);
    members[1] = cg_hash_t::getType();
    members[2] = dataTy;

    llvm::StructType * nodeTy = llvm::StructType::create(context, "ListNode");
    nodeTy->setBody(members);

    return nodeTy;
}

/// \brief Appends a new entry to the list
void genAppendEntryToList(cg_voidptr_t memoryPool, llvm::Value * headerPtr, llvm::Type * nodeTy, cg_hash_t hash, SqlTuple & tuple)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & dataLayout = codeGen.getCurrentModuleGen().getDataLayout();

//    llvm::Type * nodeTy = getListNodeTy(tuple.getLLVMType());

    // allocated node:
    size_t size = dataLayout.getTypeAllocSize( nodeTy );
    cg_size_t cgSize(size);
    cg_voidptr_t node = genMemoryPoolMallocCall(memoryPool, cgSize);

    // update header:
    llvm::Type * headerTy = getPointeeType(headerPtr);
//    Functions::genPrintfCall("allocated node: %p\n", node);

    llvm::Value * headerLastFieldPtr = codeGen->CreateStructGEP(headerTy, headerPtr, 1);
    llvm::Value * headerLastPtr = codeGen->CreateLoad(headerLastFieldPtr);

//    Functions::genPrintfCall("current last ptr: %p\n", headerLastPtr);

    llvm::Value * headerLengthPtr = codeGen->CreateStructGEP(headerTy, headerPtr, 2);
    cg_size_t length( codeGen->CreateLoad( headerLengthPtr ) );
    length = length + 1;
    codeGen->CreateStore(length, headerLengthPtr);

//    Functions::genPrintfCall("new length: %lu\n", length);

    // update last node:
    codeGen->CreateStore(node, headerLastPtr);
    codeGen->CreateStore(node, headerLastFieldPtr);

    // initialise node:
    llvm::Type * nodePtrTy = llvm::PointerType::getUnqual(nodeTy);
    llvm::Value * nodePtr = codeGen->CreatePointerCast(node, nodePtrTy);

    llvm::Value * nodeHashFieldPtr = codeGen->CreateStructGEP(nodeTy, nodePtr, 1);
    codeGen->CreateStore(hash, nodeHashFieldPtr);

    llvm::Value * nodeTupleFieldPtr = codeGen->CreateStructGEP(nodeTy, nodePtr, 2);
//    Functions::genPrintfCall("nodeTupleFieldPtr: %p\n", nodeTupleFieldPtr);
    tuple.store(nodeTupleFieldPtr);
}

/// \returns A pair which consists of the first node and the list's length
std::pair<cg_voidptr_t, cg_size_t> genListGetHeaderData(llvm::Value * headerPtr)
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * headerTy = getPointeeType(headerPtr);

    llvm::Value * headerNextFieldPtr = codeGen->CreateStructGEP(headerTy, headerPtr, 0);
    llvm::Value * headerNextPtr = codeGen->CreateLoad(headerNextFieldPtr); // llvm Type: i8*

    llvm::Value * headerLengthFieldPtr = codeGen->CreateStructGEP(headerTy, headerPtr, 2);
    llvm::Value * headerLength = codeGen->CreateLoad(headerLengthFieldPtr);

    return std::make_pair(cg_voidptr_t(headerNextPtr), cg_size_t(headerLength));
};

//-----------------------------------------------------------------------------
// operator implementation

namespace Algebra {
namespace Physical {

enum Side : size_t { Left = 0, Right = 1 };

#if false
/// \brief Calculates the joint hash of all join attributes
template<Side side>
static cg_hash_t genJoinHash(const join_pair_vec_t & joinPairs, const iu_value_mapping_t & values)
{
    cg_hash_t seed(0ul);

    // iterate over each join pair and calculate the combined hash
    bool first = true;
    for (auto & joinPair : joinPairs) {
        // pair structure: (left iu, right iu)
        auto it = values.find( std::get<side>(joinPair) );
        assert(it != values.end());

        Value * sqlValue = it->second;
        if (first) {
            seed = sqlValue->hash();
            first = false;
        } else {
            seed = genHashCombine(seed, sqlValue->hash());
        }
    }

    return seed;
}
#endif

using join_pair_vec_t = HashJoin::join_pair_vec_t;

/// \brief Calculates the joint hash of all join attributes
template<Side side>
static cg_hash_t genJoinHash(const join_pair_vec_t & joinPairs, const iu_value_mapping_t & values)
{
    cg_hash_t seed(0ul);

    // iterate over each join pair and calculate the combined hash
    bool first = true;
    for (auto & joinPair : joinPairs) {
        // pair structure: (left expr, right expr)
        auto& expr = std::get<side>(joinPair);
        auto sqlValue = expr->evaluate(values);
        if (first) {
            seed = sqlValue->hash();
            first = false;
        } else {
            seed = genHashCombine(seed, sqlValue->hash());
        }
    }

    return seed;
}

HashJoin::HashJoin(const logical_operator_t & logicalOperator,
                   std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                   join_pair_vec_t pairs) :
        BinaryOperator(std::move(logicalOperator), std::move(left), std::move(right)),
        joinPairs(std::move(pairs))
{
//    constructIUSets();

    // sanity check:
#ifndef NDEBUG
    const auto & leftRequired = _leftChild->getRequired();
    const auto & rightRequired = _rightChild->getRequired();
    /* TODO (implement Expression::getRequired())
    for (const auto & p : pairs) {
        if (leftRequired.count(p.first) != 1 || rightRequired.count(p.second) != 1) {
            throw std::runtime_error("join pair order");
        }
    }
    */
#endif
}

HashJoin::~HashJoin()
{ }

void HashJoin::produce()
{
    // initialize the pool allocator
    memoryPool = genMemoryPoolCreateCall();
    listHeaderPtr = genCreateListHeader(memoryPool);

    _leftChild->produce();

    // build hashtable
    auto headerData = genListGetHeaderData(listHeaderPtr);

    joinTable = genStaticHashtableCreateCall(headerData.first, headerData.second);
//    Functions::genPrintfCall("joinTable: %p\n", joinTable);

    _rightChild->produce();

    // cleanup
    genStaticHashtableFreeCall(joinTable);
    genMemoryPoolFreeCall(memoryPool);
}

void HashJoin::probeCandidate(cg_voidptr_t rawNodePtr)
{
    assert(listNodeTy != nullptr);

//    Functions::genPrintfCall("===\nprobeCandidate() iter node: %p\n===\n", rawNodePtr);

    auto & codeGen = getThreadLocalCodeGen();

    // load the tuple
    llvm::Type * nodePtrTy = llvm::PointerType::getUnqual(listNodeTy);
    llvm::Value * nodePtr = codeGen->CreatePointerCast(rawNodePtr, nodePtrTy);
    llvm::Value * tuplePtr = codeGen->CreateStructGEP(listNodeTy, nodePtr, 2);
    auto tuple = SqlTuple::load(tuplePtr, storedTypes);
 
    // check if all related values do also match:
#if 1
    // naive first version:

    iu_value_mapping_t values;
    iu_p_t iu;
    size_t idx;
    for (auto & pair : tupleMapping) {
        std::tie(iu, idx) = pair;
        values[iu] = tuple->values[idx].get();
    }

    cg_bool_t match(true);
    for (auto & joinPair : joinPairs) {
        auto leftExprValue = joinPair.first->evaluate(values); // build side
        auto rightExprValue = joinPair.second->evaluate(*rightProduced); // probe side
/*
        Functions::genPrintfCall("===\nprobeCandidate() compare:");
        Utils::genPrintValue(*leftExprValue);
        Functions::genPrintfCall(" == ");
        Utils::genPrintValue(*rightExprValue);
        Functions::genPrintfCall(" -> %d\n", leftExprValue->equals(*rightExprValue));
*/
        match = match && leftExprValue->equals(*rightExprValue);
    }

    IfGen check(match);
    {
        // merge both sides
        auto & probeSet = dynamic_cast<const Logical::BinaryOperator &>(_logicalOperator).getRightRequired();
        for (iu_p_t iu : probeSet) {
            values[iu] = rightProduced->at(iu);
        }

        _parent->consume(values, *this);
    }
    check.EndIf();

#else // TODO port and benchmark the following code
    // this version builds a nested "if" construct in order to avoid unnecessary comparisons

    // "IfGen"s are not copyable (otherwise the BasicBlock structure would get mixed up)
    std::vector<std::unique_ptr<IfGen>> chain;

    for (auto pairIt = joinPairs.begin(); pairIt != joinPairs.end(); ++pairIt) {
        auto & joinPair = *pairIt;

        auto tupleIt = tupleMapping.find(joinPair.first);
        iu_p_t rightIU = joinPair.second;
        assert(rightProduced->count(rightIU) == 1);

        size_t i = tupleIt->second;
        Value & left = *(tuple->values[i]);
        Value & right = *(rightProduced->at(rightIU));

        // extend the nested "if"-chain by one statement
        cg_bool_t equals = left.equals(right);
        chain.emplace_back( std::make_unique<IfGen>(funcGen, equals) );

        // at this point all join-attributes are guaranteed to be equal
        if (std::next(pairIt) == joinPairs.end()) {
            // merge both sides
            iu_value_mapping_t values;
            for (iu_p_t iu : probeSet) {
                values[iu] = rightProduced->at(iu);
            }
            for (auto & pair : tupleMapping) {
                values[pair.first] = tuple->values[pair.second].get();
            }

            parent->consume(values, *this);
        }
    }

    // close the chain in reverse order
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        it->get()->EndIf();
    }
#endif
}

void HashJoin::consumeLeft(const iu_value_mapping_t & values)
{
    std::vector<value_op_t> leftTupleValues;

    // gather tuple information
    size_t i = 0;
    auto & buildSet = dynamic_cast<const Logical::BinaryOperator &>(_logicalOperator).getLeftRequired();
    for (iu_p_t iu : buildSet) {
        value_op_t sqlValue = values.at(iu)->clone();
        storedTypes.push_back(sqlValue->type);
        leftTupleValues.push_back( std::move(sqlValue) );
        tupleMapping[iu] = i++;
    }

    SqlTuple tuple( std::move(leftTupleValues) );
//    genPrintSqlTuple(tuple);

    // remember the type for further use
    listNodeTy = getListNodeTy( tuple.getType() );

    // add the left tuple to the list
    cg_hash_t h = genJoinHash<Left>(joinPairs, values);
    genAppendEntryToList(memoryPool, listHeaderPtr, listNodeTy, h, tuple);
}

void HashJoin::consumeRight(const iu_value_mapping_t & values)
{
    rightProduced = &values;

    cg_hash_t h = genJoinHash<Right>(joinPairs, values);

    // create the bucket iteration code:
    // genStaticHashtableIter() will pass the current element to probeCandidate(),
    // then probeCandidate() generates the code to handle elements with matching hash values
    genStaticHashtableIter(joinTable, h, [=](cg_voidptr_t nodePtr) {
        this->probeCandidate(nodePtr);
    });

    rightProduced = nullptr;
}

} // end namespace Physical
} // end namespace Algebra
