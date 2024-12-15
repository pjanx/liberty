// lxdrgen-cpp-qt.cpp: Qt support code for lxdrgen-cpp.awk.
//
// Copyright (c) 2024, Přemysl Eric Janouch <p@janouch.name>
// SPDX-License-Identifier: 0BSD
#include <QString>
#include <string>

namespace LibertyXDR {

bool utf8_to_wstring(const uint8_t *utf8, size_t length, std::wstring &wide) {
	QByteArrayView view(reinterpret_cast<const char *>(utf8), length);
	if (!view.isValidUtf8())
		return false;
	wide = QString::fromUtf8(view).toStdWString();
	return true;
}

bool wstring_to_utf8(const std::wstring &wide, std::string &utf8) {
	utf8 = QString::fromStdWString(wide).toUtf8().toStdString();
	return true;
}

} // namespace LibertyXDR
