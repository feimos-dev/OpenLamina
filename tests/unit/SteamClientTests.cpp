// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/SteamClient.h"

TEST_CASE("The Steam client's programs are told apart from games")
{
	CHECK(oc_is_steam_client_program("C:\\Program Files (x86)\\Steam\\steam.exe"));
	CHECK(oc_is_steam_client_program("C:\\Program Files (x86)\\Steam\\Steam.exe"));
	CHECK(oc_is_steam_client_program("C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win7x64\\steamwebhelper.exe"));
	CHECK(oc_is_steam_client_program("D:\\Steam\\STEAMWEBHELPER.EXE"));
	CHECK(oc_is_steam_client_program("steam.exe"));
	CHECK(oc_is_steam_client_program("/home/user/.local/share/Steam/ubuntu12_32/steam"));
	CHECK(oc_is_steam_client_program("/home/user/.local/share/Steam/ubuntu12_64/steamwebhelper"));

	CHECK_FALSE(oc_is_steam_client_program(""));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Half-Life Alyx\\game\\bin\\win64\\hlvr.exe"));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\vrserver.exe"));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Games\\Steam\\steam_game.exe"));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Games\\steamer.exe"));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Steam\\game.exe"));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Games\\steam.exe.bak"));
	CHECK_FALSE(oc_is_steam_client_program("C:\\Steam\\"));
}

TEST_CASE("The test program is not the Steam client")
{
	CHECK_FALSE(oc_is_steam_client_program());
}
