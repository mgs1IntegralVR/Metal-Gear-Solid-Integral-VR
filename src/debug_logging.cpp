#include "../include/debug_logging.h"

// Static member initialization
std::ofstream DebugLogger::logFile;
std::mutex DebugLogger::logMutex;
bool DebugLogger::initialized = false;
std::string DebugLogger::currentLogPath;

std::ofstream DebugLogger::testLogFile;
std::mutex DebugLogger::testLogMutex;
bool DebugLogger::testLogInitialized = false;
std::string DebugLogger::testLogPath;
