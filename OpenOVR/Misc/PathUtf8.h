#pragma once

#include <filesystem>
#include <string>

/** Text as a path, from UTF-8. On Windows, empty if it isn't UTF-8 (MSVC's conversion throws on it); elsewhere the
 * bytes are the path as they are. */
std::filesystem::path PathFromUtf8(const std::string& text);

/** A path as UTF-8 text, for logs and documents: string() can throw on Windows for characters outside the ANSI code
 * page */
std::string PathToUtf8(const std::filesystem::path& path);
