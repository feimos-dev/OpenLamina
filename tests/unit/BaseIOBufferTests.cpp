// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseIOBuffer.h"

TEST_CASE("IOBuffer_property_container_of_a_buffer_that_is_not_open")
{
	BaseIOBuffer b;
	// SteamVR's answer is the number of its InvalidArgument error (102)
	CHECK(b.PropertyContainer(1) == 102);
	CHECK(b.PropertyContainer(0) == 102);
}
