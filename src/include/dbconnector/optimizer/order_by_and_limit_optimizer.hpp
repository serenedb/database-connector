#pragma once

#include <string>

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

#include "dbconnector/query/query_writer.hpp"

namespace dbconnector {
namespace optimizer {

class OrderByAndLimitOptimizer {
public:
	struct Config {
		bool enabled = false;
		char identifier_quote = '"';
		query::QuoteEscapeStyle escape_style = query::QuoteEscapeStyle::DOUBLE_QUOTE;
		std::string table_scan_name;
		//! False when the connector may re-apply the scan's table filters locally
		//! (inexact remote pushdown): folding LIMIT/TOP_N would then truncate the
		//! stream BEFORE the local re-check and drop rows that belong in the result.
		//! A filterless scan still folds either way, and a pure ORDER BY fold is
		//! unaffected (local filtering preserves the row order). True = always fold
		//! (exact-pushdown engines, the legacy behaviour).
		bool fold_limit_with_table_filters = true;
		//! Selects the per-type ORDER-key rewrites that make the remote sort
		//! reproduce DuckDB's ordering (see TryBuildOrderByClause).
		query::Dialect dialect = query::Dialect::Postgres;
	};

	static Config CreateConfig(duckdb::ClientContext &ctx, const std::string &enabled_option, char identifier_quote,
	                           query::QuoteEscapeStyle escape_style, std::string table_scan_name,
	                           query::Dialect dialect);

	static void Optimize(const Config &config, duckdb::OptimizerExtensionInput &input,
	                     duckdb::unique_ptr<duckdb::LogicalOperator> &op);
};

} // namespace optimizer
} // namespace dbconnector
