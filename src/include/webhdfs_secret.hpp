#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

//! A webhdfs secret: a key-value secret whose scopes match whole authorities. Core matches scopes as plain string
//! prefixes, so a secret for `webhdfs://prod` would also apply to `webhdfs://prod2/...`.
class WebHdfsSecret : public KeyValueSecret {
public:
	using KeyValueSecret::KeyValueSecret;

	int64_t MatchScore(const string &path) const override;
	//! Written like a KeyValueSecret but marked custom, so DuckDB reloads it with this type's deserializer (it
	//! reloads key-value secrets as plain KeyValueSecrets, which would lose the authority matching)
	void Serialize(Serializer &serializer) const override;
	unique_ptr<const BaseSecret> Clone() const override {
		return make_uniq<WebHdfsSecret>(*this);
	}

	//! Score of one scope for a path: its length on a match, the minimum int64 otherwise
	static int64_t ScopeMatchScore(const string &path, const string &scope);
};

//! Register the `webhdfs` secret type (CREATE SECRET (TYPE webhdfs, ...))
void RegisterWebHdfsSecretType(ExtensionLoader &loader);

} // namespace duckdb
