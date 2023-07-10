// lxdrgen-cpp-win32.cpp: Win32 support code for lxdrgen-cpp.awk.
//
// Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
// SPDX-License-Identifier: 0BSD
#include <windows.h>

#include <climits>
#include <cstdint>
#include <string>

namespace LibertyXDR {

bool utf8_to_wstring(const uint8_t *utf8, size_t length, std::wstring &wide) {
	wide.clear();
	if (!length)
		return true;
	if (length > INT_MAX)
		return false;

	int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		(LPCCH) utf8, length, nullptr, 0);
	if (size <= 0)
		return false;

	wide.resize(size);
	return !!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		(LPCCH) utf8, length, wide.data(), size);
}

bool wstring_to_utf8(const std::wstring &wide, std::string &utf8) {
	utf8.clear();
	if (wide.empty())
		return true;
	if (wide.size() > INT_MAX)
		return false;

	int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		(LPCWCH) wide.data(), wide.size(), nullptr, 0, NULL, NULL);
	if (size <= 0)
		return false;

	utf8.resize(size);
	return !!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		(LPCWCH) wide.data(), wide.size(), utf8.data(), size, NULL, NULL);
}

} // namespace LibertyXDR
