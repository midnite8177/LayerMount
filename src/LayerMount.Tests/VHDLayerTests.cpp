#include "pch.h"
#include "TestFixture.h"

#include "ExtendedAttributeTestHelpers.h"

#include "LayerMount.h"
#include "Manifest.h"
#include "VHDLayerManager.h"
#include "VolumeGuid.h"

#include <fstream>
#include <iterator>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

namespace {

// Creates a unique %TEMP% workspace with a vhd subdirectory and returns
// its root.
std::wstring MakeVhdWorkspace() {
    const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root + L"\\vhd", ec);
    return root;
}

void CleanupWorkspace(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Runs format.com with /FS:fileSystem on the volume and returns its exit
// code, or the Win32 error when format.com does not start or does not
// finish in a minute.
DWORD FormatVolume(const std::wstring& volumeGuid, const std::wstring& fileSystem) {
    std::wstring volume = volumeGuid;
    if (!volume.empty() && volume.back() == L'\\') volume.pop_back();
    std::wstring cmdLine = L"format.com " + volume + L" /FS:" + fileSystem + L" /Q /Y /X /V:LMFAT";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return ::GetLastError();
    }
    DWORD exitCode = ERROR_TIMEOUT;
    if (::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return exitCode;
}

// Creates a VHD at vhdPath, attaches it read-write, and gives its one volume
// the file system fileSystem, as format.com names it. volumeGuid receives the
// volume's \\?\Volume{GUID}\ path. The attach ends when the handle closes.
void MakeVolume(LayerMount::VHD::VHDLayerManager& mgr,
                const std::wstring& vhdPath,
                const std::wstring& fileSystem,
                LayerMount::VHD::VhdHandle& handle,
                std::wstring& volumeGuid) {
    LayerMount::VHD::VhdHandle createHandle;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        mgr.CreateVHD(vhdPath, 100ULL * 1024 * 1024, /*dynamic*/ true, createHandle));
    createHandle.Close();
    std::wstring physicalPath;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        mgr.AttachVHD(vhdPath, /*readOnly*/ false, handle, physicalPath,
                       LayerMount::VHD::AttachLifetime::ProcessScoped,
                       /*suppressDriveLetter=*/ true));
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.InitializeVHD(physicalPath, vhdPath),
        L"InitializeVHD must partition the disk and create its volume");
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        LayerMount::VHD::GetVolumeGuidForPhysicalDisk(physicalPath, volumeGuid));
    volumeGuid = LayerMount::VHD::EnsureTrailingBackslash(volumeGuid);
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, FormatVolume(volumeGuid, fileSystem),
        (L"format.com must give the volume a " + fileSystem + L" file system").c_str());

    wchar_t fsName[MAX_PATH] = {};
    Assert::IsTrue(::GetVolumeInformationW(volumeGuid.c_str(), nullptr, 0, nullptr, nullptr,
                                           nullptr, fsName, MAX_PATH) != FALSE,
        L"GetVolumeInformationW must read the formatted volume");
    Assert::IsTrue(::_wcsicmp(fileSystem.c_str(), fsName) == 0,
        (L"The test volume must be " + fileSystem).c_str());
}

}

