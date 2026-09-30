#include "stdafx.h"

#include "SudoFontMeta.h"

#include "Misc/lodepng.h"

#include <assert.h>
#include <algorithm>
#include <cmath>
#include <limits>

// The atlas alpha is glyph coverage. Preserve it when tinting so antialiased
// edges stay smooth instead of becoming a jagged binary mask.
static void BlendGlyphPixel(SudoFontMeta::pix_t& destination,
    const SudoFontMeta::pix_t& colour, uint8_t coverage)
{
	const uint32_t sourceAlpha = (uint32_t(colour.a) * coverage + 127u) / 255u;
	if (sourceAlpha == 0)
		return;
	if (sourceAlpha == 255) {
		destination = colour;
		return;
	}

	const uint32_t inverseAlpha = 255u - sourceAlpha;
	const uint32_t destinationAlpha =
	    (uint32_t(destination.a) * inverseAlpha + 127u) / 255u;
	const uint32_t outputAlpha = sourceAlpha + destinationAlpha;
	if (outputAlpha == 0)
		return;

	auto blendChannel = [&](uint8_t source, uint8_t target) -> uint8_t {
		const uint32_t premultiplied =
		    uint32_t(source) * sourceAlpha + uint32_t(target) * destinationAlpha;
		return uint8_t((premultiplied + outputAlpha / 2u) / outputAlpha);
	};

	destination.r = blendChannel(colour.r, destination.r);
	destination.g = blendChannel(colour.g, destination.g);
	destination.b = blendChannel(colour.b, destination.b);
	destination.a = uint8_t(outputAlpha);
}

std::vector<char> readBytes(std::vector<char>& data, size_t len)
{
	assert(len <= data.size());
	std::vector<char> result(data.begin(), data.begin() + len);
	data.erase(data.begin(), data.begin() + len);
	return result;
}

template <typename T>
T readData(std::vector<char>& data)
{
	T val;
	std::vector<char> sVal = readBytes(data, sizeof(val));
	memcpy(&val, sVal.data(), sizeof(val));

	return val;
}

enum SectionID_t : uint16_t {
	ID_FONT_INFO = 0,
	ID_CHARACTERS,
	ID_KERNING,
	ID_CONFIG,

	ID_END = 999,
};

SudoFontMeta::SudoFontMeta(std::vector<char> data, std::vector<char> image)
{
	string headerReq = "\x0bSudoFont1.1";
	std::vector<char> headerVec = readBytes(data, headerReq.length());
	string header(headerVec.data(), headerVec.size());
	assert(header == headerReq);

	unsigned int origTexW, origTexH;

	while (true) {
		auto sectionID = readData<SectionID_t>(data);

		if (sectionID == ID_END)
			break;

		auto sectionSize = readData<uint32_t>(data);

		std::vector<char> sec = readBytes(data, sectionSize);

		if (sectionID == ID_FONT_INFO) {
			lineHeight = readData<uint16_t>(sec);
			origTexW = readData<uint16_t>(sec);
			origTexH = readData<uint16_t>(sec);
		} else if (sectionID == ID_CHARACTERS) {
			auto count = readData<uint16_t>(sec);

			for (size_t i = 0; i < count; i++) {
				CharInfo info = { 0 };

				info.CharacterCode = readData<uint16_t>(sec);

				info.PackedX = readData<uint16_t>(sec);
				info.PackedY = readData<uint16_t>(sec);
				info.PackedWidth = readData<uint16_t>(sec);
				info.PackedHeight = readData<uint16_t>(sec);

				info.XOffset = readData<uint16_t>(sec);
				info.YOffset = readData<uint16_t>(sec);

				info.XAdvance = readData<uint16_t>(sec);

				assert(chars.count(info.CharacterCode) == 0);
				chars[info.CharacterCode] = info;
			}
		}
	}

	// Load the image itself
	lodepng::decode(
	    pixel_data,
	    imgWidth, imgHeight,
	    (const uint8_t*)image.data(), image.size(),
	    LCT_RGBA, 8);

	assert(origTexW == imgWidth);
	assert(origTexH == imgHeight);
}

SudoFontMeta::~SudoFontMeta()
{
}

