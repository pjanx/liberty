// lxdrgen-cpp-posix.cpp: POSIX support code for lxdrgen-cpp.awk.
//
// Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
// SPDX-License-Identifier: 0BSD
#include <iconv.h>

#include <cstdint>
#include <string>

// Various BSD derivatives may have a problem here.
// Linux defines __STDC_ISO_10646__, but also supports "WCHAR_T".
#ifdef APPLE
#define ICONV_WCHAR "UTF-32"
#else
#define ICONV_WCHAR "WCHAR_T"
#endif

namespace LibertyXDR {

bool utf8_to_wstring(const uint8_t *utf8, size_t length, std::wstring &wide) {
	iconv_t conv = iconv_open(ICONV_WCHAR, "UTF-8");
	if (conv == (iconv_t) -1)
		return false;

	wchar_t buffer[1024] = {};
	char *start = (char *) buffer, *out = start, *in = (char *) utf8;
	size_t available = sizeof buffer;
	wide.clear();
	while (iconv(conv, &in, &length, &out, &available) == (size_t) -1) {
		if (errno != E2BIG) {
			iconv_close(conv);
			return false;
		}

		wide.append(buffer, (out - start) / sizeof *buffer);
		out = start;
		available = sizeof buffer;
	}
	wide.append(buffer, (out - start) / sizeof *buffer);
	iconv_close(conv);
	return true;
}

bool wstring_to_utf8(const std::wstring &wide, std::string &utf8) {
	iconv_t conv = iconv_open("UTF-8", ICONV_WCHAR);
	if (conv == (iconv_t) -1)
		return false;

	char buffer[1024] = {}, *out = buffer, *in = (char *) wide.data();
	size_t available = sizeof buffer, length = wide.size() * sizeof wide[0];
	utf8.clear();
	while (iconv(conv, &in, &length, &out, &available) == (size_t) -1) {
		if (errno != E2BIG) {
			iconv_close(conv);
			return false;
		}

		utf8.append(buffer, out - buffer);
		out = buffer;
		available = sizeof buffer;
	}
	utf8.append(buffer, out - buffer);
	iconv_close(conv);
	return true;
}

} // namespace LibertyXDR
