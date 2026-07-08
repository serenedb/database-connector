#pragma once

#include <functional>
#include <string>

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

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
		//! Per-order-key veto: called with the scan and the resolved TABLE column id
		//! once an order key traces to it; return true to refuse folding that key's
		//! sort remotely (e.g. the remote engine's ordering for the column's type
		//! diverges from DuckDB's, so a remote sort would ship a different row set).
		//! Null = every traceable key is safe (legacy behaviour).
		std::function<bool(const duckdb::LogicalGet &get, duckdb::column_t column_id)> order_key_unsafe;
		//! Scan-level veto for LIMIT-carrying folds (TOP_N and LIMIT): return true to
		//! refuse when cutting rows remotely is unsafe -- e.g. the scan re-applies part
		//! of its table filters locally, so a remote LIMIT would truncate the stream
		//! BEFORE the local re-check and drop rows that belong in the result. A pure
		//! ORDER BY fold is unaffected (local filtering preserves the row order).
		//! Null = always safe (legacy behaviour).
		std::function<bool(const duckdb::LogicalGet &get)> limit_unsafe;
	};

	static Config CreateConfig(duckdb::ClientContext &ctx, const std::string &enabled_option, char identifier_quote,
	                           query::QuoteEscapeStyle escape_style, std::string table_scan_name);

	static void Optimize(const Config &config, duckdb::OptimizerExtensionInput &input,
	                     duckdb::unique_ptr<duckdb::LogicalOperator> &op);
};

} // namespace optimizer
} // namespace dbconnector
