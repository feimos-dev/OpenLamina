#include "stdafx.h"

#include "ParallelProjections.h"

#include <algorithm>
#include <cctype>

namespace parallel_projections {

namespace {
	struct KnownGameEntry {
		const char* exe;
		const char* name;
	};

	const KnownGameEntry knownGames[] = {
#include "ParallelProjectionGames.inc"
	};

	bool SameIgnoringCase(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
		});
	}
} // namespace

std::string KnownGame(std::string_view exePath)
{
	size_t slash = exePath.find_last_of("/\\");
	std::string_view file = slash == std::string_view::npos ? exePath : exePath.substr(slash + 1);
	for (const KnownGameEntry& game : knownGames) {
		if (SameIgnoringCase(file, game.exe))
			return game.name;
	}
	return {};
}

Decision Decide(Choice perApp, Choice ini, std::string_view exePath)
{
	if (perApp != Choice::Automatic)
		return { perApp == Choice::On, perApp == Choice::On ? "turned on for this game in OpenLamina's game list" : "turned off for this game in OpenLamina's game list" };
	if (ini != Choice::Automatic)
		return { ini == Choice::On, ini == Choice::On ? "turned on in openlamina.ini" : "turned off in openlamina.ini" };
	std::string game = KnownGame(exePath);
	if (!game.empty())
		return { true, game + " is a game known to need it" };
	return { false, "not a game known to need it" };
}

} // namespace parallel_projections
