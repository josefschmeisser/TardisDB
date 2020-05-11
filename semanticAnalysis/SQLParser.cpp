#include "SQLParser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <memory>
#include <optional>
#include <cassert>
#include <unordered_map>

using BindingAttribute = SQLParserResult::BindingAttribute;
using Constant = SQLParserResult::Constant;
using AttributeName = SQLParserResult::AttributeName;
using Relation = SQLParserResult::Relation;

namespace keywords {
    const std::string Select = "select";
    const std::string From = "from";
    const std::string Where = "where";
    const std::string And = "and";

    std::set<std::string> keywordset = {Select, From, Where, And};
}

enum TokenType : unsigned int {
    identifier, keyword, separator, op, literal, comment, delimiter
};

struct Token {
    TokenType type;
    std::string value;
    Token(TokenType type, std::string value) : type(type), value(value)
    { }
};

typedef enum State : unsigned int {
    Init, Select, ProjectionStar, ProjectionAttrName, ProjectionAttrSeparator,
    From, FromRelationName, FromBindingName, FromSeparator,
    Where, WhereExprLhs, WhereExprOp, WhereExprRhs, WhereAnd,
    Semicolon, Done
} state_t;

static bool is_identifier(const Token & tok) {
    return tok.type == TokenType::identifier;
}

static bool is_keyword(const Token & tok, std::string keyword) {
    std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::tolower);
    return (tok.type == TokenType::keyword && tok.value == keyword);
}

static bool is_identifier(const std::string& str) {
    if (str==keywords::Select ||
        str==keywords::From ||
        str==keywords::Where ||
        str==keywords::And ||
        std::isdigit(str[0]) ||
        str[0] == '.' ||
        str[0] == '_'
    ) {
        return false;
    }
    return str.find_first_not_of("abcdefghijklmnopqrstuvwxyz_.1234567890") == std::string::npos;
}

static bool is_escaped(const std::string & str) {
    if (str.length() > 2 && str.front() == '"') {
        return (str.back() == '"');
    } else if (str.length() > 2 && str.front() == '\'') {
        return (str.back() == '\'');
    }
    return false;
}

static std::string unescape(const std::string & str) {
    return str.substr(1, str.size() - 2);
}

BindingAttribute parse_binding_attribute(std::string value) {
//    std::cout << "binding attr: " << value << std::endl;
    auto dotPos = value.find_first_of(".");
    if (dotPos == std::string::npos) {
//        throw incorrect_sql_error("invalid binding attribute");
        return BindingAttribute({}, value);
    }

    auto binding = value.substr(0, dotPos);
    auto attribute = value.substr(dotPos+1, value.size());
    return BindingAttribute(binding, attribute);
}

struct Tokenizer {
    std::istream & _in;
    std::vector<Token> _tokens;

    Tokenizer(std::istream & in) : _in(in)
    { }

    const Token & prev(size_t i = 1) {
        assert(_tokens.size() >= i);
        return _tokens[_tokens.size()-1-i];
    }

    const Token & next() {
        std::stringstream ss;

        for (size_t i = 0;; ++i) {
            int c = _in.get();
            if (c == EOF) {
                if (i == 0 && _tokens.back().type != TokenType::delimiter) {
                    ss << ";";
                }
                break;
            } else if (std::isspace(c)) {
                if (i > 0) {
                    break;
                } else {
                    continue;
                }
            } else if (c == ',' || c == ';' || c == '=') {
                if (i > 0) {
                    _in.putback(c);
                } else {
                    ss << static_cast<char>(c);
                }
                break;
            }
            ss << static_cast<char>(c);
        }

        std::string token_value = ss.str();
        if (token_value == ",") {
            _tokens.emplace_back(TokenType::separator, token_value);
        } else if (token_value == ";") {
            _tokens.emplace_back(TokenType::delimiter, token_value);
        } else if (token_value == "=") {
            _tokens.emplace_back(TokenType::op, token_value);
        } else if (keywords::keywordset.count(token_value)) {
            _tokens.emplace_back(TokenType::keyword, token_value);
        } else if (is_identifier(token_value)) {
            _tokens.emplace_back(TokenType::identifier, token_value);
        } else if (is_escaped(token_value)) {
            _tokens.emplace_back(TokenType::literal, unescape(token_value));
        } else {
            _tokens.emplace_back(TokenType::literal, token_value);
        }

        return _tokens.back();
    }
};

