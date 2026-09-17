#include "stdafx.h"

#include "Misc/Config.h"
#include "logging.h"
#include <chrono>
#include <ctime>
#include <errno.h> // errno, ENOENT, EEXIST
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdarg.h>
#include <sys/stat.h> // stat
#ifdef _WIN32
#include <direct.h> // _mkdir
#include <shlobj.h> // SHGetKnownFolderPath, FOLDERID_Documents
#endif

// strftime format
#define LOGGER_TIME_FORMAT "%Y-%m-%d %H:%M:%S"

// printf format
#define LOGGER_MS_FORMAT ".%03d"

// convert current time to milliseconds since unix epoch
template <typename T>
static int get_ms(const std::chrono::time_point<T>& tp)
{
	using namespace std::chrono;

	auto dur = tp.time_since_epoch();
	auto s = std::chrono::duration_cast<std::chrono::seconds>(dur);
	std::chrono::duration<long, std::milli> rounded_ms = s;
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
	return static_cast<int>((ms - rounded_ms).count());
}

// format it in two parts: main part with date and time and part with milliseconds
static std::string format_time()
{
	auto tp = std::chrono::system_clock::now();
	std::time_t current_time = std::chrono::system_clock::to_time_t(tp);

	// this function use static global pointer. so it is not thread safe solution
	std::tm* time_info = std::localtime(&current_time);

	char buffer[128];

	size_t string_size = strftime(
	    buffer, sizeof(buffer),
	    LOGGER_TIME_FORMAT,
	    time_info);

	int ms = get_ms(tp);

	string_size += std::snprintf(
	    buffer + string_size, sizeof(buffer) - string_size,
	    LOGGER_MS_FORMAT, ms);

	return std::string(buffer, buffer + string_size);
}

bool isDirExist(const std::string& path)
{
#if defined(_WIN32)
	struct _stat info;
	if (_stat(path.c_str(), &info) != 0) {
		return false;
	}
	return (info.st_mode & _S_IFDIR) != 0;
#else
	struct stat info;
	if (stat(path.c_str(), &info) != 0) {
		return false;
	}
	return (info.st_mode & S_IFDIR) != 0;
#endif
}

bool makePath(const std::string& path)
{
#if defined(_WIN32)
	int ret = _mkdir(path.c_str());
#else
	mode_t mode = 0755;
	int ret = mkdir(path.c_str(), mode);
#endif
	if (ret == 0)
		return true;

	switch (errno) {
	case ENOENT:
		// parent didn't exist, try to create it
		{
			size_t pos = path.find_last_of('/');
			if (pos == std::string::npos)
#if defined(_WIN32)
				pos = path.find_last_of('\\');
			if (pos == std::string::npos)
#endif
				return false;
			if (!makePath(path.substr(0, pos)))
				return false;
		}
		// now, try to create again
#if defined(_WIN32)
		return 0 == _mkdir(path.c_str());
#else
		return 0 == mkdir(path.c_str(), mode);
#endif

	case EEXIST:
		// done!
		return isDirExist(path);

	default:
		return false;
	}
}

std::string GetEnv(const std::string& var)
{
	const char* val = std::getenv(var.c_str());
	if (val == nullptr) {
		return "";
	} else {
		return val;
	}
}

#ifdef ANDROID
#include <android/log.h>
#else
static std::ofstream stream;
// Logger calls arrive from input and render threads. Never wait for this lock
// during DLL detach, where Windows may already have stopped another thread.
static std::mutex& log_mutex() { static auto* mutex = new std::mutex; return *mutex; }
#endif

bool oovr_debug_logging_enabled()
{
	return oovr_global_configuration.DebugLogging();
}

OC_NORETURN void oovr_abort_raw_va(const char* file, long line, const char* func, const char* msg, const char* title, va_list args);

void oovr_log_raw(const char* file, long line, const char* func, const char* msg)
{
#ifdef ANDROID
	__android_log_print(ANDROID_LOG_INFO, "OpenComposite", "%s:%d \t %s", func, line, msg);
#else
	std::lock_guard<std::mutex> lock(log_mutex());
	if (!stream.is_open()) {
#ifdef _WIN32
		std::filesystem::path outputFilePath = L"OCUnleashedSKSE.log";

		// Ask Windows for the user's real Documents folder. This respects
		// OneDrive, domain policy, localization, and folders redirected to
		// another drive instead of assuming %USERPROFILE%\\Documents.
		PWSTR documentsPath = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(
		        FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsPath)) &&
		    documentsPath != nullptr) {
			std::filesystem::path outputFolder(documentsPath);
			CoTaskMemFree(documentsPath);
			outputFolder /= L"My Games";
			outputFolder /= L"Skyrim VR";
			outputFolder /= L"SKSE";

			std::error_code directoryError;
			std::filesystem::create_directories(outputFolder, directoryError);
			if (!directoryError)
				outputFilePath = outputFolder / outputFilePath;
		}
