#include "dbconnector/table_scan/filter_pushdown.hpp"

#include "duckdb/function/scalar/struct_utils.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"

#include "dbconnector/query/query_writer.hpp"

#include "dbconnector/table_scan/filter_util.hpp"
#include "dbconnector/table_scan/table_scan_exception.hpp"

namespace dbconnector {
namespace table_scan {

using namespace duckdb;

FilterPushdown::Config FilterPushdown::CreateConfig(char identifier_quote, char constant_quote,
                                                    query::QuoteEscapeStyle escape_style, query::Dialect dialect,
                                                    const std::string &blob_literal_prefix,
                                                    const std::string &blob_literal_suffix) {
	Config res;
	res.identifier =
	    query::QueryWriter::CreateConfig(identifier_quote, escape_style, std::string(), std::string(), dialect);
	res.constant =
	    query::QueryWriter::CreateConfig(constant_quote, escape_style, blob_literal_prefix, blob_literal_suffix, dialect);
	return res;
}

static bool IsOptionalFilterExpression(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &name = expr.Cast<BoundFunctionExpression>().Function().GetName();
	return name == OptionalFilterScalarFun::NAME || name == SelectivityOptionalFilterScalarFun::NAME ||
	       name == DynamicFilterScalarFun::NAME;
}

std::string FilterPushdown::CreateExpression(const query::QueryWriter::Config &identifier_config,
                                             const query::QueryWriter::Config &constant_config,
                                             const std::string &column_name,
                                             const vector<unique_ptr<Expression>> &filters, const std::string &op,
                                             column_t column_id) {
	const bool is_or = op == "OR";
	vector<std::string> filter_entries;
	for (auto &filter : filters) {
		auto new_filter = TransformExpression(identifier_config, constant_config, column_name, *filter, column_id);
		if (new_filter.empty()) {
			// Optional (advisory) pieces may be dropped from an AND (widens the
			// SQL, never the result). Dropping anything from an OR narrows the
			// result, and dropping a required AND conjunct widens the SQL past
			// the filter -- both make the SQL lie, so the whole filter renders
			// empty and stays local.
			if (!is_or && IsOptionalFilterExpression(*filter)) {
				continue;
			}
			return std::string();
		}
		filter_entries.push_back(std::move(new_filter));
	}
	if (filter_entries.empty()) {
		return std::string();
	}
	return "(" + StringUtil::Join(filter_entries, " " + op + " ") + ")";
}

std::string FilterPushdown::TransformComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "!=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw TableScanException("Unsupported expression type: '" + EnumUtil::ToString(type) + "'");
	}
}

static bool IsDirectReference(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_COLUMN_REF:
		return true;
	default:
		return false;
	}
}

string FilterPushdown::TransformConstantFilter(const query::QueryWriter::Config &constant_config,
                                               const string &column_name, ExpressionType comparison_type,
                                               const Value &constant, column_t column_id) {
	if (IsVirtualColumn(column_id)) {
		// A rowid has no remote SQL identity; rendering anything (the old FALSE)
		// makes the SQL narrower than the filter. Unrenderable -> stays local.
		return string();
	}
	string constant_string = query::QueryWriter::WriteConstant(constant_config, constant);
	auto operator_string = TransformComparison(comparison_type);
	string comparison = StringUtil::Format("%s %s %s", column_name, operator_string, constant_string);
	// Postgres forces byte-wise comparison to match DuckDB; ClickHouse's String
	// comparison is already byte-wise and rejects the COLLATE clause.
	if (constant.type().id() == LogicalTypeId::VARCHAR &&
	    constant_config.dialect == query::Dialect::Postgres) {
		comparison += " COLLATE \"C\"";
	}
	return comparison;
}

