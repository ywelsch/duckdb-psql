#define DUCKDB_EXTENSION_MAIN

#include "psql_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

// PSQL turns `A |> B |> C` into `FROM (FROM (A) B) C`: every stage is the tail
// of a FROM-first SELECT whose first table is the result of the previous stage.
// Rather than rewriting the query text, we extend DuckDB's PEG grammar so that
// any SELECT (top-level, subquery, CTE, view, INSERT source, ...) can be
// followed by pipe stages:
//
//   SelectStatement         <- SelectStatementInternal
//   SelectStatementInternal <- PsqlDelimitedPipeline / PsqlPipeline
//   PsqlDelimitedPipeline   <- '|' PsqlPipeline '|'
//   PsqlPipeline            <- PsqlPipelineSource PsqlStage*
//   PsqlPipelineSource      <- WithClause? SelectSetOpChain ResultModifiers?
//   PsqlStage               <- '|>' PsqlStageOperation
//   PsqlStageOperation      <- PsqlCopyTo / PsqlStageQuery
//   PsqlCopyTo              <- 'COPY'? 'TO' CopyFileName CopyOptions?
//   PsqlStageQuery          <- PsqlStageSetOpChain ResultModifiers?
//   PsqlStageSetOpChain     <- PsqlStageIntersectChain SelectSetOpChainTail*
//   PsqlStageIntersectChain <- PsqlStageSelect IntersectChainTail*
//   PsqlStageSelect         <- PsqlStageFromSelect WhereClause?
//                              GroupByClause? HavingClause? WindowClause?
//                              QualifyClause? SampleClause?
//   PsqlStageFromSelect     <- PsqlStageFrom SelectClause?
//   PsqlStageFrom           <- TableAlias? JoinOrPivot* (',' TableRef)*
//
// PsqlPipelineSource is the original definition of SelectStatementInternal, and
// the PsqlStage* rules mirror the shape of the core rules SelectSetOpChain,
// IntersectChain, SimpleSelect and FromSelectClause, so they reuse the core
// transforms to build their results. Only PsqlStageFrom differs from the core
// FROM clause: its first table is the stage input, a placeholder that
// PsqlPipeline later fills with the result of the previous stage. The
// PsqlDelimitedPipeline rule keeps the `(| ... |)` syntax of earlier PSQL
// versions working.
//
// A top-level pipeline can end with `|> [COPY] TO 'file' (options)`, which
// turns the SELECT statement into `COPY (pipeline) TO 'file' (options)`.

static vector<reference<ParseResult>> GetRepeatChildren(ListParseResult &list,
                                                        idx_t child_idx) {
  auto &optional = list.Child<OptionalParseResult>(child_idx);
  if (!optional.HasResult()) {
    return {};
  }
  return optional.GetResult().Cast<RepeatParseResult>().GetChildren();
}

static bool IsRule(const ParseResult &parse_result, const char *rule_name) {
  return StringUtil::CIEquals(parse_result.name, rule_name);
}

template <class T> static T TakeResult(TransformResultValue &value) {
  auto result = TryGetTransformResult<T>(value);
  if (!result) {
    throw InternalException("PSQL received an unexpected transform result");
  }
  return std::move(*result);
}

//===--------------------------------------------------------------------===//
// Stage input
//===--------------------------------------------------------------------===//
// The stage input is a subquery whose statement has no query node yet
static unique_ptr<TableRef> CreateStageInput() {
  return make_uniq<SubqueryRef>(make_uniq<SelectStatement>());
}

static optional_ptr<SubqueryRef> FindStageInput(TableRef &ref) {
  switch (ref.type) {
  case TableReferenceType::SUBQUERY: {
    auto &subquery_ref = ref.Cast<SubqueryRef>();
    if (subquery_ref.subquery && !subquery_ref.subquery->node) {
      return subquery_ref;
    }
    return nullptr;
  }
  case TableReferenceType::JOIN:
    return FindStageInput(*ref.Cast<JoinRef>().left);
  case TableReferenceType::PIVOT:
    return FindStageInput(*ref.Cast<PivotRef>().source);
  default:
    return nullptr;
  }
}