void SudoFontMeta::Blit(wchar_t ch, int x, int y, int img_width, pix_t targetColour, pix_t* rawPixels, bool hpad)
{
	pix_t* pixels = (pix_t*)rawPixels;
	const pix_t* font = (const pix_t*)pixel_data.data();

	// Safety check: skip characters not in the font to prevent crash
	auto it = chars.find(ch);
	if (it == chars.end())
		return;
	const CharInfo& info = it->second;

	// Per-character offset corrections for font metadata issues
	int xCorrection = 0;
	// The legacy Ubuntu atlas reports an unusually positive j bearing. Do not
	// apply that old correction to the new fonts, whose bearings are already sane.
	if (ch == L'j' && info.XOffset >= 4) xCorrection = -8;

	for (int ix = 0; ix < info.PackedWidth; ix++) {
		for (int iy = 0; iy < info.PackedHeight; iy++) {
			int px = ix + info.PackedX;
			int py = iy + info.PackedY;
			pix_t p = font[px + py * imgWidth];

			if (p.a == 0)
				continue;

			int tx = ix + x + (hpad ? info.XOffset : 0) + xCorrection;
			int ty = iy + y + info.YOffset;

			assert(tx > 0);
			assert(ty > 0);

			size_t idx = tx + ty * img_width;
			assert(idx >= 0);
			pix_t& out = pixels[idx];
			BlendGlyphPixel(out, targetColour, p.a);
		}
	}
}

void SudoFontMeta::BlitCentered(wchar_t ch, int boxX, int boxY, int boxW, int boxH, int img_width, pix_t targetColour, pix_t* rawPixels)
{
	pix_t* pixels = (pix_t*)rawPixels;
	const pix_t* font = (const pix_t*)pixel_data.data();

	// Safety check: skip characters not in the font
	auto it = chars.find(ch);
	if (it == chars.end())
		return;
	const CharInfo& info = it->second;

	// Calculate centered position within the box
	int drawX = boxX + (boxW - info.PackedWidth) / 2;
	int drawY = boxY + (boxH - info.PackedHeight) / 2;

	for (int ix = 0; ix < info.PackedWidth; ix++) {
		for (int iy = 0; iy < info.PackedHeight; iy++) {
			int px = ix + info.PackedX;
			int py = iy + info.PackedY;
			pix_t p = font[px + py * imgWidth];

			if (p.a == 0)
				continue;

			int tx = ix + drawX;
			int ty = iy + drawY;

			// Bounds check
			if (tx < 0 || ty < 0 || tx >= img_width)
				continue;

			size_t idx = tx + ty * img_width;
			pix_t& out = pixels[idx];
			BlendGlyphPixel(out, targetColour, p.a);
		}
	}
}

