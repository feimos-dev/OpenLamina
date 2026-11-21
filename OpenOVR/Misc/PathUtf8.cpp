#include "stdafx.h"

#include "PathUtf8.h"

namespace fs = std::filesystem;

fs::path PathFromUtf8(const std::string& text)
{
	try {
		return fs::path(std::u8string(text.begin(), text.end()));
	} catch (const std::exception&) {
		return {};
	}
}

std::string PathToUtf8(const fs::path& path)
{
	std::u8string s = path.u8string();
	return std::string(s.begin(), s.end());
}
