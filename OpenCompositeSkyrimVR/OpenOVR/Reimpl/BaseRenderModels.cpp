#include "stdafx.h"
#define BASE_IMPL
#include "BaseRenderModels.h"
#include "Misc/Config.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include "resources.h"

// Used for the hand offsets
#include "BaseCompositor.h"
#include "BaseInput.h"
#include "BaseSystem.h"
#include "Misc/Input/InteractionProfile.h"

#include "Misc/lodepng.h"
#include "Misc/xrutil.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/transform.hpp>

using glm::mat4;
using glm::quat;
using glm::vec3;
using glm::vec4;

#pragma region structs

enum OOVR_EVRRenderModelError : int {
	VRRenderModelError_None = 0,
	VRRenderModelError_Loading = 100,
	VRRenderModelError_NotSupported = 200,
	VRRenderModelError_InvalidArg = 300,
	VRRenderModelError_InvalidModel = 301,
	VRRenderModelError_NoShapes = 302,
	VRRenderModelError_MultipleShapes = 303,
	VRRenderModelError_TooManyVertices = 304,
	VRRenderModelError_MultipleTextures = 305,
	VRRenderModelError_BufferTooSmall = 306,
	VRRenderModelError_NotEnoughNormals = 307,
	VRRenderModelError_NotEnoughTexCoords = 308,

	VRRenderModelError_InvalidTexture = 400,
};

struct OOVR_RenderModel_Vertex_t {
	vr::HmdVector3_t vPosition; // position in meters in device space
	vr::HmdVector3_t vNormal;
	float rfTextureCoord[2];
};

#if defined(__linux__) || defined(__APPLE__)
// This structure was originally defined mis-packed on Linux, preserved for
// compatibility.
#pragma pack(push, 4)
#endif

struct OOVR_RenderModel_t {
	const OOVR_RenderModel_Vertex_t* rVertexData; // Vertex data for the mesh
	uint32_t unVertexCount; // Number of vertices in the vertex data
	const uint16_t* rIndexData; // Indices into the vertex data for each triangle
	uint32_t unTriangleCount; // Number of triangles in the mesh. Index count is 3 * TriangleCount
	OOVR_TextureID_t diffuseTextureId; // Session unique texture identifier. Rendermodels which share the same texture will have the same id. <0 == texture not present
};

// Must match the real OpenVR RenderModel_TextureMap_t layout exactly.
// Skyrim was compiled against the full struct — if we omit fields, Skyrim
// reads heap garbage for 'format' and 'unMipLevels', causing black textures.
enum OOVR_EVRRenderModelTextureFormat : int32_t {
	VRRenderModelTextureFormat_RGBA8_SRGB = 0,
	VRRenderModelTextureFormat_BC2 = 1,
	VRRenderModelTextureFormat_BC4 = 2,
	VRRenderModelTextureFormat_BC7 = 3,
	VRRenderModelTextureFormat_BC7_SRGB = 4,
	VRRenderModelTextureFormat_RGBA16_FLOAT = 5,
};

struct OOVR_RenderModel_TextureMap_t {
	uint16_t unWidth, unHeight; // width and height of the texture map in pixels
	const uint8_t* rubTextureMapData; // Map texture data. All textures are RGBA with 8 bits per channel per pixel. Data size is width * height * 4ub
	OOVR_EVRRenderModelTextureFormat format; // Texture format — must be set or Skyrim reads garbage
	uint16_t unMipLevels; // Number of mip levels in the texture data
};

#if defined(__linux__) || defined(__APPLE__)
#pragma pack(pop)
#endif

typedef uint32_t VRComponentProperties;
enum OOVR_EVRComponentProperty {
	VRComponentProperty_IsStatic = (1 << 0),
	VRComponentProperty_IsVisible = (1 << 1),
	VRComponentProperty_IsTouched = (1 << 2),
	VRComponentProperty_IsPressed = (1 << 3),
	VRComponentProperty_IsScrolled = (1 << 4),
};

#pragma endregion

typedef OOVR_RenderModel_t RenderModel_t;
typedef OOVR_EVRRenderModelError EVRRenderModelError;
typedef OOVR_RenderModel_TextureMap_t RenderModel_TextureMap_t;
typedef OOVR_TextureID_t TextureID_t;

