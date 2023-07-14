#define DUCKDB_EXTENSION_MAIN

#include "psql_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/extension_statement.hpp"

#include "pg_functions.hpp"

#include "re2/re2.h"

#include <sstream>

namespace duckdb {

static void LoadInternal(DatabaseInstance &instance) {
  auto &config = DBConfig::GetConfig(instance);
  PsqlParserExtension psql_parser;
  config.parser_extensions.push_back(psql_parser);
  config.operator_extensions.push_back(make_uniq<PsqlOperatorExtension>());
}

void PsqlExtension::Load(DuckDB &db) { LoadInternal(*db.instance); }

// Rewrite A | B | C to WITH $e1 AS (A), $e2 AS (FROM $e1 B) FROM $e2 C
void transform_block(const std::string &block, std::stringstream &ss) {
  std::string command;
  duckdb_re2::StringPiece input(block);
  size_t count = 0;
  RE2::Options options;
  options.set_dot_nl(true);
  RE2 re("(.*?)\\s+[|]\\s+", options);
  while (RE2::Consume(&input, re, &command)) {
    // printf("Command: %s\n", command.c_str());
    if (count > 0) {
      ss << ",\n";
    } else {
      ss << "WITH ";
    }
    ss << "_tmp" << count << " AS (";
    if (count > 0) {
      ss << "FROM "
         << "_tmp" << (count - 1) << " ";
    }
    ss << command << ")";
    ++count;
  }
  command = input.ToString();
  if (count > 0) {
    ss << "\nFROM "
       << "_tmp" << (count - 1) << " " << command;
  } else {
    ss << command;
  }
}

ParserExtensionParseResult psql_parse(ParserExtensionInfo *,
                                      const std::string &query) {
  std::stringstream ss;

  // Identify blocks, delimited by "(|" and "|)"
  RE2::Options options;
  options.set_dot_nl(true);
  RE2 block_re("(.*?)[(][|](.*?)[|][)]", options);
  duckdb_re2::StringPiece input(query);
  std::string pre_block_command;
  std::string block_command;

  while (RE2::Consume(&input, block_re, &pre_block_command, &block_command)) {
    transform_block(pre_block_command, ss);
    ss << "(";
    transform_block(block_command, ss);
    ss << ")";
  }
  std::string post_block_command;
  post_block_command = input.ToString();
  transform_block(post_block_command, ss);
  std::string result = ss.str();

  // printf("Result: %s\n", result.c_str());

  vector<unique_ptr<SQLStatement>> statements;
  try {
    Parser parser; // TODO Pass (ClientContext.GetParserOptions());
    parser.ParseQuery(result);
    statements = std::move(parser.statements);
  } catch (...) {
    duckdb_libpgquery::pg_parser_init();
    throw;
  }

  duckdb_libpgquery::pg_parser_init();

  return ParserExtensionParseResult(
      make_uniq_base<ParserExtensionParseData, PsqlParseData>(
          std::move(statements[0])));
}

ParserExtensionPlanResult
psql_plan(ParserExtensionInfo *, ClientContext &context,
          unique_ptr<ParserExtensionParseData> parse_data) {
  // We stash away the ParserExtensionParseData before throwing an exception
  // here. This allows the planning to be picked up by psql_bind instead, but
  // we're not losing important context.
  auto psql_state = make_shared<PsqlState>(std::move(parse_data));
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
