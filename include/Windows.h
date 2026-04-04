// Case-insensitive shim for Linux cross-compile with MinGW-w64.
// MinGW ships windows.h (lowercase); SDK headers include Windows.h (capital W).
// This file is found first via -I "include" and redirects to the real header.
#pragma once
#include <windows.h>