static string loadResource(int rid, int resType = RES_T_OBJ)
{
#ifndef _WIN32
	const char *start = nullptr, *end = nullptr;
	FindResourceLinux(rid, &start, &end);
	return { start, (size_t)(end - start) };
#else
	// Open our resource
	HRSRC ref = FindResource(openovr_module_id, MAKEINTRESOURCE(rid), MAKEINTRESOURCE(resType));
	if (!ref) {
		string err = "FindResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	char* cstr = (char*)LoadResource(openovr_module_id, ref);
	if (!cstr) {
		string err = "LoadResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	DWORD len = SizeofResource(openovr_module_id, ref);
	if (!len) {
		string err = "SizeofResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	return string(cstr, len);
#endif
}

static OOVR_RenderModel_Vertex_t split_face(
    const string& s,
    const std::vector<vr::HmdVector3_t>& verts,
    const std::vector<vr::HmdVector2_t>& uvs,
    const std::vector<vr::HmdVector3_t>& normals)
{

	size_t slash1 = s.find('/');
	size_t slash2 = s.find('/', slash1 + 1);

	if (slash1 == string::npos || slash2 == string::npos) {
		string err = "Bad face spec: " + s;
		OOVR_ABORT(err.c_str());
	}

	int vert = stoi(s.substr(0, slash1));
	int uv = stoi(s.substr(slash1 + 1, slash1 - slash2 - 1));
	int norm = stoi(s.substr(slash2 + 1));

	// OBJ references start at one
	vert--;
	uv--;
	norm--;

	// Build the result
	OOVR_RenderModel_Vertex_t out = { 0 };
	out.vPosition = verts[vert];
	out.vNormal = normals[norm];
	out.rfTextureCoord[0] = uvs[uv].v[0];
	out.rfTextureCoord[1] = uvs[uv].v[1];
	return out;
}

// The retired experimental Quest 3 controller renderer is preserved in Git history.

// =========================================================================
// STEAMVR RENDER MODEL PASSTHROUGH (2026-07-25)
// =========================================================================
// The built-in models are a generic hand mesh with a 1x1 flat-colour texture
// (the "gray hands"). When SteamVR is installed, serve Valve's real controller
// models + colour textures straight off the user's disk instead — loaded at
// runtime, never redistributed. Any model name the game requests that exists
// in SteamVR's rendermodels folder gets served; unknown names fall back to
// the built-in hands.

static std::map<int32_t, std::string> s_svrTexPaths; // textureId -> png path
static std::map<std::string, int32_t> s_svrTexIds; // png path -> textureId (dedupe)
static int32_t s_svrNextTexId = 1000;

static bool SvrFileExists(const std::string& p)
{
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// This build's lodepng has LODEPNG_COMPILE_DISK off — read the file ourselves
// and use the in-memory decode overload.
static unsigned SvrDecodePng(const std::string& path, std::vector<unsigned char>& out, unsigned& w, unsigned& h)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return 78; // lodepng error 78 = failed to open file
	std::vector<unsigned char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (raw.empty())
		return 78;
	return lodepng::decode(out, w, h, raw);
}

// Resolve <SteamVR>\resources\rendermodels once. Order: openvrpaths.vrpath
// (authoritative — written by SteamVR itself), then Steam registry.
static const std::string& SvrRenderModelsDir()
{
	static bool resolved = false;
	static std::string dir;
	if (resolved)
		return dir;
	resolved = true;

	auto tryRuntime = [&](std::string runtime) -> bool {
		if (runtime.empty())
			return false;
		std::string cand = runtime + "\\resources\\rendermodels";
		DWORD a = GetFileAttributesA(cand.c_str());
		if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
			dir = cand;
			OOVR_LOGF("RenderModels: SteamVR rendermodels found at %s", dir.c_str());
			return true;
		}
		return false;
	};

	// 1. openvrpaths.vrpath — %LOCALAPPDATA%\openvr\openvrpaths.vrpath,
	//    tiny JSON with a "runtime": ["<SteamVR dir>"] array.
	char localAppData[MAX_PATH] = {};
	if (GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
		std::ifstream vf(std::string(localAppData) + "\\openvr\\openvrpaths.vrpath");
		if (vf) {
			std::string json((std::istreambuf_iterator<char>(vf)), std::istreambuf_iterator<char>());
			size_t rt = json.find("\"runtime\"");
			if (rt != std::string::npos) {
				size_t q1 = json.find('"', json.find('[', rt) + 1);
				size_t q2 = (q1 != std::string::npos) ? json.find('"', q1 + 1) : std::string::npos;
				if (q1 != std::string::npos && q2 != std::string::npos) {
					std::string path = json.substr(q1 + 1, q2 - q1 - 1);
					// Unescape JSON: "\\" -> "\", "\/" -> "/"
					std::string un;
					for (size_t i = 0; i < path.size(); i++) {
						if (path[i] == '\\' && i + 1 < path.size() && (path[i + 1] == '\\' || path[i + 1] == '/')) {
							un += path[i + 1];
							i++;
						} else {
							un += path[i];
						}
					}
					if (tryRuntime(un))
						return dir;
				}
			}
		}
	}

	// 2. Steam registry -> <Steam>\steamapps\common\SteamVR
	auto tryReg = [&](HKEY root, const char* key, const char* value) -> bool {
		char buf[MAX_PATH] = {};
		DWORD len = sizeof(buf);
		if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &len) == ERROR_SUCCESS && buf[0]) {
			std::string steam(buf);
			for (auto& c : steam)
				if (c == '/')
					c = '\\';
			return tryRuntime(steam + "\\steamapps\\common\\SteamVR");
		}
		return false;
	};
	if (tryReg(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath"))
		return dir;
	if (tryReg(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath"))
		return dir;

	OOVR_LOG("RenderModels: no SteamVR install found — using built-in hand models");
	return dir; // empty = not found
}

// Valve's rendermodel OBJs use "v/vt" faces (no normals, one slash) — the
// strict split_face above aborts on them ("Bad face spec"). Parse all four
// spec styles: v, v/vt, v//vn, v/vt/vn. Missing fields come back as -1.
static bool SvrParseFaceRef(const string& s, int& vert, int& uv, int& norm)
{
	vert = uv = norm = -1;
	size_t slash1 = s.find('/');
	size_t slash2 = (slash1 == string::npos) ? string::npos : s.find('/', slash1 + 1);
	try {
		if (slash1 == string::npos) {
			vert = stoi(s) - 1; // "v"
		} else if (slash2 == string::npos) {
			vert = stoi(s.substr(0, slash1)) - 1; // "v/vt"
			uv = stoi(s.substr(slash1 + 1)) - 1;
		} else {
			vert = stoi(s.substr(0, slash1)) - 1; // "v/vt/vn" or "v//vn"
			if (slash2 > slash1 + 1)
				uv = stoi(s.substr(slash1 + 1, slash2 - slash1 - 1)) - 1;
			norm = stoi(s.substr(slash2 + 1)) - 1;
		}
	} catch (...) {
		return false;
	}
	return vert >= 0;
}

static OOVR_RenderModel_Vertex_t SvrBuildVertex(int vert, int uv, int norm,
    const std::vector<vr::HmdVector3_t>& verts,
    const std::vector<vr::HmdVector2_t>& uvs,
    const std::vector<vr::HmdVector3_t>& normals)
{
	OOVR_RenderModel_Vertex_t out = { 0 };
	if (vert >= 0 && vert < (int)verts.size())
		out.vPosition = verts[vert];
	if (uv >= 0 && uv < (int)uvs.size()) {
		out.rfTextureCoord[0] = uvs[uv].v[0];
		out.rfTextureCoord[1] = uvs[uv].v[1];
	}
	if (norm >= 0 && norm < (int)normals.size())
		out.vNormal = normals[norm];
	return out;
}

// Load <rendermodels>\<name>\<name>.obj + its diffuse texture. Returns false
// if SteamVR or the specific model is absent (caller falls back to hands).
static bool TryLoadSteamVrModel(const string& rawName, RenderModel_t** renderModel)
{
	const std::string& rmDir = SvrRenderModelsDir();
	if (rmDir.empty())
		return false;

	// Strip any "{driver}" prefix: "{indexcontroller}valve_..." -> "valve_..."
	string base = rawName;
	if (!base.empty() && base[0] == '{') {
		size_t close = base.find('}');
		if (close == string::npos)
			return false;
		base = base.substr(close + 1);
	}
	if (base.empty() || base.find("..") != string::npos || base.find('\\') != string::npos || base.find('/') != string::npos)
		return false; // no path tricks through render model names

	// Quest generation correction: the Touch interaction profile makes OCU
	// advertise quest2 for EVERY Quest generation, but SteamVR ships distinct
	// models. Pick by the runtime's real system name (VD reports the actual
	// headset). Quest 3's "Touch Plus" controllers = SteamVR's "quest_plus".
	if (base.rfind("oculus_quest2_controller_", 0) == 0 && xr_gbl) {
		std::string side = base.substr(sizeof("oculus_quest2_controller_") - 1); // "left"/"right"
		std::string sys = xr_gbl->systemProperties.systemName;
		for (auto& ch : sys)
			ch = (char)tolower((unsigned char)ch);
		std::string sub;
		if (sys.find("quest 3") != std::string::npos || sys.find("quest3") != std::string::npos)
			sub = "oculus_quest_plus_controller_" + side;
		else if (sys.find("quest pro") != std::string::npos)
			sub = "oculus_quest_pro_controller_" + side;
		if (!sub.empty() && SvrFileExists(rmDir + "\\" + sub + "\\" + sub + ".obj")) {
			OOVR_LOGF("RenderModels: system '%s' — substituting %s for %s",
			    xr_gbl->systemProperties.systemName, sub.c_str(), base.c_str());
			base = sub;
		}
	}

	std::string dir = rmDir + "\\" + base;
	std::string objPath = dir + "\\" + base + ".obj";
	if (!SvrFileExists(objPath))
		return false;

	std::ifstream res(objPath);
	if (!res)
		return false;

	std::vector<vr::HmdVector3_t> verts;
	std::vector<vr::HmdVector2_t> uvs;
	std::vector<vr::HmdVector3_t> normals;
	std::vector<OOVR_RenderModel_Vertex_t> vertexData;

	// Valve's rendermodels are authored in meters — no unit scale needed.
	while (!res.eof()) {
		string op;
		res >> op;
		if (op == "v") {
			vec3 v;
			res >> v.x >> v.y >> v.z;
			verts.push_back(G2S_v3f(v));
		} else if (op == "vt") {
			float x, y;
			res >> x >> y;
			// OBJ UVs are bottom-left origin, D3D samples top-left — flip V
			// or the diffuse atlas reads upside down (giant misplaced decals,
			// 2026-07-25 first-launch screenshot).
			uvs.push_back(vr::HmdVector2_t{ x, 1.0f - y });
		} else if (op == "vn") {
			vec3 v;
			res >> v.x >> v.y >> v.z;
			normals.push_back(G2S_v3f(v));
		} else if (op == "f") {
			string a, b, c, d;
			res >> a >> b >> c;
			std::streampos pos = res.tellg();
			bool isQuad = false;
			if (res >> d) {
				// A 4th face token starts with a digit (ops never do)
				if (!d.empty() && (isdigit((unsigned char)d[0]) || d[0] == '-'))
					isQuad = true;
				else
					res.seekg(pos);
			}
			int av, au, an, bv, bu, bn, cv, cu, cn;
			if (!SvrParseFaceRef(a, av, au, an) || !SvrParseFaceRef(b, bv, bu, bn) || !SvrParseFaceRef(c, cv, cu, cn)) {
				OOVR_LOGF("RenderModels: unparseable face in %s ('%s' '%s' '%s') — falling back", objPath.c_str(), a.c_str(), b.c_str(), c.c_str());
				return false;
			}
			vertexData.push_back(SvrBuildVertex(av, au, an, verts, uvs, normals));
			vertexData.push_back(SvrBuildVertex(bv, bu, bn, verts, uvs, normals));
			vertexData.push_back(SvrBuildVertex(cv, cu, cn, verts, uvs, normals));
			if (isQuad) {
				int dv, du, dn;
				if (!SvrParseFaceRef(d, dv, du, dn))
					return false;
				vertexData.push_back(SvrBuildVertex(av, au, an, verts, uvs, normals));
				vertexData.push_back(SvrBuildVertex(cv, cu, cn, verts, uvs, normals));
				vertexData.push_back(SvrBuildVertex(dv, du, dn, verts, uvs, normals));
			}
		} else {
			// Skip the rest of any unhandled line (mtllib/usemtl/o/s/#...)
			string skip;
			std::getline(res, skip);
		}
	}

	if (vertexData.empty())
		return false;

	// Align to OCU's exposed controller pose. Empirically dialed 2026-07-25
	// across three headset rounds: no-transform = floats off the grip;
	// invHT·rotY(180) = lined up but upside down; invHT·rotX(180) = facing
	// backwards; user's "flip the bottoms 180 toward me" from there composes
	// to IDENTITY — Valve's models are already authored in the right frame,
	// they only need the inverse hand transform (unlike the built-in hand
	// meshes, which carry their own authoring corrections).
	//
	// Residual trim (round 4 report: both controllers lean into each other
	// and sit low vs Meta's own render — a mirrored per-hand error) is user-
	// tunable via opencomposite.ini: renderModelRotX/Y/Z (degrees) and
	// renderModelOffX/Y/Z (meters). Values are RIGHT-hand; the left hand
	// mirrors automatically (x-offset, yaw and roll negated).
	{
		bool leftHand = base.find("left") != string::npos;
		bool indexModel = base.rfind("valve_controller_knu_", 0) == 0;
		float mirror = leftHand ? -1.0f : 1.0f;
		constexpr float d2r = 3.14159265f / 180.0f;
		// The user's measured renderModel* trim belongs to Touch/Quest. Index
		// has its own identity-based family so changing hardware cannot carry
		// Meta's mesh correction into Valve's correctly authored model.
		float rotX = indexModel ? oovr_global_configuration.IndexRenderModelRotX() : oovr_global_configuration.RenderModelRotX();
		float rotY = indexModel ? oovr_global_configuration.IndexRenderModelRotY() : oovr_global_configuration.RenderModelRotY();
		float rotZ = indexModel ? oovr_global_configuration.IndexRenderModelRotZ() : oovr_global_configuration.RenderModelRotZ();
		float offX = indexModel ? oovr_global_configuration.IndexRenderModelOffX() : oovr_global_configuration.RenderModelOffX();
		float offY = indexModel ? oovr_global_configuration.IndexRenderModelOffY() : oovr_global_configuration.RenderModelOffY();
		float offZ = indexModel ? oovr_global_configuration.IndexRenderModelOffZ() : oovr_global_configuration.RenderModelOffZ();
		float rmScale = indexModel ? oovr_global_configuration.IndexRenderModelScale() : oovr_global_configuration.RenderModelScale();
		mat4 trim(1.0f);
		trim = glm::translate(trim, vec3(offX * mirror, offY, offZ));
		trim *= mat4(glm::rotate(rotY * mirror * d2r, vec3(0, 1, 0)));
		trim *= mat4(glm::rotate(rotX * d2r, vec3(1, 0, 0)));
		trim *= mat4(glm::rotate(rotZ * mirror * d2r, vec3(0, 0, 1)));
		if (rmScale > 0.1f && rmScale < 10.0f && rmScale != 1.0f)
			trim *= mat4(glm::scale(glm::mat4(1.0f), vec3(rmScale)));
		// Trim applies in DEVICE space (left of the hand transform): X=right,
		// Y=up, Z=toward the user. This makes the ini knobs intuitive and lets
		// the superposition-measured correction paste in directly (2026-07-25
		// measurement: ghost pitched -18° / 3.6cm low → RotX=18, OffY=0.036).
		mat4 transform = trim * glm::inverse(BaseCompositor::GetHandTransform());
		quat rotOnly = quat(transform);
		for (auto& v : vertexData) {
			vec4 p(v.vPosition.v[0], v.vPosition.v[1], v.vPosition.v[2], 1.0f);
			p = transform * p;
			v.vPosition.v[0] = p.x;
			v.vPosition.v[1] = p.y;
			v.vPosition.v[2] = p.z;
			if (v.vNormal.v[0] != 0.0f || v.vNormal.v[1] != 0.0f || v.vNormal.v[2] != 0.0f) {
				vec3 n(v.vNormal.v[0], v.vNormal.v[1], v.vNormal.v[2]);
				n = rotOnly * n;
				v.vNormal.v[0] = n.x;
				v.vNormal.v[1] = n.y;
				v.vNormal.v[2] = n.z;
			}
		}
	}

	// Valve's OBJs carry no normals — derive flat per-face normals so the
	// model lights correctly instead of rendering black. (Runs AFTER the
	// pose transform so computed normals match the final geometry.)
	for (size_t i = 0; i + 2 < vertexData.size(); i += 3) {
		auto& n0 = vertexData[i].vNormal;
		if (n0.v[0] != 0.0f || n0.v[1] != 0.0f || n0.v[2] != 0.0f)
			continue; // file had a real normal
		const auto& p0 = vertexData[i].vPosition;
		const auto& p1 = vertexData[i + 1].vPosition;
		const auto& p2 = vertexData[i + 2].vPosition;
		vec3 e1(p1.v[0] - p0.v[0], p1.v[1] - p0.v[1], p1.v[2] - p0.v[2]);
		vec3 e2(p2.v[0] - p0.v[0], p2.v[1] - p0.v[1], p2.v[2] - p0.v[2]);
		vec3 n = glm::cross(e1, e2);
		float len = glm::length(n);
		if (len > 1e-12f)
			n /= len;
		for (int k = 0; k < 3; k++) {
			vertexData[i + k].vNormal.v[0] = n.x;
			vertexData[i + k].vNormal.v[1] = n.y;
			vertexData[i + k].vNormal.v[2] = n.z;
		}
	}

	// Diffuse texture: prefer the .mtl's map_Kd, fall back to <name>_diff.png
	std::string texPath;
	{
		std::ifstream mtl(dir + "\\" + base + ".mtl");
		if (mtl) {
			string tok;
			while (mtl >> tok) {
				if (tok == "map_Kd") {
					string texName;
					std::getline(mtl, texName);
					// trim leading spaces / trailing CR
					size_t s = texName.find_first_not_of(" \t");
					size_t e = texName.find_last_not_of(" \t\r");
					if (s != string::npos)
						texName = texName.substr(s, e - s + 1);
					if (!texName.empty() && SvrFileExists(dir + "\\" + texName)) {
						texPath = dir + "\\" + texName;
						break;
					}
				}
			}
		}
		if (texPath.empty() && SvrFileExists(dir + "\\" + base + "_diff.png"))
			texPath = dir + "\\" + base + "_diff.png";
	}

	*renderModel = new RenderModel_t();
	RenderModel_t& rm = **renderModel;
	rm.unVertexCount = (uint32_t)vertexData.size();
	OOVR_RenderModel_Vertex_t* vertexData_arr = new OOVR_RenderModel_Vertex_t[rm.unVertexCount];
	rm.rVertexData = vertexData_arr;
	for (uint32_t i = 0; i < rm.unVertexCount; i++)
		vertexData_arr[i] = vertexData[i];

	uint16_t* indexData = new uint16_t[rm.unVertexCount];
	for (uint16_t i = 0; i < rm.unVertexCount; i++)
		indexData[i] = i;
	rm.rIndexData = indexData;
	rm.unTriangleCount = rm.unVertexCount / 3;

	rm.diffuseTextureId = -1;
	if (!texPath.empty()) {
		auto known = s_svrTexIds.find(texPath);
		int32_t id;
		if (known != s_svrTexIds.end()) {
			id = known->second;
		} else {
			id = s_svrNextTexId++;
			s_svrTexIds[texPath] = id;
			s_svrTexPaths[id] = texPath;
		}
		rm.diffuseTextureId = id;
	}

	OOVR_LOGF("RenderModels: serving SteamVR model '%s' (%u verts, tex=%s)",
	    base.c_str(), rm.unVertexCount, texPath.empty() ? "none" : texPath.c_str());
	return true;
}

EVRRenderModelError BaseRenderModels::LoadRenderModel_Async(const char* pchRenderModelName, RenderModel_t** renderModel)
{
	string name = pchRenderModelName;

	// Real SteamVR models first — colour controllers instead of gray hands
	// Keep controller visibility and model selection independent. The legacy
	// option bypasses SteamVR model passthrough and reaches OCU's original grey
	// hand meshes below; renderCustomHands remains the visibility control.
	if (!oovr_global_configuration.UseLegacyGreyHands() &&
	    TryLoadSteamVrModel(name, renderModel))
		return VRRenderModelError_None;
	if (oovr_global_configuration.UseLegacyGreyHands())
		OOVR_LOG_ONCE("RenderModels: legacy grey hand meshes selected; SteamVR model passthrough disabled");
	int rid;
	float sided;
	bool isQuest3 = false; // Experimental Quest 3 renderer retired; see Git history.

	// todo: start loading correct models, !359 related
	if (name == "renderLeftHand") {
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name == "renderRightHand") {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else if (name == "oculus_quest2_controller_left") {
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name == "oculus_quest2_controller_right") {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else if (name == "{indexcontroller}valve_controller_knu_1_0_left") {
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name == "{indexcontroller}valve_controller_knu_1_0_right") {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else if (name == "oculusHmdRenderModel") {
		// no model for the HMD
		return VRRenderModelError_NotSupported;
	} else if (name.find("left") != string::npos || name.find("Left") != string::npos) {
		// Unknown name with no SteamVR model on disk — fall back to hands
		// instead of aborting (real installs request all sorts of names).
		rid = RES_O_HAND_LEFT;
		sided = 1;
	} else if (name.find("right") != string::npos || name.find("Right") != string::npos) {
		rid = RES_O_HAND_RIGHT;
		sided = -1;
	} else {
		OOVR_LOGF("Unknown render model name (no SteamVR model available): %s", pchRenderModelName);
		return VRRenderModelError_NotSupported;
	}

	std::istringstream res = std::istringstream(loadResource(rid));

	std::vector<vr::HmdVector3_t> verts;
	std::vector<vr::HmdVector2_t> uvs;
	std::vector<vr::HmdVector3_t> normals;
	std::vector<OOVR_RenderModel_Vertex_t> vertexData;

	mat4 modelTransform;
	mat4 transform;
	{
		// Transform to line up the model with the Touch controller
		modelTransform = mat4(glm::rotate(sided * math_pi / 2, vec3(0, 0, 1)));
		modelTransform[3] = vec4(sided * 0.015f, 0.0f, 0.03f, 1.0f);

		// SteamVR rotates it's models 180deg around the Y axis for some reason
		modelTransform *= mat4(glm::rotate(math_pi, vec3(0, 1, 0)));

		transform = glm::inverse(BaseCompositor::GetHandTransform()) * modelTransform;
	}
	quat rotate = quat(transform);

	while (!res.eof()) {
		string op;
		res >> op;

		if (op == "v") {
			// Vertex
			vec3 v;
			res >> v.x >> v.y >> v.z;
			if (!isQuest3) {
				// Maya exports in cm, so translate that to meters
				v *= 0.01f;
			}

			verts.push_back(G2S_v3f(v));
		} else if (op == "vt") {
			// UV
			float x, y;
			res >> x >> y;
			uvs.push_back(vr::HmdVector2_t{ x, y });
		} else if (op == "vn") {
			// Normal
			vec3 v;
			res >> v.x >> v.y >> v.z;

			normals.push_back(G2S_v3f(v));
		} else if (op == "f") {
			// Face — handle both triangles and quads
			string a, b, c, d;
			res >> a >> b >> c;

			// Peek ahead for a 4th vertex (quad)
			std::streampos pos = res.tellg();
			bool isQuad = false;
			if (res >> d) {
				// Check if d looks like a face index (contains '/')
				if (d.find('/') != string::npos) {
					isQuad = true;
				} else {
					// Not a face vertex, put it back
					res.seekg(pos);
				}
			}

			vertexData.push_back(split_face(a, verts, uvs, normals));
			vertexData.push_back(split_face(b, verts, uvs, normals));
			vertexData.push_back(split_face(c, verts, uvs, normals));

			if (isQuad) {
				// Split quad ABCD into triangles ABC and ACD
				vertexData.push_back(split_face(a, verts, uvs, normals));
				vertexData.push_back(split_face(c, verts, uvs, normals));
				vertexData.push_back(split_face(d, verts, uvs, normals));
			}
		}
	}

	*renderModel = new RenderModel_t();
	RenderModel_t& rm = **renderModel;
	rm.diffuseTextureId = 0; // stable built-in 1x1 handColour texture

	rm.unVertexCount = (uint32_t)vertexData.size();
	OOVR_RenderModel_Vertex_t* vertexData_arr = new OOVR_RenderModel_Vertex_t[rm.unVertexCount];
	rm.rVertexData = vertexData_arr;
	for (uint32_t i = 0; i < rm.unVertexCount; i++) {
		vec4 vertex = vec4(vertexData[i].vPosition.v[0], vertexData[i].vPosition.v[1], vertexData[i].vPosition.v[2], 1.0f);
		vertex = transform * vertex;
		vertexData_arr[i].vPosition.v[0] = vertex.x;
		vertexData_arr[i].vPosition.v[1] = vertex.y;
		vertexData_arr[i].vPosition.v[2] = vertex.z;
		vertexData_arr[i].vNormal = vertexData[i].vNormal;
		vertexData_arr[i].rfTextureCoord[0] = vertexData[i].rfTextureCoord[0];
		vertexData_arr[i].rfTextureCoord[1] = vertexData[i].rfTextureCoord[1];
	}

	uint16_t* indexData = new uint16_t[rm.unVertexCount];
	for (uint16_t i = 0; i < rm.unVertexCount; i++) {
		indexData[i] = i;
	}
	rm.rIndexData = indexData;
	rm.unTriangleCount = rm.unVertexCount / 3;

	return VRRenderModelError_None;
}

void BaseRenderModels::FreeRenderModel(RenderModel_t* renderModel)
{
	delete[] renderModel->rVertexData;
	delete[] renderModel->rIndexData;
	delete renderModel;
}

EVRRenderModelError BaseRenderModels::LoadTexture_Async(TextureID_t textureId, RenderModel_TextureMap_t** texture)
{
	OOVR_LOGF("LoadTexture_Async called with textureId=%d", textureId);

	// SteamVR passthrough textures — the real diffuse PNG off the user's disk
	{
		auto it = s_svrTexPaths.find(textureId);
		if (it != s_svrTexPaths.end()) {
			std::vector<unsigned char> png;
			unsigned w = 0, h = 0;
			unsigned err = SvrDecodePng(it->second, png, w, h);
			if (err == 0 && w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF) {
				*texture = new RenderModel_TextureMap_t();
				RenderModel_TextureMap_t& stx = **texture;
				stx.unWidth = (uint16_t)w;
				stx.unHeight = (uint16_t)h;
				uint8_t* data = new uint8_t[png.size()];
				memcpy(data, png.data(), png.size());
				stx.rubTextureMapData = data;
				stx.format = VRRenderModelTextureFormat_RGBA8_SRGB;
				stx.unMipLevels = 1;
				OOVR_LOGF("RenderModels: served SteamVR texture %d (%ux%u)", textureId, w, h);
				return VRRenderModelError_None;
			}
			OOVR_LOGF("RenderModels: lodepng decode failed (%u) for %s — flat colour fallback",
			    err, it->second.c_str());
		}
	}

	*texture = new RenderModel_TextureMap_t();
	RenderModel_TextureMap_t& tx = **texture;

	// Default: 1x1 single coloured texture for hands
	tx.unWidth = 1;
	tx.unHeight = 1;
	uint8_t* d = new uint8_t[tx.unWidth * tx.unHeight * 4];
	tx.rubTextureMapData = d;
	tx.format = VRRenderModelTextureFormat_RGBA8_SRGB;
	tx.unMipLevels = 1;

	vr::HmdColor_t colour = oovr_global_configuration.HandColour();
	d[0] = (uint8_t)(colour.r * 255);
	d[1] = (uint8_t)(colour.g * 255);
	d[2] = (uint8_t)(colour.b * 255);
	d[3] = (uint8_t)(colour.a * 255);

	return VRRenderModelError_None;
}

void BaseRenderModels::FreeTexture(RenderModel_TextureMap_t* texture)
{
	delete[] texture->rubTextureMapData;
	delete texture;
}

EVRRenderModelError BaseRenderModels::LoadTextureD3D11_Async(TextureID_t textureId, void* pD3D11Device, void** ppD3D11Texture2D)
{
	STUBBED();
}

EVRRenderModelError BaseRenderModels::LoadIntoTextureD3D11_Async(TextureID_t textureId, void* pDstTexture)
{
#ifndef SUPPORT_DX11
	OOVR_ABORT("Cannot load D3D11 textures without D3D11 support");
#else
	OOVR_LOGF("LoadIntoTextureD3D11_Async called with textureId=%d", textureId);

	ID3D11Texture2D* output = (ID3D11Texture2D*)pDstTexture;

	struct pix_t {
		uint8_t r, g, b, a;
	};

	// D3D11 setup
	ID3D11Device* device;
	ID3D11DeviceContext* context;
	output->GetDevice(&device);
	device->GetImmediateContext(&context);

	D3D11_TEXTURE2D_DESC desc;
	output->GetDesc(&desc);

	OOVR_LOGF("D3D11 dest texture: %ux%u, format=%u, mips=%u", desc.Width, desc.Height, desc.Format, desc.MipLevels);

	// SteamVR passthrough: decode the real diffuse PNG and upload every mip
	// (nearest-neighbour rescale per level; swizzle for BGRA destinations).
	{
		auto it = s_svrTexPaths.find(textureId);
		if (it != s_svrTexPaths.end()) {
			std::vector<unsigned char> png;
			unsigned w = 0, h = 0;
			if (SvrDecodePng(it->second, png, w, h) == 0 && w > 0 && h > 0) {
				bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
				    || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
				    || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;

				std::vector<uint8_t> level;
				for (UINT mip = 0; mip < desc.MipLevels; mip++) {
					UINT mw = desc.Width >> mip, mh = desc.Height >> mip;
					if (mw == 0) mw = 1;
					if (mh == 0) mh = 1;
					level.resize((size_t)mw * mh * 4);
					for (UINT y = 0; y < mh; y++) {
						unsigned sy = (unsigned)((uint64_t)y * h / mh);
						for (UINT x = 0; x < mw; x++) {
							unsigned sx = (unsigned)((uint64_t)x * w / mw);
							const unsigned char* s = &png[((size_t)sy * w + sx) * 4];
							uint8_t* dpx = &level[((size_t)y * mw + x) * 4];
							if (bgra) {
								dpx[0] = s[2]; dpx[1] = s[1]; dpx[2] = s[0]; dpx[3] = s[3];
							} else {
								dpx[0] = s[0]; dpx[1] = s[1]; dpx[2] = s[2]; dpx[3] = s[3];
							}
						}
					}
					context->UpdateSubresource(output, D3D11CalcSubresource(mip, 0, desc.MipLevels),
					    nullptr, level.data(), mw * 4, 0);
				}
				context->Release();
				device->Release();
				OOVR_LOGF("RenderModels: uploaded SteamVR texture %d (%ux%u -> %ux%u, %u mips%s)",
				    textureId, w, h, desc.Width, desc.Height, desc.MipLevels, bgra ? ", BGRA" : "");
				return VRRenderModelError_None;
			}
			OOVR_LOGF("RenderModels: D3D11 decode failed for %s — flat colour fallback", it->second.c_str());
		}
	}

	int px_count = desc.Width * desc.Height;

	// Flat colour fill
	pix_t pixColour;
	{
		vr::HmdColor_t colour = oovr_global_configuration.HandColour();
		pixColour = { (uint8_t)(colour.r * 255), (uint8_t)(colour.g * 255), (uint8_t)(colour.b * 255), 255 };
	}
	pix_t* pixels = new pix_t[px_count];
	for (int i = 0; i < px_count; i++) {
		pixels[i] = pixColour;
	}

	// Cross our fingers it's a four-byte RGBA format.
	int count = desc.MipLevels * desc.ArraySize;
	D3D11_SUBRESOURCE_DATA* init = new D3D11_SUBRESOURCE_DATA[count];
	for (int i = 0; i < count; i++) {
		init[i] = { pixels, desc.Width * (uint32_t)sizeof(uint32_t), 0 };
	}

	ID3D11Texture2D* tempTex;
	OOVR_FAILED_DX_ABORT(device->CreateTexture2D(&desc, init, &tempTex));

	// Copy over the texture
	context->CopyResource(output, tempTex);

	// Cleanup
	tempTex->Release();
	delete[] init;
	delete[] pixels;
	context->Release();
	device->Release();

	return VRRenderModelError_None;
#endif
}

void BaseRenderModels::FreeTextureD3D11(void* pD3D11Texture2D)
{
	STUBBED();
}

uint32_t BaseRenderModels::GetRenderModelName(uint32_t unRenderModelIndex, VR_OUT_STRING() char* pchRenderModelName, uint32_t unRenderModelNameLen)
{
	const char* renderModelName = nullptr;
	uint32_t strLen = 0;

	switch (unRenderModelIndex) {
	case 0:
		renderModelName = "renderLeftHand";
		strLen = strlen(renderModelName);
		break;
	case 1:
		renderModelName = "renderRightHand";
		strLen = strlen(renderModelName);
		break;
	default:
		break;
	}

	if (pchRenderModelName && renderModelName && unRenderModelNameLen > 0) {
		strncpy(pchRenderModelName, renderModelName, unRenderModelNameLen - 1);
		pchRenderModelName[unRenderModelNameLen - 1] = '\0';
	}

	return strLen;
}

uint32_t BaseRenderModels::GetRenderModelCount()
{
	return 2;
}

uint32_t BaseRenderModels::GetComponentCount(const char* pchRenderModelName)
{
	// Left at zero for now until I can properly test it, and add textures
	return oovr_global_configuration.RenderCustomHands() ? 1 : 0;

	// This means there are no moving components (eg buttons thumbstick etc) which
	//  can be animated via the Component functions, which thus shouldn't be called.
}

uint32_t BaseRenderModels::GetComponentName(const char* pchRenderModelName, uint32_t unComponentIndex,
    char* pchComponentName, uint32_t unComponentNameLen)
{

	string name = pchRenderModelName;
	// Any name is acceptable here — SteamVR-passthrough models (2026-07-25)
	// mean the set of valid names is whatever exists in the user's
	// rendermodels folder, so the old allowlist abort is gone.

	// Only the first component exists
	if (unComponentIndex != 0) {
		return 0;
	}

	if (pchComponentName) {
		// +1 for NULL
		if (unComponentNameLen < name.length() + 1) {
			OOVR_ABORT("unComponentNameLen too small!");
		}

		// TODO should we allow too small buffers?
		strcpy_s(pchComponentName, unComponentNameLen, name.c_str());
		pchComponentName[unComponentNameLen - 1] = 0;
	}

	// +1 for null
	return (uint32_t)name.length() + 1;
}

uint64_t BaseRenderModels::GetComponentButtonMask(const char* pchRenderModelName, const char* pchComponentName)
{
	return 0;
}

uint32_t BaseRenderModels::GetComponentRenderModelName(const char* pchRenderModelName, const char* pchComponentName,
    char* componentModelName, uint32_t componentModelNameLen)
{

	string name = pchRenderModelName;
	if (name != "renderLeftHand"
	    && name != "renderRightHand"
	    && name != "oculusHmdRenderModel"
	    && name != "oculus_quest2_controller_left"
	    && name != "oculus_quest2_controller_right"
	    && name != "{indexcontroller}valve_controller_knu_1_0_left"
	    && name != "{indexcontroller}valve_controller_knu_1_0_right") {
		string err = "Unknown render model name: " + string(pchRenderModelName);
		OOVR_ABORT(err.c_str());
		return VRRenderModelError_None;
	}

	if (name != pchComponentName) {
		OOVR_ABORT("pchRenderModelName and pchComponentName mismatch");
	}

	if (componentModelName) {
		// +1 for NULL
		if (componentModelNameLen < name.length() + 1) {
			OOVR_ABORT("componentModelNameLen too small!");
		}

		// TODO should we allow too small buffers?
		strcpy_s(componentModelName, componentModelNameLen, name.c_str());
		componentModelName[componentModelNameLen - 1] = 0;
	}

	// +1 for null
	return (uint32_t)name.length() + 1;
}

bool BaseRenderModels::GetComponentStateForDevicePath(const char* pchRenderModelName, const char* pchComponentName,
    vr::VRInputValueHandle_t devicePath, const OOVR_RenderModel_ControllerMode_State_t* pState,
    OOVR_RenderModel_ComponentState_t* pComponentState)
{
	// todo: make use of devicePath
	return GetComponentState(pchRenderModelName, pchComponentName, nullptr, pState, pComponentState);
}

bool BaseRenderModels::GetComponentState(const char* pchRenderModelName, const char* pchComponentName,
    const vr::VRControllerState_t* pControllerState, const OOVR_RenderModel_ControllerMode_State_t* pState,
    OOVR_RenderModel_ComponentState_t* pComponentState)
{
	// On the Quest 2 Touch controllers, here's SteamVR's poses for the base, handgrip and tip components:
	// Left  base:     mat4x4((-1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.999976, 0.006981, 0.000000), (-0.000000, 0.006981, -0.999976, 0.000000), (-0.003400, -0.003400, 0.149100, 1.000000))
	// Right base:     mat4x4((-1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.999976, 0.006981, 0.000000), (-0.000000, 0.006981, -0.999976, 0.000000), (0.003400, -0.003400, 0.149100, 1.000000))
	// Left  handgrip: mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.996138, 0.087799, 0.000000), (0.000000, -0.087799, 0.996138, 0.000000), (0.000000, 0.003000, 0.097000, 1.000000))
	// Right handgrip: mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.996138, 0.087799, 0.000000), (0.000000, -0.087799, 0.996138, 0.000000), (0.000000, 0.003000, 0.097000, 1.000000))
	// Left  tip:      mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.794415, -0.607376, 0.000000), (0.000000, 0.607376, 0.794415, 0.000000), (0.016694, -0.025220, 0.024687, 1.000000))
	// Right tip:      mat4x4((1.000000, 0.000000, 0.000000, 0.000000), (0.000000, 0.794415, -0.607376, 0.000000), (0.000000, 0.607376, 0.794415, 0.000000), (-0.016694, -0.025220, 0.024687, 1.000000))

	ZeroMemory(pComponentState, sizeof(*pComponentState));

	std::string componentName = pchComponentName;

	ITrackedDevice::HandType hand = ITrackedDevice::HAND_NONE;
	if (pchRenderModelName) {
		std::string renderModelName = pchRenderModelName;
		if (renderModelName == "renderLeftHand") {
			hand = ITrackedDevice::HAND_LEFT;
		} else if (renderModelName == "renderRightHand") {
			hand = ITrackedDevice::HAND_RIGHT;
		} else {
			hand = ITrackedDevice::HAND_NONE;
		}
	}

	// See if we can get it properly
	bool success = TryGetComponentState(hand, componentName, pComponentState);
	if (success)
		return true;

	// Log a warning about this component, but only do so once
	if (!warnedAboutComponents.contains(componentName)) {
		OOVR_LOGF("Unknown component %s - returning a fake identity component", componentName.c_str());
		warnedAboutComponents.insert(componentName);
	}

	vr::HmdMatrix34_t ident = { 0 };
	ident.m[0][0] = ident.m[1][1] = ident.m[2][2] = 1;

	pComponentState->mTrackingToComponentLocal = ident;
	pComponentState->mTrackingToComponentRenderModel = ident;

	pComponentState->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;

	return true;
}

bool BaseRenderModels::TryGetComponentState(ITrackedDevice::HandType hand, const std::string& componentName, OOVR_RenderModel_ComponentState_t* result)
{
	if (hand == ITrackedDevice::HAND_NONE)
		return false;

	ITrackedDevice* dev = BackendManager::Instance().GetDeviceByHand(hand);
	if (!dev)
		return false;

	// See if there's a manually-defined transform
	const InteractionProfile* profile = dev->GetInteractionProfile();
	if (profile) {
		std::optional<glm::mat4> transform = profile->GetComponentTransform(hand, componentName);
		if (transform) {
			result->mTrackingToComponentLocal = G2S_m34(transform.value());
			result->mTrackingToComponentRenderModel = G2S_m34(transform.value());
			result->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
			return true;
		}
	}

	// If the hand position is offset, then this needs to account for it
	glm::mat4 handToGripSpace = glm::identity<glm::mat4>();
	if (profile) {
		handToGripSpace = glm::affineInverse(profile->GetGripToSteamVRTransform(hand));
	}

	// The grip space is simple - it's just the hand-to-grip transform matrix
	if (componentName == "handgrip") {
		result->mTrackingToComponentLocal = G2S_m34(handToGripSpace);
		result->mTrackingToComponentRenderModel = G2S_m34(handToGripSpace);
		result->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
		return true;
	}

	// If it's not manually specified, calculate the 'tip' position to match up with the OpenXR aim pose.
	if (componentName != "tip") {
		return false;
	}

	XrSpace componentSpace, gripSpace;
	GetBaseInput()->GetHandSpace(hand, componentSpace, true);
	GetBaseInput()->GetHandSpace(hand, gripSpace, false);

	if (!componentSpace || !gripSpace) {
		return false;
	}

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	OOVR_FAILED_XR_ABORT(xrLocateSpace(componentSpace, gripSpace, xr_gbl->GetBestTime(), &location));

	if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
		// If the location is invalid, there's not really a lot we can do. Just use the
		// last known position as is currently in pose.
		OOVR_LOG_ONCE("Relative component location is not valid");
	}

	glm::mat4 gripToComponentSpace = X2G_om34_pose(location.pose);
	glm::mat4 handToComponentSpace = handToGripSpace * gripToComponentSpace;

	result->mTrackingToComponentLocal = G2S_m34(handToComponentSpace);
	result->mTrackingToComponentRenderModel = G2S_m34(handToComponentSpace);
	result->uProperties = VRComponentProperty_IsVisible | VRComponentProperty_IsStatic;
	return true;
}

bool BaseRenderModels::RenderModelHasComponent(const char* pchRenderModelName, const char* pchComponentName)
{
	STUBBED();
}

uint32_t BaseRenderModels::GetRenderModelThumbnailURL(const char* pchRenderModelName, VR_OUT_STRING() char* pchThumbnailURL, uint32_t unThumbnailURLLen, EVRRenderModelError* peError)
{
	if (peError)
		*peError = VRRenderModelError_None;

	STUBBED();
}

uint32_t BaseRenderModels::GetRenderModelOriginalPath(const char* pchRenderModelName, VR_OUT_STRING() char* pchOriginalPath, uint32_t unOriginalPathLen, EVRRenderModelError* peError)
{
	if (peError)
		*peError = VRRenderModelError_None;

	STUBBED();
}

const char* BaseRenderModels::GetRenderModelErrorNameFromEnum(EVRRenderModelError error)
{
#define ERR_HND(name)               \
	case VRRenderModelError_##name: \
		return #name;

	switch (error) {
		ERR_HND(None)
		ERR_HND(Loading)
		ERR_HND(NotSupported)
		ERR_HND(InvalidArg)
		ERR_HND(InvalidModel)
		ERR_HND(NoShapes)
		ERR_HND(MultipleShapes)
		ERR_HND(TooManyVertices)
		ERR_HND(MultipleTextures)
		ERR_HND(BufferTooSmall)
		ERR_HND(NotEnoughNormals)
		ERR_HND(NotEnoughTexCoords)
		ERR_HND(InvalidTexture)
	default:
		OOVR_ABORTF("Unknown render model error ID=%d", error);
	}
#undef ERR_HND
}
