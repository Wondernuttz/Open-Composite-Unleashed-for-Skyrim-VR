#pragma once

// Other defines...

#define RES_T_OBJ 256
#define RES_T_PNG 257
#define RES_T_FNTMETA 258
#define RES_T_KBLAYOUT 259
#define RES_T_WAV 260

#define RES_O_HAND_LEFT 1
#define RES_O_HAND_RIGHT 2

// Fonts
#define RES_O_FNT_UBUNTU 3
#define RES_O_FNT_MEDIEVAL 7
#define RES_O_FNT_PARCHMENT 8
#define RES_O_BG_PARCHMENT 9
#define RES_O_SPACEBAR 10

// Keyboard sounds
#define RES_O_SND_HOVER 11
#define RES_O_SND_PRESS 12

// Keyboard layouts
#define RES_O_KB_EN_GB 4

// Quest 3 controller models
#define RES_O_CTRL_Q3_LEFT 5
#define RES_O_CTRL_Q3_RIGHT 6

// AMD FidelityFX FSR 1.0 shader headers (HLSL source embedded as resources)
#define RES_O_FFX_A 13
#define RES_O_FFX_FSR1 14
#define RES_T_HLSL 261

// Resource list, used on Linux
// clang-format off
#define RES_LIST_LINUX(f) \
	f(RES_O_HAND_LEFT) \
	f(RES_O_HAND_RIGHT) \
	f(RES_O_FNT_UBUNTU) \
	f(RES_O_FNT_MEDIEVAL) \
	f(RES_O_FNT_PARCHMENT) \
	f(RES_O_BG_PARCHMENT) \
	f(RES_O_SPACEBAR) \
	f(RES_O_SND_HOVER) \
	f(RES_O_SND_PRESS) \
	f(RES_O_KB_EN_GB) \
	f(RES_O_CTRL_Q3_LEFT) \
	f(RES_O_CTRL_Q3_RIGHT) // clang-format on
