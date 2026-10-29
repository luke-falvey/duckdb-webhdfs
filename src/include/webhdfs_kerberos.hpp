#pragma once

// SPNEGO ("Authorization: Negotiate") tokens for Kerberos-secured WebHDFS, made with the system GSSAPI library,
// loaded with dlopen on first use. Nothing is linked at build time, so the extension loads on machines without
// Kerberos and only fails when a kerberos-authenticated request is made.

#include <string>

namespace webhdfs {

//! Create a base64 SPNEGO token for the service principal "<service>@<host>" (e.g. HTTP@nn1.example.com), using
//! the default credentials (the Kerberos ticket cache: kinit first, KRB5CCNAME selects a cache).
//! Returns false and sets `error` on failure.
bool CreateSpnegoToken(const std::string &service, const std::string &host, std::string &token, std::string &error);

//! True if a GSSAPI library could be loaded.
bool GssapiAvailable(std::string &error);

std::string Base64Encode(const unsigned char *data, size_t len);

} // namespace webhdfs
