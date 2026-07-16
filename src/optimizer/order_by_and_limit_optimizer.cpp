#include "dbconnector/optimizer/order_by_and_limit_optimizer.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

#include "dbconnector/bind_data.hpp"
#include "dbconnector/optimizer/optimizer_util.hpp"
#include "dbconnector/query/query_writer.hpp"

namespace dbconnector {
namespace optimizer {

using namespace duckdb;

OrderByAndLimitOptimizer::Config
OrderByAndLimitOptimizer::CreateConfig(ClientContext &ctx, const std::string &enabled_option, char identifier_quote,
                                       query::QuoteEscapeStyle escape_style, std::string table_scan_name,
                                       query::Dialect dialect) {
	Config res;

	res.enabled = false;
	Value enabled_val;
	if (ctx.TryGetCurrentSetting(enabled_option, enabled_val) && !enabled_val.IsNull()) {
		res.enabled = BooleanValue::Get(enabled_val);
	}

	res.identifier_quote = identifier_quote;
	res.escape_style = escape_style;
	res.table_scan_name = std::move(table_scan_name);
	res.dialect = dialect;

	return res;
}

// Traces an ORDER BY key expression to a scan column; fills `quoted` (the
// dialect-quoted column reference) and `type`. False when the key is not a
// plain scan column.
static bool TraceOrderKey(const OrderByAndLimitOptimizer::Config &config, Expression &expr, LogicalOperator &child,
                          LogicalGet &get, string &quoted, LogicalType &type) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &col_ref = expr.Cast<BoundColumnRefExpression>();
	if (col_ref.Depth() > 0) {
		return false;
	}
	auto traced = OptimizerUtil::TraceBindingToColumn(col_ref.BindingMutable(), child, get);
	if (!traced.Found()) {
		return false;
	}
	auto query_config = query::QueryWriter::CreateConfig(config.identifier_quote, config.escape_style);
	quoted = query::QueryWriter::WriteQuotedAndEscaped(query_config, traced.col_name);
	type = traced.col_type;
	return true;
}

// True when `type` nests a scalar whose ClickHouse ordering diverges from
// DuckDB's. A compound key compares field-wise on both sides, but a divergent
// field cannot be rewritten inside a whole-value comparison (no per-field
// isNaN/toString), so such keys stay local. VARCHAR fields are conservative:
// an Enum surfaces as VARCHAR locally but sorts by ordinal remotely, and the
// DuckDB type cannot tell the two apart.
static bool CompoundContainsDivergent(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::UUID:
	case LogicalTypeId::VARCHAR:
		return true;
	case LogicalTypeId::STRUCT: {
		for (auto &child : StructType::GetChildTypes(type)) {
			if (CompoundContainsDivergent(child.second)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::LIST:
		return CompoundContainsDivergent(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return CompoundContainsDivergent(ArrayType::GetChildType(type));
	case LogicalTypeId::MAP:
		return CompoundContainsDivergent(MapType::KeyType(type)) || CompoundContainsDivergent(MapType::ValueType(type));
	default:
		return false;
	}
}

static bool LimitFoldUnsafe(const OrderByAndLimitOptimizer::Config &config, const LogicalGet &get) {
	// Postgres filter pushdown is exact-or-error: every required filter runs in
	// the remote statement, WHERE before LIMIT, so folding is always safe. Other
	// engines' comparison semantics (ClickHouse: NaN, Enum ordinals, literals
	// parsed in the server time zone) force connectors to keep some required
	// filters local and re-apply them AFTER the fetch -- a folded LIMIT would
	// truncate the stream before that re-check and drop qualifying rows.
	// Optional (advisory) filters never change the row set and do not block.
	if (config.dialect == query::Dialect::Postgres) {
		return false;
	}
	for (auto &entry : get.table_filters) {
		if (!ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			return true;
		}
	}
	return false;
}

static string TryBuildOrderByClause(const OrderByAndLimitOptimizer::Config &config, vector<BoundOrderByNode> &orders,
                                    LogicalOperator &child, LogicalGet &get) {
	vector<string> fragments;
	for (auto &order : orders) {
		string quoted;
		LogicalType key_type;
		if (!TraceOrderKey(config, *order.expression, child, get, quoted, key_type)) {
			return std::string();
		}

		OrderType direction = order.type;
		OrderByNullType null_order = order.null_order;

		if (direction == OrderType::ORDER_DEFAULT) {
			direction = OrderType::ASCENDING;
		}
		if (null_order == OrderByNullType::ORDER_DEFAULT) {
			null_order =
			    (direction == OrderType::ASCENDING) ? OrderByNullType::NULLS_LAST : OrderByNullType::NULLS_FIRST;
		}

		// Emit an explicit IS [NOT] NULL prefix key for BOTH null placements instead
		// of relying on the remote engine's default, which differs per engine (MySQL
		// sorts NULLs first on ASC; Postgres and ClickHouse sort them last). A bare
		// "col ASC" for NULLS FIRST returns the wrong rows on the latter two -- and
		// since the fold removes the local sort node, nothing downstream corrects it.
		const char *null_key = (null_order == OrderByNullType::NULLS_FIRST) ? " IS NOT NULL, " : " IS NULL, ";
		const char *dir = (direction == OrderType::ASCENDING) ? " ASC" : " DESC";

		// Rewrite the value key per dialect so the remote reproduces DuckDB's
		// ordering (the NULLS prefix stays on the bare column):
		//  - ClickHouse floats compare IEEE, leaving NaN unordered; DuckDB sorts
		//    NaN above every number. An isNaN() prefix key in the key's own
		//    direction pins NaN to the greatest position.
		//  - ClickHouse text-backed columns (Enum labels, IPv4/6, JSON,
		//    Decimal(>38)) surface locally as their toString() text, and UUIDs
		//    sort by a half-swapped byte order: ordering by toString(col) matches
		//    the local byte-wise order in every case (identity for plain String).
		//  - Compound keys nesting a divergent scalar cannot be rewritten
		//    field-wise -- the sort stays local.
		//  - Postgres sorts text by locale collation; DuckDB compares bytes, so
		//    the key gets COLLATE "C". Its floats/UUIDs already match DuckDB.
		string value_key = quoted;
		string nan_key;
		switch (config.dialect) {
		case query::Dialect::ClickHouse:
			switch (key_type.id()) {
			case LogicalTypeId::FLOAT:
			case LogicalTypeId::DOUBLE:
				nan_key = "isNaN(" + quoted + ")" + dir + ", ";
				break;
			case LogicalTypeId::VARCHAR:
			case LogicalTypeId::UUID:
				value_key = "toString(" + quoted + ")";
				break;
			case LogicalTypeId::STRUCT:
			case LogicalTypeId::LIST:
			case LogicalTypeId::ARRAY:
			case LogicalTypeId::MAP:
				if (CompoundContainsDivergent(key_type)) {
					return std::string();
				}
				break;
			default:
				break;
			}
			break;
		case query::Dialect::Postgres:
			if (key_type.id() == LogicalTypeId::VARCHAR) {
				value_key = quoted + " COLLATE \"C\"";
			}
			break;
		}

		fragments.push_back(quoted + null_key + nan_key + value_key + dir);
	}
	return " ORDER BY " + StringUtil::Join(fragments, ", ");
}

static void CollectBindingRefs(Expression &expr, idx_t target_table_index, unordered_set<idx_t> &referenced) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		if (ref.BindingMutable().table_index.index == target_table_index) {
			referenced.insert(ref.BindingMutable().column_index);
		}
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](unique_ptr<Expression> &child) { CollectBindingRefs(*child, target_table_index, referenced); });
}

static void RewriteBindingRefs(Expression &expr, idx_t target_table_index, unordered_map<idx_t, idx_t> &old_to_new) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &ref = expr.Cast<BoundColumnRefExpression>();
		if (ref.BindingMutable().table_index.index == target_table_index) {
			auto it = old_to_new.find(ref.BindingMutable().column_index);
			if (it != old_to_new.end()) {
				ref.BindingMutable().column_index = ProjectionIndex(it->second);
			}
		}
	}
	ExpressionIterator::EnumerateChildren(
	    expr, [&](unique_ptr<Expression> &child) { RewriteBindingRefs(*child, target_table_index, old_to_new); });
}

