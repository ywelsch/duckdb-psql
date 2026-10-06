#define DUCKDB_EXTENSION_MAIN

#include "psql_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/peg/matcher/operator_matcher.hpp"
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
//   PsqlStageOperation      <- PsqlCopyTo / PsqlExtend / PsqlSet / PsqlDrop /
//                              PsqlRename / PsqlDistinct / PsqlStageQuery
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
// The other stage operations are pipe operators of BigQuery's pipe syntax
// (https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/pipe-syntax)
// that map directly to DuckDB's SELECT, such as EXTEND to SELECT *, ....
// A top-level pipeline can also end with `|> [COPY] TO 'file' (options)`,
// which turns the SELECT statement into `COPY (pipeline) TO 'file' (options)`.

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
static vector<reference<ParseResult>> GetRepeatChildren(ListParseResult &list,
                                                        idx_t child_idx) {
  auto &optional = list.Child<OptionalParseResult>(child_idx);
  if (!optional.HasResult()) {
    return {};
  }
  return optional.GetResult().Cast<RepeatParseResult>().GetChildren();
}

//! The elements of a List(D) in the grammar
static vector<reference<ParseResult>> GetListElements(ParseResult &list) {
  return PEGTransformerFactory::ExtractParseResultsFromList(list);
}

//! The alternative that matched for a rule of the form Rule <- A / B
static ParseResult &GetChoice(ParseResult &parse_result) {
  return parse_result.Cast<ListParseResult>()
      .Child<ChoiceParseResult>(0)
      .GetResult();
}

