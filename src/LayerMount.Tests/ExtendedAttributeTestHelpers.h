#pragma once

#include "TestFixture.h"

#include "EntryCopy.h"
#include "NtStatusUtil.h"
#include "NtdllExport.h"
#include "ScopedHandle.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace LayerMountTests {

using ::LayerMount::FullEaHeader;

// The header of FILE_GET_EA_INFORMATION, which the user-mode SDK headers
// do not define. The name and a NUL follow it.
#pragma pack(push, 1)
struct GetEaHeader {
    ULONG nextEntryOffset;
    UCHAR nameLength;
};
#pragma pack(pop)

// The four bytes of value in little-endian order, as WSL stores a number
// in an extended attribute.
inline std::string LittleEndianUlong(ULONG value) {
    return std::string(reinterpret_cast<const char*>(&value), sizeof(value));
}

using ::LayerMount::NtQueryEaFileFn;
using ::LayerMount::NtSetEaFileFn;

// One FILE_FULL_EA_INFORMATION entry for name and value, with
// NextEntryOffset 0.
inline std::vector<BYTE> FullEaEntry(const std::string& name, const std::string& value) {
    std::vector<BYTE> entry(sizeof(FullEaHeader) + name.size() + 1 + value.size());
    auto* header = reinterpret_cast<FullEaHeader*>(entry.data());
    header->nameLength = static_cast<UCHAR>(name.size());
    header->valueLength = static_cast<USHORT>(value.size());
    std::memcpy(entry.data() + sizeof(FullEaHeader), name.data(), name.size());
    std::memcpy(entry.data() + sizeof(FullEaHeader) + name.size() + 1, value.data(),
                value.size());
    return entry;
}

// Writes the extended attribute name with value on the entry at path, not
// on its target. Returns the status of NtSetEaFile, or of the open.
inline NTSTATUS SetExtendedAttribute(const std::wstring& path, const std::string& name,
                                     const std::string& value) {
    ::LayerMount::ScopedHandle handle(::CreateFileW(
        path.c_str(), FILE_WRITE_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }
    std::vector<BYTE> entry = FullEaEntry(name, value);
    const auto setEa = ::LayerMount::LoadNtdllExport<NtSetEaFileFn>("NtSetEaFile");
    IO_STATUS_BLOCK io{};
    return setEa(handle.Get(), &io, entry.data(), static_cast<ULONG>(entry.size()));
}

// Returns the value of the extended attribute name on the entry at path,
// not on its target, or none when the entry has no such attribute or the
// read fails.
inline std::optional<std::string> ExtendedAttributeOf(const std::wstring& path,
                                                      const std::string& name) {
    ::LayerMount::ScopedHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.IsValid()) {
        return std::nullopt;
    }
    std::vector<BYTE> query(sizeof(GetEaHeader) + name.size() + 1);
    reinterpret_cast<GetEaHeader*>(query.data())->nameLength = static_cast<UCHAR>(name.size());
    std::memcpy(query.data() + sizeof(GetEaHeader), name.data(), name.size());
    std::vector<BYTE> result(64 * 1024);
    const auto queryEa = ::LayerMount::LoadNtdllExport<NtQueryEaFileFn>("NtQueryEaFile");
    IO_STATUS_BLOCK io{};
    const NTSTATUS status = queryEa(handle.Get(), &io, result.data(),
                                    static_cast<ULONG>(result.size()), TRUE, query.data(),
                                    static_cast<ULONG>(query.size()), nullptr, TRUE);
    const auto* header = reinterpret_cast<const FullEaHeader*>(result.data());
    if (!NT_SUCCESS(status) || header->valueLength == 0) {
        return std::nullopt;
    }
    const auto* value = reinterpret_cast<const char*>(result.data()) + sizeof(FullEaHeader) +
                        header->nameLength + 1;
    return std::string(value, header->valueLength);
}

struct NamedExtendedAttribute {
    std::string name;
    std::string value;
};

// The file type bits of a WSL mode, as $LXMOD stores them.
constexpr ULONG kWslRegularFile = 0100000;
constexpr ULONG kWslDirectory = 0040000;
constexpr ULONG kWslCharacterDevice = 0020000;

inline const std::string kWslFileMode = LittleEndianUlong(kWslRegularFile | 0644);
inline const std::string kWslDirectoryMode = LittleEndianUlong(kWslDirectory | 0755);

// The WSL owner, group and mode with uid and gid 1000, and a user
// attribute.
inline std::vector<NamedExtendedAttribute> WslAndUserExtendedAttributes(const std::string& mode) {
    const std::string id1000 = LittleEndianUlong(1000);
    return {{"$LXUID", id1000},
            {"$LXGID", id1000},
            {"$LXMOD", mode},
            {"USER.NOTE", "lower note"}};
}

inline void SetExtendedAttributes(const std::wstring& path,
                                  const std::vector<NamedExtendedAttribute>& attributes) {
    for (const auto& attribute : attributes) {
        AssertStatus(STATUS_SUCCESS,
            SetExtendedAttribute(path, attribute.name, attribute.value),
            (L"The test must set an extended attribute on " + path).c_str());
    }
}

inline void AssertHasExtendedAttributes(const std::wstring& path,
                                        const std::vector<NamedExtendedAttribute>& attributes) {
    for (const auto& attribute : attributes) {
        ::Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
            std::optional<std::string>(attribute.value) ==
                ExtendedAttributeOf(path, attribute.name),
            (L"The entry " + path + L" must carry the extended attribute " +
             std::wstring(attribute.name.begin(), attribute.name.end()))
                .c_str());
    }
}

}