void SudoFontMeta::BlitTextCentered(const std::wstring& text,
    int boxX, int boxY, int boxW, int boxH,
    int img_width, int img_height,
    float offsetX, float offsetY, float scale,
    pix_t targetColour, pix_t* rawPixels)
{
	if (text.empty())
		return;
	scale = std::clamp(scale, 0.25f, 3.0f);

	struct Run {
		const CharInfo* glyph;
		int cursor;
	};
	std::vector<Run> runs;
	int cursor = 0;
	int minX = std::numeric_limits<int>::max();
	int minY = std::numeric_limits<int>::max();
	int maxX = std::numeric_limits<int>::min();
	int maxY = std::numeric_limits<int>::min();
	for (wchar_t ch : text) {
		auto found = chars.find(ch);
		if (found == chars.end()) {
			cursor += Width(ch);
			continue;
		}
		const CharInfo& glyph = found->second;
		if (glyph.PackedWidth > 0 && glyph.PackedHeight > 0) {
			runs.push_back({ &glyph, cursor });
			minX = std::min(minX, cursor + int(glyph.XOffset));
			minY = std::min(minY, int(glyph.YOffset));
			maxX = std::max(maxX, cursor + int(glyph.XOffset) + int(glyph.PackedWidth));
			maxY = std::max(maxY, int(glyph.YOffset) + int(glyph.PackedHeight));
		}
		cursor += Width(ch);
	}
	if (runs.empty())
		return;

	const float visualWidth = (maxX - minX) * scale;
	const float visualHeight = (maxY - minY) * scale;
	const float originX = boxX + (boxW - visualWidth) * 0.5f + offsetX - minX * scale;
	const float originY = boxY + (boxH - visualHeight) * 0.5f + offsetY - minY * scale;
	const pix_t* atlas = (const pix_t*)pixel_data.data();
	pix_t* output = (pix_t*)rawPixels;

	auto alphaAt = [&](const CharInfo& glyph, int x, int y) -> uint8_t {
		x = std::clamp(x, 0, int(glyph.PackedWidth) - 1);
		y = std::clamp(y, 0, int(glyph.PackedHeight) - 1);
		const int atlasX = int(glyph.PackedX) + x;
		const int atlasY = int(glyph.PackedY) + y;
		if (atlasX < 0 || atlasY < 0 || atlasX >= int(imgWidth) || atlasY >= int(imgHeight))
			return 0;
		return atlas[atlasX + atlasY * imgWidth].a;
	};

	for (const Run& run : runs) {
		const CharInfo& glyph = *run.glyph;
		const int outputWidth = std::max(1, int(std::ceil(glyph.PackedWidth * scale)));
		const int outputHeight = std::max(1, int(std::ceil(glyph.PackedHeight * scale)));
		const int drawX = int(std::round(originX + (run.cursor + glyph.XOffset) * scale));
		const int drawY = int(std::round(originY + glyph.YOffset * scale));

		for (int y = 0; y < outputHeight; ++y) {
			const float sourceY = (y + 0.5f) / scale - 0.5f;
			const int y0 = std::clamp(int(std::floor(sourceY)), 0, int(glyph.PackedHeight) - 1);
			const int y1 = std::min(y0 + 1, int(glyph.PackedHeight) - 1);
			const float fy = std::clamp(sourceY - std::floor(sourceY), 0.0f, 1.0f);
			for (int x = 0; x < outputWidth; ++x) {
				const int targetX = drawX + x;
				const int targetY = drawY + y;
				if (targetX < 0 || targetY < 0 || targetX >= img_width || targetY >= img_height)
					continue;

				const float sourceX = (x + 0.5f) / scale - 0.5f;
				const int x0 = std::clamp(int(std::floor(sourceX)), 0, int(glyph.PackedWidth) - 1);
				const int x1 = std::min(x0 + 1, int(glyph.PackedWidth) - 1);
				const float fx = std::clamp(sourceX - std::floor(sourceX), 0.0f, 1.0f);
				const float top = alphaAt(glyph, x0, y0)
				    + (alphaAt(glyph, x1, y0) - alphaAt(glyph, x0, y0)) * fx;
				const float bottom = alphaAt(glyph, x0, y1)
				    + (alphaAt(glyph, x1, y1) - alphaAt(glyph, x0, y1)) * fx;
				const uint8_t coverage = uint8_t(std::clamp(int(std::round(top + (bottom - top) * fy)), 0, 255));
				BlendGlyphPixel(output[targetX + targetY * img_width], targetColour, coverage);
			}
		}
	}
}

