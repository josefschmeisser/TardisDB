//
// Created by Blum Thomas on 2020-08-13.
//

#include <fstream>
#include <iostream>
#include <sstream>
#include <random>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <foundations/StringPool.hpp>

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "foundations/version_management.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "utils/general.hpp"

#if LIBXML_AVAILABLE
    #include "wikiParser/WikiParser.hpp"
#endif

#if PERF_AVAILABLE
#include "perfevent/PerfEvent.hpp"
#endif

#include "gflags/gflags.h"

DEFINE_bool(b, false, "isBenchmarking");
DEFINE_string(l, "wikidb", "database");
DEFINE_double(d, 0.5, "distribution");
DEFINE_uint64(r, 1, "runs");
DEFINE_uint64(lowerBound, 1, "lowerBound");
DEFINE_uint64(upperBound, 30303, "upperBound");

static bool ValidateDatabase(const char *flagname, const std::string &value) {
    return value.compare("wikidb") == 0;
}

static bool ValidateDistribution(const char *flagname, const std::string &value) {
    std::vector<std::string> distributions = split(value,',');
    return distributions.size() == 2;
}

DEFINE_validator(l, &ValidateDatabase);

#if USE_DATA_VERSIONING
void loadWikiDb(Database *db, int lowerBound, int upperBound)
{
    uint64_t loadDuration = 0;

    std::stringstream ssRange;
    ssRange << "_";
    ssRange << lowerBound;
    ssRange << "_";
    ssRange << upperBound;
    std::string pageRangeStr = ssRange.str();


    std::string pageFileName = "page" + pageRangeStr + ".tbl";
    std::string revisionFileName = "revision" + pageRangeStr + ".tbl";
    std::string contentFileName = "content" + pageRangeStr + ".tbl";
    std::string userFileName = "user" + pageRangeStr + ".tbl";

    QueryCompiler::compileAndExecute("CREATE TABLE page ( id INTEGER NOT NULL, title TEXT NOT NULL , userId INTEGER NOT NULL , content TEXT NOT NULL );",*db);
    QueryCompiler::compileAndExecute("CREATE TABLE user ( id INTEGER NOT NULL, name TEXT NOT NULL );",*db);

    std::ifstream streamRevision(revisionFileName);
    if (!streamRevision) { throw std::runtime_error("file not found: tables/revision.tbl"); }
    std::ifstream streamPage(pageFileName);
    if (!streamPage) { throw std::runtime_error("file not found: tables/page.tbl"); }
    std::ifstream streamContent(contentFileName);
    if (!streamContent) { throw std::runtime_error("file not found: tables/content.tbl"); }
    std::ifstream streamUser(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    Table *pageTable = db->getTable("page");
    Table *userTable = db->getTable("user");

    std::set<std::string> userIDS = {};
    std::string userRowStr;

    QueryContext userCtx(*db);
    userCtx.executionContext.branchId = 0;
    db->constructBranchLineage(0, userCtx.executionContext);
    while(std::getline(streamUser,userRowStr)) {
        std::vector<std::string> userValues = split(userRowStr,'|');

        if (userIDS.find(userValues[0]) == userIDS.end()) {
            userIDS.insert(userValues[0]);
            std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
            int counter = 0;
            for (auto &columnName : userTable->getColumnNames()) {
                sqlvalues.push_back(Native::Sql::Value::castString(userValues[counter],userTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple userTuple(std::move(sqlvalues));
            insert_tuple(userTuple, *userTable, userCtx);
        }
    }

    streamUser.close();
    streamUser = std::ifstream(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    std::string skippingPageId = "";

    std::string currentPageId = "";
    std::string currentPageTitle = "";

    std::string revisionRowStr;
    std::string pageRowStr;
    std::string contentRowStr;

    QueryContext firstctx(*db);
    firstctx.executionContext.branchId = 0;
    db->constructBranchLineage(0, firstctx.executionContext);
    while(std::getline(streamRevision, revisionRowStr)) {
        std::vector<std::string> revisionValues = split(revisionRowStr,'|');

        if (std::stoi(revisionValues[2]) < lowerBound) {
            if (skippingPageId.compare("") == 0 || skippingPageId.compare(revisionValues[2]) != 0) {
                skippingPageId = revisionValues[2];
                assert(std::getline(streamPage,pageRowStr));
            }
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            continue;
        }
        if (std::stoi(revisionValues[2]) > upperBound) {
            break;
        }

        if (currentPageId.compare("") == 0 || currentPageId.compare(revisionValues[2]) != 0) {
            currentPageId = revisionValues[2];

            assert(std::getline(streamPage,pageRowStr));
            std::vector<std::string> pageValues = split(pageRowStr,'|');
            assert(pageValues.size() == 2);
            assert(pageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamUser,userRowStr));
            std::vector<std::string> userValues = split(userRowStr,'|');
            assert(userValues.size() == 2);
            pageValues.push_back(userValues[0]);
            assert(std::getline(streamContent,contentRowStr));
            std::vector<std::string> contentValues = split(contentRowStr,'|');
            assert(contentValues.size() == 2);
            pageValues.push_back(contentValues[1]);

            std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
            int counter = 0;
            for (auto &columnName : pageTable->getColumnNames()) {
                sqlvalues.push_back(Native::Sql::Value::castString(pageValues[counter],pageTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple pageTuple(std::move(sqlvalues));
            auto loadStart = std::chrono::high_resolution_clock::now();
            insert_tuple(pageTuple, *pageTable, firstctx);
            loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();
        } else {
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
        }
    }

    QueryCompiler::compileAndExecute("CREATE BRANCH branch1 FROM master;",*db);

    streamRevision.close();
    streamPage.close();
    streamContent.close();
    streamUser.close();
    streamRevision = std::ifstream(revisionFileName);
    if (!streamRevision) { throw std::runtime_error("file not found: tables/revision.tbl"); }
    streamPage = std::ifstream(pageFileName);
    if (!streamPage) { throw std::runtime_error("file not found: tables/page.tbl"); }
    streamContent = std::ifstream(contentFileName);
    if (!streamContent) { throw std::runtime_error("file not found: tables/content.tbl"); }
    streamUser = std::ifstream(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    currentPageId = "";
    skippingPageId = "";
    std::vector<std::string> currentPageValues;
    std::vector<std::string> lastContentValues;
    std::vector<std::string> lastUserValues;
    tid_t pageTID = 0;
    bool wasFirstRevision = true;

    QueryContext secondctx(*db);
    secondctx.executionContext.branchId = 0;
    db->constructBranchLineage(0, secondctx.executionContext);
    while(std::getline(streamRevision, revisionRowStr)) {
        std::vector<std::string> revisionValues = split(revisionRowStr,'|');

        if (std::stoi(revisionValues[2]) < lowerBound) {
            if (skippingPageId.compare("") == 0 || skippingPageId.compare(revisionValues[2]) != 0) {
                skippingPageId = revisionValues[2];
                assert(std::getline(streamPage,pageRowStr));
            }
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            continue;
        }
        if (std::stoi(revisionValues[2]) > upperBound) {
            break;
        }

        if (currentPageId.compare("") == 0) {
            currentPageId = revisionValues[2];

            assert(std::getline(streamPage,pageRowStr));
            currentPageValues = split(pageRowStr,'|');
            assert(currentPageValues.size() == 2);
            assert(currentPageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            wasFirstRevision = true;
            continue;
        }

        if (currentPageId.compare(revisionValues[2]) != 0) {
            currentPageId = revisionValues[2];

            assert(std::getline(streamPage,pageRowStr));
            currentPageValues = split(pageRowStr,'|');
            assert(currentPageValues.size() == 2);
            assert(currentPageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));

            pageTID++;
            wasFirstRevision = true;
            continue;
        }

        if (!wasFirstRevision && currentPageId.compare(revisionValues[2]) == 0) {
            std::vector<std::string> lastPageValues = currentPageValues;
            lastPageValues.push_back(lastUserValues[0]);
            lastPageValues.push_back(lastContentValues[1]);

            std::vector<std::unique_ptr<Native::Sql::Value>> sqlValues;
            int counter = 0;
            for (auto &columnName : pageTable->getColumnNames()) {
                sqlValues.push_back(Native::Sql::Value::castString(lastPageValues[counter],pageTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple pageTuple(std::move(sqlValues));
            auto loadStart = std::chrono::high_resolution_clock::now();
            update_tuple(pageTID,pageTuple,*pageTable,secondctx);
            loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();
        }

        assert(std::getline(streamUser,userRowStr));
        lastUserValues = split(userRowStr,'|');
        assert(lastUserValues.size() == 2);
        assert(std::getline(streamContent,contentRowStr));
        lastContentValues = split(contentRowStr,'|');
        assert(lastContentValues.size() == 2);
        wasFirstRevision = false;
    }

    QueryCompiler::compileAndExecute("CREATE BRANCH branch2 FROM master;",*db);

    streamRevision.close();
    streamPage.close();
    streamContent.close();
    streamUser.close();
    streamRevision = std::ifstream(revisionFileName);
    if (!streamRevision) { throw std::runtime_error("file not found: tables/revision.tbl"); }
    streamPage = std::ifstream(pageFileName);
    if (!streamPage) { throw std::runtime_error("file not found: tables/page.tbl"); }
    streamContent = std::ifstream(contentFileName);
    if (!streamContent) { throw std::runtime_error("file not found: tables/content.tbl"); }
    streamUser = std::ifstream(userFileName);
    if (!streamUser) { throw std::runtime_error("file not found: tables/user.tbl"); }

    currentPageId = "";
    skippingPageId = "";
    pageTID = 0;
    bool isFirstRevisionOfPage = true;

    QueryContext thirdctx(*db);
    thirdctx.executionContext.branchId = 0;
    db->constructBranchLineage(0, thirdctx.executionContext);
    while(std::getline(streamRevision, revisionRowStr)) {
        std::vector<std::string> revisionValues = split(revisionRowStr,'|');

        if (std::stoi(revisionValues[2]) < lowerBound) {
            if (skippingPageId.compare("") == 0 || skippingPageId.compare(revisionValues[2]) != 0) {
                skippingPageId = revisionValues[2];
                assert(std::getline(streamPage,pageRowStr));
            }
            assert(std::getline(streamContent,contentRowStr));
            assert(std::getline(streamUser,userRowStr));
            continue;
        }
        if (std::stoi(revisionValues[2]) > upperBound) {
            break;
        }


        if (currentPageId.compare("") == 0) {
            currentPageId = revisionValues[2];
            isFirstRevisionOfPage = true;
            continue;
        }

        if (!isFirstRevisionOfPage && currentPageId.compare(revisionValues[2]) != 0) {
            assert(std::getline(streamPage,pageRowStr));
            std::vector<std::string> pageValues = split(pageRowStr,'|');
            assert(pageValues.size() == 2);
            assert(pageValues[0].compare(currentPageId) == 0);

            assert(std::getline(streamUser,userRowStr));
            std::vector<std::string> userValues = split(userRowStr,'|');
            assert(userValues.size() == 2);
            pageValues.push_back(userValues[0]);
            assert(std::getline(streamContent,contentRowStr));
            std::vector<std::string> contentValues = split(contentRowStr,'|');
            assert(contentValues.size() == 2);
            pageValues.push_back(contentValues[1]);

            std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
            int counter = 0;
            for (auto &columnName : pageTable->getColumnNames()) {
                sqlvalues.push_back(Native::Sql::Value::castString(pageValues[counter],pageTable->getCI(columnName)->type));
                counter++;
            }
            Native::Sql::SqlTuple pageTuple(std::move(sqlvalues));
            auto loadStart = std::chrono::high_resolution_clock::now();
            update_tuple(pageTID,pageTuple,*pageTable,thirdctx);
            loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();

            pageTID++;
            isFirstRevisionOfPage = true;
            currentPageId = revisionValues[2];
            continue;
        }

        if (currentPageId.compare(revisionValues[2]) != 0) {
            assert(std::getline(streamPage,pageRowStr));
            pageTID++;
            isFirstRevisionOfPage = true;
            currentPageId = revisionValues[2];
        } else {
            isFirstRevisionOfPage = false;
        }

        assert(std::getline(streamUser,userRowStr));
        assert(std::getline(streamContent,contentRowStr));
    }

    if (!isFirstRevisionOfPage) {
        assert(std::getline(streamPage,pageRowStr));
        std::vector<std::string> pageValues = split(pageRowStr,'|');
        assert(pageValues.size() == 2);
        assert(pageValues[0].compare(currentPageId) == 0);

        assert(std::getline(streamUser,userRowStr));
        std::vector<std::string> userValues = split(userRowStr,'|');
        assert(userValues.size() == 2);
        pageValues.push_back(userValues[0]);
        assert(std::getline(streamContent,contentRowStr));
        std::vector<std::string> contentValues = split(contentRowStr,'|');
        assert(contentValues.size() == 2);
        pageValues.push_back(contentValues[1]);

        std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
        int counter = 0;
        for (auto &columnName : pageTable->getColumnNames()) {
            sqlvalues.push_back(Native::Sql::Value::castString(pageValues[counter],pageTable->getCI(columnName)->type));
            counter++;
        }
        Native::Sql::SqlTuple pageTuple(std::move(sqlvalues));
        auto loadStart = std::chrono::high_resolution_clock::now();
        update_tuple(pageTID,pageTuple,*pageTable,thirdctx);
        loadDuration += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - loadStart).count();
    }

    streamRevision.close();
    streamPage.close();
    streamContent.close();
    streamUser.close();

    std::cout << "Table Sizes:\n";
    std::cout << "Page:\t" << db->getTable("page")->size() << "\n";
    std::cout << "User:\t" << db->getTable("user")->size() << "\n";
    std::cout << "LoadDuration:\t" << std::fixed << loadDuration / 1000 << std::endl;
}

#else

void storeTextGen(char *dest, const uint8_t * bytes, size_t len) {
    if (len > 15) {
        std::unique_ptr<uint8_t[]> data(new uint8_t[len]);
        std::memcpy(data.get(), bytes, len);
        auto & storedStr = StringPool::instance().put(sql_string_t(len, std::move(data)));
        uint8_t * beginPtr = storedStr.second.get();
        uint8_t * endPtr = beginPtr+len;

        uintptr_t first_value = reinterpret_cast<uintptr_t>(beginPtr);
        first_value ^= static_cast<uintptr_t>(1) << (8*sizeof(uintptr_t)-1);

        memcpy(dest,(char*)&first_value,8);
        memcpy(dest + 8, &endPtr, 8);
    } else {
        dest[0] = (char) len;
        memcpy(&dest[1],bytes,len);
    }
}

void genLoadTextCall(cg_ptr8_t dest, cg_ptr8_t str, cg_size_t strLen)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (const char *, const char*, size_t), false>::get(context);
    llvm::CallInst * result = codeGen.CreateCall(&storeTextGen, funcTy, {dest, str, strLen});
}

void genLoadValue(cg_ptr8_t str, cg_size_t length, Sql::SqlType type, Vector & column)
{
    auto & codeGen = getThreadLocalCodeGen();

    // this is fine for both types of null indicators
    // - there is no space within the Vector in the case of an external null indicator (see NullIndicatorTable)
    // - it wont matter for internal null indicators
    Sql::SqlType notNullableType = toNotNullableTy(type);

    // parse value
    Sql::value_op_t value;
    if (type.typeID == Sql::SqlType::TypeID::TextID) {
        cg_voidptr_t arrayPtr = cg_voidptr_t( llvm::cast<llvm::Value>(codeGen->CreateAlloca(llvm::Type::getInt64Ty(codeGen.getLLVMContext()),2)) );

        genLoadTextCall(arrayPtr,str,length);

        value = Sql::value_op_t( new Sql::Text(type, arrayPtr) );
    } else {
        value = Sql::Value::castString(str, length, notNullableType);
    }

    cg_voidptr_t destPtr = genVectoBackCall(cg_voidptr_t::fromRawPointer(&column));

    // cast destination pointer
    llvm::Type * sqlValuePtrTy = llvm::PointerType::getUnqual(toLLVMTy(notNullableType));
    llvm::Value * sqlValuePtr = codeGen->CreatePointerCast(destPtr.getValue(), sqlValuePtrTy);

    value->store(sqlValuePtr);
}

struct RowItem {
    size_t length;
    const char * str;
};

static llvm::Type * getRowItemTy()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    std::vector<llvm::Type *> members(2);

    // RowItem struct members:
    members[0] = cg_size_t::getType();
    members[1] = cg_ptr8_t::getType();

    llvm::StructType * rowItemTy = llvm::StructType::get(context, false);
    rowItemTy->setBody(members);

    return rowItemTy;
}

static inline llvm::Type * getRowTy(size_t columnCount, llvm::Type * rowItemTy)
{
    llvm::Type * rowTy = llvm::ArrayType::get(rowItemTy, columnCount);
    return rowTy;
}

llvm::Function * genLoadRowFunction(Table *table)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    // prototype: loadRow(RowItem items[])
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    FunctionGen funcGen(moduleGen, "loadRow", funcTy);

    llvm::Type * rowItemTy = getRowItemTy();
    // get types
    llvm::Type * rowTy = getRowTy(table->getColumnCount(), rowItemTy);

    // cast row pointer
    llvm::Value * rawPtr = funcGen.getArg(0);
    llvm::Value * rowPtr = codeGen->CreateBitCast(rawPtr, llvm::PointerType::getUnqual(rowTy));

    // load each value within the row
    size_t i = 0;
    for (const std::string & column : table->getColumnNames()) {
        ci_p_t ci = table->getCI(column);

#ifdef __APPLE__
        llvm::Value * itemPtr = codeGen->CreateGEP(rowTy, rowPtr, { cg_size_t(0ull), cg_size_t(i) });
#else
        llvm::Value * itemPtr = codeGen->CreateGEP(rowTy, rowPtr, { cg_size_t(0ul), cg_size_t(i) });
#endif

        llvm::Value * lengthPtr = codeGen->CreateStructGEP(rowItemTy, itemPtr, 0);
        llvm::Value * strPtr = codeGen->CreateStructGEP(rowItemTy, itemPtr, 1);

        llvm::Value * length = codeGen->CreateLoad(lengthPtr);
        llvm::Value * str = codeGen->CreateLoad(strPtr);

        genLoadValue(cg_ptr8_t(str), cg_size_t(length), ci->type, *ci->column);

        i += 1;
    }

    return funcGen.getFunction();
}

typedef void (* FunPtr)(void *);

void loadTable(std::istream & stream, Table* table, int tableType)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::Function * loadFun = genLoadRowFunction(table);

    // compile
    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();

    // lookup compiled function
    FunPtr f = reinterpret_cast<FunPtr>(ee->getPointerToFunction(loadFun));

    // Branch ID generator
    std::default_random_engine generator;

    size_t tid = 0;

    // load each row
    std::string rowStr;
    std::vector<RowItem> row(table->getColumnCount());
    while (std::getline(stream, rowStr)) {
        table->addRow(0);

        std::vector<std::string> items = split(rowStr, '|');
        assert(row.size() == items.size());

        if (tableType == 1) {
            items[3] = std::to_string(tid);
        }
        if (tableType == 2) {
            items[0] = std::to_string(tid);
        }

        size_t i = 0;
        for (const std::string & itemStr : items) {
            RowItem & item = row[i];
            item.length = itemStr.size();
            item.str = itemStr.c_str();
            i += 1;
        }

        // load row
        f(row.data());

        tid++;
    }
}

void loadWikiDb(Database *db, int lowerBound, int upperBound)
{
    std::stringstream ssRange;
    ssRange << "_";
    ssRange << lowerBound;
    ssRange << "_";
    ssRange << upperBound;
    std::string pageRangeStr = ssRange.str();

    std::string pageFileName = "page" + pageRangeStr + ".tbl";
    std::string revisionFileName = "revision" + pageRangeStr + ".tbl";
    std::string contentFileName = "content" + pageRangeStr + ".tbl";
    std::string userFileName = "user" + pageRangeStr + ".tbl";

    QueryCompiler::compileAndExecute("CREATE TABLE user ( id INTEGER NOT NULL, name TEXT NOT NULL );",*db);
    QueryCompiler::compileAndExecute("CREATE TABLE page ( id INTEGER NOT NULL, title TEXT NOT NULL);",*db);
    QueryCompiler::compileAndExecute("CREATE TABLE revision ( id INTEGER NOT NULL, parentId INTEGER NOT NULL, pageId INTEGER NOT NULL, textId INTEGER NOT NULL, userId INTEGER NOT NULL);",*db);
    QueryCompiler::compileAndExecute("CREATE TABLE content ( id INTEGER NOT NULL, text TEXT NOT NULL);",*db);

    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = db->getTable("user");
        std::ifstream fs(userFileName);
        if (!fs) { throw std::runtime_error("file not found: tables/user.tbl"); }
        loadTable(fs, item,0);
        fs.close();
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = db->getTable("page");
        std::ifstream fs(pageFileName);
        if (!fs) { throw std::runtime_error("file not found: tables/page.tbl"); }
        loadTable(fs, item,0);
        fs.close();
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = db->getTable("content");
        std::ifstream fs(contentFileName);
        if (!fs) { throw std::runtime_error("file not found: tables/content.tbl"); }
        loadTable(fs, item,2);
        fs.close();
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = db->getTable("revision");
        std::ifstream fs(revisionFileName);
        if (!fs) { throw std::runtime_error("file not found: tables/revision.tbl"); }
        loadTable(fs, item,1);
        fs.close();
    }

    std::cout << "Table Sizes:\n";
    std::cout << "User:\t" << db->getTable("user")->size() << "\n";
    std::cout << "Page:\t" << db->getTable("page")->size() << "\n";
    std::cout << "Revision:\t" << db->getTable("revision")->size() << "\n";
    std::cout << "Content:\t" << db->getTable("content")->size() << "\n";
}

#endif

void benchmarkQuery(std::string query, Database &db, unsigned runs) {
    std::vector<QueryCompiler::BenchmarkResult> results;

#ifdef PERF_AVAILABLE
    std::string header;
    std::string data;
    BenchmarkParameters params;

    {
        PerfEventBlock e(runs, params, false);
#endif

        for (int i = 0; i < runs; i++) {
            results.push_back(QueryCompiler::compileAndBenchmark(query, db));
        }

#ifdef PERF_AVAILABLE
    }
#endif

    double parsingTime = 0;
    double analsingTime = 0;
    double translationTime = 0;
    double compileTime = 0;
    double executionTime = 0;
    for (auto &result : results) {
        parsingTime += result.parsingTime;
        analsingTime += result.analysingTime;
        translationTime += result.translationTime;
        compileTime += result.llvmCompilationTime;
        executionTime += result.executionTime;
    }
    double sum = parsingTime + analsingTime + translationTime + compileTime + executionTime;
    std::cout << std::fixed << (parsingTime / runs) << " , " << (analsingTime / runs) << " , " << (translationTime / runs) << " , " << (compileTime / runs) << " , " << (executionTime / runs) << " , " << (sum / runs) << std::endl;
}

void prompt(Database &database, unsigned runs)
{
    while (true) {
        try {
            fflush(stdout);

            std::string input = readline();
            if (input == "quit\n") {
                break;
            }

            benchmarkQuery(input,database,runs);
        } catch (const std::exception & e) {
            fprintf(stderr, "Exception: %s\n", e.what());
        }
    }
}

int main(int argc, char * argv[]) {
    gflags::SetUsageMessage("semanticalBench [-b] [-l <Database Name>] [-d <Master Share>] [-r <Runs per Statement>]");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::unique_ptr<Database> db = std::make_unique<Database>();

    loadWikiDb(db.get(),FLAGS_lowerBound,FLAGS_upperBound);

    prompt(*db,FLAGS_r);

    llvm::llvm_shutdown();
}

/*void genLoadValue(cg_ptr8_t str, cg_size_t length, Sql::SqlType type, Vector & column)
{
    auto & codeGen = getThreadLocalCodeGen();

    // this is fine for both types of null indicators
    // - there is no space within the Vector in the case of an external null indicator (see NullIndicatorTable)
    // - it wont matter for internal null indicators
    Sql::SqlType notNullableType = toNotNullableTy(type);

    // parse value
    Sql::value_op_t value = Sql::Value::castString(str, length, notNullableType);
    cg_voidptr_t destPtr = genVectoBackCall(cg_voidptr_t::fromRawPointer(&column));

    // cast destination pointer
    llvm::Type * sqlValuePtrTy = llvm::PointerType::getUnqual(toLLVMTy(notNullableType));
    llvm::Value * sqlValuePtr = codeGen->CreatePointerCast(destPtr.getValue(), sqlValuePtrTy);

    value->store(sqlValuePtr);
}

struct RowItem {
    size_t length;
    const char * str;
};

static llvm::Type * getRowItemTy()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    std::vector<llvm::Type *> members(2);

    // RowItem struct members:
    members[0] = cg_size_t::getType();
    members[1] = cg_ptr8_t::getType();

    llvm::StructType * rowItemTy = llvm::StructType::get(context, false);
    rowItemTy->setBody(members);

    return rowItemTy;
}

static inline llvm::Type * getRowTy(size_t columnCount, llvm::Type * rowItemTy)
{
    llvm::Type * rowTy = llvm::ArrayType::get(rowItemTy, columnCount);
    return rowTy;
}

llvm::Function * genLoadRowFunction(Table *table)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    std::set<uint32_t> timestamp_columns = {};
    int j = 0;
    for (auto &columnName : table->getColumnNames()) {
        if (table->getCI(columnName)->type.typeID == Sql::SqlType::TypeID::TimestampID) {
            timestamp_columns.insert(j);
        }
        j++;
    }
    const char * sampleTimestamp = "20-07-09 13:56:24.0600";

    // prototype: loadRow(RowItem items[])
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *), false>::get(context);
    FunctionGen funcGen(moduleGen, "loadRow", funcTy);

    // get types
    llvm::Type * rowItemTy = getRowItemTy();
    llvm::Type * rowTy = getRowTy(table->getColumnCount(), rowItemTy);

    // cast row pointer
    llvm::Value * rawPtr = funcGen.getArg(0);
    llvm::Value * rowPtr = codeGen->CreateBitCast(rawPtr, llvm::PointerType::getUnqual(rowTy));

    // load each value within the row
    size_t i = 0;
    for (const std::string & column : table->getColumnNames()) {
        ci_p_t ci = table->getCI(column);
#ifdef __APPLE__
        llvm::Value * itemPtr = codeGen->CreateGEP(rowTy, rowPtr, { cg_size_t(0ull), cg_size_t(i) });
#else
        llvm::Value * itemPtr = codeGen->CreateGEP(rowTy, rowPtr, { cg_size_t(0ul), cg_size_t(i) });
#endif

        llvm::Value * lengthPtr = codeGen->CreateStructGEP(rowItemTy, itemPtr, 0);
        llvm::Value * strPtr = codeGen->CreateStructGEP(rowItemTy, itemPtr, 1);

        llvm::Value * length = codeGen->CreateLoad(lengthPtr);
        llvm::Value * str = codeGen->CreateLoad(strPtr);

        if (timestamp_columns.count(i) > 0) {
            genLoadValue(cg_ptr8_t::fromRawPointer(sampleTimestamp), cg_size_t(22), ci->type, *ci->column);
        } else {
            genLoadValue(cg_ptr8_t(str), cg_size_t(length), ci->type, *ci->column);
        }


        i += 1;
    }

    return funcGen.getFunction();
}

typedef void (* FunPtr)(void *);

void loadTable(std::istream & stream, Table* table, std::discrete_distribution<int> distribution)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::Function * loadFun = genLoadRowFunction(table);

    // compile
    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();

    // lookup compiled function
    FunPtr f = reinterpret_cast<FunPtr>(ee->getPointerToFunction(loadFun));

    // Branch ID generator
    std::default_random_engine generator;

    // load each row
    std::string rowStr;
    std::vector<RowItem> row(table->getColumnCount());
    while (std::getline(stream, rowStr)) {
        int branchId = distribution(generator);

        table->_version_mgmt_column.push_back(std::make_unique<VersionEntry>());
        VersionEntry * version_entry = table->_version_mgmt_column.back().get();
        version_entry->first = version_entry;
        version_entry->next = nullptr;
        version_entry->next_in_branch = nullptr;

        // branch visibility
        version_entry->branch_id = branchId;
        version_entry->branch_visibility.resize(distribution.probabilities().size(), false);
        version_entry->branch_visibility.set(branchId);
        version_entry->creation_ts = distribution.probabilities().size();

        table->addRow(branchId);

        std::vector<std::string> items = split(rowStr, '|');
        assert(row.size() == items.size());

        size_t i = 0;
        for (const std::string & itemStr : items) {
            RowItem & item = row[i];
            item.length = itemStr.size();
            item.str = itemStr.c_str();
            i += 1;
        }

        // load row
        f(row.data());
    }
}

void loadWikiTable(std::istream & stream, bool isDistributing, Table* table, std::discrete_distribution<int> distribution, int groupByColumn)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::Function * loadFun = genLoadRowFunction(table);

    // compile
    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();

    // lookup compiled function
    FunPtr f = reinterpret_cast<FunPtr>(ee->getPointerToFunction(loadFun));

    // Branch ID generator
    std::default_random_engine generator;

    std::vector<RowItem> row(table->getColumnCount());

    // load each row
    std::string rowStr;
    std::string currentGroupItem = "";

    std::vector<std::vector<std::string>> pageRows;
    std::vector<int> branchMappings;
    while (std::getline(stream, rowStr)) {
        std::vector<std::string> items = split(rowStr, '|');

        if (currentGroupItem.compare("") == 0) {
            currentGroupItem = items[groupByColumn];
            if (isDistributing) {
                branchMappings.push_back(distribution(generator));
            } else {
                branchMappings.push_back(0);
            }
            pageRows.push_back(items);
            continue;
        }

        if (currentGroupItem.compare(items[groupByColumn]) != 0) {
            assert(pageRows.size() == branchMappings.size());
            std::sort(branchMappings.begin(),branchMappings.end());
            for (int j=0; j<pageRows.size(); j++) {
                table->_version_mgmt_column.push_back(std::make_unique<VersionEntry>());
                VersionEntry * version_entry = table->_version_mgmt_column.back().get();
                version_entry->first = version_entry;
                version_entry->next = nullptr;
                version_entry->next_in_branch = nullptr;

                // branch visibility
                version_entry->branch_id = branchMappings[j];
                version_entry->branch_visibility.resize(distribution.probabilities().size(), false);
                version_entry->branch_visibility.set(branchMappings[j]);
                version_entry->creation_ts = distribution.probabilities().size();

                table->addRow(branchMappings[j]);


                assert(row.size() == pageRows[j].size());

                size_t i = 0;
                for (const std::string & itemStr : pageRows[j]) {
                    RowItem & item = row[i];
                    item.length = itemStr.size();
                    item.str = itemStr.c_str();
                    i += 1;
                }
                // load row
                f(row.data());
            }

            currentGroupItem = items[groupByColumn];
            branchMappings.clear();
            pageRows.clear();
        }

        if (isDistributing) {
            branchMappings.push_back(distribution(generator));
        } else {
            branchMappings.push_back(0);
        }
        pageRows.push_back(items);
    }

    assert(pageRows.size() == branchMappings.size());
    for (int j=0; j<pageRows.size(); j++) {
        table->_version_mgmt_column.push_back(std::make_unique<VersionEntry>());
        VersionEntry * version_entry = table->_version_mgmt_column.back().get();
        version_entry->first = version_entry;
        version_entry->next = nullptr;
        version_entry->next_in_branch = nullptr;

        // branch visibility
        version_entry->branch_id = branchMappings[j];
        version_entry->branch_visibility.resize(distribution.probabilities().size(), false);
        version_entry->branch_visibility.set(branchMappings[j]);
        version_entry->creation_ts = distribution.probabilities().size();

        table->addRow(branchMappings[j]);

        assert(row.size() == pageRows[j].size());

        size_t i = 0;
        for (const std::string & itemStr : pageRows[j]) {
            RowItem & item = row[i];
            item.length = itemStr.size();
            item.str = itemStr.c_str();
            i += 1;
        }
        // load row
        f(row.data());
    }
}

std::unique_ptr<Database> loadTPCC() {
    auto tpccDb = std::make_unique<Database>();

    {
        ModuleGen moduleGen("LoadTableModule");
        auto & warehouse = tpccDb->createTable("warehouse");
        warehouse.addColumn("w_id", Sql::getIntegerTy(false));
        warehouse.addColumn("w_name", Sql::getVarcharTy(10, false));
        warehouse.addColumn("w_street_1", Sql::getVarcharTy(20, false));
        warehouse.addColumn("w_street_2", Sql::getVarcharTy(20, false));
        warehouse.addColumn("w_city", Sql::getVarcharTy(20, false));
        warehouse.addColumn("w_state", Sql::getCharTy(2, false));
        warehouse.addColumn("w_zip", Sql::getCharTy(9, false));
        warehouse.addColumn("w_tax", Sql::getNumericTy(4, 4, false));
        warehouse.addColumn("w_ytd", Sql::getNumericTy(12, 2, false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & district = tpccDb->createTable("district");
        district.addColumn("d_id", Sql::getIntegerTy(false));
        district.addColumn("d_w_id", Sql::getIntegerTy(false));
        district.addColumn("d_name", Sql::getVarcharTy(10, false));
        district.addColumn("d_street_1", Sql::getVarcharTy(20, false));
        district.addColumn("d_street_2", Sql::getVarcharTy(20, false));
        district.addColumn("d_city", Sql::getVarcharTy(20, false));
        district.addColumn("d_state", Sql::getCharTy(2, false));
        district.addColumn("d_zip", Sql::getCharTy(9, false));
        district.addColumn("d_tax", Sql::getNumericTy(4, 4, false));
        district.addColumn("d_ytd", Sql::getNumericTy(12, 2, false));
        district.addColumn("d_next_o_id", Sql::getIntegerTy(false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & customer = tpccDb->createTable("customer");
        customer.addColumn("c_id", Sql::getIntegerTy(false));
        customer.addColumn("c_d_id", Sql::getIntegerTy(false));
        customer.addColumn("c_w_id", Sql::getIntegerTy(false));
        customer.addColumn("c_:first", Sql::getVarcharTy(16, false));
        customer.addColumn("c_middle", Sql::getCharTy(2, false));
        customer.addColumn("c_last", Sql::getVarcharTy(16, false));
        customer.addColumn("c_street_1", Sql::getVarcharTy(20, false));
        customer.addColumn("c_street_2", Sql::getVarcharTy(20, false));
        customer.addColumn("c_city", Sql::getVarcharTy(20, false));
        customer.addColumn("c_state", Sql::getCharTy(2, false));
        customer.addColumn("c_zip", Sql::getCharTy(9, false));
        customer.addColumn("c_phone", Sql::getCharTy(16, false));
        customer.addColumn("c_since", Sql::getTimestampTy(false));
        customer.addColumn("c_credit", Sql::getCharTy(2, false));
        customer.addColumn("c_credit_lim", Sql::getNumericTy(12, 2, false));
        customer.addColumn("c_discount", Sql::getNumericTy(4, 4, false));
        customer.addColumn("c_balance", Sql::getNumericTy(12, 2, false));
        customer.addColumn("c_ytd_paymenr", Sql::getNumericTy(12, 2, false));
        customer.addColumn("c_payment_cnt", Sql::getNumericTy(4, 0, false));
        customer.addColumn("c_delivery_cnt", Sql::getNumericTy(4, 0, false));
        customer.addColumn("c_data", Sql::getVarcharTy(500, false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & history = tpccDb->createTable("history");
        history.addColumn("h_c_id", Sql::getIntegerTy(false));
        history.addColumn("h_c_d_id", Sql::getIntegerTy(false));
        history.addColumn("h_c_w_id", Sql::getIntegerTy(false));
        history.addColumn("h_d_id", Sql::getIntegerTy(false));
        history.addColumn("h_w_id", Sql::getIntegerTy(false));
        history.addColumn("h_date", Sql::getTimestampTy(false));
        history.addColumn("h_amount", Sql::getNumericTy(6, 2, false));
        history.addColumn("h_data", Sql::getVarcharTy(24, false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & neworder = tpccDb->createTable("neworder");
        neworder.addColumn("no_o_id", Sql::getIntegerTy(false));
        neworder.addColumn("no_d_id", Sql::getIntegerTy(false));
        neworder.addColumn("no_w_id", Sql::getIntegerTy(false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & order = tpccDb->createTable("order");
        order.addColumn("o_id", Sql::getIntegerTy(false));
        order.addColumn("o_d_id", Sql::getIntegerTy(false));
        order.addColumn("o_w_id", Sql::getIntegerTy(false));
        order.addColumn("o_c_id", Sql::getIntegerTy(false));
        order.addColumn("o_entry_d", Sql::getTimestampTy(false));
        order.addColumn("o_carrier_id", Sql::getIntegerTy(false));
        order.addColumn("o_ol_cnt", Sql::getNumericTy(2, 0, false));
        order.addColumn("o_all_local", Sql::getNumericTy(1, 0, false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & orderline = tpccDb->createTable("orderline");
        orderline.addColumn("ol_o_id", Sql::getIntegerTy(false));
        orderline.addColumn("ol_d_id", Sql::getIntegerTy(false));
        orderline.addColumn("ol_w_id", Sql::getIntegerTy(false));
        orderline.addColumn("ol_number", Sql::getIntegerTy(false));
        orderline.addColumn("ol_i_id", Sql::getIntegerTy(false));
        orderline.addColumn("ol_supply_w_id", Sql::getIntegerTy(false));
        orderline.addColumn("ol_delivery_d", Sql::getTimestampTy(false));
        orderline.addColumn("ol_quantity", Sql::getNumericTy(2, 0, false));
        orderline.addColumn("ol_amount", Sql::getNumericTy(6, 2, false));
        orderline.addColumn("ol_dist_info", Sql::getCharTy(24, false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & item = tpccDb->createTable("item");
        item.addColumn("i_id", Sql::getIntegerTy(false));
        item.addColumn("i_im_id", Sql::getIntegerTy(false));
        item.addColumn("i_name", Sql::getVarcharTy(24, false));
        item.addColumn("i_price", Sql::getNumericTy(5, 2, false));
        item.addColumn("i_data", Sql::getVarcharTy(50, false));
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        auto & stock = tpccDb->createTable("stock");
        stock.addColumn("s_i_id", Sql::getIntegerTy(false));
        stock.addColumn("s_w_id", Sql::getIntegerTy(false));
        stock.addColumn("s_quantity", Sql::getNumericTy(4, 0, false));
        stock.addColumn("s_dist_01", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_02", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_03", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_04", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_05", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_06", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_07", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_08", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_09", Sql::getCharTy(24, false));
        stock.addColumn("s_dist_10", Sql::getCharTy(24, false));
        stock.addColumn("s_ytd", Sql::getNumericTy(8, 0, false));
        stock.addColumn("s_order_cnt", Sql::getNumericTy(4, 0, false));
        stock.addColumn("s_remote_cnt", Sql::getNumericTy(4, 0, false));
        stock.addColumn("s_data", Sql::getVarcharTy(50, false));
    }

    std::discrete_distribution<int> distribution = { 1 , 1 };
    branch_id_t highestBranchID = 0;
    for (int i=1; i<distribution.probabilities().size(); i++) {
        std::string branchName = "branch";
        branchName += std::to_string(i);
        highestBranchID = tpccDb->createBranch(branchName,highestBranchID);
    }

    {
        ModuleGen moduleGen("LoadTableModule");
        Table *warehouse = tpccDb->getTable("warehouse");
        std::ifstream fs("../tables/tpc-c/tpcc_warehouse.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/warehouse.tbl"); }
        loadTable(fs, warehouse, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *district = tpccDb->getTable("district");
        std::ifstream fs("../tables/tpc-c/tpcc_district.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/district.tbl"); }
        loadTable(fs, district, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *customer = tpccDb->getTable("customer");
        std::ifstream fs("../tables/tpc-c/tpcc_customer.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/customer.tbl"); }
        loadTable(fs, customer, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *history = tpccDb->getTable("history");
        std::ifstream fs("../tables/tpc-c/tpcc_history.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/history.tbl"); }
        loadTable(fs, history, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *neworder = tpccDb->getTable("neworder");
        std::ifstream fs("../tables/tpc-c/tpcc_neworder.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/neworder.tbl"); }
        loadTable(fs, neworder, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *order = tpccDb->getTable("order");
        std::ifstream fs("../tables/tpc-c/tpcc_order.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/order.tbl"); }
        loadTable(fs, order, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *orderline = tpccDb->getTable("orderline");
        std::ifstream fs("../tables/tpc-c/tpcc_orderline.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/orderline.tbl"); }
        loadTable(fs, orderline, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = tpccDb->getTable("item");
        std::ifstream fs("../tables/tpc-c/tpcc_item.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/item.tbl"); }
        loadTable(fs, item, distribution);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *stock = tpccDb->getTable("stock");
        std::ifstream fs("../tables/tpc-c/tpcc_stock.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/stock.tbl"); }
        loadTable(fs, stock, distribution);
    }

    std::cout << "Table Sizes:\n";
    std::cout << "Warehouse:\t" << tpccDb->getTable("warehouse")->size() << "\n";
    std::cout << "District:\t" << tpccDb->getTable("district")->size() << "\n";
    std::cout << "Customer:\t" << tpccDb->getTable("customer")->size() << "\n";
    std::cout << "History:\t" << tpccDb->getTable("history")->size() << "\n";
    std::cout << "NewOrder:\t" << tpccDb->getTable("neworder")->size() << "\n";
    std::cout << "Order:\t\t" << tpccDb->getTable("order")->size() << "\n";
    std::cout << "Orderline:\t" << tpccDb->getTable("orderline")->size() << "\n";
    std::cout << "Item:\t\t" << tpccDb->getTable("item")->size() << "\n";
    std::cout << "Stock:\t\t" << tpccDb->getTable("stock")->size() << "\n";

    return tpccDb;
}

#if LIBXML_AVAILABLE

class DistributionEngine {
public:
    std::discrete_distribution<int> distribution = { 1 , 1 };
    std::default_random_engine generator;

    DistributionEngine(std::discrete_distribution<int> distribution) : distribution(distribution) {}
    int generate() {
        distribution(generator);
    }
};

std::unique_ptr<Database> generateWikiTBL() {
    auto tpccDb = std::make_unique<Database>();

    QueryCompiler::compileAndExecute("CREATE TABLE page ( id INTEGER NOT NULL, title VARCHAR ( 30 ) NOT NULL);",*tpccDb);
    QueryCompiler::compileAndExecute("CREATE TABLE revision ( id INTEGER NOT NULL, parentId INTEGER NOT NULL, pageId INTEGER NOT NULL, textId INTEGER NOT NULL);",*tpccDb);
    QueryCompiler::compileAndExecute("CREATE TABLE content ( id INTEGER NOT NULL, text VARCHAR ( 32 ) NOT NULL);",*tpccDb);
    QueryCompiler::compileAndExecute("CREATE BRANCH branch1 FROM master;",*tpccDb);

    DistributionEngine distributionEngine = DistributionEngine({ 2 , 1 });

    std::function<void(wikiparser::Page,std::vector<wikiparser::Revision>,std::vector<wikiparser::Content>,std::vector<wikiparser::User>)> insertCallback =
            [db = tpccDb.get(),distributionEnginePtr = &distributionEngine](wikiparser::Page page, std::vector<wikiparser::Revision> revisions, std::vector<wikiparser::Content> contents, std::vector<wikiparser::User> users) {
        if (revisions.size() != contents.size()) return;

        std::vector<int> branchMappings(0);
        for (int i=0; i<revisions.size();i++) {
            branchMappings.push_back(distributionEnginePtr->generate());
        }
        std::sort(branchMappings.begin(),branchMappings.end());

        std::string statement = "";

        std::stringstream ss;
        ss << "INSERT INTO page ( id , title ) VALUES ( ";
        ss << page.id;
        ss << " , '";
        std::replace( page.title.begin(), page.title.end(), ' ', '~' );
        ss << page.title;
        ss << "' );";
        statement = ss.str();
        QueryCompiler::compileAndExecute(statement,*db);

        for (int i=0; i<revisions.size(); i++) {
            std::string branchID = "";
            if (branchMappings[i] <= 0) {
                branchID = "master";
            } else {
                std::stringstream ssBranch;
                ssBranch << "branch";
                ssBranch << branchMappings[i];
                branchID = ssBranch.str();
            }
            std::stringstream ssContent;
            ssContent << "INSERT INTO content VERSION ";
            ssContent << branchID;
            ssContent << " ( id , text ) VALUES ( ";
            ssContent << contents[i].textid;
            ssContent << " , '";
            std::replace( contents[i].text.begin(), contents[i].text.end(), ' ', '~' );
            ssContent << contents[i].text;
            ssContent << "' );";
            statement = ssContent.str();
            QueryCompiler::compileAndExecute(statement,*db);

            std::stringstream ssRevision;
            ssRevision << "INSERT INTO revision VERSION ";
            ssRevision << branchID;
            ssRevision << " ( id , parentId , pageId , textId ) VALUES ( ";
            ssRevision << revisions[i].id;
            ssRevision << " , ";
            ssRevision << revisions[i].parent;
            ssRevision << " , ";
            ssRevision << page.id;
            ssRevision << " , ";
            ssRevision << contents[i].textid;
            ssRevision << " );";
            statement = ssRevision.str();
            QueryCompiler::compileAndExecute(statement,*db);
        }

    };

    try
    {
        wikiparser::WikiParser parser(insertCallback);
        parser.set_substitute_entities(true);
        parser.parse_file("samplePage.xml");
    }
    catch(const xmlpp::exception& ex)
    {
        std::cerr << "libxml++ exception: " << ex.what() << std::endl;
    }

    std::cout << "Table Sizes:\n";
    std::cout << "Page:\t" << tpccDb->getTable("page")->size() << "\n";
    std::cout << "Revision:\t" << tpccDb->getTable("revision")->size() << "\n";
    std::cout << "Content:\t" << tpccDb->getTable("content")->size() << "\n";

    return tpccDb;
}
#endif

std::unique_ptr<Database> loadWiki(std::discrete_distribution<int> &distribution) {
    auto wikidb = std::make_unique<Database>();

    QueryCompiler::compileAndExecute("CREATE TABLE page ( id INTEGER NOT NULL, title VARCHAR ( 30 ) NOT NULL);",*wikidb);
    QueryCompiler::compileAndExecute("CREATE TABLE revision ( id INTEGER NOT NULL, parentId INTEGER NOT NULL, pageId INTEGER NOT NULL, textId INTEGER NOT NULL);",*wikidb);
    QueryCompiler::compileAndExecute("CREATE TABLE content ( id INTEGER NOT NULL, text VARCHAR ( 32 ) NOT NULL);",*wikidb);

    branch_id_t highestBranchID = 0;
    for (int i=1; i<distribution.probabilities().size(); i++) {
        std::string branchName = "branch";
        branchName += std::to_string(i);
        highestBranchID = wikidb->createBranch(branchName,highestBranchID);
    }

    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = wikidb->getTable("page");
        std::ifstream fs("page_trunc.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/page.tbl"); }
        loadWikiTable(fs, false, item, distribution,0);
    }

    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = wikidb->getTable("content");
        std::ifstream fs("content_trunc.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/content.tbl"); }
        loadWikiTable(fs, true, item, distribution,0);
    }
    {
        ModuleGen moduleGen("LoadTableModule");
        Table *item = wikidb->getTable("revision");
        std::ifstream fs("revision_trunc.tbl");
        if (!fs) { throw std::runtime_error("file not found: tables/revision.tbl"); }
        loadWikiTable(fs, true, item, distribution,2);
    }

    std::cout << "Table Sizes:\n";
    std::cout << "Page:\t" << wikidb->getTable("page")->size() << "\n";
    std::cout << "Revision:\t" << wikidb->getTable("revision")->size() << "\n";
    std::cout << "Content:\t" << wikidb->getTable("content")->size() << "\n";

    return wikidb;
}*/