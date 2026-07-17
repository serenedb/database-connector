#include "dbconnector/table_scan/filter_util.hpp"

#include "duckdb/planner/filter/expression_filter.hpp"

namespace dbconnector {
namespace table_scan {

const duckdb::Expression &FilterUtil::GetExpression(const duckdb::TableFilter &filter,
                                                    const std::string &call_context) {
	auto &expr_filter = duckdb::ExpressionFilter::GetExpressionFilter(filter, call_context.c_str());
	return *expr_filter.expr;
}

bool FilterUtil::IsInternalFilter(const duckdb::TableFilter &filter) {
	// Any optional filter is safe to skip. In practice only the dynamic filter needs this: the non-dynamic ones
	// already have a string representation for filter pushdown. But if we ever forget to implement one, skipping it
	// is still correct as long as it's optional, instead of raising an error.
	return duckdb::ExpressionFilter::IsOptionalFilter(filter);
}

std::string FilterUtil::ToString(const duckdb::TableFilter &filter) {
	auto &expr = GetExpression(filter, "FilterPushdown::ToString");
	return expr.ToString();
}

} // namespace table_scan
} // namespace dbconnector