TEST_CLASS(VHDLayerTests) {
public:
    TEST_METHOD(CheckElevation_ReturnsErrorSuccessWhenAdmin) {
        UNIT_SKIP_IF_NOT_ADMIN();
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                                LayerMount::VHD::VHDLayerManager::CheckElevation());
    }

    TEST_METHOD(CreateDynamicVHDX_ProducesValidFile) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\dynamic.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle handle;
        const DWORD rc = mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, /*dynamic*/ true, handle);

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc,
                                L"CreateVHD should succeed");
        Assert::IsTrue(handle.IsValid(), L"CreateVHD should return a valid handle");
        Assert::IsTrue(std::filesystem::exists(vhdPath),
                       L"VHDX file must exist on disk");

        handle.Close();
        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachVHD_ReturnsPhysicalPath) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\attachable.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle createHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, true, createHandle));
        createHandle.Close();

        LayerMount::VHD::VhdHandle attachHandle;
        std::wstring physicalPath;
        const DWORD rc = mgr.AttachVHD(vhdPath, /*readOnly*/ false,
                                        attachHandle, physicalPath,
                                        LayerMount::VHD::AttachLifetime::Permanent,
                                        /*suppressDriveLetter=*/ false);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc);
        Assert::IsFalse(physicalPath.empty(),
                        L"AttachVHD must return a \\\\.\\PhysicalDriveN path");

        mgr.DetachVHD(vhdPath);
        CleanupWorkspace(root);
    }

    TEST_METHOD(DifferencingVHD_ChildAttaches) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring parent = root + L"\\vhd\\parent.vhdx";
        const std::wstring child  = root + L"\\vhd\\child.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");

        LayerMount::VHD::VhdHandle parentHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(parent, 64ULL * 1024 * 1024, true, parentHandle));
        parentHandle.Close();

        LayerMount::VHD::VhdHandle childHandle;
        const DWORD rc = mgr.CreateDifferencingVHD(child, parent, childHandle);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc,
                                L"CreateDifferencingVHD should succeed");
        childHandle.Close();

        Assert::IsTrue(std::filesystem::exists(child),
                       L"Child differencing VHDX must exist");
        const auto childSize = std::filesystem::file_size(child);
        Assert::IsTrue(childSize > 0, L"Child VHDX file must have nonzero size");

        CleanupWorkspace(root);
    }

    TEST_METHOD(ExportToDirectory_SkipsNtfsSystemDirsAndCopiesUserContent) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src    = root + L"\\src";
        const std::wstring nested = src + L"\\nested";
        const std::wstring vhd    = root + L"\\exp.vhdx";
        const std::wstring out    = root + L"\\out";

        std::filesystem::create_directories(nested);
        { std::ofstream(src + L"\\a.txt")          << "hello"; }
        { std::ofstream(nested + L"\\b.txt")       << "world"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");

        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.ImportDirectory(src, vhd, /*sizeBytes=*/ 0),
            L"Import must succeed before we can export");

        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.ExportToDirectory(vhd, out),
            L"Export must succeed on a freshly-imported NTFS volume");

        Assert::IsTrue(std::filesystem::exists(out + L"\\a.txt"),
                       L"Top-level user file must land in destination");
        Assert::IsTrue(std::filesystem::exists(out + L"\\nested\\b.txt"),
                       L"Nested user file must land in destination");

        Assert::IsFalse(std::filesystem::exists(out + L"\\System Volume Information"),
                        L"System Volume Information must be filtered out");

        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachVHD_SuppressDriveLetter_NoNewDriveAssigned) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src = root + L"\\src";
        const std::wstring vhd = root + L"\\suppressed.vhdx";
        std::filesystem::create_directories(src);
        { std::ofstream(src + L"\\marker.txt") << "x"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ImportDirectory(src, vhd, /*sizeBytes=*/ 0));

        const DWORD before = ::GetLogicalDrives();

        LayerMount::VHD::VhdHandle handle;
        std::wstring physicalPath;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.AttachVHD(vhd, /*readOnly*/ true, handle, physicalPath,
                           LayerMount::VHD::AttachLifetime::ProcessScoped,
                           /*suppressDriveLetter=*/ true));

        // The Mount Manager assigns a drive letter asynchronously, within a
        // few hundred ms of an attach.
        ::Sleep(750);
        const DWORD after = ::GetLogicalDrives();

        const DWORD newLetters = after & ~before;

        mgr.DetachVHD(vhd);
        handle.Close();

        Assert::AreEqual<DWORD>(0, newLetters,
            L"No new drive letter must appear when suppressDriveLetter=true");

        CleanupWorkspace(root);
    }

    TEST_METHOD(ImportThenExport_RoundTripsUserContent) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src  = root + L"\\src";
        const std::wstring vhd  = root + L"\\roundtrip.vhdx";
        const std::wstring out  = root + L"\\out";

        std::filesystem::create_directories(src + L"\\dir1");
        { std::ofstream(src + L"\\top.txt")      << "top-content"; }
        { std::ofstream(src + L"\\dir1\\nested") << "nested-content"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ImportDirectory(src, vhd, /*sizeBytes=*/ 0));
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ExportToDirectory(vhd, out));

        auto ReadAll = [](const std::wstring& p) -> std::string {
            std::ifstream f(p, std::ios::binary);
            return { std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>() };
        };

        Assert::AreEqual(std::string("top-content"),    ReadAll(out + L"\\top.txt"));
        Assert::AreEqual(std::string("nested-content"), ReadAll(out + L"\\dir1\\nested"));

        CleanupWorkspace(root);
    }

    TEST_METHOD(ImportThenExport_ExtendedWorkDirWithTrailingSeparator_RoundTripsUserContent) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src  = root + L"\\src";
        const std::wstring vhd  = root + L"\\roundtrip.vhdx";
        const std::wstring out  = root + L"\\out";

        std::filesystem::create_directories(src);
        { std::ofstream(src + L"\\top.txt") << "top-content"; }

        LayerMount::VHD::VHDLayerManager mgr(ExtendedDirWithSeparator(root + L"\\vhd"));
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ImportDirectory(src, vhd, /*sizeBytes=*/ 0),
                                L"Import must mount the VHD under a work dir that ends in a separator");
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ExportToDirectory(vhd, out),
                                L"Export must mount the VHD under a work dir that ends in a separator");

        std::ifstream f(out + L"\\top.txt", std::ios::binary);
        Assert::AreEqual(std::string("top-content"),
                         std::string(std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>()));

        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachLifetime_Permanent_SurvivesHandleClose) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\permanent.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle createHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, true, createHandle));
        createHandle.Close();

        LayerMount::VHD::VhdHandle attachHandle;
        std::wstring physicalPath;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.AttachVHD(vhdPath, /*readOnly*/ false, attachHandle,
                           physicalPath,
                           LayerMount::VHD::AttachLifetime::Permanent,
                           /*suppressDriveLetter=*/ false));
        attachHandle.Close();

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.DetachVHD(vhdPath),
            L"Permanent attach must survive handle close until DetachVHD");

        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachLifetime_ProcessScoped_ReleasesOnHandleClose) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\scoped.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle createHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, true, createHandle));
        createHandle.Close();

        std::wstring physicalPath;
        {
            LayerMount::VHD::VhdHandle attachHandle;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                mgr.AttachVHD(vhdPath, /*readOnly*/ false, attachHandle,
                               physicalPath,
                               LayerMount::VHD::AttachLifetime::ProcessScoped,
                               /*suppressDriveLetter=*/ false));
            Assert::IsFalse(physicalPath.empty());
        }

        LayerMount::VHD::VhdHandle reattachHandle;
        std::wstring newPhysicalPath;
        const DWORD rc = mgr.AttachVHD(vhdPath, /*readOnly*/ false,
                                        reattachHandle, newPhysicalPath,
                                        LayerMount::VHD::AttachLifetime::ProcessScoped,
                                        /*suppressDriveLetter=*/ false);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc,
            L"Re-attach after ProcessScoped handle close must succeed");

        reattachHandle.Close();
        CleanupWorkspace(root);
    }

    TEST_METHOD(ResolvePath_FileInVhdLowerOverDeeperLowerDirectory_HidesDeeperChildren) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src = root + L"\\src";
        const std::wstring vhd = root + L"\\vhd\\layer.vhdx";
        std::filesystem::create_directories(src + L"\\layer");
        { std::ofstream(src + L"\\layer\\d") << "vhd"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ImportDirectory(src, vhd, /*sizeBytes=*/ 0));
        LayerMount::VHD::VhdHandle handle;
        std::wstring physicalPath;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.AttachVHD(vhd, /*readOnly*/ true, handle, physicalPath,
                           LayerMount::VHD::AttachLifetime::ProcessScoped,
                           /*suppressDriveLetter=*/ true));
        std::wstring volumeGuid;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            LayerMount::VHD::GetVolumeGuidForPhysicalDisk(physicalPath, volumeGuid));

        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "deeper");
        auto config = env.MakeConfig();
        config.lowerPaths.insert(config.lowerPaths.begin(),
                                 LayerMount::VHD::EnsureTrailingBackslash(volumeGuid) + L"layer");
        ::LayerMount::Cache cache;
        ::LayerMount::WhiteoutManager wm(config, &cache);
        ::LayerMount::PathResolver resolver(config, wm, cache);
        const ::LayerMount::ResolvedPath file = resolver.ResolvePath(L"d");
        const bool childFound = resolver.ResolvePath(L"d\\inner.txt").Found();

        mgr.DetachVHD(vhd);
        handle.Close();
        CleanupWorkspace(root);

        Assert::IsTrue(file.Found() && file.lowerIndex == 0, L"The VHD lower's file must resolve");
        Assert::IsFalse(childFound,
            L"The file in the VHD lower must hide the children of the deeper lower's directory");
    }

    TEST_METHOD(ResolvePath_FileInFat32LowerOverDeeperLowerDirectory_HidesDeeperChildren) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhd = root + L"\\vhd\\fat32.vhdx";
        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle handle;
        std::wstring volumeGuid;
        MakeVolume(mgr, vhd, L"FAT32", handle, volumeGuid);

        TempLayerEnvironment env(1);
        const std::wstring fatLayer = volumeGuid + L"layer";
        env.WriteFile(fatLayer, L"d", "fat32");
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "deeper");
        auto config = env.MakeConfig();
        config.lowerPaths.insert(config.lowerPaths.begin(), fatLayer);
        ::LayerMount::Cache cache;
        ::LayerMount::WhiteoutManager wm(config, &cache);
        ::LayerMount::PathResolver resolver(config, wm, cache);
        const ::LayerMount::ResolvedPath file = resolver.ResolvePath(L"d");
        const bool childFound = resolver.ResolvePath(L"d\\inner.txt").Found();

        mgr.DetachVHD(vhd);
        handle.Close();
        CleanupWorkspace(root);

        Assert::IsTrue(file.Found() && file.lowerIndex == 0, L"The FAT32 lower's file must resolve");
        Assert::IsFalse(childFound,
            L"The file in the FAT32 lower must hide the children of the deeper lower's directory");
    }

    TEST_METHOD(ResolvePath_FileInUpperOverFat32LowerDirectory_HidesLowerChildren) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhd = root + L"\\vhd\\fat32.vhdx";
        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle handle;
        std::wstring volumeGuid;
        MakeVolume(mgr, vhd, L"FAT32", handle, volumeGuid);

        TempLayerEnvironment env(0);
        const std::wstring fatLayer = volumeGuid + L"layer";
        env.WriteFile(fatLayer, L"d\\inner.txt", "fat32");
        env.WriteFile(env.Upper(), L"d", "upper");
        auto config = env.MakeConfig();
        config.lowerPaths.push_back(fatLayer);
        ::LayerMount::Cache cache;
        ::LayerMount::WhiteoutManager wm(config, &cache);
        ::LayerMount::PathResolver resolver(config, wm, cache);
        const ::LayerMount::ResolvedPath lowerDir = resolver.ResolveLowerPath(L"d");
        const bool childFound = resolver.ResolvePath(L"d\\inner.txt").Found();

        mgr.DetachVHD(vhd);
        handle.Close();
        CleanupWorkspace(root);

        Assert::IsTrue(lowerDir.Found() && (lowerDir.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"The FAT32 lower's directory must resolve through the lower lookup");
        Assert::IsFalse(childFound,
            L"The upper file must hide the children of the FAT32 lower's directory");
    }

    TEST_METHOD(CopyUpFile_UpperOnExFatVolume_CopiesUpLowerFileWithExtendedAttributes) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhd = root + L"\\vhd\\exfat.vhdx";
        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle handle;
        std::wstring volumeGuid;
        MakeVolume(mgr, vhd, L"exFAT", handle, volumeGuid);

        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"wsl.txt", "content");
        const NTSTATUS eaStatus =
            SetExtendedAttribute(env.Lower(0) + L"\\wsl.txt", "$LXUID", LittleEndianUlong(1000));
        LayerMount::LayerConfig config = env.MakeConfig();
        config.upperPath = volumeGuid + L"upper";
        config.workDirPath = volumeGuid + L"work";
        config.hostCapabilities &= ~LM_CAP_ADS;
        ::CreateDirectoryW(config.upperPath.c_str(), nullptr);
        ::CreateDirectoryW(config.workDirPath.c_str(), nullptr);
        NTSTATUS copyUpStatus = STATUS_UNSUCCESSFUL;
        std::string upperContent;
        {
            CopyUpAndRenameRig rig(config);
            copyUpStatus = rig.copyUp.CopyUpFile(L"wsl.txt");
            std::ifstream upperFile(config.upperPath + L"\\wsl.txt", std::ios::binary);
            upperContent.assign(std::istreambuf_iterator<char>(upperFile),
                                std::istreambuf_iterator<char>());
        }

        mgr.DetachVHD(vhd);
        handle.Close();
        CleanupWorkspace(root);

        AssertStatus(STATUS_SUCCESS, eaStatus,
            L"The test must set an extended attribute on the lower file");
        AssertStatus(STATUS_SUCCESS, copyUpStatus,
            L"A copy-up to an upper without extended attribute support must succeed");
        Assert::AreEqual(std::string("content"), upperContent,
            L"The upper copy must have the data of the lower file");
    }

    TEST_METHOD(Prepare_WorkDirectoryOnAnotherVolume_FailsWithInvalidArg) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhd = root + L"\\vhd\\work.vhdx";
        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle handle;
        std::wstring volumeGuid;
        MakeVolume(mgr, vhd, L"FAT32", handle, volumeGuid);

        TempLayerEnvironment env(0);
        LayerMount::LayerConfig config = env.MakeConfig();
        config.workDirPath = volumeGuid + L"work";
        const bool workMade = ::CreateDirectoryW(config.workDirPath.c_str(), nullptr) != FALSE;
        std::wstring error;
        const HRESULT hr = config.Prepare(error);

        mgr.DetachVHD(vhd);
        handle.Close();
        CleanupWorkspace(root);

        Assert::IsTrue(workMade, L"The test must make the work directory on the FAT32 volume");
        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"A work directory on another volume than the upper fails the prepare");
        Assert::IsFalse(error.empty(), L"The failed prepare says why");
    }
};

