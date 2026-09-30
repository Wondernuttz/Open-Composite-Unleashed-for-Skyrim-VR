#include "stdafx.h"

#include "KeyboardLayout.h"
#include "VRKeyboard.h"

#include <algorithm>
#include <cwctype>

static bool parseBoolToken(const wstring& value)
{
	return value == L"true" || value == L"1" || value == L"yes";
}

static void parseColorToken(const wstring& value, uint8_t (&rgba)[4])
{
	wstring hex = value;
	if (!hex.empty() && hex[0] == L'#')
		hex.erase(0, 1);
	if (hex.length() != 6 && hex.length() != 8)
		OOVR_ABORT("Keyboard layout: color must be #RRGGBB or #RRGGBBAA");
	try {
		unsigned long packed = std::stoul(hex, nullptr, 16);
		if (hex.length() == 6) {
			rgba[0] = uint8_t((packed >> 16) & 0xFF);
			rgba[1] = uint8_t((packed >> 8) & 0xFF);
			rgba[2] = uint8_t(packed & 0xFF);
			rgba[3] = 255;
		} else {
			rgba[0] = uint8_t((packed >> 24) & 0xFF);
			rgba[1] = uint8_t((packed >> 16) & 0xFF);
			rgba[2] = uint8_t((packed >> 8) & 0xFF);
			rgba[3] = uint8_t(packed & 0xFF);
		}
	} catch (...) {
		OOVR_ABORT("Keyboard layout: invalid color value");
	}
}

static void wstrim(wstring& s)
{
	s.erase(s.begin(), find_if(s.begin(), s.end(), [](wchar_t ch) {
		return !std::iswspace(ch);
	}));

	s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t ch) {
		return !std::iswspace(ch);
	}).base(),
	    s.end());
}

static wstring pullstring(wstring& in)
{
	// Keyboard Studio quotes labels so spaces and an intentionally empty label
	// survive a save/load round trip. Legacy unquoted layout tokens still parse
	// exactly as before.
	if (!in.empty() && in[0] == L'"') {
		wstring word;
		bool escaped = false;
		size_t pos = 1;
		for (; pos < in.length(); ++pos) {
			wchar_t ch = in[pos];
			if (escaped) {
				word.push_back(ch);
				escaped = false;
				continue;
			}
			if (ch == L'\\' && pos + 1 < in.length()
			    && (in[pos + 1] == L'\\' || in[pos + 1] == L'"')) {
				escaped = true;
				continue;
			}
			if (ch == L'"') {
				++pos;
				break;
			}
			word.push_back(ch);
		}

		while (pos < in.length() && std::iswspace(in[pos]))
			++pos;
		in.erase(0, pos);
		return word;
	}

	size_t wordlen = 0;

	while (wordlen < in.length() && !std::iswspace(in[wordlen])) {
		wordlen++;
	}

	wstring word = in.substr(0, wordlen);

	// Chop the spaces off the remander
	size_t spacelen = wordlen;
	while (spacelen < in.length() && std::iswspace(in[spacelen])) {
		spacelen++;
	}

	in.erase(0, spacelen);

	return word;
}

