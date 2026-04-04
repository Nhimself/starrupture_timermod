// Pre-included on all TUs for clang cross-compile (--target=x86_64-w64-mingw32).
// The Dumper-7 SDK headers pull in C standard library functions via MSVC's
// transitive Windows SDK headers; MinGW does not do this automatically.
#pragma once
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <cstdio>
