
#include "Database.hpp"

#include <cmath>
#include <climits>

#include <llvm/IR/TypeBuilder.h>

#include "exceptions.hpp"

//-----------------------------------------------------------------------------
// NullIndicatorColumn

BitmapTable::BitmapTable() :
        BitmapTable(8) // FIXME once resize() is implemented this will no longer be necessary
{ }

BitmapTable::BitmapTable(size_t columnCountHint)
{
    unsigned bytesPerTuple = static_cast<unsigned>(std::ceil(columnCountHint / 8.0));
    _availableCount = bytesPerTuple*8;
    _data = std::make_unique<Vector>(bytesPerTuple);
}

unsigned BitmapTable::addColumn()
{
    unsigned column = _columnCount;
    _columnCount += 1;
    if (_columnCount > _availableCount) {
        resize(); // allocated one additional byte
    }

    for (size_t tid = 0, limit = _data->size(); tid < limit; ++tid) {
        set(tid, column, false);
    }

    return column;
}

void BitmapTable::addRow()
{
    void * row = _data->reserve_back();
    unsigned byteCount = _availableCount/8;
    memset(row, 0, byteCount);
}

void BitmapTable::set(tid_t tid, unsigned column, bool value)
{
    assert(column < _columnCount);

    uint8_t * tuple = static_cast<uint8_t *>(_data->at(tid));

    static_assert(CHAR_BIT == 8, "not supported");
    size_t byte = column >> 3; // column / 8
    unsigned sectionIndex = column & 7; // column % 8

    // calculate section
    uint8_t * sectionPtr = tuple + byte;
    uint8_t section = *sectionPtr;
    section ^= (-value ^ section) & (1 << sectionIndex); // set null indicator bit
    *sectionPtr = section;
}

bool BitmapTable::isSet(tid_t tid, unsigned column)
{
    assert(column < _columnCount);

    uint8_t * tuple = static_cast<uint8_t *>(_data->at(tid));

    static_assert(CHAR_BIT == 8, "not supported");
    size_t byte = column >> 3; // column / 8
    unsigned sectionIndex = column & 7; // column % 8

    // calculate section
    uint8_t * sectionPtr = tuple + byte;
    uint8_t section = *sectionPtr;
    return static_cast<bool>((section >> sectionIndex) & 1); // test null indicator bit
}

void BitmapTable::resize()
{
    throw NotImplementedException("BitmapTable::resize()");
}

static cg_bool_t retrieveValue(BitmapTable & table, cg_tid_t tid, cg_unsigned_t column)
{
    auto & codeGen = getThreadLocalCodeGen();

    cg_ptr8_t tablePtr = cg_ptr8_t::fromRawPointer(table.data());

    // section index within the row
    cg_size_t sectionIndex( codeGen->CreateLShr(column, 3) );

    cg_size_t bytesPerRow = table.getRowSize();
    cg_size_t offset = tid * bytesPerRow;

    llvm::Value * indicatorIndex = codeGen->CreateAnd(column, cg_unsigned_t(7));
    cg_size_t byteIndex = sectionIndex + offset;

    // calculate section
    cg_ptr8_t sectionPtr( tablePtr + byteIndex.llvmValue );
    cg_u8_t section( codeGen->CreateLoad(cg_u8_t::getType(), sectionPtr) );
    cg_u8_t shifted( codeGen->CreateLShr(section, indicatorIndex) );
    cg_bool_t result( codeGen->CreateTrunc(shifted, cg_bool_t::getType()) );
    return result;
}

cg_bool_t genNullIndicatorLoad(BitmapTable & table, cg_tid_t tid, cg_unsigned_t column)
{
    return retrieveValue(table, tid, column);
}

cg_bool_t isVisibleInBranch(BitmapTable & branchBitmap, cg_tid_t tid, cg_branch_id_t branchId)
{
    return retrieveValue(branchBitmap, tid, branchId);
}

//-----------------------------------------------------------------------------
// Table

Table::Table()
{
    createBranch("master");
}

void Table::addColumn(const std::string & columnName, Sql::SqlType type)
{
    _columnNames.push_back(columnName);

    // the null indicator is not part of the permanent storage layout (for both types)
#ifndef USE_INTERNAL_NULL_INDICATOR
    size_t valueSize = getValueSize( Sql::toNotNullableTy(type) );
#else
    size_t valueSize = getValueSize(type);
#endif

    // set-up column
    auto column = std::make_unique<Vector>(valueSize);
    auto ci = std::make_unique<ColumnInformation>();
    ci->column = column.get();
    ci->columnName = columnName;
    ci->type = type;

    if (type.nullable) {
#ifndef USE_INTERNAL_NULL_INDICATOR
        ci->nullColumnIndex = _nullIndicatorTable.addColumn();
        ci->nullIndicatorType = ColumnInformation::NullIndicatorType::Column;
#else
        ci->nullIndicatorType = ColumnInformation::NullIndicatorType::Embedded;
#endif
    }

    _columns.emplace(columnName, std::make_pair(std::move(ci), std::move(column)));
}

void Table::addRow()
{
    for (auto & column : _columns) {
        column.second.second->reserve_back();
    }
    _nullIndicatorTable.addRow();
    _branchBitmap.addRow();
    // TODO update visibility
}

void Table::createBranch(const std::string & name)
{
    _branchBitmap.addColumn();
    // TODO update visibilities
}

ci_p_t Table::getCI(const std::string & columnName) const
{
    return _columns.at(columnName).first.get();
}

const Vector & Table::getColumn(const std::string & columnName) const
{
    return *_columns.at(columnName).second;
}

size_t Table::getColumnCount() const
{
    return _columns.size();
}

const std::vector<std::string> & Table::getColumnNames() const
{
    return _columnNames;
}

size_t Table::size() const
{
    if (_columns.empty()) {
        return 0;
    } else {
        return _columns.begin()->second.second->size();
    }
}

// wrapper functions
static void tableAddRow(Table * table)
{
    table->addRow();
}

// generator functions
void genTableAddRowCall(cg_voidptr_t table)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    codeGen.CreateCall(&tableAddRow, funcTy, table.getValue());
}

//-----------------------------------------------------------------------------
// Database

void Database::addTable(std::unique_ptr<Table> table, const std::string & name)
{
    _tables.emplace(name, std::move(table));
}

Table * Database::getTable(const std::string & tableName)
{
    // TODO search case insensitive
    auto it = _tables.find(tableName);
    if (it != _tables.end()) {
        return it->second.get();
    } else {
        return nullptr;
    }
}
