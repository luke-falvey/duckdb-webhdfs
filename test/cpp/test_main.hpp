#pragma once

// Minimal test harness for the cluster-free unit tests (make test_unit).

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace unit {

struct TestCase {
	const char *name;
	std::function<void()> fn;
};

inline std::vector<TestCase> &Registry() {
	static std::vector<TestCase> tests;
	return tests;
}

struct Register {
	Register(const char *name, std::function<void()> fn) {
		Registry().push_back({name, std::move(fn)});
	}
};

struct Failure {
	std::string message;
};

template <class A, class B>
void CheckEqual(const A &actual, const B &expected, const char *expr, const char *file, int line) {
	if (!(actual == expected)) {
		std::ostringstream out;
		out << file << ":" << line << ": " << expr << "\n    actual:   " << actual << "\n    expected: " << expected;
		throw Failure {out.str()};
	}
}

inline void Check(bool ok, const char *expr, const char *file, int line) {
	if (!ok) {
		std::ostringstream out;
		out << file << ":" << line << ": check failed: " << expr;
		throw Failure {out.str()};
	}
}

inline int RunAll() {
	int failed = 0;
	for (auto &test : Registry()) {
		try {
			test.fn();
			std::cout << "ok   " << test.name << "\n";
		} catch (Failure &f) {
			failed++;
			std::cout << "FAIL " << test.name << "\n  " << f.message << "\n";
		} catch (std::exception &e) {
			failed++;
			std::cout << "FAIL " << test.name << "\n  unexpected exception: " << e.what() << "\n";
		}
	}
	std::cout << (Registry().size() - failed) << "/" << Registry().size() << " tests passed\n";
	return failed == 0 ? 0 : 1;
}

} // namespace unit

#define UNIT_CONCAT2(a, b) a##b
#define UNIT_CONCAT(a, b)  UNIT_CONCAT2(a, b)
#define TEST(name)                                                                                                     \
	static void name();                                                                                                \
	static unit::Register UNIT_CONCAT(register_, name)(#name, name);                                                  \
	static void name()
#define CHECK(expr)         unit::Check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)      unit::CheckEqual((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_THROWS(expr)                                                                                             \
	do {                                                                                                               \
		bool threw = false;                                                                                            \
		try {                                                                                                          \
			expr;                                                                                                      \
		} catch (...) {                                                                                                \
			threw = true;                                                                                              \
		}                                                                                                              \
		unit::Check(threw, "expected exception: " #expr, __FILE__, __LINE__);                                         \
	} while (0)
