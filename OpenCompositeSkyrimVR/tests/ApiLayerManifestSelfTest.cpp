// Self-test for xrlayers/ manifest discovery.
//
// DrvOpenXR::ReadApiLayerManifest can't be linked directly - it is static, and reaching it would
// mean dragging in the whole OpenXR backend. Instead this runs the fixtures through the same
// vendored jsoncpp with the same predicates, and copies the two containment helpers verbatim.
// The duplication is the point: if the production code changes and this stops agreeing with it,
// the fixtures table in docs/API-LAYERS-TESTING.md is what needs revisiting.
//
// Build (excluded from normal builds):  cmake --build <dir> --target OCUApiLayerManifestSelfTest
// Run:                                  <dir>/tests/OCUApiLayerManifestSelfTest.exe

#include <json/json.h>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef OCU_FIXTURE_DIR
#define OCU_FIXTURE_DIR "xrlayers-fixtures"
#endif

static int failures = 0;

// ── Copied verbatim from DrvOpenXR.cpp ────────────────────────────────────────────────────────

static std::string CanonicalPath(const std::string& path)
{
	char full[MAX_PATH]{};
	const DWORD len = GetFullPathNameA(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
	if (len == 0 || len >= std::size(full))
		return {};
	return full;
}

static bool PathIsWithin(const std::string& root, const std::string& path)
{
	if (root.empty() || path.size() <= root.size())
		return false;
	if (_strnicmp(root.c_str(), path.c_str(), root.size()) != 0)
		return false;
	return path[root.size()] == '\\' || path[root.size()] == '/';
}

// ── Manifest validation, mirroring ReadApiLayerManifest's rejection order ──────────────────────

// The short tag each fixture is expected to produce.
static std::string Classify(const std::string& path)
{
	std::ifstream stream(path);
	if (!stream.is_open())
		return "cannot-open";

	Json::CharReaderBuilder builder;
	Json::Value root;
	std::string errors;
	if (!Json::parseFromStream(builder, stream, &root, &errors) || root.isNull())
		return "bad-json";

	const Json::Value& layer = root["api_layer"];
	if (!layer.isObject() || !layer["name"].isString() || !layer["library_path"].isString())
		return "bad-fields";

	const std::string name = layer["name"].asString();
	const std::string lib = layer["library_path"].asString();
	if (name.empty() || lib.empty())
		return "empty-field";

	if (lib.find_first_of("\\/") == std::string::npos)
		return "bare-filename";

	return "parsed";
}

// ocu_priority as ReadApiLayerManifest reads it: absent or the wrong type both mean the default.
static int ClassifyPriority(const std::string& path)
{
	std::ifstream stream(path);
	Json::CharReaderBuilder builder;
	Json::Value root;
	std::string errors;
	if (!stream.is_open() || !Json::parseFromStream(builder, stream, &root, &errors))
		return 100;

	const Json::Value& value = root["api_layer"]["ocu_priority"];
	return value.isInt() ? value.asInt() : 100;
}

struct FixtureCase {
	const char* file;
	const char* expected;
	int priority;
};

// Every fixture, and the stage it must be rejected at. "parsed" means it survives parsing and
// goes on to the containment and existence checks, which need a real game folder.
static const FixtureCase kFixtures[] = {
	{ "01-valid-reference.json", "parsed", 100 },
	{ "02-malformed-json.json", "bad-json", 100 },
	{ "03-missing-fields.json", "bad-fields", 100 },
	{ "04-no-api-layer-node.json", "bad-fields", 100 },
	{ "05-missing-dll.json", "parsed", 100 },
	{ "06-escapes-game-root.json", "parsed", 100 },
	{ "07-absolute-outside.json", "parsed", 100 },
	{ "08-bare-filename.json", "bare-filename", 100 },
	{ "09-empty-name.json", "empty-field", 100 },
	{ "10-wrong-types.json", "bad-fields", 100 },
	{ "11-empty-file.json", "bad-json", 100 },
	{ "12-priority-explicit.json", "parsed", 25 },
	{ "13-priority-wrong-type.json", "parsed", 100 },
};

static void ManifestTests()
{
	printf("Manifest validation (%s)\n", OCU_FIXTURE_DIR);
	for (const FixtureCase& c : kFixtures) {
		const std::string full = std::string(OCU_FIXTURE_DIR) + "\\" + c.file;
		const std::string got = Classify(full);
		const int priority = ClassifyPriority(full);
		const bool ok = got == c.expected && priority == c.priority;
		printf("  %-30s %-14s prio %-4d %s\n", c.file, got.c_str(), priority,
		    ok ? "ok" : "*** FAIL ***");
		if (!ok) {
			printf("      expected %s prio %d\n", c.expected, c.priority);
			failures++;
		}
	}
}

// ── Containment ───────────────────────────────────────────────────────────────────────────────

static void Expect(const char* what, const std::string& root, const std::string& raw, bool want)
{
	const bool got = PathIsWithin(root, CanonicalPath(raw));
	printf("  %-52s %-8s %s\n", what, got ? "INSIDE" : "OUTSIDE", got == want ? "ok" : "*** FAIL ***");
	if (got != want)
		failures++;
}

static void ContainmentTests()
{
	const std::string root = "C:\\Game\\SkyrimVR";
	printf("\nContainment (game folder %s)\n", root.c_str());

	Expect("xrlayers\\.\\layer.dll", root, root + "\\xrlayers\\.\\layer.dll", true);
	Expect("xrlayers\\..\\Data\\SKSE\\Plugins\\mod.dll (SKSE hybrid)", root,
	    root + "\\xrlayers\\..\\Data\\SKSE\\Plugins\\mod.dll", true);
	Expect("deep subdirectory", root, root + "\\a\\b\\c\\d.dll", true);
	Expect("lowercase drive, mixed case elsewhere", root, "c:\\game\\skyrimvr\\xrlayers\\x.dll", true);
	Expect("forward slashes", root, root + "/xrlayers/x.dll", true);

	Expect("..\\..\\..\\Windows\\System32\\version.dll", root,
	    root + "\\xrlayers\\..\\..\\..\\Windows\\System32\\version.dll", false);
	Expect("absolute C:\\Windows\\System32\\version.dll", root, "C:\\Windows\\System32\\version.dll", false);
	Expect("sibling sharing a prefix, C:\\Game\\SkyrimVROther\\x.dll", root,
	    "C:\\Game\\SkyrimVROther\\x.dll", false);
	Expect("the root itself, not a child of it", root, root, false);
	Expect("the root's parent", root, "C:\\Game\\x.dll", false);
}

int main()
{
	ManifestTests();
	ContainmentTests();
	printf("\n%s\n", failures == 0 ? "PASS - all cases as expected" : "FAIL");
	return failures == 0 ? 0 : 1;
}