static state_t parse_next_token(Tokenizer & token_src, const state_t state, SQLParserResult & query) {
    auto & token = token_src.next();
//    std::cout << "current token: " << token.value << std::endl;
    if (token.type == TokenType::delimiter) {
        /*if (state != State::FromBindingName &&
            state != State::WhereExprRhs &&
            state != State::Semicolon) {
            throw incorrect_sql_error("unexpected end of input");
        }*/
        return State::Done;
    }

    state_t new_state;
    std::string token_value = token.value;
    std::string lowercase_token_value;
    std::transform(token_value.begin(), token_value.end(), std::back_inserter(lowercase_token_value), tolower);
    switch(state) {
    case State::Init:
        if (lowercase_token_value == keywords::Select) {
            new_state = State::Select;
        } else {
            throw incorrect_sql_error("Expected 'Select', found '" + token_value + "'");
        }
        break;
    case State::Select: /* fallthrough */
    case State::ProjectionAttrSeparator:
        if (token_value == "*") {
            new_state = State::ProjectionStar;
        } else if (is_identifier(token)) {
            query.projections.push_back(token_value);
            new_state = ProjectionAttrName;
        } else {
            throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
        }
        break;
    case ProjectionStar: /* fallthrough */
        if (lowercase_token_value == keywords::From) {
            new_state = State::From;
        } else {
            // NOTE: as defined in the given gramma
            throw incorrect_sql_error("Expected 'From' after '*', found '" + token_value + "'");
        }
        break;
    case State::ProjectionAttrName:
        if (lowercase_token_value == keywords::From) {
            new_state = State::From;
        } else if (token_value == ",") {
            new_state = State::ProjectionAttrSeparator;
        } else {
            throw incorrect_sql_error("Expected ',' or 'From' after attribute name, found '" + token_value + "'");
        }
        break;
    case From: /* fallthrough */
    case FromSeparator:
        if (is_identifier(token)) {
            // token contains a relation name
            using rel_t = decltype(query.relations)::value_type;
            query.relations.push_back(rel_t(token_value, { }));
            new_state = FromRelationName;
        } else {
            throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
        }
        break;
    case State::FromRelationName:
        if (is_identifier(token)) {
            // token contains the binding name
            using rel_t = decltype(query.relations)::value_type;
            auto current = query.relations.back();
            query.relations.pop_back();
            query.relations.push_back(rel_t(current.first, token_value));
            new_state = State::FromBindingName;
        } else {
            throw incorrect_sql_error("Expected binding name after relation name, found '" + token_value + "'");
        }
        break;
    case State::FromBindingName:
        if (token_value == keywords::Where) {
            new_state = State::Where;
        } else if (token_value == ",") {
            new_state = State::FromSeparator;
        } else if (token_value == ";") {
            new_state = State::Semicolon;
        } else {
            throw incorrect_sql_error("Expected ',' or 'Where' after relation reference, found '" + token_value + "'");
        }
        break;
    case State::Where:
    case State::WhereAnd: /* fallthrough */
        // e.g. s.matrnr will be handled as a single identifier
        if (is_identifier(token)) {
            // attribute binding
            new_state = State::WhereExprLhs;
        } else {
            throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
        }
        break;
    case State::WhereExprLhs:
        if (token.type == TokenType::op && token_value == "=") {
            new_state = State::WhereExprOp;
        } else {
            throw incorrect_sql_error("Expected '=', found '" + token_value + "'");
        }
        break;
    case State::WhereExprOp:
        // e.g. s.matrnr will be handled as a single identifier
        if (is_identifier(token)) {
            // attribute binding
            BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
            BindingAttribute rhs = parse_binding_attribute(token_value);
            query.joinConditions.push_back(std::make_pair(lhs, rhs));
            new_state = State::WhereExprRhs;
        } else if (token.type == TokenType::literal) {
            // constant
            BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
            Constant rhs = unescape(token_value);
            query.selections.push_back(std::make_pair(lhs, token_value));
            new_state = State::WhereExprRhs;
        } else {
            throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
        }
        break;
    case State::WhereExprRhs:
        if (is_keyword(token, "and")) {
            new_state = State::WhereAnd;
        } else if (token_value == ";") {
            new_state = State::Semicolon;
        }
        break;
    case State::Semicolon:
        new_state = State::Done;
        break;
    default:
        throw incorrect_sql_error("unexpected token: " + token_value);
    }

    return new_state;
}