static void PruneProjectionLayer(LogicalProjection &proj, const unordered_set<idx_t> &keep_indices,
                                 vector<LogicalProjection *> &above_projs) {
	vector<unique_ptr<Expression>> new_exprs;
	unordered_map<idx_t, idx_t> old_to_new;
	for (idx_t i = 0; i < proj.expressions.size(); i++) {
		if (keep_indices.count(i)) {
			old_to_new[i] = new_exprs.size();
			new_exprs.push_back(std::move(proj.expressions[i]));
		}
	}
	proj.expressions = std::move(new_exprs);

	for (auto *above : above_projs) {
		for (auto &expr : above->expressions) {
			RewriteBindingRefs(*expr, proj.table_index.index, old_to_new);
		}
	}
}

static void PruneColumnsAfterOrderByRemoval(LogicalOperator &child, LogicalGet &get,
                                            const vector<ProjectionIndex> &projection_map) {
	if (child.type == LogicalOperatorType::LOGICAL_GET) {
		// projection_map entries are positions in the child's OUTPUT; resolve them
		// through the bindings (which honor projection_ids) instead of indexing
		// column_ids positionally -- the two spaces differ on non-identity scans.
		auto bindings = get.GetColumnBindings();
		vector<ColumnIndex> new_ids;
		for (auto pos : projection_map) {
			new_ids.push_back(get.GetColumnIndex(bindings[pos]));
		}
		get.SetColumnIds(std::move(new_ids));
		get.projection_ids.clear();
		return;
	}

	if (child.type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}

	vector<LogicalProjection *> proj_chain;
	reference<LogicalOperator> current = child;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		proj_chain.push_back(&current.get().Cast<LogicalProjection>());
		if (current.get().children.empty()) {
			break;
		}
		current = *current.get().children[0];
	}

	if (proj_chain.size() < 2) {
		return;
	}

	auto &top_proj = *proj_chain[0];
	vector<LogicalProjection *> above_projs;
	above_projs.push_back(&top_proj);

	for (idx_t layer = 1; layer < proj_chain.size(); layer++) {
		auto &prev_proj = *proj_chain[layer - 1];
		auto &curr_proj = *proj_chain[layer];

		unordered_set<idx_t> needed;
		for (auto &expr : prev_proj.expressions) {
			CollectBindingRefs(*expr, curr_proj.table_index.index, needed);
		}

		if (needed.size() < curr_proj.expressions.size()) {
			PruneProjectionLayer(curr_proj, needed, above_projs);
		}
		above_projs.push_back(&curr_proj);
	}

	auto &bottom_proj = *proj_chain.back();
	unordered_set<idx_t> get_referenced;
	for (auto &expr : bottom_proj.expressions) {
		CollectBindingRefs(*expr, get.table_index.index, get_referenced);
	}

	auto &column_ids = get.GetColumnIds();
	if (get_referenced.size() < column_ids.size()) {
		vector<ColumnIndex> new_ids;
		unordered_map<idx_t, idx_t> get_old_to_new;
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (get_referenced.count(i)) {
				get_old_to_new[i] = new_ids.size();
				new_ids.push_back(column_ids[i]);
			}
		}
		get.SetColumnIds(std::move(new_ids));
		get.projection_ids.clear();

		for (auto *proj : proj_chain) {
			for (auto &expr : proj->expressions) {
				RewriteBindingRefs(*expr, get.table_index.index, get_old_to_new);
			}
		}
	}
}

