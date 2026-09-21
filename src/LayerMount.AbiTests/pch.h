#pragma once

// AbiTests consumes LayerMount.dll purely through its public C ABI. The test
// DLL does not reach into impl/ headers, does not link any host-adapter
// SDK, and does not spawn a subprocess — every assertion is in-process,
// against the shipped DLL's exports.
//
// WIN32_NO_STATUS keeps <windows.h> from defining the basic STATUS_* macros,
// so <ntstatus.h> supplies the full set. winnt.h defines a few newer codes
// outside that guard, and they collide with ntstatus.h, so C4005 stays
// suppressed around that one include and out of every test's build output.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#pragma warning(push)
#pragma warning(disable: 4005)
#include <ntstatus.h>
#pragma warning(pop)

#include <CppUnitTest.h>

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <filesystem>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <fstream>
#include <cstring>
#include <system_error>

#include "LayerMount.h"
