#pragma once
// A cube map's six faces as one equirect (lat-long) image, for runtimes without cube layers

#include "Image.h"

#include <array>

// Faces in SetSkyboxOverride's order: front (-Z), back (+Z), left (-X), right (+X), top (+Y), bottom (-Y), each seen
// from the inside with up at the top (the top face's edge at the front is its bottom, the bottom face's its top).
// The result is width x width/2, longitude 0 (the front) in its middle; an empty image if a face is missing or
// width is under 2.
Image CubeFacesToEquirect(const std::array<const Image*, 6>& faces, uint32_t width);
