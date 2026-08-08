//
// Created by ZNix on 25/10/2020.
//

#include "DrvOpenXR.h"

#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/android_api.h"
#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "ASWProvider.h"
#include "SpaceWarpProvider.h"
#include "XrBackend.h"
#include "generated/static_bases.gen.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

static XrBackend* currentBackend;
static bool initialised = false;
static std::shared_ptr<BaseInput> sessionInputKeepalive;

#ifdef _WIN32
// xrEnumerateApiLayerProperties only reports EXPLICIT layers. The layers that
// silently wrap every xr* call — the Vive/Oculus/WMR runtime compat shims that
// cause teardown crashes and phantom device identity — are IMPLICIT and invisible
// to the app. Read them straight from the OpenXR loader's registry so they land in
// our log for support. (yujujo, 2026-07-05: five foreign implicit layers on an Index.)
static void LogImplicitOpenXRLayersFromHive(HKEY root, const char* rootName)
{
	HKEY key;
	if (RegOpenKeyExA(root, "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS)
		return;

	char valueName[1024];
	for (DWORD idx = 0;; ++idx) {
		DWORD nameLen = sizeof(valueName);
		DWORD type = 0, data = 0, dataLen = sizeof(data);
		LSTATUS st = RegEnumValueA(key, idx, valueName, &nameLen, nullptr, &type, (LPBYTE)&data, &dataLen);
		if (st != ERROR_SUCCESS)
			break; // ERROR_NO_MORE_ITEMS or failure — done

		// Loader convention (matches Vulkan): DWORD 0 = enabled, non-zero = disabled.
		bool enabled = (type == REG_DWORD) ? (data == 0) : true;

		std::string lower = valueName;
		for (char& c : lower)
			if (c >= 'A' && c <= 'Z')
				c += 32;
		bool foreign = lower.find("oculus") != std::string::npos || lower.find("meta") != std::string::npos
		    || lower.find("libovr") != std::string::npos || lower.find("vive") != std::string::npos
		    || lower.find("htc") != std::string::npos || lower.find("mixedreality") != std::string::npos;

		OOVR_LOGF("Implicit OpenXR layer [%s]: %s (%s%s)", rootName, valueName,
		    enabled ? "ENABLED" : "disabled",
		    (enabled && foreign) ? " -- FOREIGN RUNTIME, possible teardown/tracking conflict" : "");
	}
	RegCloseKey(key);
}

static void LogImplicitOpenXRLayers()
{
	OOVR_LOG("Enumerating IMPLICIT OpenXR API layers from registry (invisible to xrEnumerateApiLayerProperties):");
	LogImplicitOpenXRLayersFromHive(HKEY_LOCAL_MACHINE, "HKLM");
	LogImplicitOpenXRLayersFromHive(HKEY_CURRENT_USER, "HKCU");
}
#else
static void LogImplicitOpenXRLayers() {}
#endif

// OpenXR API layers shipped alongside OCU: any manifest (*.json) in the xrlayers/ folder next to
// this DLL is discovered and enabled. A manifest plus the DLL it names is the entire contract -
// no layer is known by name at compile time. See docs/API-LAYERS.md.
//
// Three loader behaviours this depends on, all learned the hard way:
//   * XR_API_LAYER_PATH must be set before xrEnumerateApiLayerProperties, not merely before
//     xrCreateInstance - the loader caches discovery on first enumeration.
//   * The loader reads it via PlatformUtilsGetSecureEnv, which returns nothing to a
//     high-integrity process. Under an elevated MO2 this silently finds nothing; the log lines
//     below are how you tell (a manifest found with no layer enabled means exactly this).
//   * Setting it suppresses the loader's registry search for explicit layers entirely, so
//     system-wide explicit layers are invisible while xrlayers/ exists.
//
// Every rejection below is logged and skipped. Naming a layer the loader did not report is the
// one fatal mistake available here - xrCreateInstance would return XR_ERROR_API_LAYER_NOT_PRESENT
// and take the game down - so the enable loop intersects against what it actually enumerated.

struct DiscoveredLayer {
	std::string manifest; // full path of the .json
	std::string name; // api_layer.name, the string xrCreateInstance wants
	std::string library; // resolved, canonical path of the layer's DLL
};

#ifdef _WIN32
// The directory part of a path, with "..\" and friends resolved. Empty if it can't be resolved.
static std::string CanonicalPath(const std::string& path)
{
	char full[MAX_PATH]{};
	const DWORD len = GetFullPathNameA(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
	if (len == 0 || len >= std::size(full))
		return {};
	return full;
}

static std::string DirectoryOf(const std::string& path)
{
	const size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

// Both canonical, both without a trailing separator. The boundary check stops C:\Game matching
// C:\GameOther.
static bool PathIsWithin(const std::string& root, const std::string& path)
{
	if (root.empty() || path.size() <= root.size())
		return false;
	if (_strnicmp(root.c_str(), path.c_str(), root.size()) != 0)
		return false;
	return path[root.size()] == '\\' || path[root.size()] == '/';
}

// Whole value, however long. Empty if unset - a fixed buffer would silently truncate, and the
// caller writes this variable back.
static std::string ReadEnvironmentVariable(const char* name)
{
	DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
	if (needed == 0)
		return {};
	std::string value(needed, '\0');
	const DWORD len = GetEnvironmentVariableA(name, value.data(), needed);
	if (len == 0 || len >= needed)
		return {};
	value.resize(len);
	return value;
}

// Is `entry` already one of the ';'-separated paths in `list`? Compared canonically, so casing,
// trailing separators and '/' vs '\' don't produce a spurious miss.
static bool PathListContains(const std::string& list, const std::string& entry)
{
	const std::string wanted = CanonicalPath(entry);
	if (wanted.empty())
		return false;

	for (size_t at = 0; at <= list.size();) {
		const size_t end = list.find(';', at);
		std::string one = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
		at = (end == std::string::npos) ? list.size() + 1 : end + 1;

		while (!one.empty() && (one.back() == '\\' || one.back() == '/'))
			one.pop_back();
		if (one.empty())
			continue;
		if (_stricmp(CanonicalPath(one).c_str(), wanted.c_str()) == 0)
			return true;
	}
	return false;
}

// The folder holding the running executable.
static std::string GameRootFolder()
{
	char exePath[MAX_PATH]{};
	if (GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
		return {};
	return CanonicalPath(DirectoryOf(exePath));
}

// The xrlayers folder next to this DLL, or empty if we can't work out where we live.
static std::string ApiLayerFolder()
{
	char dllPath[MAX_PATH]{};
	HMODULE self = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	        reinterpret_cast<LPCSTR>(&ApiLayerFolder), &self)
	    || GetModuleFileNameA(self, dllPath, static_cast<DWORD>(std::size(dllPath))) == 0) {
		OOVR_LOG("API layers: could not locate our own DLL, skipping layer discovery");
		return {};
	}

	const std::string dir = DirectoryOf(dllPath);
	if (dir.empty())
		return {};
	return dir + "\\xrlayers";
}

// Read one manifest and work out where its DLL is. A broken third-party layer must never stop the
// game starting, so everything here skips rather than aborts.
static bool ReadApiLayerManifest(const std::string& path, const std::string& gameRoot, DiscoveredLayer& out)
{
	std::ifstream stream(path);
	if (!stream.is_open()) {
		OOVR_LOGF("API layers: cannot open %s, skipping", path.c_str());
		return false;
	}

	Json::CharReaderBuilder builder;
	Json::Value root;
	std::string errors;
	if (!Json::parseFromStream(builder, stream, &root, &errors) || root.isNull()) {
		OOVR_LOGF("API layers: %s is not valid JSON (%s), skipping", path.c_str(), errors.c_str());
		return false;
	}

	const Json::Value& layer = root["api_layer"];
	if (!layer.isObject() || !layer["name"].isString() || !layer["library_path"].isString()) {
		OOVR_LOGF("API layers: %s has no api_layer.name / api_layer.library_path, skipping", path.c_str());
		return false;
	}

	const std::string name = layer["name"].asString();
	const std::string libraryPath = layer["library_path"].asString();
	if (name.empty() || libraryPath.empty()) {
		OOVR_LOGF("API layers: %s has an empty name or library_path, skipping", path.c_str());
		return false;
	}

	// The loader would accept a bare filename here and hand it to the normal DLL search order,
	// which reaches well outside the game folder. We can't confine that, so we don't allow it.
	if (libraryPath.find_first_of("\\/") == std::string::npos) {
		OOVR_LOGF("API layers: %s gives library_path '%s' as a bare filename - it must be a path"
		          " (e.g. \".\\\\%s\"), skipping layer '%s'",
		    path.c_str(), libraryPath.c_str(), libraryPath.c_str(), name.c_str());
		return false;
	}

	// Absolute as-is, otherwise relative to the manifest - the loader's own rule.
	const bool absolute = (libraryPath.size() >= 2 && libraryPath[1] == ':')
	    || (libraryPath.size() >= 2 && (libraryPath[0] == '\\' || libraryPath[0] == '/')
	        && (libraryPath[1] == '\\' || libraryPath[1] == '/'));
	const std::string resolved = CanonicalPath(absolute ? libraryPath : DirectoryOf(path) + "\\" + libraryPath);
	if (resolved.empty()) {
		OOVR_LOGF("API layers: %s gives library_path '%s', which does not resolve to a real path,"
		          " skipping layer '%s'",
		    path.c_str(), libraryPath.c_str(), name.c_str());
		return false;
	}

	// A layer must ship inside the game folder, so it stays within the mod manager's view and an
	// uninstall is complete. A sanity boundary, not a security one - a junction would defeat it.
	if (!PathIsWithin(gameRoot, resolved)) {
		OOVR_LOGF("API layers: %s points at %s, which is outside the game folder (%s) -"
		          " skipping layer '%s'",
		    path.c_str(), resolved.c_str(), gameRoot.c_str(), name.c_str());
		return false;
	}

	// The loader would reject this manifest too, so say so - a half-installed mod otherwise looks
	// like a layer that is present and silently doing nothing.
	if (GetFileAttributesA(resolved.c_str()) == INVALID_FILE_ATTRIBUTES) {
		OOVR_LOGF("API layers: %s names %s, which does not exist - skipping layer '%s'",
		    path.c_str(), resolved.c_str(), name.c_str());
		return false;
	}

	out.manifest = path;
	out.name = name;
	out.library = resolved;
	return true;
}

// Find every manifest in the folder and put the loader's search path in place. Sorted by
// filename so the chain order is deterministic and a mod can steer it with a numeric prefix;
// earlier files sit nearer the application, later ones nearer the runtime.
static std::vector<DiscoveredLayer> DiscoverApiLayers()
{
	std::vector<DiscoveredLayer> found;

	const std::string layerDir = ApiLayerFolder();
	if (layerDir.empty())
		return found;

	const std::string gameRoot = GameRootFolder();
	if (gameRoot.empty()) {
		OOVR_LOG("API layers: could not determine the game folder, skipping layer discovery");
		return found;
	}

	if (GetFileAttributesA(layerDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
		OOVR_LOGF("API layers: no xrlayers folder at %s - none installed", layerDir.c_str());
		return found;
	}

	std::vector<std::string> manifests;
	WIN32_FIND_DATAA entry{};
	const HANDLE search = FindFirstFileA((layerDir + "\\*.json").c_str(), &entry);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				manifests.push_back(layerDir + "\\" + entry.cFileName);
		} while (FindNextFileA(search, &entry));
		FindClose(search);
	}
	std::sort(manifests.begin(), manifests.end());

	if (manifests.empty()) {
		OOVR_LOGF("API layers: %s exists but holds no manifests - none installed", layerDir.c_str());
		return found;
	}

	// Prepend rather than overwrite - other tools (and OCU's own validation-layer path) use this.
	//
	// The already-listed test compares canonicalised entries, not raw text. It has to: the loader
	// deduplicates manifests by neither name nor path, so listing the same folder twice inserts
	// every layer in it into the chain twice, and two copies of one layer DLL share its globals.
	// Someone who set XR_API_LAYER_PATH by hand to debug a layer - the likeliest way this variable
	// is ever already set - would rarely type the exact casing GetModuleFileName returns.
	const std::string existing = ReadEnvironmentVariable("XR_API_LAYER_PATH");
	if (PathListContains(existing, layerDir)) {
		OOVR_LOGF("API layers: %s already on XR_API_LAYER_PATH", layerDir.c_str());
	} else {
		const std::string value = existing.empty() ? layerDir : layerDir + ";" + existing;
		if (SetEnvironmentVariableA("XR_API_LAYER_PATH", value.c_str()))
			OOVR_LOGF("API layers: XR_API_LAYER_PATH = %s", value.c_str());
		else
			OOVR_LOG("API layers: failed to set XR_API_LAYER_PATH - no layers will be found");
	}

	for (const std::string& path : manifests) {
		DiscoveredLayer layer;
		if (!ReadApiLayerManifest(path, gameRoot, layer))
			continue;
		OOVR_LOGF("API layers: manifest %s -> layer '%s' (%s)", path.c_str(), layer.name.c_str(),
		    layer.library.c_str());
		found.push_back(std::move(layer));
	}

	return found;
}
#else
static std::vector<DiscoveredLayer> DiscoverApiLayers() { return {}; }
#endif

#ifdef _WIN32
std::string GetExeName()
{
	char exePath[MAX_PATH + 1] = { 0 };
	DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	PathStripPathA(exePath);
	return { exePath };
}
#else
#include <libgen.h> // basename
#include <linux/limits.h> // PATH_MAX
#include <unistd.h> // readlink

std::string GetExeName()
{
	char exePath[PATH_MAX + 1] = { 0 };
	ssize_t count = readlink("/proc/self/exe", exePath, PATH_MAX);
	if (count != -1) {
		return { basename(exePath) };
	}
	return { "" };
}
#endif

void DrvOpenXR::GetXRAppName(char (&appName)[128])
{
	std::string exeName = GetExeName();
	if (exeName.size() > 0) {
		std::string ocAppName{ "OpenComposite_" };
		ocAppName += exeName;
		// Strip .exe so VD can match game profiles but Steam won't auto-launch SteamVR
		auto pos = ocAppName.find(".exe");
		if (pos != std::string::npos && pos > 0)
			ocAppName = ocAppName.substr(0, pos);
		OOVR_LOGF("Setting application name to %s", ocAppName.c_str());
		strcpy_arr(appName, ocAppName.c_str());
	} else {
		strcpy_arr(appName, "OpenComposite");
	}
}

#ifdef _DEBUG
static XrDebugUtilsMessengerEXT dbgMessenger = NULL;

static XrBool32 XRAPI_CALL debugCallback(
    XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
    XrDebugUtilsMessageTypeFlagsEXT messageTypes,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
	OOVR_LOGF("debugCallback %s", callbackData->message);
	return XR_FALSE;
}
#endif

static void CreateSystemID()
{
	// Create a system - this is when we choose what form factor we want, in this case an HMD
	XrSystemGetInfo systemInfo{};
	systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	OOVR_FAILED_XR_ABORT(xrGetSystem(xr_instance, &systemInfo, &xr_system));
}

IBackend* DrvOpenXR::CreateOpenXRBackend()
{
	// TODO handle something like Unity which stops and restarts the instance
	if (initialised) {
		OOVR_ABORT("Cannot double-initialise OpenXR");
	}
	initialised = true;

	// TODO make these work on Linux
#ifdef XR_VALIDATION_LAYER_PATH
	OOVR_FALSE_ABORT(SetEnvironmentVariableA("XR_CORE_VALIDATION_EXPORT_TYPE", "text"));
	OOVR_FALSE_ABORT(SetEnvironmentVariableA("XR_API_LAYER_PATH", XR_VALIDATION_LAYER_PATH));
	OOVR_LOGF("Set OpenXR Layer path: %s", XR_VALIDATION_LAYER_PATH);
#endif
#ifdef XR_VALIDATION_FILE_NAME
	OOVR_FALSE_ABORT(SetEnvironmentVariableA("XR_CORE_VALIDATION_FILE_NAME", XR_VALIDATION_FILE_NAME));
	OOVR_LOGF("Set OpenXR validation file path: %s", XR_VALIDATION_FILE_NAME);
#endif

	// Has to precede the enumeration below - the loader caches layer discovery on first use
	std::vector<DiscoveredLayer> discoveredLayers;
	if (oovr_global_configuration.EnableApiLayers())
		discoveredLayers = DiscoverApiLayers();
	else
		OOVR_LOG("API layers: disabled by enableApiLayers=false");

	// Enumerate the available extensions
	uint32_t availableExtensionsCount;
	OOVR_FAILED_XR_ABORT(xrEnumerateInstanceExtensionProperties(nullptr, 0, &availableExtensionsCount, nullptr));
	std::vector<XrExtensionProperties> extensionProperties;
	extensionProperties.resize(availableExtensionsCount, { XR_TYPE_EXTENSION_PROPERTIES });
	OOVR_FAILED_XR_ABORT(xrEnumerateInstanceExtensionProperties(nullptr,
	    extensionProperties.size(), &availableExtensionsCount, extensionProperties.data()));
	std::set<std::string> availableExtensions;
	for (const XrExtensionProperties& ext : extensionProperties) {
		availableExtensions.insert(ext.extensionName);
		OOVR_LOGF("Extension: %s", ext.extensionName);
	}

	uint32_t availableLayersCount;
	OOVR_FAILED_XR_ABORT(xrEnumerateApiLayerProperties(0, &availableLayersCount, nullptr));
	OOVR_LOGF("Num layers available: %d ", availableLayersCount);
	std::vector<XrApiLayerProperties> layerProperties;
	layerProperties.resize(availableLayersCount, { XR_TYPE_API_LAYER_PROPERTIES });
	OOVR_FAILED_XR_ABORT(xrEnumerateApiLayerProperties(
	    layerProperties.size(), &availableLayersCount, layerProperties.data()));
	std::set<std::string> availableLayers;
	for (const XrApiLayerProperties& layer : layerProperties) {
		availableLayers.insert(layer.layerName);
		OOVR_LOGF("Layer (explicit): %s", layer.layerName);
	}

	// Explicit layers above are only half the story — log the implicit ones too.
	LogImplicitOpenXRLayers();

	// Create the OpenXR instance - this is the overall handle that connects us to the runtime
	// https://www.khronos.org/registry/OpenXR/specs/1.0/refguide/openxr-10-reference-guide.pdf
	XrApplicationInfo appInfo{};
	GetXRAppName(appInfo.applicationName);
	appInfo.applicationVersion = 1;
	appInfo.apiVersion = XR_CURRENT_API_VERSION;

	std::vector<const char*> extensions;
	XrGraphicsApiSupportedFlags apiFlags = 0;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (availableExtensions.count("XR_KHR_D3D11_enable")) {
		extensions.push_back("XR_KHR_D3D11_enable");
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_D3D11;
	}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	if (availableExtensions.count("XR_KHR_D3D12_enable")) {
		extensions.push_back("XR_KHR_D3D12_enable");
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_D3D12;
	}
#endif
#if defined(SUPPORT_VK)
	if (availableExtensions.count(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
		extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_VK;
	}
#endif
#if defined(SUPPORT_GL)
	if (availableExtensions.count(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
		extensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_GL;
	}
#endif
#if defined(SUPPORT_GLES)
	if (availableExtensions.count(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
		extensions.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_GLES;
	}
#endif
#if defined(ANDROID)
	if (availableExtensions.count(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
	if (availableExtensions.count(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
		extensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

	// If the visibility mask is available use it, otherwise no big deal
	if (availableExtensions.count(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME))
		extensions.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);

	if (availableExtensions.count(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
		extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

	if (availableExtensions.contains(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME))
		extensions.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);

	// Body trackers: VDXR exposes Virtual Desktop's body tracking through this,
	// SteamVR's OpenXR runtime exposes real Vive/Tundra/SlimeVR trackers the same way
	if (availableExtensions.count("XR_HTCX_vive_tracker_interaction")) {
		extensions.push_back("XR_HTCX_vive_tracker_interaction");
		xr_htcxViveTrackers = true;
	}

	// XR_FB_space_warp — runtime-side ASW (Meta Quest via Link/AirLink)
	if (availableExtensions.count("XR_FB_space_warp")) {
		extensions.push_back("XR_FB_space_warp");
		g_spaceWarpAvailable = true;
		OOVR_LOG("XR_FB_space_warp extension available and enabled");
	}

	std::vector<const char*> layers;
#ifdef XR_VALIDATION_LAYER_PATH
	layers.push_back("XR_APILAYER_LUNARG_core_validation");
#endif

	// An explicit layer does nothing until the application names it, so this loop is the switch.
	// Only ever name one the loader reported - see the fatal case above. The c_str()s stay valid
	// because discoveredLayers is not touched again before xrCreateInstance returns.
	int enabledLayers = 0;
	for (const DiscoveredLayer& layer : discoveredLayers) {
		if (availableLayers.count(layer.name)) {
			layers.push_back(layer.name.c_str());
			enabledLayers++;
			OOVR_LOGF("API layers: enabling '%s'", layer.name.c_str());
		} else {
			OOVR_LOGF("API layers: '%s' (%s) was not picked up by the loader, skipping it",
			    layer.name.c_str(), layer.manifest.c_str());
		}
	}
	if (!discoveredLayers.empty())
		OOVR_LOGF("API layers: %d of %d enabled, in application-first order",
		    enabledLayers, (int)discoveredLayers.size());

	XrInstanceCreateInfo createInfo{};
	createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	createInfo.applicationInfo = appInfo;
	createInfo.enabledExtensionNames = extensions.data();
	createInfo.enabledExtensionCount = extensions.size();
	createInfo.enabledApiLayerNames = layers.data();
	createInfo.enabledApiLayerCount = layers.size();

#if ANDROID
	if (!OpenComposite_Android_Create_Info) {
		OOVR_ABORT("Cannot create OpenXR instance - OpenComposite_Android_Create_Info not set");
	}
	XrInstanceCreateInfoAndroidKHR androidInfo = *OpenComposite_Android_Create_Info;
	androidInfo.next = nullptr;
	createInfo.next = &androidInfo;
#endif

	OOVR_FAILED_XR_ABORT(xrCreateInstance(&createInfo, &xr_instance));

#ifdef _DEBUG
	XrDebugUtilsMessengerCreateInfoEXT dbgCreateInfo{};
	dbgCreateInfo.type = XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dbgCreateInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	dbgCreateInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
	dbgCreateInfo.next = NULL;
	dbgCreateInfo.userData = NULL;
	if (dbgMessenger != NULL) {
		OOVR_FAILED_XR_ABORT(xrDestroyDebugUtilsMessengerEXT(dbgMessenger));
		dbgMessenger = NULL;
	}
	dbgCreateInfo.userCallback = debugCallback;
	OOVR_FAILED_XR_ABORT(xrCreateDebugUtilsMessengerEXT(xr_instance, &dbgCreateInfo, &dbgMessenger));
#endif

	// Load the function pointers for the extension functions
	xr_ext = new XrExt(apiFlags, extensions);

	CreateSystemID();

	// List off the views and store them locally for easy access
	uint32_t viewCount = 0;
	OOVR_FAILED_XR_ABORT(xrEnumerateViewConfigurationViews(xr_instance, xr_system,
	    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));

	xr_views_list = std::vector<XrViewConfigurationView>(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });

	OOVR_FAILED_XR_ABORT(xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    viewCount, &viewCount, xr_views_list.data()));

	OOVR_FALSE_ABORT(viewCount == xr_views_list.size());

	// Create a session - this tells the runtime that sooner or later we'd like to submit frames
	// This is when we have to choose what graphics API to use

	bool useVulkanTmpGfx = (apiFlags & XR_SUPPORTED_GRAPHICS_API_VK) && oovr_global_configuration.InitUsingVulkan();
	bool useD3D11TmpGfx = (apiFlags & XR_SUPPORTED_GRAPHICS_API_D3D11);

#if !defined(SUPPORT_VK) && !defined(SUPPORT_DX) && !defined(SUPPORT_DX11)
#error No available temporary graphics implementation
#endif

	// Build a backend that works with OpenXR
	currentBackend = new XrBackend(useVulkanTmpGfx, useD3D11TmpGfx);

	// Setup our OpenXR session
	SetupSession();

	return currentBackend;
}

void DrvOpenXR::SetupSession()
{
	// SetupSession is used to restart the session, and as such, we want to prevent other threads from attempting to use the session
	// while we're rebuilding it (otherwise we get gross nondescript crashes), so we will put a lock on it here.
	auto lock = xr_session.lock();
	if (xr_gbl) {
		ShutdownSession();
	}

	XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

	XrSessionCreateInfo sessionInfo{};
	sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionInfo.systemId = xr_system;
	sessionInfo.next = currentBackend->GetCurrentGraphicsBinding();
	OOVR_FAILED_XR_ABORT(xrCreateSession(xr_instance, &sessionInfo, &xr_session.get()));

	// Setup the OpenXR globals, which uses the current session so we have to do this last
	xr_gbl = new XrSessionGlobals();

	// Print the current version for diagnostic purposes
	OOVR_LOGF("Started OpenXR session on runtime '%s', hand tracking supported: %d",
	    xr_gbl->systemProperties.systemName, xr_gbl->handTrackingProperties.supportsHandTracking);

	// Attach inputs early so implicit layers (like VD) see an attached
	// action set before the first xrSyncActions / xrWaitFrame.
	auto inputShared = GetBaseInput();
	if (!inputShared) {
		inputShared = GetCreateBaseInput();
	}
	sessionInputKeepalive = inputShared;
	BaseInput* input = inputShared.get();
	if (input) {
		if (!input->AreActionsLoaded()) {
			input->LoadEmptyManifestIfRequired(false);
		} else {
			input->BindInputsForSession();
		}
	}

	currentBackend->OnSessionCreated();
}

void DrvOpenXR::ShutdownSession()
{
	BackendManager* instance = BackendManager::InstancePtr();
	// Is it already being shut down?
	// Note that this is indirectly called by the XrBackend destructor, which will have
	// already called this function in that case.
	if (instance) {
		auto* backend = (XrBackend*)instance->GetBackendInstance();
		if (backend)
			backend->PrepareForSessionShutdown();
	}

	if (currentBackend->sessionActive) {
		// Hey it turns out that xrDestroySession can be called whenever - how convenient
		OOVR_FAILED_XR_ABORT(xrRequestExitSession(xr_session.get()));
		int count = 0;
		while (currentBackend->GetSessionState() != XR_SESSION_STATE_EXITING && count++ < 10) {
			OOVR_LOGF("currentBackend: %d", currentBackend->GetSessionState());
			const int durationMs = 250;
			OOVR_LOGF("Session Exit state has not been reached yet, waiting %dms ...", durationMs);
#ifdef _WIN32
			Sleep(durationMs);
#else
			struct timespec ts = { 0, durationMs * 1000000 };
			nanosleep(&ts, &ts);
#endif
			currentBackend->PumpEvents();
		}
	}

	OOVR_FAILED_XR_ABORT(xrDestroySession(xr_session.get()));
	xr_session.reset();

	// Delete xr_gbl AFTER session is fully destroyed — PumpEvents() and other
	// shutdown code may still reference xr_gbl->seatedSpace, nextPredictedFrameTime, etc.
	delete xr_gbl;
	xr_gbl = nullptr;

	CreateSystemID();
}

void DrvOpenXR::FullShutdown()
{
	if (xr_session.get())
		ShutdownSession();

#ifdef _DEBUG
	if (dbgMessenger != NULL) {
		OOVR_FAILED_XR_ABORT(xrDestroyDebugUtilsMessengerEXT(dbgMessenger));
		dbgMessenger = NULL;
	}
#endif

	if (xr_instance) {
		OOVR_FAILED_XR_ABORT(xrDestroyInstance(xr_instance));
		xr_instance = XR_NULL_HANDLE;
	}

	initialised = false;
	currentBackend = nullptr;
	sessionInputKeepalive.reset();
}
