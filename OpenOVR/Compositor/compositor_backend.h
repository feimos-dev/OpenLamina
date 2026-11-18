#pragma once

#include "compositor.h"

// Include compositor backends as indicated by compiler flags

#ifdef SUPPORT_DX
// Direct3D 10 textures go through the Direct3D 11 path
#ifdef SUPPORT_DX10
#include "dx10compositor.h"
#endif
#ifdef SUPPORT_DX11
#include "dx11compositor.h"
#endif
#ifdef SUPPORT_DX12
#include "dx12compositor.h"
#endif
#endif
#ifdef SUPPORT_VK
#include "vkcompositor.h"
#endif
#if defined(SUPPORT_GL) || defined(SUPPORT_GLES)
#include "glcompositor.h"
#endif
#ifdef SUPPORT_GLES
#include "glescompositor.h"
#endif
