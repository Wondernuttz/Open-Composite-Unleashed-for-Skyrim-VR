#pragma once
#include <map>
#include <string>
#include <vector>

class SudoFontMeta {
public:
	SudoFontMeta(std::vector<char> data, std::vector<char> image);
	~SudoFontMeta();

	struct CharInfo {
		// Which character this Character references.
		wchar_t CharacterCode;

		// Location of this character in the packed image.
		short PackedX;
		short PackedY;
		short PackedWidth;
		short PackedHeight;

		// Where to draw this character on the target.
		short XOffset;
		short YOffset;

		// How much to advance your X position after drawing this character.
		// The total amount to advance for each char is ( XAdvance + GetKerning( nextChar ) ).
		short XAdvance;
	};

	struct pix_t {
		uint8_t r, g, b, a;
	};

	/**
	 * Copy a character onto pixel data.
	 *
	 * Does not copy transparent regions, so this respects the background.
	 */
	void Blit(wchar_t ch, int x, int y, int img_width, pix_t targetColour, pix_t* rawPixels, bool hpad = true);

	/**
	 * Copy a character centered within a box.
	 * Centers both horizontally and vertically based on actual glyph dimensions.
	 */
	void BlitCentered(wchar_t ch, int boxX, int boxY, int boxW, int boxH, int img_width, pix_t targetColour, pix_t* rawPixels);

	/**
	 * Draw an entire label centered by its visible glyph bounds. This is shared
	 * by one-character and word labels so F10, Shift, Ctrl, etc. no longer sit
	 * on a different baseline. Supports Keyboard Studio placement and scaling.
	 */
	void BlitTextCentered(const std::wstring& text,
	    int boxX, int boxY, int boxW, int boxH,
	    int img_width, int img_height,
	    float offsetX, float offsetY, float scale,
	    pix_t targetColour, pix_t* rawPixels);

	/**
	 * Draw the two-ring glow used behind a centered label. The old keyboard
	 * renderer called BlitTextCentered once per glow offset, repeating glyph
	 * layout and atlas resampling sixteen times. This path lays the label out
	 * and samples its atlas once, then fans that coverage out to both rings.
	 */
	void BlitTextGlowCentered(const std::wstring& text,
	    int boxX, int boxY, int boxW, int boxH,
	    int img_width, int img_height,
	    float offsetX, float offsetY, float scale,
	    int outerRadius, int innerRadius,
	    pix_t outerColour, pix_t innerColour, pix_t* rawPixels);

	int Width(wchar_t ch);
	int Width(std::wstring str);

	unsigned int GetLineHeight() { return lineHeight; }

private:
	std::map<wchar_t, CharInfo> chars;
	std::vector<uint8_t> pixel_data;

	unsigned int imgWidth;
	unsigned int imgHeight;

	unsigned int lineHeight;
};
