#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "MetadataStore.h"
#include "NtdllExport.h"
#include "NtStatusUtil.h"

#include <winioctl.h>
#include <cstring>
#include <set>

#include "StreamTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AccessDenied;
using LayerMountTestShared::BackupPrivilegeDisabledOnThread;
using LayerMountTestShared::HasOverlayStream;

namespace LayerMountTests {

static_assert(RefusesTemporaryConfig<CopyUp, PathResolver&, WhiteoutManager&, Cache&, LayerMountStats&>,
    "CopyUp keeps a reference to its LayerConfig");

namespace {

// The user-mode SDK headers define no names for the WSL FIFO, character
// device and block device tags.
constexpr DWORD kReparseTagLxFifo = 0x80000024;
constexpr DWORD kReparseTagLxChr = 0x80000025;
constexpr DWORD kReparseTagLxBlk = 0x80000026;

// The header of FILE_FULL_EA_INFORMATION, which the user-mode SDK headers
// do not define. The name, a NUL and the value follow it.
struct FullEaHeader {
    ULONG nextEntryOffset;
    UCHAR flags;
    UCHAR nameLength;
    USHORT valueLength;
};

using NtSetEaFileFn = NTSTATUS(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, PVOID, ULONG);
using NtQueryEaFileFn = NTSTATUS(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, PVOID, ULONG, BOOLEAN,
                                         PVOID, ULONG, PULONG, BOOLEAN);

// Writes the extended attribute name with value on the entry at path, not
// on its target. Returns the status of NtSetEaFile, or of the open.
NTSTATUS SetExtendedAttribute(const std::wstring& path, const std::string& name,
                              const std::string& value) {
    ScopedHandle handle(::CreateFileW(
        path.c_str(), FILE_WRITE_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.IsValid()) {
        return NtStatusFromWin32(::GetLastError());
    }
    std::vector<BYTE> buffer(sizeof(FullEaHeader) + name.size() + 1 + value.size());
    auto* header = reinterpret_cast<FullEaHeader*>(buffer.data());
    header->nameLength = static_cast<UCHAR>(name.size());
    header->valueLength = static_cast<USHORT>(value.size());
    std::memcpy(buffer.data() + sizeof(FullEaHeader), name.data(), name.size());
    std::memcpy(buffer.data() + sizeof(FullEaHeader) + name.size() + 1, value.data(),
                value.size());
    const auto setEa = LoadNtdllExport<NtSetEaFileFn>("NtSetEaFile");
    IO_STATUS_BLOCK io{};
    return setEa(handle.Get(), &io, buffer.data(), static_cast<ULONG>(buffer.size()));
}

// Returns the value of the extended attribute name on the entry at path,
// not on its target, or none when the entry has no such attribute or the
// read fails.
std::optional<std::string> ExtendedAttributeOf(const std::wstring& path,
                                               const std::string& name) {
    ScopedHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.IsValid()) {
        return std::nullopt;
    }
    // A FILE_GET_EA_INFORMATION entry: the offset of the next entry in a
    // ULONG, the name length in one byte, the name and a NUL.
    constexpr size_t kNameOffset = sizeof(ULONG) + 1;
    std::vector<BYTE> query(kNameOffset + name.size() + 1);
    query[sizeof(ULONG)] = static_cast<BYTE>(name.size());
    std::memcpy(query.data() + kNameOffset, name.data(), name.size());
    std::vector<BYTE> result(64 * 1024);
    const auto queryEa = LoadNtdllExport<NtQueryEaFileFn>("NtQueryEaFile");
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

void AssertUpperLinkReadsThroughToTarget(const TempLayerEnvironment& env,
                                         const std::wstring& linkName,
                                         const std::wstring& readPath,
                                         const std::string& targetContent,
                                         const std::wstring& target) {
    Assert::IsTrue(HasAttribute(env.Upper() + L"\\" + linkName, FILE_ATTRIBUTE_REPARSE_POINT),
        (L"The upper " + linkName + L" must be a link").c_str());
    Assert::AreEqual(targetContent, env.ReadFile(env.Upper(), readPath),
        (L"The upper " + linkName + L" must point to the lower link's target").c_str());
    Assert::IsFalse(HasOverlayStream(target),
        L"The copy-up must write no :overlay stream onto the link target");
}

void AssertLowerDirectoryLinkCopiesUpAsALink(LinkCreator createLink) {
    TempLayerEnvironment env(1);
    env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
    const std::wstring target = env.Root() + L"\\target";
    if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link", target)) {
        return;
    }
    LayerConfig config = env.MakeConfig();
    Assert::IsTrue((config.hostCapabilities & LM_CAP_ADS) != 0,
        L"The metadata store must use ADS");
    CopyUpAndRenameRig rig(config);
    const LayerSnapshot targetBefore(target);

    AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"link"),
        L"The copy-up of the lower directory link must succeed");