TEST_CLASS(ManifestPathTests) {
public:
    TEST_METHOD(DefaultPath_IsCanonicalFileName) {
        const std::wstring dir = L"C:\\Temp\\demo";
        const std::wstring resolved = LayerMount::VHD::Manifest::DefaultPath(dir);
        Assert::IsTrue(resolved.find(L"layers.manifest.json") != std::wstring::npos,
                       L"DefaultPath must resolve to layers.manifest.json");
    }

    TEST_METHOD(ManagerWriteAndCliReadAgreeOnPath) {
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);

        {
            LayerMount::VHD::Manifest m;
            LayerMount::VHD::LayerEntry entry;
            entry.id = L"test-id-123";
            entry.type = LayerMount::VHD::LayerType::VHD;
            entry.path = root + L"\\sample.vhdx";
            entry.createdAt = L"2026-04-16T00:00:00Z";
            m.AddLayer(entry);
            Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                m.Save(LayerMount::VHD::Manifest::DefaultPath(root)));
        }

        {
            LayerMount::VHD::Manifest m;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                m.Load(LayerMount::VHD::Manifest::DefaultPath(root)));
            const auto* entry = m.GetLayer(L"test-id-123");
            Assert::IsNotNull(entry, L"Layer written via DefaultPath must be readable via DefaultPath");
            Assert::IsTrue(entry->type == LayerMount::VHD::LayerType::VHD);
        }

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    TEST_METHOD(Manifest_RemoveLayerAndSave_RoundTrip) {
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);

        {
            LayerMount::VHD::Manifest m;
            LayerMount::VHD::LayerEntry e;
            e.id = L"layer-a";
            e.type = LayerMount::VHD::LayerType::VHD;
            e.path = root + L"\\a.vhdx";
            m.AddLayer(e);
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Save(path));
        }

        {
            LayerMount::VHD::Manifest m;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Load(path));
            Assert::IsTrue(m.RemoveLayer(L"layer-a"));
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Save(path));
        }

        {
            LayerMount::VHD::Manifest m;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Load(path));
            Assert::IsNull(m.GetLayer(L"layer-a"),
                           L"After unregister + save, entry must be absent on fresh load");
        }

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    TEST_METHOD(Manifest_RemoveLayer_MissingIsIdempotent) {
        LayerMount::VHD::Manifest m;
        Assert::IsFalse(m.RemoveLayer(L"never-existed"),
                        L"Removing a non-existent id must return false, not throw");
    }

    TEST_METHOD(Manifest_SaveIsAtomic_NoPartialFileOnCrash) {
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);

        LayerMount::VHD::Manifest m;
        LayerMount::VHD::LayerEntry e;
        e.id = L"atomicity-probe";
        e.type = LayerMount::VHD::LayerType::VHD;
        e.path = L"C:\\ignored.vhdx";
        m.AddLayer(e);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Save(path));

        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(root, ec)) {
            auto name = de.path().filename().wstring();
            Assert::IsFalse(name.find(L".tmp.") != std::wstring::npos,
                            L"No .tmp.* stragglers must remain after a successful Save");
        }

        LayerMount::VHD::Manifest reloaded;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, reloaded.Load(path));
        Assert::IsNotNull(reloaded.GetLayer(L"atomicity-probe"));

        std::filesystem::remove_all(root, ec);
    }

    TEST_METHOD(Manager_OnCorruptRegistry_LeavesRegistryBytesUnchanged) {
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);
        const std::string corrupt = "{ \"schemaVersion\": 1, \"layers\": [ truncated";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(corrupt.data(), static_cast<std::streamsize>(corrupt.size()));
        }

        {
            LayerMount::VHD::VHDLayerManager manager(root);
        }

        std::string after;
        {
            std::ifstream in(path, std::ios::binary);
            after.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);

        Assert::IsTrue(after == corrupt,
            L"Constructing and destroying the manager must not rewrite a registry it cannot parse");
    }

    TEST_METHOD(ManifestLock_SerializesAcrossThreads) {
        // A Win32 mutex is recursive for its owning thread, so only a second
        // thread can observe that the lock is held.
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);

        LayerMount::VHD::ManifestLock first(path, /*timeoutMs=*/ 200);
        Assert::IsTrue(first.Held(), L"First lock must acquire the mutex");

        bool peerHeld = true;
        std::thread peer([&] {
            LayerMount::VHD::ManifestLock second(path, /*timeoutMs=*/ 200);
            peerHeld = second.Held();
        });
        peer.join();

        Assert::IsFalse(peerHeld,
            L"Peer-thread ManifestLock must time out while main thread owns the mutex");

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

}
