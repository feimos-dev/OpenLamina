#pragma once
#include "generated/interfaces/vrtypes.h"

#if defined(BASE_IMPL) || defined(GENFILE)
#include "logging.h"
#endif

// For shared pointers in the generated GVRxyz.gen.h headers
#include <memory>

class CVRCommon {
public:
	// Ensure the resources of the base get freed properly
	virtual ~CVRCommon() {}

	virtual void** _GetStatFuncList() = 0;

	// Given an object which implements CVRCommon as it's not-first interface, the compiler (or at least GCC) can't
	// figure out where the real vtable is with our InheritCVRLayout. It can do virtual function calls just fine for
	// whatever reasons, so use that to implement a deletor.
	// **WARNING**: You are left with a dangling pointer after calling this, it does what it says on the tin.
	virtual void Delete() = 0;
};

#if defined(GENFILE)
#define BASE_FLAG(...)
#define GEN_INTERFACE(if_name, version, ...)
#endif