static wchar_t escapeChar(const wstring& str)
{
	if (str[0] != '\\')
		return str[0];

	wchar_t esc = str[1];

	switch (esc) {
	case 't':
		return '\t';
	case '\\':
		return '\\';
	case 'n':
		return '\n';
	case 'b':
		return '\b'; // Backspace
	case 's':
		return ' '; // For whitespace-deliminated stuff
	case 'z':
		return '\x01'; // Shift
	case 'c':
		return '\x02'; // Caps lock
	case 'q':
		return '\x03'; // Done
	case 'U':
		return '\x04'; // Arrow Up
	case 'D':
		return '\x05'; // Arrow Down
	case 'L':
		return '\x06'; // Arrow Left
	case 'R':
		return '\x07'; // Arrow Right
	case 'e':
		return '\x0E'; // Escape
	case 'm':
		return '\x0F'; // Mouse click (console crosshair)
	case 'v':
		return '\x1C'; // Target mode toggle
	case 'E':
		return '\x1D'; // End key
	case 'C':
		return '\x1E'; // Ctrl modifier
	case 'P':
		return '\x1F'; // Print Screen
	case 'o':
		return L'"'; // Double quote (safe in Keyboard Studio token output)
	case 'f':
		// F1-F12 keys: \f1 through \f12
		// str format: "\f1", "\f2", ..., "\f10", "\f11", "\f12"
		// str[0] = '\', str[1] = 'f', str[2] = first digit, str[3] = second digit (if any)
		if (str.length() >= 3) {
			// Parse the number after '\f'
			int fNum = 0;
			if (str[2] >= '1' && str[2] <= '9') {
				fNum = str[2] - '0';
				// Check for two-digit F-keys (F10, F11, F12)
				if (str.length() >= 4 && str[3] >= '0' && str[3] <= '2') {
					fNum = fNum * 10 + (str[3] - '0');
				}
			}
			if (fNum >= 1 && fNum <= 12) {
				return '\x10' + (fNum - 1); // F1=0x10, F2=0x11, ..., F12=0x1B
			}
		}
		break;
	}

	string utf = VRKeyboard::CHAR_CONV.to_bytes(str);

	string msg = "Unknown escape sequence '" + utf + "'";
	OOVR_ABORT(msg.c_str());
}

