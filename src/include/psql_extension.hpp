#pragma once

#include "duckdb.hpp"

namespace duckdb {

class PsqlExtension : public Extension {
public:
  void Load(ExtensionLoader &loader) override;
  std::string Name() override { return "psql"; }
};

BoundStatement psql_bind(ClientContext &context, Binder &binder,
                         OperatorExtensionInfo *info, SQLStatement &statement);

struct PsqlOperatorExtension : public OperatorExtension {
  PsqlOperatorExtension() : OperatorExtension() { Bind = psql_bind; }

  std::string GetName() override { return "psql"; }

  unique_ptr<LogicalExtensionOperator>
  Deserialize(Deserializer &deserializer) override {
    throw InternalException("psql operator should not be serialized");
  }
};

ParserExtensionParseResult psql_parse(ParserExtensionInfo *,
                                      const std::string &query);

ParserExtensionPlanResult psql_plan(ParserExtensionInfo *, ClientContext &,
                                    unique_ptr<ParserExtensionParseData>);

struct PsqlParserExtension : public ParserExtension {
  PsqlParserExtension() : ParserExtension() {
    parse_function = psql_parse;
    plan_function = psql_plan;
  }
};

struct PsqlParseData : ParserExtensionParseData {
  unique_ptr<SQLStatement> statement;

  unique_ptr<ParserExtensionParseData> Copy() const override {
    return make_uniq_base<ParserExtensionParseData, PsqlParseData>(
        statement->Copy());
  }

  virtual string ToString() const override { return "PsqlParseData"; }

  PsqlParseData(unique_ptr<SQLStatement> statement)
      : statement(std::move(statement)) {}
};

class PsqlState : public ClientContextState {
public:
  explicit PsqlState(unique_ptr<ParserExtensionParseData> parse_data)
      : parse_data(std::move(parse_data)) {}

  void QueryEnd() override { parse_data.reset(); }

  unique_ptr<ParserExtensionParseData> parse_data;
};

} // namespace duckdb