// identifier -> (binding, Attribute)
using scope_t = std::unordered_map<std::string, std::pair<std::string,ci_p_t>>;

static bool in_scope(const scope_t & scope, const BindingAttribute & binding_attr) {
    std::string identifier = binding_attr.first + "." + binding_attr.second;
    return scope.count(identifier) > 0;
}

static scope_t construct_scope(Database& db, const SQLParserResult & result) {
    scope_t scope;
    for (auto & rel_pair : result.relations) {
        if (!db.hasTable(rel_pair.first)) {
            throw incorrect_sql_error("unknown relation '" + rel_pair.first + "'");
        }
        auto * table = db.getTable(rel_pair.first);
        size_t attr_cnt = table->getColumnCount();
        for (size_t i = 0; i < attr_cnt; ++i) {
            auto * attr = table->getCI(table->getColumnNames()[i]);
            scope[rel_pair.second + "." + attr->columnName] = std::make_pair(rel_pair.second, attr);
            if (scope.count(attr->columnName) > 0) {
                scope[attr->columnName].second = nullptr;
            } else {
                scope[attr->columnName] = std::make_pair(rel_pair.second, attr);
            }
        }
    }
    return scope;
}

static BindingAttribute fully_qualify(const BindingAttribute & current, const scope_t & scope) {
    if (!current.first.empty()) {
        return current;
    }
    auto it = scope.find(current.second);
    if (it == scope.end()) {
        throw incorrect_sql_error("unknown column '" + current.second + "'");
    }
    return BindingAttribute(it->second.first, current.second);
}

static void fully_qualify_names(const scope_t & scope, SQLParserResult & result) {
    for (size_t i = 0; i < result.selections.size(); ++i) {
        auto & [binding_attr, _] = result.selections[i];
        result.selections[i].first = fully_qualify(binding_attr, scope);
    }

    for (size_t i = 0; i < result.joinConditions.size(); ++i) {
        auto & [lhs, rhs] = result.joinConditions[i];
        result.joinConditions[i].first = fully_qualify(lhs, scope);
        result.joinConditions[i].second = fully_qualify(rhs, scope);
    }

    // TODO same for result.projections
}

static void validate_sql_statement(const scope_t & scope, Database& db, const SQLParserResult & result) {
    for (auto & attr_name : result.projections) {
        auto it = scope.find(attr_name);
        if (it == scope.end()) {
            throw incorrect_sql_error("unknown column '" + attr_name + "'");
        } else if (it->second.second == nullptr) {
            throw incorrect_sql_error("'" + attr_name + "' is ambiguous");
        }
    }

    for (auto & selection_pair : result.selections) {
        auto & binding_attr = selection_pair.first;
        if (!in_scope(scope, binding_attr)) {
            throw incorrect_sql_error("unknown column '" + binding_attr.first + "." + binding_attr.second + "'");
        }
    }

    for (auto & join_pair : result.joinConditions) {
        auto & lhs = join_pair.first;
        if (!in_scope(scope, lhs)) {
            throw incorrect_sql_error("unknown column '" + lhs.first + "." + lhs.second + "'");
        }
        auto & rhs = join_pair.second;
        if (!in_scope(scope, rhs)) {
            throw incorrect_sql_error("unknown column '" + rhs.first + "." + rhs.second + "'");
        }
    }
}

SQLParserResult parse_and_analyse_sql_statement(Database& db, std::string sql) {
    SQLParserResult result;

    std::stringstream in(sql);
    Tokenizer tokenizer(in);

    state_t state = State::Init;
    while (state != State::Done) {
        state = parse_next_token(tokenizer, state, result);
    }

    auto scope = construct_scope(db, result);
    fully_qualify_names(scope, result);
    validate_sql_statement(scope, db, result);
    return result;
}
