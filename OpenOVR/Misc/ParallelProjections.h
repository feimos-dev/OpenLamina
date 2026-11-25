#pragma once

#include <string>
#include <string_view>

// Whether a game gets eyes that look straight ahead (Config::ParallelProjections), from the user's choices and the
// games known to need it
namespace parallel_projections {

enum class Choice {
	Automatic,
	On,
	Off,
};

// The name of the game a program is, by its executable's path or file name, if it's one known to need parallel
// projections; empty otherwise
std::string KnownGame(std::string_view exePath);

struct Decision {
	bool on = false;
	// Why, for the log
	std::string reason;
};

// The choice made for this program in the settings app's game list wins, then openlamina.ini's, then the list of
// known games
Decision Decide(Choice perApp, Choice ini, std::string_view exePath);

} // namespace parallel_projections