void SudoFontMeta::BlitTextGlowCentered(const std::wstring& text,
    int boxX, int boxY, int boxW, int boxH,
    int img_width, int img_height,
    float offsetX, float offsetY, float scale,
    int outerRadius, int innerRadius,
    pix_t outerColour, pix_t innerColour, pix_t* rawPixels)
{
	if (text.empty() || outerColour.a == 0)
		return;
	scale = std::clamp(scale, 0.25f, 3.0f);
	outerRadius = std::max(1, outerRadius);
	innerRadius = std::max(1, innerRadius);

	struct Run {
		const CharInfo* glyph;
		int cursor;
	};
	std::vector<Run> runs;
	int cursor = 0;
	int minX = std::numeric_limits<int>::max();
	int minY = std::numeric_limits<int>::max();
	int maxX = std::numeric_limits<int>::min();
	int maxY = std::numeric_limits<int>::min();
	for (wchar_t ch : text) {
		auto found = chars.find(ch);
		if (found == chars.end()) {
			cursor += Width(ch);
			continue;
		}
		const CharInfo& glyph = found->second;
		if (glyph.PackedWidth > 0 && glyph.PackedHeight > 0) {
			runs.push_back({ &glyph, cursor });
			minX = std::min(minX, cursor + int(glyph.XOffset));
			minY = std::min(minY, int(glyph.YOffset));
			maxX = std::max(maxX, cursor + int(glyph.XOffset) + int(glyph.PackedWidth));
			maxY = std::max(maxY, int(glyph.YOffset) + int(glyph.PackedHeight));
		}
		cursor += Width(ch);
	}
	if (runs.empty())
		return;

	const float visualWidth = (maxX - minX) * scale;
	const float visualHeight = (maxY - minY) * scale;
	const float originX = boxX + (boxW - visualWidth) * 0.5f + offsetX - minX * scale;
	const float originY = boxY + (boxH - visualHeight) * 0.5f + offsetY - minY * scale;
	const pix_t* atlas = reinterpret_cast<const pix_t*>(pixel_data.data());
	pix_t* output = reinterpret_cast<pix_t*>(rawPixels);

	auto alphaAt = [&](const CharInfo& glyph, int x, int y) -> uint8_t {
		x = std::clamp(x, 0, int(glyph.PackedWidth) - 1);
		y = std::clamp(y, 0, int(glyph.PackedHeight) - 1);
		const int atlasX = int(glyph.PackedX) + x;
		const int atlasY = int(glyph.PackedY) + y;
		if (atlasX < 0 || atlasY < 0 || atlasX >= int(imgWidth) || atlasY >= int(imgHeight))
			return 0;
		return atlas[atlasX + atlasY * imgWidth].a;
	};

	const int ringX[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
	const int ringY[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
	auto stampRing = [&](int targetX, int targetY, int radius,
	                     const pix_t& colour, uint8_t coverage) {
		if (colour.a == 0)
			return;
		for (int index = 0; index < 8; ++index) {
			const int x = targetX + ringX[index] * radius;
			const int y = targetY + ringY[index] * radius;
			if (x < 0 || y < 0 || x >= img_width || y >= img_height)
				continue;
			BlendGlyphPixel(output[x + y * img_width], colour, coverage);
		}
	};

	for (const Run& run : runs) {
		const CharInfo& glyph = *run.glyph;
		const int outputWidth = std::max(1, int(std::ceil(glyph.PackedWidth * scale)));
		const int outputHeight = std::max(1, int(std::ceil(glyph.PackedHeight * scale)));
		const int drawX = int(std::round(originX + (run.cursor + glyph.XOffset) * scale));
		const int drawY = int(std::round(originY + glyph.YOffset * scale));

		for (int y = 0; y < outputHeight; ++y) {
			const float sourceY = (y + 0.5f) / scale - 0.5f;
			const int y0 = std::clamp(int(std::floor(sourceY)), 0, int(glyph.PackedHeight) - 1);
			const int y1 = std::min(y0 + 1, int(glyph.PackedHeight) - 1);
			const float fy = std::clamp(sourceY - std::floor(sourceY), 0.0f, 1.0f);
			for (int x = 0; x < outputWidth; ++x) {
				const float sourceX = (x + 0.5f) / scale - 0.5f;
				const int x0 = std::clamp(int(std::floor(sourceX)), 0, int(glyph.PackedWidth) - 1);
				const int x1 = std::min(x0 + 1, int(glyph.PackedWidth) - 1);
				const float fx = std::clamp(sourceX - std::floor(sourceX), 0.0f, 1.0f);
				const float top = alphaAt(glyph, x0, y0)
				    + (alphaAt(glyph, x1, y0) - alphaAt(glyph, x0, y0)) * fx;
				const float bottom = alphaAt(glyph, x0, y1)
				    + (alphaAt(glyph, x1, y1) - alphaAt(glyph, x0, y1)) * fx;
				const uint8_t coverage = uint8_t(std::clamp(
				    int(std::round(top + (bottom - top) * fy)), 0, 255));
				if (coverage == 0)
					continue;
				const int targetX = drawX + x;
				const int targetY = drawY + y;
				stampRing(targetX, targetY, outerRadius, outerColour, coverage);
				if (innerRadius != outerRadius)
					stampRing(targetX, targetY, innerRadius, innerColour, coverage);
			}
		}
	}
}

int SudoFontMeta::Width(wchar_t ch)
{
	// SFN atlases commonly omit the blank space glyph entirely. Text buffers
	// still contain U+0020, so give it a stable advance derived from the font's
	// line height. This fixes visible word spacing and caret placement without
	// changing the SFN, .kb, or .ocukb formats.
	auto it = chars.find(ch);
	if (it != chars.end() && it->second.XAdvance > 0)
		return it->second.XAdvance;
	if (ch == L' ')
		return std::max(4, int(lineHeight) / 3);
	return 0;
}

int SudoFontMeta::Width(wstring str)
{
	int width = 0;
	for (wchar_t ch : str)
		width += Width(ch);
	return width;
}