KeyboardLayout::KeyboardLayout(std::vector<char> data)
{
	wstring contents = VRKeyboard::CHAR_CONV.from_bytes(string(data.data(), data.size()));

	Key* last = nullptr;

	auto addKey = [&](wchar_t ch, wchar_t shift, float x, float y) -> Key& {
		int id = (int)keys.size();

		keys.push_back(Key());
		Key& key = keys.back();
		last = &key;

		key = { 0 };

		key.id = id;

		key.ch = ch;
		key.shift = shift;

		key.x = x;
		key.y = y;

		key.label = ch;
		key.labelShift = shift;

		key.w = 1;
		key.h = 1;
		key.labelOffsetX = 0;
		key.labelOffsetY = 0;
		key.labelScale = 1;

		return key;
	};

	auto addNextKey = [&](wchar_t ch, wchar_t shift) {
		if (!last)
			OOVR_ABORT("Cannot add bank or unpositioned key as first key!");

		addKey(ch, shift, last->x + last->w, last->y);
	};

	while (!contents.empty()) {
		size_t newlinePos = contents.find_first_of('\n');
		wstring line = contents.substr(0, newlinePos);
		contents.erase(0, newlinePos + 1);

		wstrim(line);
		if (line.empty())
			continue;

		// Keyboard Studio uses namespaced comment metadata for optional visual
		// additions. Older runtimes safely ignore these lines as comments.
		if (line[0] == '#') {
			wstring metadata = line;
			pullstring(metadata); // '#'
			wstring metadataOp = pullstring(metadata);
			if (metadataOp == L"ocu_input_fill_color") {
				parseColorToken(pullstring(metadata), visualStyle.inputFillColor);
				visualStyle.inputFillOverride = true;
			} else if (metadataOp == L"ocu_input_outline_enabled") {
				const wstring value = pullstring(metadata);
				visualStyle.inputOutlineVisible = value == L"true" || value == L"1" || value == L"yes";
			} else if (metadataOp == L"ocu_input_outline_color") {
				parseColorToken(pullstring(metadata), visualStyle.inputOutlineColor);
				visualStyle.inputOutlineOverride = true;
			} else if (metadataOp == L"ocu_input_outline_width") {
				visualStyle.inputOutlineWidth = std::clamp(std::stoi(pullstring(metadata)), 0, 8);
				visualStyle.inputOutlineOverride = true;
			} else if (metadataOp == L"ocu_input_title_offset") {
				visualStyle.inputTitleOffsetX = std::clamp(std::stof(pullstring(metadata)), -2048.0f, 2048.0f);
				visualStyle.inputTitleOffsetY = std::clamp(std::stof(pullstring(metadata)), -240.0f, 240.0f);
			} else if (metadataOp == L"ocu_input_text_offset") {
				visualStyle.inputTextOffsetX = std::clamp(std::stof(pullstring(metadata)), -2048.0f, 2048.0f);
				visualStyle.inputTextOffsetY = std::clamp(std::stof(pullstring(metadata)), -240.0f, 240.0f);
			} else if (metadataOp == L"ocu_console_input_background") {
				consoleInputBackgroundFile = VRKeyboard::CHAR_CONV.to_bytes(pullstring(metadata));
			} else if (metadataOp == L"ocu_top_state_art") {
				const wstring slot = pullstring(metadata);
				const std::string file = VRKeyboard::CHAR_CONV.to_bytes(pullstring(metadata));
				if (slot == L"mode_vr") modeVrArtworkFile = file;
				else if (slot == L"mode_pc") modePcArtworkFile = file;
				else if (slot == L"lock_world") lockWorldArtworkFile = file;
				else if (slot == L"lock_head") lockHeadArtworkFile = file;
			} else if (metadataOp == L"ocu_top_state_text") {
				const wstring slot = pullstring(metadata);
				const wstring value = pullstring(metadata);
				const bool enabled = value == L"true" || value == L"1" || value == L"yes";
				if (slot == L"mode") modeTextOverArtwork = enabled;
				else if (slot == L"lock") lockTextOverArtwork = enabled;
			} else if (metadataOp == L"ocu_top_state_art_design") {
				const wstring slot = pullstring(metadata);
				const float x = std::clamp(std::stof(pullstring(metadata)), -2048.0f, 2048.0f);
				const float y = std::clamp(std::stof(pullstring(metadata)), -1120.0f, 1120.0f);
				const float width = std::clamp(std::stof(pullstring(metadata)), 0.0f, 2048.0f);
				const float height = std::clamp(std::stof(pullstring(metadata)), 0.0f, 1120.0f);
				if (slot == L"mode") {
					modeArtworkOffsetX = x; modeArtworkOffsetY = y;
					modeArtworkWidth = width; modeArtworkHeight = height;
				} else if (slot == L"lock") {
					lockArtworkOffsetX = x; lockArtworkOffsetY = y;
					lockArtworkWidth = width; lockArtworkHeight = height;
				}
			} else if (metadataOp == L"ocu_top_state_text_offset") {
				const wstring slot = pullstring(metadata);
				const float x = std::clamp(std::stof(pullstring(metadata)), -2048.0f, 2048.0f);
				const float y = std::clamp(std::stof(pullstring(metadata)), -1120.0f, 1120.0f);
				if (slot == L"mode") {
					modeTextOffsetX = x; modeTextOffsetY = y;
				} else if (slot == L"lock") {
					lockTextOffsetX = x; lockTextOffsetY = y;
				}
			}
			continue;
		}

		wstring op = pullstring(line);

		if (op == L"bank") {
			wstring chars = pullstring(line);
			wstring shift = pullstring(line);

			if (chars.length() != shift.length())
				OOVR_ABORTF("Keyboard layout: bank lower/upper length mismatch: '%ls' vs '%ls'", chars.c_str(), shift.c_str());

			for (int i = 0; i < chars.length(); i++) {
				addNextKey(chars[i], shift[i]);
			}
			continue;
		}

		if (op == L"width") {
			width = std::stoi(pullstring(line));
			continue;
		}
		if (op == L"base_theme") {
			baseTheme = VRKeyboard::CHAR_CONV.to_bytes(pullstring(line));
			continue;
		}
		if (op == L"font") {
			fontName = VRKeyboard::CHAR_CONV.to_bytes(pullstring(line));
			continue;
		}

		// Optional layout-wide visual settings authored by Keyboard Studio.
		// They are deliberately independent of opencomposite.ini so a keyboard
		// can ship as its own MO2 Root Builder mod without replacing user config.
		if (op == L"style") {
			visualStyle.enabled = pullstring(line) == L"custom";
			continue;
		}
		if (op == L"key_plates") {
			visualStyle.keyPlatesEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"top_button_plates") {
			visualStyle.topButtonPlatesEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"input_bar_plate") {
			visualStyle.inputBarPlateEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"parchment_ribbon") {
			visualStyle.parchmentRibbonEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"font_color") {
			parseColorToken(pullstring(line), visualStyle.fontColor);
			continue;
		}
		if (op == L"font_outline_color") {
			parseColorToken(pullstring(line), visualStyle.fontOutlineColor);
			continue;
		}
		if (op == L"font_glow_color") {
			parseColorToken(pullstring(line), visualStyle.fontGlowColor);
			continue;
		}
		if (op == L"font_glow_enabled") {
			visualStyle.fontGlowEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"font_glow_strength") {
			visualStyle.fontGlowStrength = std::clamp(std::stoi(pullstring(line)), 0, 100);
			continue;
		}
		if (op == L"font_glow_radius") {
			visualStyle.fontGlowRadius = std::clamp(std::stoi(pullstring(line)), 1, 8);
			continue;
		}
		if (op == L"font_breathe") {
			visualStyle.fontBreatheEnabled = parseBoolToken(pullstring(line));
			wstring minimum = pullstring(line);
			wstring period = pullstring(line);
			wstring phase = pullstring(line);
			visualStyle.fontBreatheMinPercent = minimum.empty() ? 35 : std::clamp(std::stoi(minimum), 0, 100);
			visualStyle.fontBreathePeriodSeconds = period.empty() ? 2.0f : std::clamp(std::stof(period), 0.5f, 10.0f);
			visualStyle.fontBreathePhaseDegrees = phase.empty() ? 0.0f : std::stof(phase);
			continue;
		}
		if (op == L"key_color") {
			parseColorToken(pullstring(line), visualStyle.keyColor);
			continue;
		}
		if (op == L"plate_fill_color") {
			parseColorToken(pullstring(line), visualStyle.plateFillColor);
			continue;
		}
		if (op == L"plate_outline_width") {
			visualStyle.plateOutlineWidth = std::clamp(std::stoi(pullstring(line)), 0, 8);
			continue;
		}
		if (op == L"glow_color") {
			parseColorToken(pullstring(line), visualStyle.glowColor);
			continue;
		}
		if (op == L"hover_color") {
			parseColorToken(pullstring(line), visualStyle.hoverColor);
			continue;
		}
		if (op == L"glow_enabled") {
			visualStyle.glowEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"glow_strength") {
			visualStyle.glowStrength = std::clamp(std::stoi(pullstring(line)), 0, 100);
			continue;
		}
		if (op == L"glow_radius") {
			visualStyle.glowRadius = std::clamp(std::stoi(pullstring(line)), 1, 8);
			continue;
		}
		if (op == L"hover_enabled") {
			visualStyle.hoverEnabled = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"hover_strength") {
			visualStyle.hoverStrength = std::clamp(std::stoi(pullstring(line)), 0, 100);
			continue;
		}
		if (op == L"label_outline") {
			visualStyle.labelOutline = parseBoolToken(pullstring(line));
			continue;
		}
		if (op == L"key_roundness") {
			visualStyle.keyRoundness = std::clamp(std::stoi(pullstring(line)), 0, 24);
			continue;
		}
		if (op == L"key_breathe") {
			visualStyle.keyBreatheEnabled = parseBoolToken(pullstring(line));
			wstring minimum = pullstring(line);
			wstring period = pullstring(line);
			wstring phase = pullstring(line);
			visualStyle.keyBreatheMinPercent = minimum.empty() ? 35 : std::clamp(std::stoi(minimum), 0, 100);
			visualStyle.keyBreathePeriodSeconds = period.empty() ? 2.0f : std::clamp(std::stof(period), 0.5f, 10.0f);
			visualStyle.keyBreathePhaseDegrees = phase.empty() ? 0.0f : std::stof(phase);
			continue;
		}
		if (op == L"background") {
			background.file = VRKeyboard::CHAR_CONV.to_bytes(pullstring(line));
			continue;
		}
		if (op == L"background_rect") {
			background.x = std::stof(pullstring(line));
			background.y = std::stof(pullstring(line));
			background.width = std::max(1.0f, std::stof(pullstring(line)));
			background.height = std::max(1.0f, std::stof(pullstring(line)));
			wstring opacity = pullstring(line);
			wstring fade = pullstring(line);
			wstring rotation = pullstring(line);
			wstring breathe = pullstring(line);
			wstring breatheMinimum = pullstring(line);
			wstring breathePeriod = pullstring(line);
			wstring breathePhase = pullstring(line);
			wstring roundness = pullstring(line);
			background.opacity = opacity.empty() ? 100 : std::clamp(std::stoi(opacity), 0, 100);
			background.edgeFade = fade.empty() ? 0 : std::clamp(std::stoi(fade), 0, 300);
			background.rotation = rotation.empty() ? 0.0f : std::stof(rotation);
			background.breatheEnabled = !breathe.empty() && parseBoolToken(breathe);
			background.breatheMinPercent = breatheMinimum.empty() ? 35 : std::clamp(std::stoi(breatheMinimum), 0, 100);
			background.breathePeriodSeconds = breathePeriod.empty() ? 2.0f : std::clamp(std::stof(breathePeriod), 0.5f, 10.0f);
			background.breathePhaseDegrees = breathePhase.empty() ? 0.0f : std::stof(breathePhase);
			background.roundness = roundness.empty() ? 0 : std::clamp(std::stoi(roundness), 0, 100);
			continue;
		}
		if (op == L"sprite") {
			ImageLayer sprite;
			sprite.file = VRKeyboard::CHAR_CONV.to_bytes(pullstring(line));
			sprite.x = std::stof(pullstring(line));
			sprite.y = std::stof(pullstring(line));
			sprite.width = std::max(1.0f, std::stof(pullstring(line)));
			sprite.height = std::max(1.0f, std::stof(pullstring(line)));
			wstring opacity = pullstring(line);
			wstring fade = pullstring(line);
			wstring rotation = pullstring(line);
			wstring breathe = pullstring(line);
			wstring breatheMinimum = pullstring(line);
			wstring breathePeriod = pullstring(line);
			wstring breathePhase = pullstring(line);
			wstring glowEnabled = pullstring(line);
			wstring glowColor = pullstring(line);
			wstring glowStrength = pullstring(line);
			wstring glowRadius = pullstring(line);
			sprite.opacity = opacity.empty() ? 100 : std::clamp(std::stoi(opacity), 0, 100);
			sprite.edgeFade = fade.empty() ? 0 : std::clamp(std::stoi(fade), 0, 300);
			sprite.rotation = rotation.empty() ? 0.0f : std::stof(rotation);
			sprite.breatheEnabled = !breathe.empty() && parseBoolToken(breathe);
			sprite.breatheMinPercent = breatheMinimum.empty() ? 35 : std::clamp(std::stoi(breatheMinimum), 0, 100);
			sprite.breathePeriodSeconds = breathePeriod.empty() ? 2.0f : std::clamp(std::stof(breathePeriod), 0.5f, 10.0f);
			sprite.breathePhaseDegrees = breathePhase.empty() ? 0.0f : std::stof(breathePhase);
			sprite.glowEnabled = glowEnabled.empty() ? sprite.breatheEnabled : parseBoolToken(glowEnabled);
			if (!glowColor.empty())
				parseColorToken(glowColor, sprite.glowColor);
			sprite.glowStrength = glowStrength.empty() ? 55 : std::clamp(std::stoi(glowStrength), 0, 100);
			sprite.glowRadius = glowRadius.empty() ? 12 : std::clamp(std::stoi(glowRadius), 1, 48);
			sprites.push_back(std::move(sprite));
			continue;
		}
		if (op == L"control_offset") {
			wstring control = pullstring(line);
			float x = std::stof(pullstring(line));
			float y = std::stof(pullstring(line));
			if (control == L"size") {
				sizeControlOffsetX = x;
				sizeControlOffsetY = y;
			} else if (control == L"opacity") {
				opacityControlOffsetX = x;
				opacityControlOffsetY = y;
			} else if (control == L"tilt") {
				tiltControlOffsetX = x;
				tiltControlOffsetY = y;
			}
			continue;
		}
		if (op == L"top_offset") {
			wstring element = pullstring(line);
			wstring xToken = pullstring(line);
			wstring yToken = pullstring(line);
			if (xToken.empty() || yToken.empty())
				OOVR_ABORT("Keyboard layout: top_offset requires an element, x and y");
			float x = std::stof(xToken);
			float y = std::stof(yToken);
			if (element == L"textbar") {
				textBarOffsetX = x; textBarOffsetY = y;
			} else if (element == L"mode") {
				modeButtonOffsetX = x; modeButtonOffsetY = y;
			} else if (element == L"lock") {
				lockButtonOffsetX = x; lockButtonOffsetY = y;
			} else {
				OOVR_ABORT("Keyboard layout: unknown top_offset target");
			}
			continue;
		}
		if (op == L"top_design") {
			wstring element = pullstring(line);
			wstring widthToken = pullstring(line);
			wstring heightToken = pullstring(line);
			wstring scaleToken = pullstring(line);
			if (widthToken.empty() || heightToken.empty() || scaleToken.empty())
				OOVR_ABORT("Keyboard layout: top_design requires an element, width, height and font scale");
			TopElementDesign* design = nullptr;
			if (element == L"textbar") design = &textBarDesign;
			else if (element == L"mode") design = &modeButtonDesign;
			else if (element == L"lock") design = &lockButtonDesign;
			else OOVR_ABORT("Keyboard layout: unknown top_design target");
			design->width = std::max(0.0f, std::stof(widthToken));
			design->height = std::max(0.0f, std::stof(heightToken));
			design->fontScale = std::clamp(std::stof(scaleToken), 0.0f, 3.0f);
			continue;
		}
		if (op == L"control_box") {
			wstring control = pullstring(line);
			ControlDesign* design = nullptr;
			if (control == L"size") design = &sizeControlDesign;
			else if (control == L"opacity") design = &opacityControlDesign;
			else if (control == L"tilt") design = &tiltControlDesign;
			else OOVR_ABORT("Keyboard layout: unknown control_box target");
			wstring widthToken = pullstring(line);
			wstring heightToken = pullstring(line);
			if (widthToken.empty() || heightToken.empty())
				OOVR_ABORT("Keyboard layout: control_box requires width and height");
			design->width = std::max(24.0f, std::stof(widthToken));
			design->height = std::max(40.0f, std::stof(heightToken));
			continue;
		}
		if (op == L"control_part") {
			wstring control = pullstring(line);
			wstring part = pullstring(line);
			ControlDesign* design = nullptr;
			if (control == L"size") design = &sizeControlDesign;
			else if (control == L"opacity") design = &opacityControlDesign;
			else if (control == L"tilt") design = &tiltControlDesign;
			else OOVR_ABORT("Keyboard layout: unknown control_part target");
			wstring xToken = pullstring(line);
			wstring yToken = pullstring(line);
			wstring widthToken = pullstring(line);
			wstring heightToken = pullstring(line);
			if (xToken.empty() || yToken.empty() || widthToken.empty() || heightToken.empty())
				OOVR_ABORT("Keyboard layout: control_part requires x, y, width and height");
			float x = std::stof(xToken);
			float y = std::stof(yToken);
			float partWidth = std::max(4.0f, std::stof(widthToken));
			float partHeight = std::max(4.0f, std::stof(heightToken));
			if (part == L"up") {
				design->upOffsetX = x; design->upOffsetY = y;
				design->upWidth = partWidth; design->upHeight = partHeight;
			} else if (part == L"down") {
				design->downOffsetX = x; design->downOffsetY = y;
				design->downWidth = partWidth; design->downHeight = partHeight;
			} else {
				OOVR_ABORT("Keyboard layout: control_part must be up or down");
			}
			continue;
		}
		if (op == L"control_text") {
			wstring control = pullstring(line);
			wstring part = pullstring(line);
			ControlDesign* design = nullptr;
			if (control == L"size") design = &sizeControlDesign;
			else if (control == L"opacity") design = &opacityControlDesign;
			else if (control == L"tilt") design = &tiltControlDesign;
			else OOVR_ABORT("Keyboard layout: unknown control_text target");
			wstring xToken = pullstring(line);
			wstring yToken = pullstring(line);
			wstring scaleToken = pullstring(line);
			if (xToken.empty() || yToken.empty() || scaleToken.empty())
				OOVR_ABORT("Keyboard layout: control_text requires x, y and scale");
			float x = std::stof(xToken);
			float y = std::stof(yToken);
			float scale = std::clamp(std::stof(scaleToken), 0.2f, 3.0f);
			if (part == L"label") {
				design->labelOffsetX = x; design->labelOffsetY = y; design->labelScale = scale;
			} else if (part == L"value") {
				design->valueOffsetX = x; design->valueOffsetY = y; design->valueScale = scale;
			} else {
				OOVR_ABORT("Keyboard layout: control_text must be label or value");
			}
			continue;
		}
		if (op == L"control_arrow") {
			controlArrowFile = VRKeyboard::CHAR_CONV.to_bytes(pullstring(line));
			wstring rotation = pullstring(line);
			wstring breathe = pullstring(line);
			wstring breatheMinimum = pullstring(line);
			wstring breathePeriod = pullstring(line);
			wstring breathePhase = pullstring(line);
			controlArrowRotation = rotation.empty() ? 0.0f : std::stof(rotation);
			controlArrowBreatheEnabled = !breathe.empty() && parseBoolToken(breathe);
			controlArrowGlowEnabled = controlArrowBreatheEnabled;
			controlArrowBreatheMinPercent = breatheMinimum.empty() ? 35 : std::clamp(std::stoi(breatheMinimum), 0, 100);
			controlArrowBreathePeriodSeconds = breathePeriod.empty() ? 2.0f : std::clamp(std::stof(breathePeriod), 0.5f, 10.0f);
			controlArrowBreathePhaseDegrees = breathePhase.empty() ? 0.0f : std::stof(breathePhase);
			continue;
		}
		if (op == L"control_arrow_effect") {
			wstring glowEnabled = pullstring(line);
			wstring glowColor = pullstring(line);
			wstring glowStrength = pullstring(line);
			wstring glowRadius = pullstring(line);
			wstring breathe = pullstring(line);
			wstring breatheMinimum = pullstring(line);
			wstring breathePeriod = pullstring(line);
			wstring breathePhase = pullstring(line);
			if (glowEnabled.empty() || glowColor.empty() || glowStrength.empty() || glowRadius.empty()
			    || breathe.empty() || breatheMinimum.empty() || breathePeriod.empty() || breathePhase.empty())
				OOVR_ABORT("Keyboard layout: control_arrow_effect requires 8 values");
			controlArrowGlowEnabled = parseBoolToken(glowEnabled);
			parseColorToken(glowColor, controlArrowGlowColor);
			controlArrowGlowStrength = std::clamp(std::stoi(glowStrength), 0, 100);
			controlArrowGlowRadius = std::clamp(std::stoi(glowRadius), 1, 48);
			controlArrowBreatheEnabled = parseBoolToken(breathe);
			controlArrowBreatheMinPercent = std::clamp(std::stoi(breatheMinimum), 0, 100);
			controlArrowBreathePeriodSeconds = std::clamp(std::stof(breathePeriod), 0.5f, 10.0f);
			controlArrowBreathePhaseDegrees = std::stof(breathePhase);
			continue;
		}
		if (op == L"overlay") {
			overlayFile = VRKeyboard::CHAR_CONV.to_bytes(pullstring(line));
			continue;
		}
		if (op == L"overlay_rect") {
			overlayX = std::stof(pullstring(line));
			overlayY = std::stof(pullstring(line));
			overlayWidth = std::max(1.0f, std::stof(pullstring(line)));
			overlayHeight = std::max(1.0f, std::stof(pullstring(line)));
			wstring opacity = pullstring(line);
			overlayOpacity = opacity.empty() ? 100 : std::clamp(std::stoi(opacity), 0, 100);
			continue;
		}

		if (op[0] == '.') {
			wstring prop = op.substr(1);
			string utfProp = VRKeyboard::CHAR_CONV.to_bytes(prop);

			if (!last) {
				string msg = "Cannot set property '" + utfProp + "' without a prior key!";
				OOVR_ABORT(msg.c_str());
			}

			if (prop == L"spans_to_right") {
				last->spansToRight = pullstring(line) != L"false";
				continue;
			} else if (prop == L"label") {
				last->label = pullstring(line);
				last->labelShift = last->label;
				continue;
			} else if (prop == L"label_shift") {
				last->labelShift = pullstring(line);
				continue;
			} else if (prop == L"label_offset") {
				wstring x = pullstring(line);
				wstring y = pullstring(line);
				if (x.empty() || y.empty())
					OOVR_ABORT("Keyboard layout: label_offset requires x and y");
				last->labelOffsetX = std::stof(x);
				last->labelOffsetY = std::stof(y);
				continue;
			} else if (prop == L"label_scale") {
				wstring scale = pullstring(line);
				if (scale.empty())
					OOVR_ABORT("Keyboard layout: label_scale requires a value");
				last->labelScale = std::clamp(std::stof(scale), 0.25f, 3.0f);
				continue;
			}

			string msg = "Unknown property property '" + utfProp + "' for key '" + VRKeyboard::CHAR_CONV.to_bytes(last->label) + "'";
			OOVR_ABORT(msg.c_str());

			continue;
		}

		wchar_t ch = escapeChar(pullstring(line));
		wchar_t shift = escapeChar(pullstring(line));

		wstring wx, wy;

		wx = pullstring(line);
		wy = pullstring(line);

		if (wx.empty() != wy.empty()) {
			string msg = string("x/y empty mismatch for keyboard char '") + ((char)ch) + "'";
			OOVR_ABORT(msg.c_str());
		}

		if (wx.empty()) {
			addNextKey(ch, shift);
		} else {
			Key& key = addKey(ch, shift, std::stof(wx), std::stof(wy));

			wstring ww = pullstring(line);
			wstring wh = pullstring(line);

			if (!ww.empty()) {
				if (wh.empty()) {
					string msg = string("word width/height mismatch for keyboard char '") + ((char)ch) + "'";
					OOVR_ABORT(msg.c_str());
				}

				key.w = std::stof(ww);
				key.h = std::stof(wh);
			}
		}
	}

	if (width == 0)
		OOVR_ABORT("Missing keyboard layout width specifier");

	if (!overlayFile.empty() && sprites.empty()) {
		ImageLayer legacyOverlay;
		legacyOverlay.file = overlayFile;
		legacyOverlay.x = overlayX;
		legacyOverlay.y = overlayY;
		legacyOverlay.width = overlayWidth;
		legacyOverlay.height = overlayHeight;
		legacyOverlay.opacity = overlayOpacity;
		sprites.push_back(std::move(legacyOverlay));
	}

	// Calculate the adjacent keys
	for (Key& key : keys) {
		int ids[4] = { -1, -1, -1, -1 };
		float distances[4] = { 1000, 1000, 1000, 1000 };
		// Find the nearest keys in each direction
		for (Key& to : keys) {
			if (&key == &to)
				continue;

			float dx = to.x - key.x;
			float dy = to.y - key.y;

			float lenSq = dx * dx + dy * dy;

			int angle = (int)(atan2(-dy, dx) * 180 / math_pi);

			// Spin everything back 45deg, so the borders between regions are diagonal
			angle -= 45;

			if (angle < 0)
				angle += 360;

			int index = angle / 90;

			int deviation = abs(angle - (index * 90 + 45));
			lenSq += deviation / 45.0f;

			if (lenSq < distances[index]) {
				distances[index] = lenSq;
				ids[index] = to.id;
			}
		}

		key.toRight = ids[0];
		key.toDown = ids[1];
		key.toLeft = ids[2];
		key.toUp = ids[3];
	}
}

KeyboardLayout::~KeyboardLayout()
{
}
