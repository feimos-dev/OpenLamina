//
// Platform-specific functions
// Created by ZNix on 14/02/2021.
//

#pragma once

typedef void* (*alternativeCoreFactory_t)(const char*, int*);

alternativeCoreFactory_t PlatformGetAlternativeCoreFactory();

// The process is ending with the session still up: the interfaces and the backend are left to it, undestroyed
void oc_abandon_at_exit();