static optional_ptr<ParseResult> GetOptional(ListParseResult &list,
                                             idx_t child_idx) {
  auto &optional = list.Child<OptionalParseResult>(child_idx);
  if (!optional.HasResult()) {
    return nullptr;
  }
  return optional.GetResult();
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

static unique_ptr<TransformResultValue>
SelectResult(unique_ptr<SelectStatement> statement) {
  return make_uniq<TypedTransformResult<unique_ptr<SelectStatement>>>(
      std::move(statement));
}

static unique_ptr<TransformResultValue>
SelectResult(unique_ptr<QueryNode> node) {
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(node);
  return SelectResult(std::move(statement));
}

//===--------------------------------------------------------------------===//
// Stage input
//===--------------------------------------------------------------------===//
// The stage input is a subquery whose statement has no query node yet
static unique_ptr<TableRef> CreateStageInput() {
  return make_uniq<SubqueryRef>(make_uniq<SelectStatement>());
}

static bool IsStageInput(const SubqueryRef &ref) {
  return ref.subquery && !ref.subquery->node;
}

static optional_ptr<SubqueryRef> FindStageInput(TableRef &ref) {
  switch (ref.type) {
  case TableReferenceType::SUBQUERY: {
    auto &subquery_ref = ref.Cast<SubqueryRef>();
    return IsStageInput(subquery_ref) ? &subquery_ref : nullptr;
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

static bool IsPlainStar(const ParsedExpression &expression) {
  if (expression.GetExpressionClass() != ExpressionClass::STAR ||
      !expression.GetAlias().empty()) {
    return false;
  }
  auto &star = expression.Cast<StarExpression>();
  return star.RelationName().empty() && star.ExcludeList().empty() &&
         star.ReplaceList().empty() && star.RenameList().empty() &&
         !star.IsColumns() && !star.Expression();
}

//! A stage like `|> AS t` that only gives the stage input an alias
static optional_ptr<SubqueryRef> GetAliasOnlyStage(SelectStatement &stage) {
  if (stage.node->type != QueryNodeType::SELECT_NODE) {
    return nullptr;
  }
  auto &node = stage.node->Cast<SelectNode>();
  if (node.select_list.size() != 1 || !IsPlainStar(*node.select_list[0]) ||
      node.where_clause || node.having || node.qualify || node.sample ||
      !node.groups.group_expressions.empty() || !node.modifiers.empty() ||
      node.aggregate_handling != AggregateHandling::STANDARD_HANDLING ||
      node.from_table->type != TableReferenceType::SUBQUERY) {
    return nullptr;
  }
  auto &input = node.from_table->Cast<SubqueryRef>();
  if (!IsStageInput(input) || input.alias.empty()) {
    return nullptr;
  }
  return input;
}

//! SELECT * FROM <stage input>
static unique_ptr<SelectNode> CreateStageSelect() {
  auto node = make_uniq<SelectNode>();
  node->select_list.push_back(make_uniq<StarExpression>());
  node->from_table = CreateStageInput();
  return node;
}

//===--------------------------------------------------------------------===//
// Generic transforms
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
  return make_uniq<PsqlForwardProcess>(GetChoice(parse_result));
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

//! Pushes children so that they are transformed in order, as pending children
//! are transformed in reverse order of being pushed. Slot i receives the
//! result of children[i], absent children leave their slot empty.
static void PushChildren(GeneratedTransformProcess &process,
                         vector<optional_ptr<ParseResult>> children) {
  process.ReserveChildSlots(children.size());
  for (idx_t i = children.size(); i > 0; i--) {
    if (children[i - 1]) {
      process.PushChild({*children[i - 1]}, i - 1);
    }
  }
}

//===--------------------------------------------------------------------===//
// Stage queries
//===--------------------------------------------------------------------===//
// SelectStatementInternal itself is replaced, so its core transform is not
// available by rule name
static const TransformFrameOps PSQL_PIPELINE_SOURCE_OPS = {
    "PsqlPipelineSource",
    &PEGTransformerFactory::InitializeSelectStatementInternalTrampoline,
    &PEGTransformerFactory::FinalizeSelectStatementInternalTrampoline};

// PsqlStageQuery <- PsqlStageSetOpChain ResultModifiers?
static void InitializeStageQuery(PEGTransformer &,
                                 GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  PushChildren(process, {list.GetChild(0), GetOptional(list, 1)});
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
  return SelectResult(std::move(statement));
}

static const TransformFrameOps PSQL_STAGE_QUERY_OPS = {
    "PsqlStageQuery", InitializeStageQuery, FinalizeStageQuery};

// The core initializers of SelectSetOpChain and IntersectChain transform their
// first child as IntersectChain and SelectAtom, so only their finalizers are
// reused
// Rule <- Head Tail*
static void InitializeHeadTail(PEGTransformer &,
                               GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  vector<optional_ptr<ParseResult>> children{list.GetChild(0)};
  for (auto &tail : GetRepeatChildren(list, 1)) {
    children.push_back(tail.get());
  }
  PushChildren(process, children);
}

static const TransformFrameOps PSQL_STAGE_SET_OP_CHAIN_OPS = {
    "PsqlStageSetOpChain", InitializeHeadTail,
    &PEGTransformerFactory::FinalizeSelectSetOpChainTrampoline};

static const TransformFrameOps PSQL_STAGE_INTERSECT_CHAIN_OPS = {
    "PsqlStageIntersectChain", InitializeHeadTail,
    &PEGTransformerFactory::FinalizeIntersectChainTrampoline};

// The core initializer of FromSelectClause transforms its first child as a
// FromClause, so only its finalizer is reused
// PsqlStageFromSelect <- PsqlStageFrom SelectClause?
static void InitializeStageFromSelect(PEGTransformer &,
                                      GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  PushChildren(process, {list.GetChild(0), GetOptional(list, 1)});
}

static const TransformFrameOps PSQL_STAGE_FROM_SELECT_OPS = {
    "PsqlStageFromSelect", InitializeStageFromSelect,
    &PEGTransformerFactory::FinalizeFromSelectClauseTrampoline};

// PsqlStageFrom <- TableAlias? JoinOrPivot* (',' TableRef)*
// Child slots: the alias, then the joins, then the tables
static void InitializeStageFrom(PEGTransformer &,
                                GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  vector<optional_ptr<ParseResult>> children{GetOptional(list, 0)};
  for (auto &join : GetRepeatChildren(list, 1)) {
    children.push_back(join.get());
  }
  for (auto &table : GetRepeatChildren(list, 2)) {
    children.push_back(table.get().Cast<ListParseResult>().GetChild(1));
  }
  PushChildren(process, children);
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
// Pipe operators
//===--------------------------------------------------------------------===//
// |> EXTEND expression [[AS] alias] [, ...] [WINDOW name AS window_spec, ...]
// is SELECT *, expression [[AS] alias] [, ...] FROM <input> [WINDOW ...]
// PsqlExtend <- 'EXTEND' TargetList WindowClause?
static void InitializeExtend(PEGTransformer &transformer,
                             GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  // named windows are only visible in their own EXTEND, as in a SELECT
  transformer.window_clauses.emplace_back();
  // named windows have to be known before transforming the expressions
  PushChildren(process, {GetOptional(list, 2), list.GetChild(1)});
}

static unique_ptr<TransformResultValue>
FinalizeExtend(PEGTransformer &transformer,
               GeneratedTransformProcess &process) {
  if (process.child_results[0]) {
    // the named windows are registered with the transformer
    process.TakeResult<vector<unique_ptr<ParsedExpression>>>(0);
  }
  auto expressions =
      process.TakeResult<vector<unique_ptr<ParsedExpression>>>(1);
  transformer.window_clauses.pop_back();
  auto node = CreateStageSelect();
  for (auto &expression : expressions) {
    node->select_list.push_back(std::move(expression));
  }
  return SelectResult(std::move(node));
}

static const TransformFrameOps PSQL_EXTEND_OPS = {
    "PsqlExtend", InitializeExtend, FinalizeExtend};

// |> SET column = expression [, ...] is SELECT * REPLACE (expression AS column)
// PsqlSet <- 'SET' List(PsqlSetItem)
// PsqlSetItem <- ColId '=' Expression
static void InitializeSet(PEGTransformer &,
                          GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  vector<optional_ptr<ParseResult>> children;
  for (auto &item_ref : GetListElements(list.GetChild(1))) {
    auto &item = item_ref.get().Cast<ListParseResult>();
    children.push_back(item.GetChild(0));
    children.push_back(item.GetChild(2));
  }
  PushChildren(process, children);
}

static unique_ptr<TransformResultValue>
FinalizeSet(PEGTransformer &, GeneratedTransformProcess &process) {
  auto node = CreateStageSelect();
  auto &star = node->select_list[0]->Cast<StarExpression>();
  for (idx_t slot = 0; slot < process.child_results.size(); slot += 2) {
    auto column = process.TakeResult<Identifier>(slot);
    if (star.ReplaceList().find(column) != star.ReplaceList().end()) {
      throw ParserException("Column \"%s\" is set more than once", column);
    }
    star.ReplaceListMutable()[column] =
        process.TakeResult<unique_ptr<ParsedExpression>>(slot + 1);
  }
  return SelectResult(std::move(node));
}

static const TransformFrameOps PSQL_SET_OPS = {"PsqlSet", InitializeSet,
                                               FinalizeSet};

// |> DROP column [, ...] is SELECT * EXCLUDE (column [, ...])
// PsqlDrop <- 'DROP' List(ColId)
static void InitializeDrop(PEGTransformer &,
                           GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  vector<optional_ptr<ParseResult>> children;
  for (auto &column : GetListElements(list.GetChild(1))) {
    children.push_back(column.get());
  }
  PushChildren(process, children);
}

static unique_ptr<TransformResultValue>
FinalizeDrop(PEGTransformer &, GeneratedTransformProcess &process) {
  auto node = CreateStageSelect();
  auto &star = node->select_list[0]->Cast<StarExpression>();
  for (idx_t slot = 0; slot < process.child_results.size(); slot++) {
    star.ExcludeListMutable().insert(
        QualifiedColumnName(process.TakeResult<Identifier>(slot)));
  }
  return SelectResult(std::move(node));
}

static const TransformFrameOps PSQL_DROP_OPS = {"PsqlDrop", InitializeDrop,
                                                FinalizeDrop};

// |> RENAME old [AS] new [, ...] is SELECT * RENAME (old AS new [, ...])
// PsqlRename <- 'RENAME' List(PsqlRenameItem)
// PsqlRenameItem <- ColId 'AS'? ColId
static void InitializeRename(PEGTransformer &,
                             GeneratedTransformProcess &process) {
  auto &list = process.parse_result.Cast<ListParseResult>();
  vector<optional_ptr<ParseResult>> children;
  for (auto &item_ref : GetListElements(list.GetChild(1))) {
    auto &item = item_ref.get().Cast<ListParseResult>();
    children.push_back(item.GetChild(0));
    children.push_back(item.GetChild(2));
  }
  PushChildren(process, children);
}

static unique_ptr<TransformResultValue>
FinalizeRename(PEGTransformer &, GeneratedTransformProcess &process) {
  auto node = CreateStageSelect();
  auto &star = node->select_list[0]->Cast<StarExpression>();
  for (idx_t slot = 0; slot < process.child_results.size(); slot += 2) {
    QualifiedColumnName column(process.TakeResult<Identifier>(slot));
    if (star.RenameList().find(column) != star.RenameList().end()) {
      throw ParserException("Column \"%s\" is renamed more than once",
                            column.column);
    }
    star.RenameListMutable()[column] = process.TakeResult<Identifier>(slot + 1);
  }
  return SelectResult(std::move(node));
}

static const TransformFrameOps PSQL_RENAME_OPS = {
    "PsqlRename", InitializeRename, FinalizeRename};

// |> DISTINCT is SELECT DISTINCT * FROM <input>
static void InitializeDistinct(PEGTransformer &, GeneratedTransformProcess &) {}

static unique_ptr<TransformResultValue>
FinalizeDistinct(PEGTransformer &, GeneratedTransformProcess &) {
  auto node = CreateStageSelect();
  node->modifiers.push_back(make_uniq<DistinctModifier>());
  return SelectResult(std::move(node));
}

static const TransformFrameOps PSQL_DISTINCT_OPS = {
    "PsqlDistinct", InitializeDistinct, FinalizeDistinct};

//===--------------------------------------------------------------------===//
// Pipelines
//===--------------------------------------------------------------------===//
//! Transforms the pipeline source and stages in order, plugging each result
//! into the input of the next stage. At the top level, a final `|> TO` stage
//! turns the pipeline into a COPY statement.
class PsqlPipelineProcess final : public TransformProcess {
public:
  PsqlPipelineProcess(PEGTransformer &transformer_p, ParseResult &parse_result,
                      bool top_level_p)
      : transformer(transformer_p), top_level(top_level_p) {
    auto &list = parse_result.Cast<ListParseResult>();
    children.push_back({ChildType::QUERY, list.GetChild(0)});
    auto stages = GetRepeatChildren(list, 1);
    for (idx_t i = 0; i < stages.size(); i++) {
      // PsqlStage <- '|>' PsqlStageOperation
      auto &stage = stages[i].get().Cast<ListParseResult>();
      auto &pipe = stage.GetChild(0);
      auto &operation = GetChoice(stage.GetChild(1));
      if (IsRule(operation, "PsqlCopyTo")) {
        // writing to a file is a statement, not a query
        if (!top_level || i + 1 < stages.size()) {
          throw ParserException(pipe.GetLocation(),
                                "|> TO is only supported as the last stage of "
                                "a top-level query");
        }
        // PsqlCopyTo <- 'COPY'? 'TO' CopyFileName CopyOptions?
        auto &copy_to = operation.Cast<ListParseResult>();
        children.push_back({ChildType::COPY_FILE_NAME, copy_to.GetChild(2)});
        auto copy_options = GetOptional(copy_to, 3);
        if (copy_options) {
          children.push_back({ChildType::COPY_OPTIONS, *copy_options});
        }
        is_copy = true;
        continue;
      }
      // every part of a stage query is optional, but a stage that matched
      // nothing is most likely a mistake
      if (operation.GetLocation().length == 0) {
        throw ParserException(
            pipe.GetLocation(),
            "syntax error at or near \"|>\": empty pipe stage");
      }
      children.push_back({ChildType::QUERY, operation});
    }
  }

  TransformStep Resume(unique_ptr<TransformResultValue> child_result) override {
    if (child_result) {
      switch (children[next_child - 1].type) {
      case ChildType::QUERY:
        AddQuery(TakeResult<unique_ptr<SelectStatement>>(*child_result));
        break;
      case ChildType::COPY_FILE_NAME:
        file_name = TakeResult<unique_ptr<ParsedExpression>>(*child_result);
        break;
      case ChildType::COPY_OPTIONS:
        copy_options = TakeResult<vector<GenericCopyOption>>(*child_result);
        break;
      }
    }
    if (next_child < children.size()) {
      return TransformStep::Child(children[next_child++].parse_result.get());
    }
    // a WITH clause in front of the pipeline is visible in all of its stages
    result->node->cte_map = std::move(cte_map);
    if (!top_level) {
      return TransformStep::Complete(SelectResult(std::move(result)));
    }
    unique_ptr<SQLStatement> statement;
    if (is_copy) {
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
  enum class ChildType : uint8_t { QUERY, COPY_FILE_NAME, COPY_OPTIONS };

  struct PipelineChild {
    ChildType type;
    reference<ParseResult> parse_result;
  };

  void AddQuery(unique_ptr<SelectStatement> statement) {
    if (!result) {
      // the pipeline source
      result = std::move(statement);
      cte_map = std::move(result->node->cte_map);
      result->node->cte_map = CommonTableExpressionMap();
      return;
    }
    // `|> AS t` gives the input of the following stages the alias t, until a
    // stage gives its input another alias
    auto alias_only = GetAliasOnlyStage(*statement);
    auto input = FindStageInput(*statement->node);
    if (!input) {
      throw InternalException("PSQL stage has no input");
    }
    if (input->alias.empty()) {
      input->alias = pipeline_alias;
    } else {
      pipeline_alias = Identifier();
    }
    if (alias_only) {
      pipeline_alias = alias_only->alias;
    }
    input->subquery = std::move(result);
    result = std::move(statement);
  }

  PEGTransformer &transformer;
  //! Whether the pipeline is a top-level SELECT statement
  bool top_level;
  vector<PipelineChild> children;
  idx_t next_child = 0;
  unique_ptr<SelectStatement> result;
  CommonTableExpressionMap cte_map;
  //! The alias given by a preceding `|> AS alias` stage
  Identifier pipeline_alias;
  //! Whether the pipeline ends with `|> TO`
  bool is_copy = false;
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
  auto pipeline = &GetChoice(select);
  if (IsRule(*pipeline, "PsqlDelimitedPipeline")) {
    pipeline = &pipeline->Cast<ListParseResult>().GetChild(1);
  }
  return make_uniq<PsqlPipelineProcess>(transformer, *pipeline, true);
}

//===--------------------------------------------------------------------===//
// Operators in expressions
//===--------------------------------------------------------------------===//
// DuckDB accepts any operator token as an infix operator in expressions. An
// expression that ends a stage, such as `x` in `SELECT x |> SET y = 1`, would
// then continue with `|> SET` as a call of the operator |>, unless the stage
// starts with a reserved keyword. PsqlOperatorLiteral replaces OperatorLiteral
// in NamedOtherOperator and matches the same operators except |>.
class PsqlOperatorMatcher final : public AtomicMatcher {
public:
  PsqlOperatorMatcher() : AtomicMatcher(MatcherType::OPERATOR) {}

  MatcherResult MatchAtomic(MatchState &state) const override {
    auto token = state.token_iterator.Current();
    if (token && token->text == "|>") {
      return MatcherResult::Failure();
    }
    return operator_matcher.MatchAtomic(state);
  }

  SuggestionType AddSuggestionInternal(MatchState &) const override {
    return SuggestionType::MANDATORY;
  }

  string ToString() const override { return "OPERATOR"; }

private:
  OperatorMatcher operator_matcher;
};

static bool IsOperatorLiteralReference(const PEGExpression &expression) {
  return expression.type == PEGExpression::Type::REFERENCE &&
         StringUtil::CIEquals(expression.text.GetString(), "OperatorLiteral");
}

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
    auto add = [&](const char *rule,
                   grammar_transform_process_function_t transform = nullptr) {
      changes.push_back(GrammarChange::AddRule(rule, std::move(transform)));
    };
    changes.push_back(GrammarChange::ReplaceRule(
        "SelectStatement <- SelectStatementInternal", StartStatementTransform));
    changes.push_back(GrammarChange::ReplaceRule(
        "SelectStatementInternal <- PsqlDelimitedPipeline / PsqlPipeline",
        StartChoiceTransform));
    add("PsqlDelimitedPipeline <- '|' PsqlPipeline '|'", TransformChild(1));
    add("PsqlPipeline <- PsqlPipelineSource PsqlStage*",
        StartPipelineTransform);
    add("PsqlPipelineSource <- WithClause? SelectSetOpChain ResultModifiers?",
        TransformWith(PSQL_PIPELINE_SOURCE_OPS));
    // PsqlPipelineProcess transforms the parts of these rules
    add("PsqlStage <- '|>' PsqlStageOperation");
    add("PsqlStageOperation <- PsqlCopyTo / PsqlExtend / PsqlSet / PsqlDrop / "
        "PsqlRename / PsqlDistinct / PsqlStageQuery");
    add("PsqlCopyTo <- 'COPY'? 'TO' CopyFileName CopyOptions?");

    add("PsqlStageQuery <- PsqlStageSetOpChain ResultModifiers?",
        TransformWith(PSQL_STAGE_QUERY_OPS));
    add("PsqlStageSetOpChain <- PsqlStageIntersectChain SelectSetOpChainTail*",
        TransformWith(PSQL_STAGE_SET_OP_CHAIN_OPS));
    add("PsqlStageIntersectChain <- PsqlStageSelect IntersectChainTail*",
        TransformWith(PSQL_STAGE_INTERSECT_CHAIN_OPS));
    add("PsqlStageSelect <- PsqlStageFromSelect WhereClause? GroupByClause? "
        "HavingClause? WindowClause? QualifyClause? SampleClause?",
        TransformLike("SimpleSelect"));
    add("PsqlStageFromSelect <- PsqlStageFrom SelectClause?",
        TransformWith(PSQL_STAGE_FROM_SELECT_OPS));
    add("PsqlStageFrom <- TableAlias? JoinOrPivot* (',' TableRef)*",
        TransformWith(PSQL_STAGE_FROM_OPS));

    add("PsqlExtend <- 'EXTEND' TargetList WindowClause?",
        TransformWith(PSQL_EXTEND_OPS));
    add("PsqlSet <- 'SET' List(PsqlSetItem)", TransformWith(PSQL_SET_OPS));
    add("PsqlSetItem <- ColId '=' Expression");
    add("PsqlDrop <- 'DROP' List(ColId)", TransformWith(PSQL_DROP_OPS));
    add("PsqlRename <- 'RENAME' List(PsqlRenameItem)",
        TransformWith(PSQL_RENAME_OPS));
    add("PsqlRenameItem <- ColId 'AS'? ColId");
    add("PsqlDistinct <- 'DISTINCT'", TransformWith(PSQL_DISTINCT_OPS));

    add("PsqlOperatorLiteral <- Identifier", TransformLike("OperatorLiteral"));
    changes.push_back(GrammarChange::AddTerminalRuleOverride(
        "PsqlOperatorLiteral", [](const PEGKeywordHelper &) {
          return make_uniq<PsqlOperatorMatcher>();
        }));
    changes.push_back(GrammarChange::ReplaceChoice("NamedOtherOperator",
                                                   "PsqlOperatorLiteral",
                                                   IsOperatorLiteralReference));
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
