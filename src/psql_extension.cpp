#define DUCKDB_EXTENSION_MAIN

#include "psql_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/extension_statement.hpp"

#include "re2/re2.h"

#include <sstream>

namespace duckdb {

static void LoadInternal(DatabaseInstance &instance) {
  auto &config = DBConfig::GetConfig(instance);
  PsqlParserExtension psql_parser;
  config.parser_extensions.push_back(psql_parser);
  config.operator_extensions.push_back(make_unique<PsqlOperatorExtension>());
}

void PsqlExtension::Load(DuckDB &db) { LoadInternal(*db.instance); }

ParserExtensionParseResult psql_parse(ParserExtensionInfo *,
                                      const std::string &query) {
  // Rewrite A | B | C to WITH $e1 AS (A), $e2 AS (FROM $e1 B) FROM $e2 C

  size_t count = 0;
  size_t pos = 0;
  std::string command;
  std::string prev_name = "";
  std::stringstream ss;
  duckdb_re2::StringPiece input(query);
  RE2::Options options;
  options.set_dot_nl(true);
  RE2 re("(.*?)\\s+[|]\\s+", options);
  while (RE2::Consume(&input, re, &command)) {
    //printf("Command: %s\n", command.c_str());
    if (count > 0) {
      ss << ",\n";
    } else {
      ss << "WITH ";
    }
    std::string name;
    bool as_expr = RE2::FullMatch(command, "as\\s+(\\w+)", &name);
    if (!as_expr) {
      name = "_tmp" + std::to_string(count);
    }
    ss << name << " AS (";
    if (count > 0) {
      ss << "FROM " << prev_name << " ";
    }
    if (!as_expr) {
      ss << command;
    }
    ss << ")";
    prev_name = name;
    ++count;
  }
  command = input.ToString();
  if (count > 0) {
    ss << "\nFROM " << prev_name << " " << command;
  } else {
    ss << query;
  }
  std::string result = ss.str();
  
  //printf("Result: %s\n", result.c_str());

  Parser parser; // TODO Pass (ClientContext.GetParserOptions());
  parser.ParseQuery(move(result));
  vector<unique_ptr<SQLStatement>> statements = std::move(parser.statements);

  return ParserExtensionParseResult(
      make_unique_base<ParserExtensionParseData, PsqlParseData>(
          move(statements[0])));
}

ParserExtensionPlanResult
psql_plan(ParserExtensionInfo *, ClientContext &context,
          unique_ptr<ParserExtensionParseData> parse_data) {
  // We stash away the ParserExtensionParseData before throwing an exception
  // here. This allows the planning to be picked up by psql_bind instead, but
  // we're not losing important context.
  auto psql_state = make_shared<PsqlState>(move(parse_data));
  context.registered_state["psql"] = psql_state;
  throw BinderException("Use psql_bind instead");
}

BoundStatement psql_bind(ClientContext &context, Binder &binder,
                         OperatorExtensionInfo *info, SQLStatement &statement) {
  switch (statement.type) {
  case StatementType::EXTENSION_STATEMENT: {
    auto &extension_statement = dynamic_cast<ExtensionStatement &>(statement);
    if (extension_statement.extension.parse_function == psql_parse) {
      auto lookup = context.registered_state.find("psql");
      if (lookup != context.registered_state.end()) {
        auto psql_state = (PsqlState *)lookup->second.get();
        auto psql_binder = Binder::CreateBinder(context);
        auto psql_parse_data =
            dynamic_cast<PsqlParseData *>(psql_state->parse_data.get());
        return psql_binder->Bind(*(psql_parse_data->statement));
      }
      throw BinderException("Registered state not found");
    }
  }
  default:
    // No-op empty
    return {};
  }
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void psql_init(duckdb::DatabaseInstance &db) {
  LoadInternal(db);
}

DUCKDB_EXTENSION_API const char *psql_version() {
  return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif