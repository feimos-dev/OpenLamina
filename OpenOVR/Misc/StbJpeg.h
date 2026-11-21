#pragma once

#include "Misc/Image.h"

#include <optional>
#include <string>

/** A JPEG file's bytes as RGBA8 (stb_image), at most kMaxImageFileSide pixels on a side; nothing (with why) for bytes
 * that aren't a JPEG it can read, or are too big */
std::optional<Image> DecodeJpeg(const std::string& bytes, std::string* why);
