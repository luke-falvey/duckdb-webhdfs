#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

//! A webhdfs secret: a key-value secret whose scopes match whole authorities. Core matches scopes as plain string
//! prefixes, so a secret for `webhdfs://prod` would also apply to `webhdfs://prod2/...`.
//!
//! It is persisted as a plain KEY_VALUE_SECRET, so a session without webhdfs loaded can still read every persistent
//! secret (e.g. FROM duckdb_secrets()). Reloaded secrets are therefore plain KeyValueSecrets with core's prefix
//! MatchScore: the extension never relies on MatchScore or on SecretManager::LookupSecret and instead picks the
//! secret itself with ScopeMatchScore (see LookupSecret in webhdfs_config.cpp), so in-memory and persistent secrets
//! resolve alike. The MatchScore override only keeps which_secret() exact for secrets created in this session.
class WebHdfsSecret : public KeyValueSecret {
public:
	using KeyValueSecret::KeyValueSecret;

	int64_t MatchScore(const string &path) const override;
	unique_ptr<const BaseSecret> Clone() const override {
		return make_uniq<WebHdfsSecret>(*this);
	}

	//! Score of one scope for a path: its length on a match, the minimum int64 otherwise
	static int64_t ScopeMatchScore(const string &path, const string &scope);
	//! Best ScopeMatchScore over the scopes of any secret (0 for a secret without scopes)
	static int64_t SecretMatchScore(const BaseSecret &secret, const string &path);
};

//! Register the `webhdfs` secret type (CREATE SECRET (TYPE webhdfs, ...))
void RegisterWebHdfsSecretType(ExtensionLoader &loader);

} // namespace duckdb
