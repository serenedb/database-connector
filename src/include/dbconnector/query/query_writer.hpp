#pragma once

#include <string>

#include "duckdb/common/types/value.hpp"

#include "dbconnector/query/query_writer_exception.hpp"

namespace dbconnector {
namespace query {

enum class QuoteEscapeStyle { BACKSLASH, DOUBLE_QUOTE };

// The remote SQL dialect a pushdown targets. Selects the dialect-specific bits
// that are not expressible through the quote/escape/blob knobs alone: string
// constant escaping, VARCHAR collation, and struct/tuple field access.
enum class Dialect { Postgres, ClickHouse };

class QueryWriter {
public:
	struct Config {
		char quote = '"';
		QuoteEscapeStyle escape_style = QuoteEscapeStyle::DOUBLE_QUOTE;
		std::string blob_literal_prefix;
		std::string blob_literal_suffix;
		Dialect dialect = Dialect::Postgres;
	};

	static Config CreateConfig(char quote, QuoteEscapeStyle escape_style,
	                           const std::string &blob_literal_prefix = std::string(),
	                           const std::string &blob_literal_suffix = std::string(),
	                           Dialect dialect = Dialect::Postgres);
	static std::string WriteQuotedAndEscaped(const QueryWriter::Config &config, const std::string &text);
	static std::string WriteConstant(const QueryWriter::Config &config, const duckdb::Value &val);

private:
	static std::string EncodeBlob(const QueryWriter::Config &config, const std::string &val);
};

} // namespace query
} // namespace dbconnector
