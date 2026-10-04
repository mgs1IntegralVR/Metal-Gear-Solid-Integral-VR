#pragma once
#include <windows.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <mutex>
#include <string>
#include <cstdarg>
#include <ctime>

class DebugLogger {
private:
	static std::ofstream logFile;
	static std::mutex logMutex;
	static bool initialized;
	static std::string currentLogPath;

	static std::ofstream testLogFile;
	static std::mutex testLogMutex;
	static bool testLogInitialized;
	static std::string testLogPath;

public:
	// Initialize logging
	static void Initialize() {
		if (initialized) return;

		std::lock_guard<std::mutex> lock(logMutex);

		char exePath[MAX_PATH] = { 0 };
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		std::string exeDir(exePath);
		auto slash = exeDir.find_last_of("\\/");
		if (slash != std::string::npos) {
			exeDir = exeDir.substr(0, slash);
		}

		// One session per file (like mgs1_vr_test.log): the log starts fresh
		// every launch. The previous session is kept as
		// mgs1_vr_debug.prev.log, so a log from a run that hung is not lost
		// when the game is relaunched.
		currentLogPath = exeDir + "\\mgs1_vr_debug.log";
		MoveFileExA(currentLogPath.c_str(), (exeDir + "\\mgs1_vr_debug.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
		logFile.open(currentLogPath, std::ios::trunc);

		if (!logFile.is_open()) {
			char tempPath[MAX_PATH] = { 0 };
			GetTempPathA(MAX_PATH, tempPath);
			currentLogPath = std::string(tempPath) + "mgs1_vr_debug.log";
			logFile.open(currentLogPath, std::ios::trunc);
		}

		if (logFile.is_open()) {
			auto now = std::chrono::system_clock::now();
			auto time = std::chrono::system_clock::to_time_t(now);
			std::tm tmValue{};
			localtime_s(&tmValue, &time);
			char stamp[64] = { 0 };
			strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmValue);
			logFile << "========================================" << std::endl;
			logFile << "[" << stamp << "] MGS1 VR Mod - Debug Session Started" << std::endl;
			logFile << "========================================" << std::endl;
			logFile.flush();
			initialized = true;
		}
	}

	// Main logging function
	static void Log(const std::string& message) {
		std::lock_guard<std::mutex> lock(logMutex);

		if (!logFile.is_open()) return;

		// Get current time
		auto now = std::chrono::system_clock::now();
		auto time = std::chrono::system_clock::to_time_t(now);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			now.time_since_epoch()) % 1000;

		char timestamp[32];
		std::tm tmValue{};
		localtime_s(&tmValue, &time);
		strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tmValue);

		// Write to file
		logFile << "[" << timestamp << "." << ms.count() << "] " << message << std::endl;
		logFile.flush();

		// Also output to debug console (Visual Studio)
		OutputDebugStringA(("[VR] " + message + "\n").c_str());
	}

	// Formatted logging
	static void LogFormat(const char* format, ...) {
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
		va_end(args);
		Log(buffer);
	}

	// --- Separate, low-noise log just for the rotation candidate test ---
	// Writes to its own file (mgs1_vr_test.log) instead of the main debug
	// log, which is dominated by per-frame OpenXR action-state spam when a
	// headset is connected. Opens lazily on first use, same directory as
	// the main log.
	static void TestLog(const std::string& message) {
		std::lock_guard<std::mutex> lock(testLogMutex);

		if (!testLogInitialized) {
			char exePath[MAX_PATH] = { 0 };
			GetModuleFileNameA(NULL, exePath, MAX_PATH);
			std::string exeDir(exePath);
			auto slash = exeDir.find_last_of("\\/");
			if (slash != std::string::npos) {
				exeDir = exeDir.substr(0, slash);
			}

			testLogPath = exeDir + "\\mgs1_vr_test.log";
			testLogFile.open(testLogPath, std::ios::trunc); // fresh file each session, not appended
			if (!testLogFile.is_open()) {
				char tempPath[MAX_PATH] = { 0 };
				GetTempPathA(MAX_PATH, tempPath);
				testLogPath = std::string(tempPath) + "mgs1_vr_test.log";
				testLogFile.open(testLogPath, std::ios::trunc);
			}
			testLogInitialized = true;
		}

		if (!testLogFile.is_open()) return;

		auto now = std::chrono::system_clock::now();
		auto time = std::chrono::system_clock::to_time_t(now);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			now.time_since_epoch()) % 1000;

		char timestamp[32];
		std::tm tmValue{};
		localtime_s(&tmValue, &time);
		strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tmValue);

		testLogFile << "[" << timestamp << "." << ms.count() << "] " << message << std::endl;
		testLogFile.flush(); // flush every line -- this file is meant to be tailed live
	}

	static void TestLogFormat(const char* format, ...) {
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
		va_end(args);
		TestLog(buffer);
	}

	static std::string GetTestLogPath() {
		return testLogPath;
	}

	// Shutdown
	static void Shutdown() {
		std::lock_guard<std::mutex> lock(logMutex);
		if (logFile.is_open()) {
			logFile << "========================================" << std::endl;
			logFile << "MGS1 VR Mod - Debug Session Ended" << std::endl;
			logFile << "========================================" << std::endl;
			logFile.flush();
			logFile.close();
		}
	}

	// Get log file location
	static std::string GetLogPath() {
		return currentLogPath;
	}
};
