//
// Created by josef on 31.12.16.
//

#include "algebra/physical/TableScan.hpp"

#include <llvm/IR/TypeBuilder.h>
#include "foundations/version_management.hpp"
#include <unordered_map>

#include "sql/SqlTuple.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

TableScan::TableScan(const logical_operator_t & logicalOperator, Table & table, branch_id_t branchId, QueryContext &queryContext) :
        NullaryOperator(std::move(logicalOperator), queryContext),
        table(table),
        branchId(branchId)
{
    // collect all information which is necessary to access the columns
    for (auto iu : getRequired()) {
        auto ci = getColumnInformation(iu);
        if (ci->columnName.compare("tid") == 0) {
            continue;
        }

        SqlType storedSqlType;
        if (ci->type.nullable) {
            // the null indicator will be stored in the NullIndicatorTable
            storedSqlType = toNotNullableTy( ci->type );
        } else {
            storedSqlType = ci->type;
        }

        size_t tableSize = table.size();
        llvm::Type * elemTy = toLLVMTy(storedSqlType);
        llvm::Type * columnTy = llvm::ArrayType::get(elemTy, tableSize);
        llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);
        size_t columnIndex = 0;
        for (int i = 0; i<table.getColumnCount(); i++) {
            if (table.getColumnNames()[i].compare(ci->columnName) == 0) {
                columnIndex = i;
                break;
            }
        }

        columns.emplace_back(ci, columnTy, columnPtr, columnIndex, nullptr);
    }
}

TableScan::~TableScan()
{ }

void TableScan::produce()
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) return;  // nothing to produce

    // iterate over all tuples
#ifdef __APPLE__
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ull)}});
#else
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
#endif
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

#if USE_DATA_VERSIONING
        IfGen visibilityCheck(isVisible(tid, branchId));
        {
            produce(tid, branchId);
        }
        visibilityCheck.EndIf();
#else
        produce(tid);
#endif
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}


#if USE_DATA_VERSIONING
void TableScan::produce(cg_tid_t tid, branch_id_t branchId) {
    iu_value_mapping_t values;

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    iu_set_t required = getRequired();

    cg_voidptr_t resultPtr;
    cg_bool_t ptrIsNotNull(false);
    if (branchId != master_branch_id) {
        resultPtr = genGetLatestEntryCall(tid,branchId);
        ptrIsNotNull = nullPointerCheck(resultPtr);
    }


    size_t i = 0;
    for (auto iu : required) {
        // the final value
        value_op_t sqlValue;

        if (iu->columnInformation->columnName.compare("tid") == 0) {

            //Add tid to the produced values
            llvm::Value *tidValue = tid.getValue();
            tidSqlValue = std::make_unique<LongInteger>(tidValue);
            values[iu] = tidSqlValue.get();
        } else {
            column_t & column = columns[i];

            ci_p_t ci = std::get<0>(column);

            llvm::Value *elemPtr;
            if (branchId != master_branch_id) {
                elemPtr = getBranchElemPtr(tid,column,resultPtr,ptrIsNotNull);
            } else {
                elemPtr = getMasterElemPtr(tid,column);
            }

            // calculate the SQL value pointer

            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);
                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
                SqlType notNullableType = toNotNullableTy(ci->type);
                auto loadedValue = Value::load(elemPtr, notNullableType);
                std::get<4>(column) = NullableValue::create(std::move(loadedValue), isNull);
            } else {
                // load the SQL value
                std::get<4>(column) = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = std::get<4>(column).get();

            i += 1;
        }
    }

    _parent->consume(values, *this);
}
#else
void TableScan::produce(cg_tid_t tid) {
    iu_value_mapping_t values;

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    iu_set_t required = getRequired();


    size_t i = 0;
    for (auto iu : required) {
        // the final value
        value_op_t sqlValue;

        if (iu->columnInformation->columnName.compare("tid") == 0) {
            //Add tid to the produced values
            llvm::Value *tidValue = tid.getValue();
            tidSqlValue = std::make_unique<LongInteger>(tidValue);
            values[iu] = tidSqlValue.get();
        } else {
            column_t & column = columns[i];

            ci_p_t ci = std::get<0>(column);

            llvm::Value *elemPtr = getMasterElemPtr(tid,column);

            // calculate the SQL value pointer

            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);
                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
                SqlType notNullableType = toNotNullableTy(ci->type);
                auto loadedValue = Value::load(elemPtr, notNullableType);
                std::get<4>(column) = NullableValue::create(std::move(loadedValue), isNull);
            } else {
                // load the SQL value
                std::get<4>(column) = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = std::get<4>(column).get();

            i += 1;
        }
    }

    _parent->consume(values, *this);
}
#endif

llvm::Value *TableScan::getMasterElemPtr(cg_tid_t &tid, column_t &column) {
#ifdef __APPLE__
    llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ull), tid });
#else
    llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });
#endif
    return elemPtr;
}

llvm::Value *TableScan::getBranchElemPtr(cg_tid_t &tid, column_t &column, cg_voidptr_t &resultPtr, cg_bool_t &ptrIsNotNull) {
    IfGen check( _codeGen.getCurrentFunctionGen(), ptrIsNotNull, {{"elemPtr", cg_int_t(0)}} );
    {
        llvm::Value * valuePtr = tupleToElemPtr(resultPtr,column);
        check.setVar(0, valuePtr);
    }
    check.Else();
    {
        llvm::Value *elemPtr = getMasterElemPtr(tid,column);
        check.setVar(0, elemPtr);
    }
    check.EndIf();
    return check.getResult(0);
}

cg_voidptr_t TableScan::genGetLatestEntryCall(cg_tid_t tid, branch_id_t branchId) {
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (size_t, void * , void* , void *), false>::get(_codeGen.getLLVMContext());
    llvm::Function * func = llvm::cast<llvm::Function>( getThreadLocalCodeGen().getCurrentModuleGen().getModule().getOrInsertFunction("get_latest_entry", funcTy) );
    getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&get_latest_entry);
    llvm::CallInst * result = _codeGen->CreateCall(func, {tid, cg_ptr8_t::fromRawPointer(&table), cg_u32_t(branchId), _codeGen.getCurrentFunctionGen().getArg(1)});

    return cg_voidptr_t( llvm::cast<llvm::Value>(result) );
}

cg_bool_t TableScan::nullPointerCheck(cg_voidptr_t &ptr) {
#ifdef __APPLE__
    return cg_bool_t(cg_size_t(_codeGen->CreatePtrToInt(ptr, _codeGen->getIntNTy(64))) != cg_size_t(0ull));
#else
    return cg_bool_t(cg_size_t(_codeGen->CreatePtrToInt(ptr, _codeGen->getIntNTy(64))) != cg_size_t(0ul));
#endif
}

llvm::Value *TableScan::tupleToElemPtr(cg_voidptr_t &ptr, column_t &column) {
    llvm::Type * tupleTy = Sql::SqlTuple::getType(table.getTupleType());
    llvm::Type * tuplePtrTy = llvm::PointerType::getUnqual(tupleTy);
    llvm::Value * tuplePtr = _codeGen->CreatePointerCast(ptr, tuplePtrTy);

    return _codeGen->CreateStructGEP(tupleTy, tuplePtr, std::get<3>(column));
}

cg_bool_t TableScan::isVisible(cg_tid_t tid, cg_branch_id_t branchId)
{
    auto & branchBitmap = table.getBranchBitmap();
    return isVisibleInBranch(branchBitmap, tid, branchId);
}

} // end namespace Physical
} // end namespace Algebra
