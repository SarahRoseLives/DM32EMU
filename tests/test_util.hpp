// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdio>

namespace test {

inline int check_failed(const char *file, int line, const char *expr)
{
	std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
	return 1;
}

inline int check_eq_failed(const char *file, int line, const char *aexpr, long long a, const char *bexpr, long long b)
{
	std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", file, line, aexpr, a, bexpr, b);
	return 1;
}

} // namespace test

#define CHECK(expr) \
	do { \
		if (!(expr)) return test::check_failed(__FILE__, __LINE__, #expr); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		const long long _a = (long long)(a); \
		const long long _b = (long long)(b); \
		if (_a != _b) return test::check_eq_failed(__FILE__, __LINE__, #a, _a, #b, _b); \
	} while (0)