// The stage input is the leftmost table of the first SELECT of the stage
static optional_ptr<SubqueryRef> FindStageInput(QueryNode &node) {
  switch (node.type) {
  case QueryNodeType::SELECT_NODE:
    return FindStageInput(*node.Cast<SelectNode>().from_table);
  case QueryNodeType::SET_OPERATION_NODE:
    return FindStageInput(*node.Cast<SetOperationNode>().children[0]);
  default:
    return nullptr;
  }
}

//===--------------------------------------------------------------------===//
// Transforms
//===--------------------------------------------------------------------===//
//! Transforms a single child of the matched rule and returns its result
//! unchanged
class PsqlForwardProcess final : public TransformProcess {
public:
  explicit PsqlForwardProcess(ParseResult &child_p) : child(child_p) {}

  TransformStep Resume(unique_ptr<TransformResultValue> child_result) override {
    if (child_result) {
      return TransformStep::Complete(std::move(child_result));
    }
    return TransformStep::Child(child);
  }

private:
  ParseResult &child;
};

static unique_ptr<TransformProcess>
StartChoiceTransform(PEGTransformer &, ParseResult &parse_result) {
  auto &list = parse_result.Cast<ListParseResult>();
  return make_uniq<PsqlForwardProcess>(
      list.Child<ChoiceParseResult>(0).GetResult());
}

static grammar_transform_process_function_t TransformChild(idx_t child_idx) {
  return [child_idx](PEGTransformer &, ParseResult &parse_result) {
    auto &list = parse_result.Cast<ListParseResult>();
    return make_uniq<PsqlForwardProcess>(list.GetChild(child_idx));
  };
}

static grammar_transform_process_function_t
TransformWith(const TransformFrameOps &ops) {
  return [&ops](PEGTransformer &transformer, ParseResult &parse_result) {
    return make_uniq<GeneratedTransformProcess>(transformer, parse_result, ops);
  };
}

//! Transforms a rule with the transform of a core rule of the same shape
static grammar_transform_process_function_t
TransformLike(const char *rule_name) {
  return [rule_name](PEGTransformer &transformer, ParseResult &parse_result) {
    return transformer.GetRule(rule_name).StartTransform(transformer,
                                                         parse_result);
  };
}

// SelectStatementInternal itself is replaced, so its core transform is not
// available by rule name
static const TransformFrameOps PSQL_PIPELINE_SOURCE_OPS = {
    "PsqlPipelineSource",
    &PEGTransformerFactory::InitializeSelectStatementInternalTrampoline,
    &PEGTransformerFactory::FinalizeSelectStatementInternalTrampoline};

// The core initializers of the following rules transform their children with
// the core child rules, e.g. IntersectChain transforms its first child as a
// SelectAtom. Our rules have the same shape, but different child rules, so they
// only reuse the core finalizers, which build the result from the transformed
// children.

// Rule <- Head Tail*
static void InitializeHeadTail(PEGTransformer &,
                               GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  auto tails = GetRepeatChildren(list, 1);
  process.ReserveChildSlots(1 + tails.size());
  // pending children are transformed in reverse order of being pushed
  for (idx_t i = tails.size(); i > 0; i--) {
    process.PushChild({tails[i - 1].get()}, i);
  }
  process.PushChild({list.GetChild(0)}, 0);
}

static const TransformFrameOps PSQL_STAGE_SET_OP_CHAIN_OPS = {
    "PsqlStageSetOpChain", InitializeHeadTail,
    &PEGTransformerFactory::FinalizeSelectSetOpChainTrampoline};

static const TransformFrameOps PSQL_STAGE_INTERSECT_CHAIN_OPS = {
    "PsqlStageIntersectChain", InitializeHeadTail,
    &PEGTransformerFactory::FinalizeIntersectChainTrampoline};

