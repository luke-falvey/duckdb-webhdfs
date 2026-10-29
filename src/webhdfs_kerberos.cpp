#include "webhdfs_kerberos.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace webhdfs {

std::string Base64Encode(const unsigned char *data, size_t len) {
	static const char *TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	size_t i = 0;
	while (i + 2 < len) {
		uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
		out += TABLE[(n >> 18) & 63];
		out += TABLE[(n >> 12) & 63];
		out += TABLE[(n >> 6) & 63];
		out += TABLE[n & 63];
		i += 3;
	}
	if (i + 1 == len) {
		uint32_t n = uint32_t(data[i]) << 16;
		out += TABLE[(n >> 18) & 63];
		out += TABLE[(n >> 12) & 63];
		out += "==";
	} else if (i + 2 == len) {
		uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
		out += TABLE[(n >> 18) & 63];
		out += TABLE[(n >> 12) & 63];
		out += TABLE[(n >> 6) & 63];
		out += '=';
	}
	return out;
}

#ifdef _WIN32

bool GssapiAvailable(std::string &error) {
	error = "Kerberos authentication is not supported on Windows yet";
	return false;
}

bool CreateSpnegoToken(const std::string &, const std::string &, std::string &, std::string &error) {
	GssapiAvailable(error);
	return false;
}

#else

// The subset of the GSSAPI C ABI (RFC 2744) we call. Apple's GSS.framework packs these structs to 2 bytes.
#ifdef __APPLE__
#pragma pack(push, 2)
#endif
typedef uint32_t OM_uint32;
struct gss_buffer_desc_struct {
	size_t length;
	void *value;
};
struct gss_OID_desc_struct {
	OM_uint32 length;
	void *elements;
};
#ifdef __APPLE__
#pragma pack(pop)
#endif
typedef gss_buffer_desc_struct gss_buffer_desc;
typedef gss_buffer_desc *gss_buffer_t;
typedef gss_OID_desc_struct gss_OID_desc;
typedef gss_OID_desc *gss_OID;
typedef void *gss_name_t;
typedef void *gss_ctx_id_t;
typedef void *gss_cred_id_t;
typedef void *gss_channel_bindings_t;

typedef OM_uint32 (*gss_import_name_fn)(OM_uint32 *, gss_buffer_t, gss_OID, gss_name_t *);
typedef OM_uint32 (*gss_init_sec_context_fn)(OM_uint32 *, gss_cred_id_t, gss_ctx_id_t *, gss_name_t, gss_OID, OM_uint32,
                                             OM_uint32, gss_channel_bindings_t, gss_buffer_t, gss_OID *, gss_buffer_t,
                                             OM_uint32 *, OM_uint32 *);
typedef OM_uint32 (*gss_release_buffer_fn)(OM_uint32 *, gss_buffer_t);
typedef OM_uint32 (*gss_release_name_fn)(OM_uint32 *, gss_name_t *);
typedef OM_uint32 (*gss_delete_sec_context_fn)(OM_uint32 *, gss_ctx_id_t *, gss_buffer_t);
typedef OM_uint32 (*gss_display_status_fn)(OM_uint32 *, OM_uint32, int, gss_OID, OM_uint32 *, gss_buffer_t);

static constexpr OM_uint32 GSS_C_MUTUAL_FLAG = 2;
static constexpr int GSS_C_GSS_CODE = 1;
static constexpr int GSS_C_MECH_CODE = 2;

struct GssapiLibrary {
	void *handle = nullptr;
	std::string load_error;
	gss_import_name_fn import_name = nullptr;
	gss_init_sec_context_fn init_sec_context = nullptr;
	gss_release_buffer_fn release_buffer = nullptr;
	gss_release_name_fn release_name = nullptr;
	gss_delete_sec_context_fn delete_sec_context = nullptr;
	gss_display_status_fn display_status = nullptr;
};

