#include "Equirect.h"

#include <cmath>
#include <vector>

namespace {
enum Face { Front, Back, Left, Right, Top, Bottom };

constexpr double kPi = 3.14159265358979323846;

// The face a direction (OpenXR axes: -Z forward, +Y up, +X right) points at, and where on it (0..1, from the top-left)
Face FaceOf(double x, double y, double z, double& u, double& v)
{
	double ax = std::abs(x), ay = std::abs(y), az = std::abs(z);
	if (az >= ax && az >= ay) {
		u = ((z < 0 ? x : -x) / az + 1) / 2;
		v = (-y / az + 1) / 2;
		return z < 0 ? Front : Back;
	}
	if (ax >= ay) {
		u = ((x > 0 ? z : -z) / ax + 1) / 2;
		v = (-y / ax + 1) / 2;
		return x > 0 ? Right : Left;
	}
	u = (x / ay + 1) / 2;
	v = ((y > 0 ? -z : z) / ay + 1) / 2;
	return y > 0 ? Top : Bottom;
}
} // namespace

Image CubeFacesToEquirect(const std::array<const Image*, 6>& faces, uint32_t width)
{
	Image out;
	if (width < 2)
		return out;
	for (const Image* face : faces) {
		if (!face || face->Empty())
			return out;
	}
	out.width = width;
	out.height = width / 2;
	out.rgba.resize((size_t)out.width * out.height * 4);
	std::vector<double> sinLon(out.width), cosLon(out.width);
	for (uint32_t px = 0; px < out.width; px++) {
		double lon = (px + 0.5) / out.width * 2 * kPi - kPi;
		sinLon[px] = std::sin(lon);
		cosLon[px] = std::cos(lon);
	}
	for (uint32_t py = 0; py < out.height; py++) {
		double lat = kPi / 2 - (py + 0.5) / out.height * kPi;
		double sinLat = std::sin(lat), cosLat = std::cos(lat);
		for (uint32_t px = 0; px < out.width; px++) {
			double u, v;
			Face f = FaceOf(cosLat * sinLon[px], sinLat, -cosLat * cosLon[px], u, v);
			const Image& face = *faces[f];
			uint32_t fx = (uint32_t)std::min<double>(face.width - 1, u * face.width);
			uint32_t fy = (uint32_t)std::min<double>(face.height - 1, v * face.height);
			const uint8_t* src = &face.rgba[((size_t)fy * face.width + fx) * 4];
			uint8_t* dst = &out.rgba[((size_t)py * out.width + px) * 4];
			for (int c = 0; c < 4; c++)
				dst[c] = src[c];
		}
	}
	return out;
}
