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

static void LoadInternal(ExtensionLoader &loader) {
  auto &instance = loader.GetDatabaseInstance();
  auto &config = DBConfig::GetConfig(instance);
  PsqlParserExtension psql_parser;
  config.parser_extensions.push_back(psql_parser);
  config.operator_extensions.push_back(make_uniq<PsqlOperatorExtension>());
}

void PsqlExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }

// Rewrite A | B | C to FROM ( FROM ( A ) B ) C
bool transform_block(const std::string &block, std::stringstream &ss) {
  std::string command;
  duckdb_re2::StringPiece input(block);
  size_t count = 0;
  RE2::Options options;
  options.set_dot_nl(true);
  RE2 re("(.*?)\\s+[|][>]\\s+", options);
  std::stringstream intermediates;
  while (RE2::Consume(&input, re, &command)) {
    // printf("Command: %s\n", command.c_str());
    intermediates << command << " )";
    ++count;
  }
  for (size_t i = 0; i < count; ++i) {
    ss << "FROM "
       << "( ";
  }
  ss << intermediates.str();
  command = input.ToString();
  ss << command;
  return count > 0;
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
  bool psql_found = false;

  while (RE2::Consume(&input, block_re, &pre_block_command, &block_command)) {
    psql_found = true;
    transform_block(pre_block_command, ss);
    ss << "(";
    transform_block(block_command, ss);
    ss << ")";
  }
  std::string post_block_command;
  post_block_command = input.ToString();
  psql_found |= transform_block(post_block_command, ss);
  std::string result = ss.str();

  if (!psql_found) {
    // throw original exception message
    return ParserExtensionParseResult();
  }

  // printf("Result: %s\n", result.c_str());

  Parser parser; // TODO Pass (ClientContext.GetParserOptions());
  parser.ParseQuery(result);
  auto statements = std::move(parser.statements);

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
  auto psql_state = make_shared_ptr<PsqlState>(std::move(parse_data));
  context.registered_state->Remove("psql");
  context.registered_state->Insert("psql", psql_state);
  throw BinderException("Use psql_bind instead");
}

BoundStatement psql_bind(ClientContext &context, Binder &binder,
                         OperatorExtensionInfo *info, SQLStatement &statement) {
  switch (statement.type) {
  case StatementType::EXTENSION_STATEMENT: {
    auto &extension_statement = dynamic_cast<ExtensionStatement &>(statement);
    if (extension_statement.extension.parse_function == psql_parse) {
      auto lookup = context.registered_state->Get<PsqlState>("psql");
      if (lookup) {
        auto psql_state = (PsqlState *)lookup.get();
        auto psql_binder = Binder::CreateBinder(context, &binder);
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

DUCKDB_CPP_EXTENSION_ENTRY(psql, loader) { LoadInternal(loader); }
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