// PsqlStageFromSelect <- PsqlStageFrom SelectClause?
static void InitializeStageFromSelect(PEGTransformer &,
                                      GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  process.ReserveChildSlots(2);
  auto &select_clause = list.Child<OptionalParseResult>(1);
  if (select_clause.HasResult()) {
    process.PushChild({select_clause.GetResult()}, 1);
  }
  process.PushChild({list.GetChild(0)}, 0);
}

static const TransformFrameOps PSQL_STAGE_FROM_SELECT_OPS = {
    "PsqlStageFromSelect", InitializeStageFromSelect,
    &PEGTransformerFactory::FinalizeFromSelectClauseTrampoline};

//! Transforms the pipeline source and stages in order, plugging each result
//! into the input of the next stage. At the top level, a final `|> TO` stage
//! turns the pipeline into a COPY statement.
class PsqlPipelineProcess final : public TransformProcess {
public:
  PsqlPipelineProcess(PEGTransformer &transformer_p, ParseResult &parse_result,
                      bool top_level_p)
      : transformer(transformer_p), top_level(top_level_p) {
    auto &list = parse_result.Cast<ListParseResult>();
    children.push_back(list.GetChild(0));
    auto stages = GetRepeatChildren(list, 1);
    for (idx_t i = 0; i < stages.size(); i++) {
      // PsqlStage <- '|>' PsqlStageOperation
      auto &stage = stages[i].get().Cast<ListParseResult>();
      auto &pipe_location = stage.GetChild(0);
      auto &operation = stage.Child<ListParseResult>(1)
                            .Child<ChoiceParseResult>(0)
                            .GetResult();
      if (IsRule(operation, "PsqlCopyTo")) {
        // writing to a file is a statement, not a query
        if (!top_level || i + 1 < stages.size()) {
          throw ParserException(pipe_location.GetLocation(),
                                "|> TO is only supported as the last stage of "
                                "a top-level query");
        }
        copy_to = operation.Cast<ListParseResult>();
        continue;
      }
      // every part of a stage is optional, but a stage that matched nothing is
      // most likely a mistake
      if (operation.GetLocation().length == 0) {
        throw ParserException(
            pipe_location.GetLocation(),
            "syntax error at or near \"|>\": empty pipe stage");
      }
      children.push_back(operation);
    }
    query_count = children.size();
    if (copy_to) {
      // PsqlCopyTo <- 'COPY'? 'TO' CopyFileName CopyOptions?
      children.push_back(copy_to->GetChild(2));
      auto &copy_options = copy_to->Child<OptionalParseResult>(3);
      if (copy_options.HasResult()) {
        children.push_back(copy_options.GetResult());
      }
    }
  }

  TransformStep Resume(unique_ptr<TransformResultValue> child_result) override {
    if (child_result) {
      auto child_idx = next_child - 1;
      if (child_idx < query_count) {
        AddQuery(TakeResult<unique_ptr<SelectStatement>>(*child_result));
      } else if (child_idx == query_count) {
        file_name = TakeResult<unique_ptr<ParsedExpression>>(*child_result);
      } else {
        copy_options = TakeResult<vector<GenericCopyOption>>(*child_result);
      }
    }
    if (next_child < children.size()) {
      return TransformStep::Child(children[next_child++].get());
    }
    if (!cte_map.map.empty()) {
      result->node->cte_map = std::move(cte_map);
    }
    if (!top_level) {
      return TransformStep::Complete(
          make_uniq<TypedTransformResult<unique_ptr<SelectStatement>>>(
              std::move(result)));
    }
    unique_ptr<SQLStatement> statement;
    if (copy_to) {
      statement = PEGTransformerFactory::TransformCopySelect(
          transformer, std::move(result), std::move(file_name), copy_options);
    } else {
      statement = std::move(result);
    }
    return TransformStep::Complete(
        make_uniq<TypedTransformResult<unique_ptr<SQLStatement>>>(
            std::move(statement)));
  }

private:
  void AddQuery(unique_ptr<SelectStatement> statement) {
    if (!result) {
      result = std::move(statement);
      if (query_count > 1) {
        // a WITH clause in front of the pipeline is visible in all of its
        // stages
        cte_map = std::move(result->node->cte_map);
        result->node->cte_map = CommonTableExpressionMap();
      }
      return;
    }
    auto input = FindStageInput(*statement->node);
    if (!input) {
      throw InternalException("PSQL stage has no input");
    }
    input->subquery = std::move(result);
    result = std::move(statement);
  }

