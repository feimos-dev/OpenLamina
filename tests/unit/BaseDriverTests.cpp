// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseControlPanel.h"
#include "Reimpl/BaseDriverManager.h"

TEST_CASE("Control_panel_lists_the_driver_manager_s_driver")
{
	BaseDriverManager manager;
	BaseControlPanel panel;
	CHECK(panel.GetDriverCount() == manager.GetDriverCount());

	char fromManager[64] = {};
	char fromPanel[64] = {};
	const uint32_t size = manager.GetDriverName(0, fromManager, sizeof(fromManager));
	CHECK(panel.GetDriverId(0, fromPanel, sizeof(fromPanel)) == size);
	CHECK(std::string(fromPanel) == fromManager);

	// Too tooShort a buffer: the size needed, nothing copied
	char tooShort[2] = { 'x', 'x' };
	CHECK(panel.GetDriverId(0, tooShort, 1) == size);
	CHECK(tooShort[0] == '\0');

	// No such driver
	CHECK(panel.GetDriverId(1, fromPanel, sizeof(fromPanel)) == 0);
	CHECK(fromPanel[0] == '\0');
}