    AssertUpperLinkReadsThroughToTarget(env, L"link", L"link\\inside.txt", "inside", target);
    targetBefore.AssertUnchanged(L"The copy-up must not change the link target's entries");
}

void AssertCopiedUpLinkKeepsTheLowerLinkId(LinkCreator createLink, LinkTarget targetKind,
                                           UINT32 hostCapabilities) {
    TempLayerEnvironment env(1);
    const std::wstring target = LinkTargetPath(env, targetKind);
    const std::wstring lowerLink = env.Lower(0) + L"\\link";
    if (!LinkToTargetCreatedOrSkipped(env, createLink, targetKind, lowerLink)) {
        return;
    }
    const UINT64 lowerLinkId = NtfsFileIdOf(lowerLink, LinkOpen::Itself);
    const UINT64 targetId = NtfsFileIdOf(target, LinkOpen::Follow);
    LayerConfig config = env.MakeConfig();
    config.hostCapabilities = hostCapabilities;
    {
        CopyUpAndRenameRig rig(config);
        AssertStatus(STATUS_SUCCESS,
            targetKind == LinkTarget::File ? rig.copyUp.CopyUpFile(L"link")
                                           : rig.copyUp.CopyUpDirectory(L"link"),
            L"The copy-up of the lower link must succeed");
    }
    const std::wstring upperLink = env.Upper() + L"\\link";
    Assert::IsTrue(HasAttribute(upperLink, FILE_ATTRIBUTE_REPARSE_POINT),
        L"The upper must hold the link");
    Assert::AreNotEqual(lowerLinkId, NtfsFileIdOf(upperLink, LinkOpen::Itself),
        L"The upper link must be a new NTFS entry");
    ::LayerMount::LayerMount mount(config);

    Assert::AreEqual(lowerLinkId,
        IndexNumberThroughMount(mount, L"link", FILE_OPEN_REPARSE_POINT),
        L"An open of the upper link as a link must report the lower link's file ID");
    Assert::AreEqual(targetId, IndexNumberThroughMount(mount, L"link", kNoCreateOptions),
        L"An open that follows the upper link must report the target's file ID");
    Assert::IsFalse(HasOverlayStream(target),
        L"The copy-up must write no :overlay stream onto the link target");
}

}