  PEGTransformer &transformer;
  //! Whether the pipeline is a top-level SELECT statement
  bool top_level;
  //! The pipeline source and query stages, followed by the file name and
  //! options of the `|> TO` stage
  vector<reference<ParseResult>> children;
  idx_t query_count = 0;
  idx_t next_child = 0;
  optional_ptr<ListParseResult> copy_to;
  unique_ptr<SelectStatement> result;
  CommonTableExpressionMap cte_map;
  unique_ptr<ParsedExpression> file_name;
  optional<vector<GenericCopyOption>> copy_options;
};

static unique_ptr<TransformProcess>
StartPipelineTransform(PEGTransformer &transformer, ParseResult &parse_result) {
  return make_uniq<PsqlPipelineProcess>(transformer, parse_result, false);
}

// SelectStatement <- SelectStatementInternal
static unique_ptr<TransformProcess>
StartStatementTransform(PEGTransformer &transformer,
                        ParseResult &parse_result) {
  auto &select = parse_result.Cast<ListParseResult>().GetChild(0);
  auto pipeline =
      &select.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
  if (IsRule(*pipeline, "PsqlDelimitedPipeline")) {
    pipeline = &pipeline->Cast<ListParseResult>().GetChild(1);
  }
  return make_uniq<PsqlPipelineProcess>(transformer, *pipeline, true);
}

// PsqlStageQuery <- PsqlStageSetOpChain ResultModifiers?
static void InitializeStageQuery(PEGTransformer &,
                                 GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  process.ReserveChildSlots(2);
  auto &result_modifiers = list.Child<OptionalParseResult>(1);
  if (result_modifiers.HasResult()) {
    process.PushChild({result_modifiers.GetResult()}, 1);
  }
  process.PushChild({list.GetChild(0)}, 0);
}

static unique_ptr<TransformResultValue>
FinalizeStageQuery(PEGTransformer &, GeneratedTransformProcess &process) {
  auto statement = process.TakeResult<unique_ptr<SelectStatement>>(0);
  if (process.child_results[1]) {
    auto result_modifiers =
        process.TakeResult<vector<unique_ptr<ResultModifier>>>(1);
    for (auto &result_modifier : result_modifiers) {
      statement->node->modifiers.push_back(std::move(result_modifier));
    }
  }
  return make_uniq<TypedTransformResult<unique_ptr<SelectStatement>>>(
      std::move(statement));
}

static const TransformFrameOps PSQL_STAGE_QUERY_OPS = {
    "PsqlStageQuery", InitializeStageQuery, FinalizeStageQuery};

// PsqlStageFrom <- TableAlias? JoinOrPivot* (',' TableRef)*
// Child slots: [0] alias, [1, 1 + #joins) joins, [1 + #joins, 1 + #joins +
// #tables) tables
static void InitializeStageFrom(PEGTransformer &,
                                GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  auto joins = GetRepeatChildren(list, 1);
  auto tables = GetRepeatChildren(list, 2);
  process.ReserveChildSlots(1 + joins.size() + tables.size());
  // pending children are transformed in reverse order of being pushed
  for (idx_t i = tables.size(); i > 0; i--) {
    auto &table_ref = tables[i - 1].get().Cast<ListParseResult>().GetChild(1);
    process.PushChild({table_ref}, 1 + joins.size() + i - 1);
  }
  for (idx_t i = joins.size(); i > 0; i--) {
    process.PushChild({joins[i - 1].get()}, 1 + i - 1);
  }
  auto &table_alias = list.Child<OptionalParseResult>(0);
  if (table_alias.HasResult()) {
    process.PushChild({table_alias.GetResult()}, 0);
  }
}

