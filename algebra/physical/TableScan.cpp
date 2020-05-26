//
// Created by josef on 31.12.16.
//

#include "algebra/physical/TableScan.hpp"

#include <llvm/IR/TypeBuilder.h>
#include "foundations/version_management.hpp"
#include <unordered_map>

#include "codegen/PhiNode.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

TableScan::TableScan(const logical_operator_t & logicalOperator, Table & table) :
        NullaryOperator(std::move(logicalOperator)),
        table(table)
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

        columns.emplace_back(ci, columnTy, columnPtr, columnIndex);
    }
}

TableScan::~TableScan()
{ }

void TableScan::produce()
{
    //Functions::genPrintfCall("%s\n",_codeGen.getCurrentFunctionGen().getArg(1));

    //llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(_codeGen.getLLVMContext());
    //lvm::CallInst * result = _codeGen.CreateCall(&sampleFunction, funcTy, {_codeGen.getCurrentFunctionGen().getArg(1)});

    auto & funcGen = _codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) {
        return; // nothing to produce
    }

    // iterate over all tuples
#ifdef __APPLE__
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ull)}});
#else
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
#endif
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        auto branchId = _context.executionContext.branchId;
        IfGen visibilityCheck(isVisible(tid, branchId));
        {
            produce(tid);
        }
        visibilityCheck.EndIf();
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}

void *getValuePointer(size_t idx, Native::Sql::SqlTuple *tuple) {
    Native::Sql::Value *value = tuple->values[idx].get();
    switch (value->type.typeID) {
        case Native::Sql::SqlType::TypeID::UnknownID:
            return nullptr;
        case Native::Sql::SqlType::TypeID::BoolID:
            return &((Native::Sql::Bool *)value)->value;
        case Native::Sql::SqlType::TypeID::IntegerID:
            return &((Native::Sql::Integer *)value)->value;
        case Native::Sql::SqlType::TypeID::VarcharID:
            return &((Native::Sql::Text *)value)->value;
//        case SqlType::TypeID::CharID:
//            return Char::load(ptr, type);
        case Native::Sql::SqlType::TypeID::NumericID:
            return &((Native::Sql::Numeric *)value)->value;
        case Native::Sql::SqlType::TypeID::DateID:
            return &((Native::Sql::Date *)value)->value;
        case Native::Sql::SqlType::TypeID::TimestampID:
            return &((Native::Sql::Timestamp *)value)->value;
        case Native::Sql::SqlType::TypeID::TextID:
            return &((Native::Sql::Text *)value)->value;
        default:
            throw InvalidOperationException("unknown type");
    }
}




Native::Sql::SqlTuple *get_latest_tuple_wrapper(tid_t tid, Table & table, QueryContext & ctx) {
    std::unique_ptr<Native::Sql::SqlTuple> nativeSqlTuple = get_latest_tuple(tid,table,ctx);

    return new Native::Sql::SqlTuple(std::move(nativeSqlTuple->values));
}

    /*
Sql::Value *getTupleValueOffset(size_t idx, Sql::SqlTuple *tuple) {
    return tuple->values[idx].get();
}

void printTuple(Native::Sql::SqlTuple* tuple) {
    std::cout << "Str: " << Native::Sql::toString(*tuple) << "\n";
}

Sql::SqlType castTypeFromSqlType(Native::Sql::SqlType original, size_t len) {
    Sql::SqlType type;
    switch (original.typeID) {
        case SqlType::TypeID::UnknownID:
            return Sql::SqlType(Sql::SqlType::TypeID::UnknownID, original.nullable);
        case SqlType::TypeID::BoolID:
            return Sql::SqlType(Sql::SqlType::TypeID::BoolID, original.nullable);
        case SqlType::TypeID::IntegerID:
            return Sql::SqlType(Sql::SqlType::TypeID::IntegerID, original.nullable);
        case SqlType::TypeID::VarcharID:
            type = Sql::SqlType(Sql::SqlType::TypeID::VarcharID, original.nullable);
            type.length = len;
            break;
//        case SqlType::TypeID::CharID:
//            return Char::load(ptr, type);
        case SqlType::TypeID::NumericID:
            return Sql::SqlType(Sql::SqlType::TypeID::NumericID, original.nullable);
        case SqlType::TypeID::DateID:
            return Sql::SqlType(Sql::SqlType::TypeID::DateID, original.nullable);
        case SqlType::TypeID::TimestampID:
            return Sql::SqlType(Sql::SqlType::TypeID::TimestampID, original.nullable);
        case SqlType::TypeID::TextID:
            type = Sql::SqlType(Sql::SqlType::TypeID::VarcharID, original.nullable);
            type.length = len;
            break;
        default:
            throw InvalidOperationException("unknown type");
    }
    return type;
}

std::unique_ptr<Sql::Value> castValueFromSqlValue(std::unique_ptr<Native::Sql::Value> &original) {
    std::string valueString = Native::Sql::toString(*original);
    return Sql::Value::castString(valueString,castTypeFromSqlType(original->type,valueString.size()));
}

Sql::SqlTuple *get_latest_tuple_wrapper(tid_t tid, Table & table, QueryContext & ctx) {
    std::unique_ptr<Native::Sql::SqlTuple> nativeSqlTuple = get_latest_tuple(tid,table,ctx);
    std::vector<Native::Sql::value_op_t> &nativeValues = nativeSqlTuple->values;
    std::vector<Sql::value_op_t> values;
    for (auto &nativeValue : nativeValues) {
        values.emplace_back(std::move(castValueFromSqlValue(nativeValue)));
    }
    return new Sql::SqlTuple(std::move(values));
}
*/


