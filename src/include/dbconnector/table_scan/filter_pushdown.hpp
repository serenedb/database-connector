#pragma once

#include <string>

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/table_filter.hpp"

#include "dbconnector/query/query_writer.hpp"

namespace dbconnector {
namespace table_scan {

class FilterPushdown {
	struct Config {
		char identifier_quote = '"';
		char constant_quote = '\'';
		query::QuoteEscapeStyle escape_style = query::QuoteEscapeStyle::DOUBLE_QUOTE;
		std::string blob_literal_prefix;
		std::string blob_literal_suffix;
		query::Dialect dialect = query::Dialect::Postgres;
	};

public:
	static Config CreateConfig(char identifier_quote, char constant_quote, query::QuoteEscapeStyle escape_style,
	                           const std::string &blob_literal_prefix = std::string(),
	                           const std::string &blob_literal_suffix = std::string(),
	                           query::Dialect dialect = query::Dialect::Postgres);

	// `exact` (optional): when provided, set to false if the rendered SQL is WIDER
	// than the filter (a dropped AND conjunct -> superset the caller must re-apply
	// locally); a partial OR renders empty (the whole disjunction stays local)
	// rather than a wrong subset. Passing nullptr keeps the legacy drop-and-widen
	// behaviour, so existing callers are unaffected.
	static std::string TransformFilter(const Config &config, const std::string &column_name,
	                                   const duckdb::TableFilter &filter, duckdb::column_t column_id,
	                                   bool *exact = nullptr);

private:
	static std::string TransformExpression(const query::QueryWriter::Config &identifier_config,
	                                       const query::QueryWriter::Config &constant_config,
	                                       const std::string &column_name, const duckdb::Expression &expr,
	                                       duckdb::column_t column_id, bool *exact);
	static std::string TransformExpressionSubject(const query::QueryWriter::Config &identifier_config,
	                                              const std::string &column_name, const duckdb::Expression &expr);
	static std::string TransformConstantFilter(const query::QueryWriter::Config &constant_config,
	                                           const std::string &column_name, duckdb::ExpressionType comparison_type,
	                                           const duckdb::Value &constant, duckdb::column_t column_id);
	static std::string TransformComparison(duckdb::ExpressionType type);
	static std::string CreateExpression(const query::QueryWriter::Config &identifier_config,
	                                    const query::QueryWriter::Config &constant_config,
	                                    const std::string &column_name,
	                                    const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &filters,
	                                    const std::string &op, duckdb::column_t column_id, bool *exact);
};

} // namespace table_scan
} // namespace dbconnector