void OrderByAndLimitOptimizer::Optimize(const OrderByAndLimitOptimizer::Config &config, OptimizerExtensionInput &input,
                                        unique_ptr<LogicalOperator> &op) {
	if (!config.enabled) {
		return;
	}

	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		auto &topn = op->Cast<LogicalTopN>();
		LogicalGet *get = nullptr;
		dbconnector::BindData *bind_data = nullptr;
		if (OptimizerUtil::FindExtensionGet(config.table_scan_name, *op->children[0], get, bind_data) &&
		    !LimitFoldUnsafe(config, *get)) {
			string order_clause = TryBuildOrderByClause(config, topn.orders, *op->children[0], *get);
			if (!order_clause.empty()) {
				auto &order_by_and_limit_bind_data = bind_data->GetOrderByAndLimitBindData();
				order_by_and_limit_bind_data.order_by_clause = order_clause;
				order_by_and_limit_bind_data.limit_clause = " LIMIT " + to_string(topn.limit);
				if (topn.offset > 0) {
					order_by_and_limit_bind_data.limit_clause += " OFFSET " + to_string(topn.offset);
				}
				op = std::move(op->children[0]);
				return;
			}
		}
		for (auto &child : op->children) {
			Optimize(config, input, child);
		}
		return;
	}
	if (op->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		auto &order = op->Cast<LogicalOrder>();
		LogicalGet *get = nullptr;
		dbconnector::BindData *bind_data = nullptr;
		if (OptimizerUtil::FindExtensionGet(config.table_scan_name, *op->children[0], get, bind_data)) {
			string order_clause = TryBuildOrderByClause(config, order.orders, *op->children[0], *get);
			if (!order_clause.empty()) {
				auto &order_by_and_limit_bind_data = bind_data->GetOrderByAndLimitBindData();
				order_by_and_limit_bind_data.order_by_clause = order_clause;
				if (!order.projection_map.empty()) {
					PruneColumnsAfterOrderByRemoval(*op->children[0], *get, order.projection_map);
				}
				op = std::move(op->children[0]);
				return;
			}
		}
		for (auto &child : op->children) {
			Optimize(config, input, child);
		}
		return;
	}
	if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op->Cast<LogicalLimit>();
		LogicalGet *get = nullptr;
		dbconnector::BindData *bind_data = nullptr;
		if (!OptimizerUtil::FindExtensionGet(config.table_scan_name, *op->children[0], get, bind_data) ||
		    LimitFoldUnsafe(config, *get)) {
			return;
		}
		switch (limit.limit_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			return;
		}
		switch (limit.offset_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			return;
		}
		auto &order_by_and_limit_bind_data = bind_data->GetOrderByAndLimitBindData();
		if (!order_by_and_limit_bind_data.limit_clause.empty()) {
			return;
		}
		bool has_limit = (limit.limit_val.Type() != LimitNodeType::UNSET);
		bool has_offset = (limit.offset_val.Type() != LimitNodeType::UNSET);
		if (!has_limit && has_offset) {
			return;
		}
		if (has_limit) {
			order_by_and_limit_bind_data.limit_clause = " LIMIT " + to_string(limit.limit_val.GetConstantValue());
		}
		if (has_offset) {
			order_by_and_limit_bind_data.limit_clause += " OFFSET " + to_string(limit.offset_val.GetConstantValue());
		}
		op = std::move(op->children[0]);
		return;
	}
	for (auto &child : op->children) {
		Optimize(config, input, child);
	}
}

} // namespace optimizer
} // namespace dbconnector
