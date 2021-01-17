#ifndef PROTODB_IUFACTORY_HPP
#define PROTODB_IUFACTORY_HPP

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include "foundations/Database.hpp"
#include "foundations/InformationUnit.hpp"
#include "sql/SqlType.hpp"

class IUFactory {
public:
    IUFactory() = default;

    uint32_t getUID() { return operatorUID++; }

    /// \brief Create an iu for a temporary
    iu_p_t createIU(Sql::SqlType type);

    /// \brief Create an iu for a given column
    iu_p_t createIU(const uint32_t operatorUID, ci_p_t columnInformation);

private:
    using iu_op_t = std::unique_ptr<InformationUnit>;

    uint32_t operatorUID = 0;
    std::vector<iu_op_t> iu_vec;
};

#endif // PROTODB_IUFACTORY_HPP