static unique_ptr<TransformResultValue>
FinalizeStageFrom(PEGTransformer &, GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  auto join_count = GetRepeatChildren(list, 1).size();
  auto table_count = GetRepeatChildren(list, 2).size();

  auto result = CreateStageInput();
  if (process.child_results[0]) {
    auto table_alias = process.TakeResult<TableAlias>(0);
    result->alias = std::move(table_alias.name);
    result->column_name_alias = std::move(table_alias.column_name_alias);
  }
  for (idx_t i = 0; i < join_count; i++) {
    auto join_or_pivot = process.TakeResult<unique_ptr<TableRef>>(1 + i);
    if (join_or_pivot->type == TableReferenceType::JOIN) {
      join_or_pivot->Cast<JoinRef>().left = std::move(result);
    } else if (join_or_pivot->type == TableReferenceType::PIVOT) {
      join_or_pivot->Cast<PivotRef>().source = std::move(result);
    } else {
      throw NotImplementedException("Unsupported TableRef type encountered: %s",
                                    EnumUtil::ToString(join_or_pivot->type));
    }
    result = std::move(join_or_pivot);
  }
  for (idx_t i = 0; i < table_count; i++) {
    auto cross_product = make_uniq<JoinRef>();
    cross_product->left = std::move(result);
    cross_product->right =
        process.TakeResult<unique_ptr<TableRef>>(1 + join_count + i);
    cross_product->ref_type = JoinRefType::CROSS;
    cross_product->is_implicit = true;
    result = std::move(cross_product);
  }
  return make_uniq<TypedTransformResult<unique_ptr<TableRef>>>(
      std::move(result));
}

static const TransformFrameOps PSQL_STAGE_FROM_OPS = {
    "PsqlStageFrom", InitializeStageFrom, FinalizeStageFrom};

//===--------------------------------------------------------------------===//
// Grammar extension
//===--------------------------------------------------------------------===//
class PsqlGrammarExtension final : public GrammarExtension {
public:
  PsqlGrammarExtension()
      : GrammarExtension("psql",
                         "Piped SQL: `A |> B` evaluates `FROM (A) B`, see "
                         "https://github.com/ywelsch/duckdb-psql") {}

  vector<GrammarChange> GetChanges() const override {
    vector<GrammarChange> changes;
    changes.push_back(GrammarChange::ReplaceRule(
        "SelectStatement <- SelectStatementInternal", StartStatementTransform));
    changes.push_back(GrammarChange::ReplaceRule(
        "SelectStatementInternal <- PsqlDelimitedPipeline / PsqlPipeline",
        StartChoiceTransform));
    changes.push_back(GrammarChange::AddRule(
        "PsqlDelimitedPipeline <- '|' PsqlPipeline '|'", TransformChild(1)));
    changes.push_back(
        GrammarChange::AddRule("PsqlPipeline <- PsqlPipelineSource PsqlStage*",
                               StartPipelineTransform));
    changes.push_back(GrammarChange::AddRule(
        "PsqlPipelineSource <- WithClause? SelectSetOpChain ResultModifiers?",
        TransformWith(PSQL_PIPELINE_SOURCE_OPS)));
    // PsqlPipelineProcess transforms the parts of these rules
    changes.push_back(
        GrammarChange::AddRule("PsqlStage <- '|>' PsqlStageOperation"));
    changes.push_back(GrammarChange::AddRule(
        "PsqlStageOperation <- PsqlCopyTo / PsqlStageQuery"));
    changes.push_back(GrammarChange::AddRule(
        "PsqlCopyTo <- 'COPY'? 'TO' CopyFileName CopyOptions?"));
    changes.push_back(GrammarChange::AddRule(
        "PsqlStageQuery <- PsqlStageSetOpChain ResultModifiers?",
        TransformWith(PSQL_STAGE_QUERY_OPS)));
    changes.push_back(GrammarChange::AddRule(
        "PsqlStageSetOpChain <- PsqlStageIntersectChain SelectSetOpChainTail*",
        TransformWith(PSQL_STAGE_SET_OP_CHAIN_OPS)));
    changes.push_back(GrammarChange::AddRule(
        "PsqlStageIntersectChain <- PsqlStageSelect IntersectChainTail*",
        TransformWith(PSQL_STAGE_INTERSECT_CHAIN_OPS)));
    changes.push_back(
        GrammarChange::AddRule("PsqlStageSelect <- PsqlStageFromSelect "
                               "WhereClause? GroupByClause? HavingClause? "
                               "WindowClause? QualifyClause? SampleClause?",
                               TransformLike("SimpleSelect")));
    changes.push_back(GrammarChange::AddRule(
        "PsqlStageFromSelect <- PsqlStageFrom SelectClause?",
        TransformWith(PSQL_STAGE_FROM_SELECT_OPS)));
    changes.push_back(GrammarChange::AddRule(
        "PsqlStageFrom <- TableAlias? JoinOrPivot* (',' TableRef)*",
        TransformWith(PSQL_STAGE_FROM_OPS)));
    return changes;
  }
};