#else
		string outputFilePath = "OCUnleashedSKSE.log";
		string outputFolder = GetEnv("XDG_STATE_HOME");
		if (outputFolder.empty()) {
			outputFolder = GetEnv("HOME");
			if (!outputFolder.empty())
				outputFolder = outputFolder + "/.local/state";
		}
		if (!outputFolder.empty())
			outputFolder = outputFolder + "/OpenComposite/logs";
		if (!outputFolder.empty() && makePath(outputFolder))
			outputFilePath = outputFolder + "/" + outputFilePath;
#endif

		// Open with trunc mode to overwrite/erase log on each game restart
		stream.open(outputFilePath.c_str(), std::ios::out | std::ios::trunc);
	}

	stream << "[" << format_time() << "] " << func << ":" << line << "\t- " << (msg ? msg : "NULL") << '\n';

	// Write it to stdout
	// TODO on Windows, write it into the debug log
#ifndef _WIN32
	printf("[OC] %s:%ld \t %s\n", func, line, msg);
#endif

	// Buffered diagnostics: no forced disk flush for every render/input report.
	static auto lastFlush = std::chrono::steady_clock::now();
	const auto now = std::chrono::steady_clock::now();
	if (now - lastFlush >= std::chrono::seconds(1)) {
		stream.flush();
		lastFlush = now;
	}
#endif
}

void oovr_log_flush()
{
#ifndef ANDROID
	std::lock_guard<std::mutex> lock(log_mutex());
	if (stream.is_open()) stream.flush();
#endif
}

// Called from DLL_PROCESS_DETACH to flush and close the log stream
void oovr_log_shutdown()
{
#ifndef ANDROID
	std::unique_lock<std::mutex> lock(log_mutex(), std::try_to_lock);
	if (!lock.owns_lock()) return;
	if (stream.is_open()) {
		stream.flush();
		stream.close();
	}
#endif
}

void oovr_log_raw_format(const char* file, long line, const char* func, const char* msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buff[2048];
	vsnprintf(buff, sizeof(buff), msg, args);

	oovr_log_raw(file, line, func, buff);

	va_end(args);
}

OC_NORETURN void oovr_abort_raw(const char* file, long line, const char* func, const char* msg, const char* title, ...)
{
	va_list args;
	va_start(args, title);
	oovr_abort_raw_va(file, line, func, msg, title, args);
	va_end(args); // Well we probably don't need this, but it should keep any tools happy
}

OC_NORETURN void oovr_abort_raw_va(const char* file, long line, const char* func, const char* msg, const char* title, va_list args)
{
	if (title == nullptr) {
		title = "OpenComposite Error - info in log";
		OOVR_LOG("Abort!");
	} else {
		OOVR_LOG(title);
	}

	char buff[2048];
	vsnprintf(buff, sizeof(buff), msg, args);
	buff[sizeof(buff) - 1] = 0;

	oovr_log_raw(file, line, func, buff);

	// Ensure everything gets written
#ifdef ANDROID
	__android_log_print(ANDROID_LOG_ERROR, "OpenComposite", "ERROR: %s:%d \t %s", func, line, buff);
#else
	{ std::lock_guard<std::mutex> lock(log_mutex()); stream << std::flush; }
#endif

	OOVR_MESSAGE(buff, title);
	DebugBreak();
	abort();
}

void oovr_soft_abort_raw(const char* file, long line, const char* func, int* count, const char* msg, ...)
{
	// If this has been hit already, just ignore it - if we needed a crash that would've been done.
	if (*count > 0) {
		*count++;
		return;
	}

	va_list args;
	va_start(args, msg);

	// Otherwise, if we're in debug mode do a hard abort. Otherwise log and continue.
	// TODO fix this
	if (oovr_global_configuration.StopOnSoftAbort()) {
		oovr_abort_raw_va(file, line, func, msg, "OpenComposite Debug Error", args);
	}

	char buff[256];
	vsnprintf(buff, sizeof(buff), msg, args);
	buff[sizeof(buff) - 1] = 0;

	oovr_log_raw_format(file, line, func, "Soft Abort triggered (in non-debug mode, continuing - this will only print once): %s", buff);

	*count = 1;

	va_end(args);
}

void oovr_message_raw(const char* message, const char* title)
{
	// No need to log this, it will have already been done by the caller

#ifdef WIN32
	// Display a message box on Windows
	MessageBoxA(nullptr, message, title, MB_OK);
#else
	// Print to stderr on Linux
	std::cerr << "OOVR_MESSAGE: " << title << ": " << message << std::endl;
#endif
}
