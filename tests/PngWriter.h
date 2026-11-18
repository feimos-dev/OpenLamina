#pragma once
// A PNG of RGBA8 pixels (rows top to bottom), for tests: stored (uncompressed) deflate blocks, so it needs no encoder

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace test_png {

inline uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc = 0xffffffffu)
{
	for (size_t i = 0; i < size; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return crc;
}

inline void Put32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8)
		out.push_back((uint8_t)(v >> s));
}

inline void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
	Put32(out, (uint32_t)data.size());
	size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data.begin(), data.end());
	Put32(out, Crc(&out[start], out.size() - start) ^ 0xffffffffu);
}

inline std::vector<uint8_t> Encode(const uint8_t* rgba, uint32_t width, uint32_t height)
{
	std::vector<uint8_t> raw;
	for (uint32_t y = 0; y < height; y++) {
		raw.push_back(0); // no filter
		raw.insert(raw.end(), rgba + (size_t)y * width * 4, rgba + (size_t)(y + 1) * width * 4);
	}
	std::vector<uint8_t> z = { 0x78, 0x01 };
	for (size_t at = 0; at < raw.size() || at == 0;) {
		size_t n = std::min<size_t>(65535, raw.size() - at);
		bool last = at + n >= raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back((uint8_t)n);
		z.push_back((uint8_t)(n >> 8));
		z.push_back((uint8_t)~n);
		z.push_back((uint8_t)(~n >> 8));
		z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
		at += n;
		if (last)
			break;
	}
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw) {
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	Put32(z, (b << 16) | a);

	std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	std::vector<uint8_t> header;
	Put32(header, width);
	Put32(header, height);
	header.insert(header.end(), { 8, 6, 0, 0, 0 }); // 8-bit RGBA
	Chunk(png, "IHDR", header);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", {});
	return png;
}

} // namespace test_png