string FilterPushdown::TransformExpressionSubject(const query::QueryWriter::Config &identifier_config,
                                                  const string &column_name, const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_COLUMN_REF:
		return column_name;
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		idx_t child_idx;
		if (!TryGetStructExtractChildIndex(func, child_idx) || func.GetChildren().empty()) {
			return string();
		}
		auto parent_name = TransformExpressionSubject(identifier_config, column_name, *func.GetChildren()[0]);
		if (parent_name.empty()) {
			return string();
		}
		auto &struct_type = func.GetChildren()[0]->GetReturnType();
		if (struct_type.id() != LogicalTypeId::STRUCT || StructType::IsUnnamed(struct_type)) {
			return string();
		}
		auto field = StructType::GetChildName(struct_type, child_idx).GetIdentifierName();
		if (identifier_config.dialect == query::Dialect::ClickHouse) {
			// ClickHouse addresses a Tuple field as tupleElement(col, 'name').
			auto constant_config = query::QueryWriter::CreateConfig('\'', identifier_config.escape_style);
			return "tupleElement(" + parent_name + ", " +
			       query::QueryWriter::WriteQuotedAndEscaped(constant_config, field) + ")";
		}
		auto child_name = query::QueryWriter::WriteQuotedAndEscaped(identifier_config, field);
		return "(" + parent_name + ")." + child_name;
	}
	default:
		return string();
	}
}

std::string FilterPushdown::TransformExpression(const query::QueryWriter::Config &identifier_config,
                                                const query::QueryWriter::Config &constant_config,
                                                const std::string &column_name, const Expression &expr,
                                                column_t column_id) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto comparison_type = comparison.GetExpressionType();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		auto subject = TransformExpressionSubject(identifier_config, column_name, left);
		const Value *constant = nullptr;
		if (!subject.empty() && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant = &right.Cast<BoundConstantExpression>().GetValue();
		} else {
			subject = TransformExpressionSubject(identifier_config, column_name, right);
			if (!subject.empty() && left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				constant = &left.Cast<BoundConstantExpression>().GetValue();
				comparison_type = FlipComparisonExpression(comparison_type);
			}
		}
		if (!constant || subject.empty()) {
			return string();
		}
		return TransformConstantFilter(constant_config, subject, comparison_type, *constant, column_id);
	}

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		switch (conjunction.GetExpressionType()) {
		case ExpressionType::CONJUNCTION_AND:
			return CreateExpression(identifier_config, constant_config, column_name, conjunction.GetChildren(), "AND",
			                        column_id);
		case ExpressionType::CONJUNCTION_OR:
			return CreateExpression(identifier_config, constant_config, column_name, conjunction.GetChildren(), "OR",
			                        column_id);
		default:
			return std::string();
		}
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto subject = op.GetChildren().empty()
		                   ? string()
		                   : TransformExpressionSubject(identifier_config, column_name, *op.GetChildren()[0]);
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			if (!subject.empty()) {
				return subject + " IS NULL";
			}
			return std::string();
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			if (!subject.empty()) {
				return subject + " IS NOT NULL";
			}
			return std::string();
		case ExpressionType::COMPARE_IN: {
			if (subject.empty() || IsVirtualColumn(column_id)) {
				return string();
			}
			std::string in_list;
			for (idx_t i = 1; i < op.GetChildren().size(); i++) {
				if (op.GetChildren()[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return std::string();
				}
				if (!in_list.empty()) {
					in_list += ", ";
				}
				in_list += query::QueryWriter::WriteConstant(
				    constant_config, op.GetChildren()[i]->Cast<BoundConstantExpression>().GetValue());
			}
			return subject + " IN (" + in_list + ")";
		}
		default:
			return std::string();
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		auto &name = func.Function().GetName();
		optional_ptr<const Expression> child;
		if (func.BindInfo() && name == OptionalFilterScalarFun::NAME) {
			child = func.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr.get();
		} else if (func.BindInfo() && name == SelectivityOptionalFilterScalarFun::NAME) {
			child = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get();
		}
		if (!child) {
			return std::string();
		}
		return TransformExpression(identifier_config, constant_config, column_name, *child, column_id);
	}
	default:
		return std::string();
	}
}

std::string FilterPushdown::TransformFilter(const FilterPushdown::Config &config, const std::string &column_name,
                                            const TableFilter &filter, column_t column_id) {
	std::string column_name_quoted = query::QueryWriter::WriteQuotedAndEscaped(config.identifier, column_name);
	auto &expr = FilterUtil::GetExpression(filter, "FilterPushdown::TransformFilter");
	return TransformExpression(config.identifier, config.constant, column_name_quoted, expr, column_id);
}

} // namespace table_scan
} // namespace dbconnector
