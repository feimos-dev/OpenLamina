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

// Each buffer is sized first and written at offsets: GCC's -Wstringop-overflow takes growing vectors for overflows
// when the standard library's assertions are on (-D_GLIBCXX_ASSERTIONS)
inline void Set32(uint8_t* at, uint32_t v)
{
	at[0] = (uint8_t)(v >> 24);
	at[1] = (uint8_t)(v >> 16);
	at[2] = (uint8_t)(v >> 8);
	at[3] = (uint8_t)v;
}

// Appends a chunk (length, type, data, CRC) to out
inline void Chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t size)
{
	size_t start = out.size();
	out.resize(start + 12 + size);
	uint8_t* p = out.data() + start;
	Set32(p, (uint32_t)size);
	memcpy(p + 4, type, 4);
	if (size)
		memcpy(p + 8, data, size);
	Set32(p + 8 + size, Crc(p + 4, 4 + size) ^ 0xffffffffu);
}

inline std::vector<uint8_t> Encode(const uint8_t* rgba, uint32_t width, uint32_t height)
{
	const size_t row = (size_t)width * 4;
	std::vector<uint8_t> raw((row + 1) * height);
	for (uint32_t y = 0; y < height; y++) {
		raw[y * (row + 1)] = 0; // no filter
		if (row)
			memcpy(raw.data() + y * (row + 1) + 1, rgba + y * row, row);
	}
	// Stored blocks of at most 65535 bytes (one, empty, for no data), then the Adler-32
	const size_t blocks = std::max<size_t>(1, (raw.size() + 65534) / 65535);
	std::vector<uint8_t> z(2 + blocks * 5 + raw.size() + 4);
	z[0] = 0x78;
	z[1] = 0x01;
	size_t w = 2;
	for (size_t at = 0, i = 0; i < blocks; i++) {
		size_t n = std::min<size_t>(65535, raw.size() - at);
		z[w++] = i + 1 == blocks ? 1 : 0;
		z[w++] = (uint8_t)n;
		z[w++] = (uint8_t)(n >> 8);
		z[w++] = (uint8_t)~n;
		z[w++] = (uint8_t)(~n >> 8);
		if (n)
			memcpy(z.data() + w, raw.data() + at, n);
		w += n;
		at += n;
	}
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw) {
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	Set32(z.data() + w, (b << 16) | a);

	static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	std::vector<uint8_t> png(signature, signature + 8);
	uint8_t header[13] = {};
	Set32(header, width);
	Set32(header + 4, height);
	header[8] = 8; // 8-bit RGBA
	header[9] = 6;
	Chunk(png, "IHDR", header, sizeof(header));
	Chunk(png, "IDAT", z.data(), z.size());
	Chunk(png, "IEND", nullptr, 0);
	return png;
}

} // namespace test_png