//===--------------------------------------------------------------------===//
// Enabling psql
//===--------------------------------------------------------------------===//
// Grammar extensions are activated per connection with the
// active_grammar_extensions setting. Loading psql activates it on all
// connections, existing and future, and CALL psql_enable() activates it again
// on the calling connection, e.g. after RESET active_grammar_extensions.

//! Adds psql to the active grammar extensions of a connection, keeping any
//! other active grammar extensions. Must run on the thread that uses the
//! connection, since the setting is not synchronized.
static void EnablePsql(ClientContext &context) {
  auto active = ActiveGrammarExtensionsSetting::GetSetting(context);
  vector<Value> extensions;
  for (auto &extension : ListValue::GetChildren(active)) {
    if (StringUtil::CIEquals(StringValue::Get(extension), "psql")) {
      return;
    }
    extensions.push_back(extension);
  }
  extensions.emplace_back("psql");
  ActiveGrammarExtensionsSetting::SetLocal(
      context, Value::LIST(LogicalType::VARCHAR, std::move(extensions)));
}

static unique_ptr<FunctionData>
PsqlEnableBind(ClientContext &, TableFunctionBindInput &,
               vector<LogicalType> &return_types, vector<Identifier> &names) {
  return_types.emplace_back(LogicalType::BOOLEAN);
  names.emplace_back("Success");
  return nullptr;
}

static void PsqlEnableFunction(ClientContext &context, TableFunctionInput &,
                               DataChunk &) {
  EnablePsql(context);
}

//! Connections opened after loading psql enable it while being opened
class PsqlConnectionCallback final : public ExtensionCallback {
public:
  void OnConnectionOpened(ClientContext &context) override {
    EnablePsql(context);
  }
};

static constexpr const char *PSQL_ENABLE_STATE = "psql_enable";

//! Connections that are open while loading psql may be running a query on
//! another thread, so they enable psql at the end of their current or next
//! query. For the connection that runs LOAD, that is the LOAD itself.
class PsqlEnableAtQueryEnd final : public ClientContextState {
public:
  using ClientContextState::QueryEnd;

  void QueryEnd(ClientContext &context) override {
    context.registered_state->Remove(PSQL_ENABLE_STATE);
    EnablePsql(context);
  }
};

static void LoadInternal(ExtensionLoader &loader) {
  auto &db = loader.GetDatabaseInstance();
  GrammarExtension::Register(db, make_shared_ptr<PsqlGrammarExtension>());
  loader.RegisterFunction(
      TableFunction("psql_enable", {}, PsqlEnableFunction, PsqlEnableBind));
  // register the callback before listing the connections, so that a
  // connection opened in between is not missed
  ExtensionCallback::Register(DBConfig::GetConfig(db),
                              make_shared_ptr<PsqlConnectionCallback>());
  for (auto &context : ConnectionManager::Get(db).GetConnectionList()) {
    context->registered_state->GetOrCreate<PsqlEnableAtQueryEnd>(
        PSQL_ENABLE_STATE);
  }
}

void PsqlExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(psql, loader) { duckdb::LoadInternal(loader); }
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
