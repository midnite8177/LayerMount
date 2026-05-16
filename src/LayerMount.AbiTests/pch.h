#pragma once

// AbiTests consumes LayerMount.dll purely through its public C ABI. The test
// DLL does not reach into impl/ headers, does not link any host-adapter
// SDK, and does not spawn a subprocess — every assertion is in-process,
// against the shipped DLL's exports.
#include <windows.h>

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