TEST_CLASS(CopyUpTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(GenerateWorkPath_ReturnsUniquePathInWorkDir) {
        TempLayerEnvironment env(1);
        CopyUpAndRenameRig rig(env.MakeConfig());

        std::wstring a = rig.copyUp.GenerateWorkPath();
        std::wstring b = rig.copyUp.GenerateWorkPath();

        Assert::AreNotEqual(a, b, L"Two calls should return different paths");
        Assert::IsTrue(a.find(env.Work()) == 0, L"Path should be under work dir");
        Assert::IsTrue(a.find(L".tmp") != std::wstring::npos, L"Path should end with .tmp");
    }

    TEST_METHOD(Prepare_WorkDirectoryOnTheUppersVolume_CreatesItAndSucceeds) {
        TempLayerEnvironment env(0);
        LayerConfig config = env.MakeConfig();
        config.workDirPath = env.Root() + L"\\new-work";
        std::wstring error;

        Assert::AreEqual<HRESULT>(S_OK, config.Prepare(error),
            L"A work directory on the upper's volume passes the prepare");
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(config.workDirPath.c_str()),
            L"The prepare creates a missing work directory");

        config.workDirPath = config.upperPath;
        Assert::AreEqual<HRESULT>(S_OK, config.Prepare(error),
            L"The upper as its own work directory passes the prepare");
    }

     TEST_METHOD(GenerateWorkPath_LongWorkDirPath_StillUniqueNoTruncation) {
         TempLayerEnvironment env(1);
         LayerConfig config = env.MakeConfig();

         std::wstring deepBase = env.Root();
         while (deepBase.size() < 240) {
             deepBase += L"\\padding-segment";
         }
         // create_directories fails on a path past MAX_PATH without the \\?\ prefix.
         const std::wstring deepWork = L"\\\\?\\" + deepBase + L"\\overlay-work";
         std::error_code ec;
         std::filesystem::create_directories(deepWork, ec);
         Assert::IsFalse(static_cast<bool>(ec),
             L"Precondition: long-path work dir must be creatable with \\?\\ prefix");

         config.workDirPath = deepWork;

         CopyUpAndRenameRig rig(config);

         std::vector<std::wstring> paths;
         constexpr int kCount = 32;
         paths.reserve(kCount);
         for (int i = 0; i < kCount; ++i) {
             paths.push_back(rig.copyUp.GenerateWorkPath());
         }

         for (const auto& p : paths) {
             Assert::IsTrue(p.size() > deepWork.size(),
                 L"Generated path must be longer than the work dir prefix");
             Assert::IsTrue(p.compare(0, deepWork.size(), deepWork) == 0,
                 L"Generated path must start with the full workDirPath");
             Assert::IsTrue(p.find(L".tmp") == p.size() - 4,
                 L"Generated path must end with .tmp");
         }

         std::set<std::wstring> uniq(paths.begin(), paths.end());
         wchar_t msg[200];
         swprintf_s(msg, L"Generated %d work paths, only %zu unique under a "
                         L"long work dir.",
                    kCount, uniq.size());
         Assert::IsTrue(uniq.size() == static_cast<size_t>(kCount), msg);
     }

    TEST_METHOD(CleanWorkDirectory_RemovesHashTempFiles_LeavesOthers) {
        TempLayerEnvironment env(1);
        CopyUpAndRenameRig rig(env.MakeConfig());

        std::wstring tempFile = env.Work() + L"\\#abc.tmp";
        HANDLE h1 = ::CreateFileW(tempFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h1);

        std::wstring otherFile = env.Work() + L"\\other.txt";
        HANDLE h2 = ::CreateFileW(otherFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h2);

        rig.copyUp.CleanWorkDirectory();

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(tempFile.c_str()),
            L"Hash-prefixed .tmp file should be removed");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(otherFile.c_str()),
            L"Non-matching file should remain");
    }

    TEST_METHOD(CleanWorkDirectory_ReadOnlyHashTempFile_RemovesIt) {
        TempLayerEnvironment env(1);
        CopyUpAndRenameRig rig(env.MakeConfig());

        env.WriteFile(env.Work(), L"#abc.tmp", "staged");
        const std::wstring tempFile = env.Work() + L"\\#abc.tmp";
        Assert::IsTrue(::SetFileAttributesW(tempFile.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
            L"The test must make the staged file read-only");

        rig.copyUp.CleanWorkDirectory();

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES, ::GetFileAttributesW(tempFile.c_str()),
            L"CleanWorkDirectory removes a read-only staged file");
    }

    TEST_METHOD(CleanWorkDirectory_ExtendedWorkDirWithTrailingSeparator_RemovesHashTempFiles) {
        TempLayerEnvironment env(1);
        LayerConfig config = env.MakeConfig();
        config.workDirPath = ExtendedDirWithSeparator(env.Work());
        CopyUpAndRenameRig rig(config);

        std::wstring tempFile = env.Work() + L"\\#abc.tmp";
        env.WriteFile(env.Work(), L"#abc.tmp", "staged");

        rig.copyUp.CleanWorkDirectory();

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(tempFile.c_str()),
            L"CleanWorkDirectory must delete the #abc.tmp file");
    }

    TEST_METHOD(CommitFromWorkDir_MovesFileFromWorkToFinalPath) {
        TempLayerEnvironment env(1);
        CopyUpAndRenameRig rig(env.MakeConfig());

        std::wstring workPath = env.Work() + L"\\test.tmp";
        std::wstring finalPath = env.Upper() + L"\\final.txt";
        env.WriteFile(env.Work(), L"test.tmp", "content");

        NTSTATUS status = rig.copyUp.CommitFromWorkDir(workPath, finalPath);

        Assert::IsTrue(NT_SUCCESS(status), L"CommitFromWorkDir should succeed");
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(workPath.c_str()),
            L"Work path should no longer exist");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(finalPath.c_str()),
            L"Final path should exist");
    }

    TEST_METHOD(CommitFromWorkDir_CreatesParentDirectoriesIfMissing) {
        TempLayerEnvironment env(1);
        CopyUpAndRenameRig rig(env.MakeConfig());

        std::wstring workPath = env.Work() + L"\\temp.tmp";
        env.WriteFile(env.Work(), L"temp.tmp", "x");
        std::wstring finalPath = env.Upper() + L"\\a\\b\\c.txt";

        NTSTATUS status = rig.copyUp.CommitFromWorkDir(workPath, finalPath);

        Assert::IsTrue(NT_SUCCESS(status));
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(finalPath.c_str()),
            L"Final path under nested dirs should exist");
    }

    TEST_METHOD(CopyUpFile_LowerFileOnly_CopiesContentToUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "lower content");

        CopyUpAndRenameRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.CopyUpFile(L"foo.txt");
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::IsTrue(env.FileExists(env.Upper(), L"foo.txt"));
        Assert::AreEqual(std::string("lower content"),
            env.ReadFile(env.Upper(), L"foo.txt"));
    }

    TEST_METHOD(CopyUpFile_LowerFileSymlink_NamesTheUpperInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"target.txt", "target");
        const std::wstring lowerLink = env.Lower(0) + L"\\Link.TXT";
        if (!LinkCreatedOrSkipped(CreateFileSymlink, lowerLink, L"target.txt")) {
            return;
        }

        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"link.txt"),
            L"The copy-up of the lower file symlink must succeed");

        const std::wstring upperLink = env.Upper() + L"\\link.txt";
        Assert::IsTrue(HasAttribute(upperLink, FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper must hold the symlink");
        Assert::AreEqual(std::wstring(L"Link.TXT"), StoredLeafName(upperLink),
            L"The upper symlink must keep the lower's name");
    }

    TEST_METHOD(CopyUpFile_LowerFileSymlinkWithAds_CopiesTheLinkAndWritesNoStreamOntoItsTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target.txt", "target");
        const std::wstring target = env.Root() + L"\\target.txt";
        if (!LinkCreatedOrSkipped(CreateFileSymlink, env.Lower(0) + L"\\link.txt", target)) {
            return;
        }
        LayerConfig config = env.MakeConfig();
        Assert::IsTrue((config.hostCapabilities & LM_CAP_ADS) != 0,
            L"The metadata store must use ADS");
        CopyUpAndRenameRig rig(config);

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"link.txt"),
            L"The copy-up of the lower file symlink must succeed");

        AssertUpperLinkReadsThroughToTarget(env, L"link.txt", L"link.txt", "target", target);
    }

    TEST_METHOD(CopyUpFile_LowerCloudPlaceholderFile_CopiesUpAPlainFileWithTheLowersData) {
        CloudPlaceholderLayers layers;
        if (!layers.PlaceholderFileOrSkipped(L"x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"x.txt"),
            L"The copy-up of the cloud placeholder file must succeed");

        Assert::IsFalse(HasAttribute(env.Upper() + L"\\x.txt", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper file must not be a reparse point");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Upper(), L"x.txt"),
            L"The upper file must hold the data of the lower file");
    }

    TEST_METHOD(CopyUpFile_LowerWslSpecialFileOrAppExecutionAlias_CopiesUpWithItsReparseTag) {
        UNIT_SKIP_IF_NOT_NTFS();
        const struct {
            const wchar_t* name;
            DWORD tag;
        } entries[] = {
            {L"socket", IO_REPARSE_TAG_AF_UNIX},
            {L"fifo", kReparseTagLxFifo},
            {L"chr", kReparseTagLxChr},
            {L"blk", kReparseTagLxBlk},
            {L"alias.exe", IO_REPARSE_TAG_APPEXECLINK},
        };
        TempLayerEnvironment env(1);
        for (const auto& entry : entries) {
            env.WriteFile(env.Lower(0), entry.name, "");
            if (!MicrosoftReparseTagSetOrSkipped(env.Lower(0) + L"\\" + entry.name, entry.tag)) {
                return;
            }
        }
        CopyUpAndRenameRig rig(env.MakeConfig());

        for (const auto& entry : entries) {
            const std::wstring name = entry.name;
            AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(name),
                (L"The copy-up of " + name + L" must succeed").c_str());
            Assert::AreEqual(static_cast<DWORD>(entry.tag), ReparseTagOf(env.Upper() + L"\\" + name),
                (L"The upper " + name + L" must carry the reparse tag of the lower").c_str());
        }
    }

    TEST_METHOD(CopyUpFile_LowerWslCharacterDeviceWithExtendedAttributes_CopiesUpTheExtendedAttributes) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        const std::wstring lowerPath = env.Lower(0) + L"\\chr";
        env.WriteFile(env.Lower(0), L"chr", "");
        if (!MicrosoftReparseTagSetOrSkipped(lowerPath, kReparseTagLxChr)) {
            return;
        }
        const std::string mode("\xA4\x21\x00\x00", 4);
        const std::string device("\x05\x00\x00\x00\x01\x00\x00\x00", 8);
        AssertStatus(STATUS_SUCCESS, SetExtendedAttribute(lowerPath, "$LXMOD", mode),
            L"The test must set $LXMOD on the lower file");
        AssertStatus(STATUS_SUCCESS, SetExtendedAttribute(lowerPath, "$LXDEV", device),
            L"The test must set $LXDEV on the lower file");
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"chr"),
            L"The copy-up of the WSL character device must succeed");

        const std::wstring upperPath = env.Upper() + L"\\chr";
        Assert::AreEqual(kReparseTagLxChr, ReparseTagOf(upperPath),
            L"The upper file must carry the reparse tag of the lower");
        Assert::IsTrue(std::optional<std::string>(mode) == ExtendedAttributeOf(upperPath, "$LXMOD"),
            L"The upper file must carry the $LXMOD of the lower");
        Assert::IsTrue(std::optional<std::string>(device) == ExtendedAttributeOf(upperPath, "$LXDEV"),
            L"The upper file must carry the $LXDEV of the lower");
    }

    TEST_METHOD(CopyUpFile_LowerFileWithUnhandledReparseTag_FailsAndLeavesNoUpperOrWorkEntry) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"x.txt", "lower");
        if (!NonLinkReparseTagSetOrSkipped(env.Lower(0) + L"\\x.txt")) {
            return;
        }
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_IO_REPARSE_TAG_NOT_HANDLED, rig.copyUp.CopyUpFile(L"x.txt"),
            L"The copy-up of a file whose reparse tag no filter handles must fail");

        Assert::IsFalse(fs::exists(env.Upper() + L"\\x.txt"),
            L"The failed copy-up must leave no upper entry");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The failed copy-up must leave nothing in the work directory");
    }

    TEST_METHOD(CopyUpFile_ExtendedWorkDirWithTrailingSeparator_CopiesContentToUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "lower content");

        LayerConfig config = env.MakeConfig();
        config.workDirPath = ExtendedDirWithSeparator(env.Work());
        CopyUpAndRenameRig rig(config);

        NTSTATUS status = rig.copyUp.CopyUpFile(L"foo.txt");
        Assert::IsTrue(NT_SUCCESS(status),
            L"Copy-up must stage through a work dir that ends in a separator");

        Assert::AreEqual(std::string("lower content"),
            env.ReadFile(env.Upper(), L"foo.txt"));
    }

    TEST_METHOD(CopyUpFile_PreservesFileSize) {
        TempLayerEnvironment env(1);
        std::string content(8192, 'A');
        env.WriteFile(env.Lower(0), L"big.bin", content);

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"big.bin");

        std::string copied = env.ReadFile(env.Upper(), L"big.bin");
        Assert::AreEqual(content.size(), copied.size());
    }

    TEST_METHOD(CopyUpFile_PreservesTimestamps) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ts.txt", "x");

        std::wstring srcPath = env.Lower(0) + L"\\ts.txt";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"ts.txt");

        std::wstring upPath = env.Upper() + L"\\ts.txt";
        FILETIME dstCreation, dstAccess, dstWrite;
        {
            HANDLE h = ::CreateFileW(upPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &dstCreation, &dstAccess, &dstWrite);
            ::CloseHandle(h);
        }

        Assert::IsTrue(::CompareFileTime(&srcCreation, &dstCreation) == 0,
            L"CreationTime not preserved");
        Assert::IsTrue(::CompareFileTime(&srcWrite, &dstWrite) == 0,
            L"LastWriteTime not preserved");
    }

    TEST_METHOD(CopyUpFile_PreservesAttributes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"readonly.txt", "x");

        std::wstring srcPath = env.Lower(0) + L"\\readonly.txt";
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_READONLY);

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"readonly.txt");

        std::wstring upPath = env.Upper() + L"\\readonly.txt";
        DWORD upAttrs = ::GetFileAttributesW(upPath.c_str());
        Assert::IsTrue((upAttrs & FILE_ATTRIBUTE_READONLY) != 0,
            L"READONLY attribute not preserved");
    }

    TEST_METHOD(CopyUpFile_CreatesParentDirectoryInUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\nested.txt", "x");

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"sub\\nested.txt");

        std::wstring upSub = env.Upper() + L"\\sub";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upSub.c_str()),
            L"Parent dir should be auto-created in upper");
    }

    TEST_METHOD(CopyUpFile_InvalidatesCacheForPath) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"c.txt", "x");

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.resolver.ResolvePath(L"c.txt");
        Assert::IsTrue(rig.cache.Get(L"c.txt").has_value(), L"Should be cached");

        rig.copyUp.CopyUpFile(L"c.txt");

        Assert::IsFalse(rig.cache.Get(L"c.txt").has_value(),
            L"CopyUpFile should invalidate the cache");
    }

    TEST_METHOD(CopyUpFile_AlreadyInUpper_IsNoOp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"both.txt", "upper");
        env.WriteFile(env.Lower(0), L"both.txt", "lower");

        CopyUpAndRenameRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.CopyUpFile(L"both.txt");
        Assert::IsTrue(NT_SUCCESS(status));
        Assert::AreEqual(std::string("upper"),
            env.ReadFile(env.Upper(), L"both.txt"),
            L"Upper content should be unchanged");
    }

    TEST_METHOD(CopyUpFile_IncrementsCopyUpCount) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"count.txt", "x");

        CopyUpAndRenameRig rig(env.MakeConfig());

        uint64_t before = rig.stats.copyUpCount.load();
        rig.copyUp.CopyUpFile(L"count.txt");
        Assert::AreEqual(before + 1, rig.stats.copyUpCount.load());
    }

    TEST_METHOD(CopyUpDirectory_LowerDir_CreatesEntryOnlyNotContents) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"mydir");
        env.WriteFile(env.Lower(0), L"mydir\\child.txt", "child");

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpDirectory(L"mydir");

        std::wstring upDir = env.Upper() + L"\\mydir";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upDir.c_str()),
            L"Directory entry should be created in upper");

        std::wstring upChild = env.Upper() + L"\\mydir\\child.txt";
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upChild.c_str()),
            L"Child file should NOT be recursively copied");
    }

    TEST_METHOD(CopyUpDirectory_PreservesDirectoryAttributes) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"hidden_dir");

        std::wstring srcDir = env.Lower(0) + L"\\hidden_dir";
        ::SetFileAttributesW(srcDir.c_str(),
            FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN);

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpDirectory(L"hidden_dir");

        std::wstring upDir = env.Upper() + L"\\hidden_dir";
        DWORD upAttrs = ::GetFileAttributesW(upDir.c_str());
        Assert::IsTrue((upAttrs & FILE_ATTRIBUTE_HIDDEN) != 0,
            L"HIDDEN attribute should be preserved for directory");
    }

    TEST_METHOD(CopyUpDirectory_PreservesDirectoryTimestamps) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"td");

        std::wstring srcDir = env.Lower(0) + L"\\td";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcDir.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpDirectory(L"td");

        std::wstring upDir = env.Upper() + L"\\td";
        FILETIME dstCreation, dstAccess, dstWrite;
        {
            HANDLE h = ::CreateFileW(upDir.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            ::GetFileTime(h, &dstCreation, &dstAccess, &dstWrite);
            ::CloseHandle(h);
        }

        Assert::IsTrue(::CompareFileTime(&srcCreation, &dstCreation) == 0,
            L"Dir CreationTime not preserved");
        Assert::IsTrue(::CompareFileTime(&srcWrite, &dstWrite) == 0,
            L"Dir LastWriteTime not preserved");
    }

    TEST_METHOD(CopyUpDirectory_LowerJunctionWithAds_CopiesTheLinkAndWritesNoStreamOntoItsTarget) {
        AssertLowerDirectoryLinkCopiesUpAsALink(CreateDirectoryJunction);
    }

    TEST_METHOD(CopyUpDirectory_LowerDirectorySymlinkWithAds_CopiesTheLinkAndWritesNoStreamOntoItsTarget) {
        AssertLowerDirectoryLinkCopiesUpAsALink(CreateDirectorySymlink);
    }

    TEST_METHOD(CopyUpFile_LowerFileSymlink_ReportsTheLowerLinkIdThroughTheLink) {
        AssertCopiedUpLinkKeepsTheLowerLinkId(
            CreateFileSymlink, LinkTarget::File, kDefaultHostCapabilities);
    }

    TEST_METHOD(CopyUpDirectory_LowerDirectorySymlink_ReportsTheLowerLinkIdThroughTheLink) {
        AssertCopiedUpLinkKeepsTheLowerLinkId(
            CreateDirectorySymlink, LinkTarget::Directory, kDefaultHostCapabilities);
    }

    TEST_METHOD(CopyUpDirectory_LowerJunction_ReportsTheLowerLinkIdThroughTheLink) {
        AssertCopiedUpLinkKeepsTheLowerLinkId(
            CreateDirectoryJunction, LinkTarget::Directory, kDefaultHostCapabilities);
    }

    TEST_METHOD(CopyUpFile_LowerFileSymlinkWithoutAds_ReportsTheLowerLinkIdThroughTheLink) {
        AssertCopiedUpLinkKeepsTheLowerLinkId(
            CreateFileSymlink, LinkTarget::File, kHostCapabilitiesWithoutAds);
    }

    TEST_METHOD(CopyUpDirectory_LowerDirectorySymlinkWithoutAds_ReportsTheLowerLinkIdThroughTheLink) {
        AssertCopiedUpLinkKeepsTheLowerLinkId(
            CreateDirectorySymlink, LinkTarget::Directory, kHostCapabilitiesWithoutAds);
    }

    TEST_METHOD(CopyUpDirectory_LowerJunctionWithoutAds_ReportsTheLowerLinkIdThroughTheLink) {
        AssertCopiedUpLinkKeepsTheLowerLinkId(
            CreateDirectoryJunction, LinkTarget::Directory, kHostCapabilitiesWithoutAds);
    }

    TEST_METHOD(CopyUpFile_LowerFile_ReportsTheLowerFileId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.txt", "lower");
            const UINT64 lowerId = NtfsFileIdOf(env.Lower(0) + L"\\a.txt", LinkOpen::Follow);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            {
                CopyUpAndRenameRig rig(config);
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"a.txt"),
                    L"The copy-up of the lower file must succeed");
            }
            ::LayerMount::LayerMount mount(config);

            Assert::AreEqual(lowerId, IndexNumberThroughMount(mount, L"a.txt", kNoCreateOptions),
                L"An open of the copied-up file must report the lower file's ID");
        });
    }

    TEST_METHOD(Open_FollowedUpperLinkToCopiedUpFile_ReportsTheTargetsStableId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.txt", "lower");
            const UINT64 lowerId = NtfsFileIdOf(env.Lower(0) + L"\\a.txt", LinkOpen::Follow);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            {
                CopyUpAndRenameRig rig(config);
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"a.txt"),
                    L"The copy-up of the lower file must succeed");
            }
            if (!LinkCreatedOrSkipped(CreateFileSymlink, env.Upper() + L"\\link.txt",
                                      env.Upper() + L"\\a.txt")) {
                return;
            }
            ::LayerMount::LayerMount mount(config);

            Assert::AreEqual(lowerId,
                IndexNumberThroughMount(mount, L"link.txt", kNoCreateOptions),
                L"An open that follows the link must report the copied-up target's stable ID");
            Assert::AreEqual(NtfsFileIdOf(env.Upper() + L"\\link.txt", LinkOpen::Itself),
                IndexNumberThroughMount(mount, L"link.txt", FILE_OPEN_REPARSE_POINT),
                L"An open of a link with no record must report the link's own file ID");
        });
    }

    TEST_METHOD(Open_FollowedUpperLinkWithTheUpperPathThroughAJunctionWithoutAds_ReportsTheTargetsStableId) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.txt", "lower");
        const std::wstring upperThroughJunction = env.Root() + L"\\upper-junction";
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, upperThroughJunction, env.Upper())) {
            return;
        }
        const UINT64 lowerId = NtfsFileIdOf(env.Lower(0) + L"\\a.txt", LinkOpen::Follow);
        LayerConfig config = env.MakeConfig();
        config.upperPath = upperThroughJunction;
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        {
            CopyUpAndRenameRig rig(config);
            AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"a.txt"),
                L"The copy-up of the lower file must succeed");
        }
        if (!LinkCreatedOrSkipped(CreateFileSymlink, env.Upper() + L"\\link.txt",
                                  env.Upper() + L"\\a.txt")) {
            return;
        }
        ::LayerMount::LayerMount mount(config);

        Assert::AreEqual(lowerId,
            IndexNumberThroughMount(mount, L"link.txt", kNoCreateOptions),
            L"An open that follows the link must report the copied-up target's stable ID");
    }

    TEST_METHOD(CopyUpFile_FailsAfterTheRecordWriteWithoutAds_LeavesNoRecordAtThePath) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.txt", "lower");
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        {
            AccessDenied timesDenied(env.Lower(0) + L"\\a.txt", FILE_WRITE_ATTRIBUTES);
            CopyUpAndRenameRig rig(config);
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            DisableRestorePrivilegeOnThread();

            Assert::IsFalse(NT_SUCCESS(rig.copyUp.CopyUpFile(L"a.txt")),
                L"The copy-up must fail when the upper copy refuses its times");
        }
        Assert::IsFalse(fs::exists(env.Upper() + L"\\a.txt"),
            L"The failed copy-up must remove the upper copy");
        env.WriteFile(env.Upper(), L"a.txt", "upper");
        ::LayerMount::LayerMount mount(config);

        Assert::AreEqual(NtfsFileIdOf(env.Upper() + L"\\a.txt", LinkOpen::Follow),
            IndexNumberThroughMount(mount, L"a.txt", kNoCreateOptions),
            L"A new file at the path must report its own ID, not the lower file's");
    }

    TEST_METHOD(RenameUpperDirectory_MovesDirectoryOnly) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"srcdir");
        env.WriteFile(env.Upper(), L"srcdir\\file.txt", "data");

        CopyUpAndRenameRig rig(env.MakeConfig());

        NTSTATUS status = rig.directoryRename.RenameUpperDirectory(
            CallerPath(L"srcdir"), CallerPath(L"dstdir"),
            RenameEntryKind::Directory, ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(status));

        std::wstring src = env.Upper() + L"\\srcdir";
        std::wstring dst = env.Upper() + L"\\dstdir";
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(src.c_str()),
            L"Source should no longer exist");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(dst.c_str()),
            L"Dest should exist");
        Assert::IsTrue(env.FileExists(env.Upper(), L"dstdir\\file.txt"),
            L"File should move with the directory");
    }

    TEST_METHOD(RenameLowerDirectory_CopiesUpAsOpaque) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"ldir");
        env.WriteFile(env.Lower(0), L"ldir\\file.txt", "lower data");

        CopyUpAndRenameRig rig(env.MakeConfig());

        NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"ldir"), CallerPath(L"newdir"),
            RenameEntryKind::Directory, ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\file.txt"));

        Assert::IsTrue(rig.whiteouts.IsOpaque(L"newdir"));
    }
};