static GssapiLibrary &Library() {
	static GssapiLibrary library;
	static std::once_flag once;
	std::call_once(once, [] {
		const char *candidates[] = {
#ifdef __APPLE__
		    "/System/Library/Frameworks/GSS.framework/GSS",
#endif
		    "libgssapi_krb5.so.2", "libgssapi_krb5.so", "libgssapi.so.3", "libgssapi.so"};
		for (auto name : candidates) {
			library.handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
			if (library.handle) {
				break;
			}
		}
		if (!library.handle) {
			library.load_error = "could not load a GSSAPI library (install MIT Kerberos, e.g. libgssapi-krb5-2 or "
			                     "krb5-libs)";
			return;
		}
		library.import_name = reinterpret_cast<gss_import_name_fn>(dlsym(library.handle, "gss_import_name"));
		library.init_sec_context =
		    reinterpret_cast<gss_init_sec_context_fn>(dlsym(library.handle, "gss_init_sec_context"));
		library.release_buffer = reinterpret_cast<gss_release_buffer_fn>(dlsym(library.handle, "gss_release_buffer"));
		library.release_name = reinterpret_cast<gss_release_name_fn>(dlsym(library.handle, "gss_release_name"));
		library.delete_sec_context =
		    reinterpret_cast<gss_delete_sec_context_fn>(dlsym(library.handle, "gss_delete_sec_context"));
		library.display_status = reinterpret_cast<gss_display_status_fn>(dlsym(library.handle, "gss_display_status"));
		if (!library.import_name || !library.init_sec_context || !library.release_buffer || !library.release_name ||
		    !library.delete_sec_context || !library.display_status) {
			library.load_error = "the GSSAPI library is missing required functions";
			library.handle = nullptr;
		}
	});
	return library;
}

bool GssapiAvailable(std::string &error) {
	auto &library = Library();
	if (!library.handle) {
		error = library.load_error;
		return false;
	}
	return true;
}

static std::string DescribeStatus(GssapiLibrary &library, OM_uint32 major, OM_uint32 minor) {
	std::string result;
	for (int pass = 0; pass < 2; pass++) {
		OM_uint32 code = pass == 0 ? major : minor;
		int type = pass == 0 ? GSS_C_GSS_CODE : GSS_C_MECH_CODE;
		if (pass == 1 && minor == 0) {
			break;
		}
		OM_uint32 message_context = 0;
		do {
			OM_uint32 ignored = 0;
			gss_buffer_desc text {0, nullptr};
			if (library.display_status(&ignored, code, type, nullptr, &message_context, &text) != 0) {
				break;
			}
			if (!result.empty()) {
				result += "; ";
			}
			result.append(static_cast<const char *>(text.value), text.length);
			library.release_buffer(&ignored, &text);
		} while (message_context != 0);
	}
	return result.empty() ? "unknown GSSAPI error" : result;
}

bool CreateSpnegoToken(const std::string &service, const std::string &host, std::string &token, std::string &error) {
	auto &library = Library();
	if (!library.handle) {
		error = library.load_error;
		return false;
	}
	// 1.2.840.113554.1.2.1.4 (GSS_C_NT_HOSTBASED_SERVICE) and 1.3.6.1.5.5.2 (SPNEGO)
	static unsigned char hostbased_oid_bytes[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x12, 0x01, 0x02, 0x01, 0x04};
	static unsigned char spnego_oid_bytes[] = {0x2b, 0x06, 0x01, 0x05, 0x05, 0x02};
	gss_OID_desc hostbased_oid {sizeof(hostbased_oid_bytes), hostbased_oid_bytes};
	gss_OID_desc spnego_oid {sizeof(spnego_oid_bytes), spnego_oid_bytes};

	auto principal = service + "@" + host;
	gss_buffer_desc name_buffer {principal.size(), const_cast<char *>(principal.data())};
	OM_uint32 minor = 0;
	gss_name_t target = nullptr;
	auto major = library.import_name(&minor, &name_buffer, &hostbased_oid, &target);
	if (major & 0xffff0000) {
		error = "gss_import_name(" + principal + "): " + DescribeStatus(library, major, minor);
		return false;
	}
	gss_ctx_id_t context = nullptr;
	gss_buffer_desc output {0, nullptr};
	major = library.init_sec_context(&minor, nullptr, &context, target, &spnego_oid, GSS_C_MUTUAL_FLAG, 0, nullptr,
	                                 nullptr, nullptr, &output, nullptr, nullptr);
	bool ok = (major & 0xffff0000) == 0 && output.length > 0;
	if (ok) {
		token = Base64Encode(static_cast<const unsigned char *>(output.value), output.length);
	} else {
		error = "Kerberos authentication for " + principal + " failed: " + DescribeStatus(library, major, minor) +
		        " (is there a valid ticket? run kinit)";
	}
	OM_uint32 ignored = 0;
	if (output.value) {
		library.release_buffer(&ignored, &output);
	}
	if (context) {
		library.delete_sec_context(&ignored, &context, nullptr);
	}
	library.release_name(&ignored, &target);
	return ok;
}

#endif

} // namespace webhdfs
