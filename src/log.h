// Tiny append-only log written next to the DLL (nwn_controller.log).
#pragma once

#include <string>

void LogOpen(const std::string& path);
void Log(const char* fmt, ...);
