// stb_image (BundledLibs/stb_image.h, public domain), for the JPEG textures of the controller models
#include "stdafx.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wconversion"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "Misc/StbJpeg.h"

#include <climits>
#include <mutex>

std::optional<Image> DecodeJpeg(const std::string& bytes, std::string* why)
{
	auto fail = [&](const char* reason) -> std::optional<Image> {
		if (why)
			*why = reason;
		return std::nullopt;
	};
	if (bytes.size() > (size_t)INT_MAX)
		return fail("it's too big");
	int width = 0, height = 0, channels = 0;
	const auto* data = (const stbi_uc*)bytes.data();
	// Without thread locals stb keeps its failure reason in one global: one decode at a time (model loads run on
	// threads of their own)
	static std::mutex* decodeLock = new std::mutex();
	std::unique_lock<std::mutex> lock(*decodeLock);
	if (!stbi_info_from_memory(data, (int)bytes.size(), &width, &height, &channels))
		return fail("it isn't a JPEG image it can read");
	if (width <= 0 || height <= 0 || (uint32_t)width > kMaxImageFileSide || (uint32_t)height > kMaxImageFileSide)
		return fail("it's bigger than 8192 pixels on a side");
	stbi_uc* pixels = stbi_load_from_memory(data, (int)bytes.size(), &width, &height, &channels, 4);
	lock.unlock();
	if (!pixels)
		return fail("it's damaged");
	Image image;
	image.width = (uint32_t)width;
	image.height = (uint32_t)height;
	try {
		image.rgba.assign(pixels, pixels + (size_t)width * (size_t)height * 4);
	} catch (const std::exception&) {
		stbi_image_free(pixels);
		return fail("it's too big for memory");
	}
	stbi_image_free(pixels);
	return image;
}