void TableScan::produce(cg_tid_t tid) {
    std::unordered_map<const InformationUnit*, std::unique_ptr<Sql::Value>> values;

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    iu_set_t required = getRequired();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (size_t, void * , void *), false>::get(_codeGen.getLLVMContext());
    llvm::CallInst * result = _codeGen.CreateCall(&get_latest_tuple_wrapper, funcTy, {tid, cg_ptr8_t::fromRawPointer(&table), _codeGen.getCurrentFunctionGen().getArg(1)});

    cg_voidptr_t tuplePtr = cg_voidptr_t( llvm::cast<llvm::Value>(result) );

    size_t i = 0;
    for (auto iu : required) {
        // the final value
        value_op_t sqlValue;

        if (iu->columnInformation->columnName.compare("tid") == 0) {

            //Add tid to the produced values
            llvm::Value *tidValue = tid.getValue();
            sqlValue = std::make_unique<LongInteger>(tidValue);
            values[iu] = std::move(sqlValue);

        } else {
            column_t & column = columns[i];

            ci_p_t ci = std::get<0>(column);

            // calculate the SQL value pointer
/*#ifdef __APPLE__
            llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ull), tid });
#else
            llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });
#endif*/

            llvm::FunctionType * funcGetValuePtr = llvm::TypeBuilder<void* (size_t, void *), false>::get(_codeGen.getLLVMContext());
            cg_size_t index_gen = cg_size_t(std::get<3>(column));
            llvm::CallInst * resultGetValuePtr = _codeGen.CreateCall(&getValuePointer, funcGetValuePtr, {index_gen, tuplePtr});
            cg_voidptr_t value_ptr = cg_voidptr_t( llvm::cast<llvm::Value>(resultGetValuePtr) );

            llvm::Value * elemPtr = _codeGen->CreatePointerCast(value_ptr,llvm::PointerType::getUnqual(toLLVMTy(ci->type)));


            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);
                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
                SqlType notNullableType = toNotNullableTy(ci->type);
                auto loadedValue = Value::load(elemPtr, notNullableType);
                sqlValue = NullableValue::create(std::move(loadedValue), isNull);
            } else {
                // load the SQL value
                sqlValue = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = std::move(sqlValue);

            i += 1;
        }
    }

    iu_value_mapping_t returnValues;
    for (auto& mapping : values) {
        returnValues.emplace(mapping.first,mapping.second.get());
    }

    _parent->consume(returnValues, *this);
}

cg_bool_t TableScan::isVisible(cg_tid_t tid, cg_branch_id_t branchId)
{
    auto & branchBitmap = table.getBranchBitmap();
    return isVisibleInBranch(branchBitmap, tid, branchId);
}

} // end namespace Physical
} // end namespace Algebra