TEST_CLASS(MetacopyTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CopyUpMetadataOnly_CreatesFileWithSourceSizeButNoData) {
        TempLayerEnvironment env(1);
        std::string content(4096, 'Z');
        env.WriteFile(env.Lower(0), L"lazy.bin", content);

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"lazy.bin");

        std::wstring upPath = env.Upper() + L"\\lazy.bin";

        HANDLE h = ::CreateFileW(upPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        Assert::AreNotEqual(INVALID_HANDLE_VALUE, h);
        LARGE_INTEGER sz = {};
        ::GetFileSizeEx(h, &sz);
        ::CloseHandle(h);
        Assert::AreEqual(static_cast<LONGLONG>(content.size()), sz.QuadPart,
            L"Logical size should match source");
    }

    TEST_METHOD(CopyUpMetadataOnly_MixedCaseLowerFile_NamesTheShellInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Lazy.BIN", "x");

        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpMetadataOnly(L"lazy.bin"),
            L"The metadata-only copy-up must succeed");

        Assert::AreEqual(std::wstring(L"Lazy.BIN"), StoredLeafName(env.Upper() + L"\\lazy.bin"),
            L"The upper shell must keep the lower's name");
    }

    TEST_METHOD(CopyUpMetadataOnly_PinnedCloudPlaceholderFile_GivesTheShellNoPinnedAttributeAndFillsTheLowersData) {
        CloudPlaceholderLayers layers;
        const std::string content(2 * 1024 * 1024, 'P');
        if (!layers.PlaceholderFileOrSkipped(L"big.bin", content) ||
            !layers.syncRoot.PinnedOrSkipped(layers.env.Lower(0) + L"\\big.bin")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpMetadataOnly(L"big.bin"),
            L"The metadata-only copy-up of the pinned placeholder file must succeed");
        Assert::IsFalse(HasAttribute(env.Upper() + L"\\big.bin", FILE_ATTRIBUTE_PINNED),
            L"The upper shell must not carry the pin state of the lower file");
        AssertStatus(STATUS_SUCCESS, rig.copyUp.CompleteLazyCopyUp(L"big.bin"),
            L"The fill of the upper shell must succeed");

        Assert::IsTrue(content == env.ReadFile(env.Upper(), L"big.bin"),
            L"The upper file must hold the data of the lower file");
    }

    TEST_METHOD(CopyUpMetadataOnly_WritesMetacopyADS) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"m.txt", "x");

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"m.txt");

        std::wstring upPath = env.Upper() + L"\\m.txt";
        LayerMountMetadata md = MetadataStore::ReadLayerMountMetadata(upPath, nullptr);
        Assert::IsTrue(md.metacopy, L"metacopy flag should be set");
        Assert::IsFalse(md.originLayer.empty(), L"originLayer should be set");
    }

    TEST_METHOD(CopyUpMetadataOnly_PreservesTimestamps) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"lts.txt", "x");

        std::wstring srcPath = env.Lower(0) + L"\\lts.txt";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"lts.txt");

        std::wstring upPath = env.Upper() + L"\\lts.txt";
        FILETIME dstCreation, dstAccess, dstWrite;
        {
            HANDLE h = ::CreateFileW(upPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &dstCreation, &dstAccess, &dstWrite);
            ::CloseHandle(h);
        }

        Assert::IsTrue(::CompareFileTime(&srcCreation, &dstCreation) == 0);
        Assert::IsTrue(::CompareFileTime(&srcWrite, &dstWrite) == 0);
    }

    TEST_METHOD(CopyUpMetadataOnly_PreservesAttributes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ro.bin", "x");
        std::wstring srcPath = env.Lower(0) + L"\\ro.bin";
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_READONLY);

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"ro.bin");

        std::wstring upPath = env.Upper() + L"\\ro.bin";
        DWORD attrs = ::GetFileAttributesW(upPath.c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_READONLY) != 0);
    }

    TEST_METHOD(CompleteLazyCopyUp_CopiesDataFromOriginLayer) {
        TempLayerEnvironment env(1);
        std::string content = "complete me please";
        env.WriteFile(env.Lower(0), L"cl.txt", content);

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"cl.txt");
        NTSTATUS status = rig.copyUp.CompleteLazyCopyUp(L"cl.txt");
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::AreEqual(content, env.ReadFile(env.Upper(), L"cl.txt"));
    }

    TEST_METHOD(CompleteLazyCopyUp_ClearsMetacopyFlag) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"mc.txt", "data");

        CopyUpAndRenameRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"mc.txt");
        std::wstring upPath = env.Upper() + L"\\mc.txt";
        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(upPath, nullptr).metacopy);

        rig.copyUp.CompleteLazyCopyUp(L"mc.txt");
        Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(upPath, nullptr).metacopy,
            L"metacopy flag should be cleared after completion");
    }

    TEST_METHOD(CompleteLazyCopyUp_AlreadyFullyCopied_IsNoOp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"full.txt", "data");

        CopyUpAndRenameRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.CompleteLazyCopyUp(L"full.txt");
        Assert::IsTrue(NT_SUCCESS(status),
            L"Should succeed as no-op when file is not a metacopy");
    }
};

}
