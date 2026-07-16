#pragma once

#include <string>
#include <vector>

namespace dbconnector {
namespace optimizer {

struct OrderByAndLimitBindData {
	std::string limit_clause;
	std::string order_by_clause;
	//! Per TABLE column (parallel to the scan's names): true = the remote engine's
	//! ordering for this column's type diverges from DuckDB's, so an ORDER BY key
	//! tracing to it must not be folded remotely (the sorted row set would differ).
	//! Empty = every column is safe (the legacy behaviour). Filled by the connector
	//! from its bind-time type metadata, typically lazily in its
	//! GetOrderByAndLimitBindData() override.
	std::vector<bool> order_key_unsafe;
};

} // namespace optimizer
} // namespace dbconnector
