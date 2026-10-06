#include "pch.h"
#include "TestFixture.h"

#include "MetadataStore.h"
#include "WorkDirectory.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AccessDenied;
using LayerMountTestShared::DirectoryListingDenied;

namespace LayerMountTests {

namespace {

std::unique_ptr<WorkDirectory> OpenWorkDirectory(const LayerConfig& config) {
    std::unique_ptr<WorkDirectory> workDirectory;
    std::wstring error;
    const HRESULT hr = WorkDirectory::Open(config, &workDirectory, error);
    Assert::AreEqual<HRESULT>(S_OK, hr, (L"WorkDirectory::Open failed: " + error).c_str());
    Assert::IsTrue(workDirectory != nullptr, L"A successful open returns the work directory");
    return workDirectory;
}

bool Exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void MakeReadOnly(const std::wstring& path) {
    Assert::IsTrue(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
        (L"The test must make " + path + L" read-only").c_str());
}

void AssertStagingAreaEmpty(const TempLayerEnvironment& env, const wchar_t* message) {
    Assert::IsTrue(fs::is_directory(env.Staging()), L"The staging area exists after the open");
    Assert::IsTrue(EntriesUnder(env.Staging()).empty(), message);
}

}

TEST_CLASS(WorkDirectoryTests) {
public:
    TEST_METHOD(Open_LeftoverStagedTree_LeavesTheStagingAreaEmpty) {
        TempLayerEnvironment env(0);
        const std::wstring container = env.Staging() + L"\\#1.2.3.4.tmp";
        const std::wstring entry = container + L"\\entry";
        env.WriteFile(entry, L"sub\\data.txt", "staged");
        env.WriteFile(env.Staging(), L"#1.2.3.5.tmp", "staged");
        MakeReadOnly(entry + L"\\sub\\data.txt");
        MakeReadOnly(env.Staging() + L"\\#1.2.3.5.tmp");
        const AccessDenied entryDeniesDelete(entry, DELETE);
        const DirectoryListingDenied entryDeniesListing(entry);
        const AccessDenied containerDeniesChildDelete(container, FILE_DELETE_CHILD);

        const auto workDirectory = OpenWorkDirectory(env.MakeConfig());

        AssertStagingAreaEmpty(env, L"The open deletes every leftover in the staging area");
    }

    TEST_METHOD(Open_KeepsTheOtherEntriesOfTheWorkDirectory) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Work(), L"layers.manifest.json", "{}");
        env.CreateDir(env.Work(), L"temp_mounts\\id");
        env.WriteFile(env.Work(), L"#1.2.3.4.tmp", "beside the staging area");

        const auto workDirectory = OpenWorkDirectory(env.MakeConfig());

        Assert::IsTrue(Exists(env.Work() + L"\\layers.manifest.json"),
            L"A file of the host in the work directory stays");
        Assert::IsTrue(Exists(env.Work() + L"\\temp_mounts\\id"),
            L"A directory beside the staging area stays");
        Assert::IsTrue(Exists(env.Work() + L"\\#1.2.3.4.tmp"),
            L"An entry beside the staging area stays");
    }

    TEST_METHOD(Open_MissingStagingArea_CreatesIt) {
        TempLayerEnvironment env(0);
        std::error_code ec;
        fs::remove_all(env.Staging(), ec);
        Assert::IsFalse(Exists(env.Staging()), L"Precondition: the staging area is gone");

        const auto workDirectory = OpenWorkDirectory(env.MakeConfig());

        AssertStagingAreaEmpty(env, L"The open creates an empty staging area");
    }

    TEST_METHOD(Open_LeftoverLinks_DeletesTheLinksAndNotTheirTargets) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(0);
            env.WriteFile(env.Root(), L"target\\inside.txt", "target");
            const std::wstring target = env.Root() + L"\\target";
            env.CreateDir(env.Staging(), L"#1.2.3.4.tmp");
            if (!LinkCreatedOrSkipped(createLink, env.Staging() + L"\\#1.2.3.5.tmp", target) ||
                !LinkCreatedOrSkipped(createLink, env.Staging() + L"\\#1.2.3.4.tmp\\link",
                                      target)) {
                continue;
            }

            const auto workDirectory = OpenWorkDirectory(env.MakeConfig());

            AssertStagingAreaEmpty(env, L"The open deletes the leftover links");
            Assert::AreEqual(std::string("target"), env.ReadFile(target, L"inside.txt"),
                L"The open leaves the targets of the links as they were");
        }
    }

    TEST_METHOD(Open_SidecarStore_RemovesTheRecordsOfTheLeftovers) {
        TempLayerEnvironment env(0);
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        config.workDirPath = ExtendedDirWithSeparator(env.Work());
        std::wstring stagedPath;
        {
            CopyUpAndRenameRig rig(config);
            stagedPath = rig.copyUp.GenerateStagingPath();
        }
        const std::wstring childPath = stagedPath + L"\\child.txt";
        env.WriteFile(stagedPath, L"child.txt", "staged");
        LayerMountMetadata record;
        record.metacopy = true;
        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(stagedPath, record, &config),
            L"The test must write the record of the staged directory");
        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(childPath, record, &config),
            L"The test must write the record of the staged file");
        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(stagedPath, &config),
            L"The test must mark the staged directory opaque");

        const auto workDirectory = OpenWorkDirectory(config);

        AssertStagingAreaEmpty(env, L"The open deletes the leftover");
        Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(stagedPath, &config).metacopy,
            L"The record of the deleted directory is gone");
        Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(childPath, &config).metacopy,
            L"The record of the deleted file is gone");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(stagedPath, &config),
            L"The opaque marker of the deleted directory is gone");
    }

    TEST_METHOD(Open_HeldWorkDirectory_FailsAsBusyUntilTheHolderCloses) {
        TempLayerEnvironment env(0);
        auto first = OpenWorkDirectory(env.MakeConfig());
        LayerConfig alias = env.MakeConfig();
        alias.workDirPath = ExtendedDirWithSeparator(env.Work());

        std::unique_ptr<WorkDirectory> second;
        std::wstring error;
        const HRESULT hr = WorkDirectory::Open(alias, &second, error);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BUSY), hr,
            L"A second open of a held work directory fails as busy");
        Assert::IsTrue(second == nullptr, L"The failed open returns no work directory");
        Assert::IsFalse(error.empty(), L"The failed open says why");

        first.reset();
        const auto third = OpenWorkDirectory(env.MakeConfig());
    }

    TEST_METHOD(Open_HeldUpperWithAnotherWorkDirectory_FailsAsBusyNamingTheUpper) {
        TempLayerEnvironment env(0);
        auto first = OpenWorkDirectory(env.MakeConfig());
        LayerConfig other = env.MakeConfig();
        other.workDirPath = env.Root() + L"\\other-work";
        env.CreateDir(env.Root(), L"other-work");

        std::unique_ptr<WorkDirectory> second;
        std::wstring error;
        const HRESULT hr = WorkDirectory::Open(other, &second, error);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BUSY), hr,
            L"An open of a held upper fails as busy, whatever its work directory");
        Assert::IsTrue(second == nullptr, L"The failed open returns no work directory");
        Assert::IsTrue(error.find(env.Upper()) != std::wstring::npos,
            (L"The error names the upper: " + error).c_str());

        first.reset();
        const auto third = OpenWorkDirectory(other);
    }

    TEST_METHOD(Open_WorkDirectoryInTheUppersSidecarStore_TakesBothLocks) {
        TempLayerEnvironment env(0);
        LayerConfig config = env.MakeConfig();
        config.workDirPath = env.Upper() + L"\\" + kSidecarDirName;
        env.CreateDir(env.Upper(), kSidecarDirName);

        const auto workDirectory = OpenWorkDirectory(config);

        Assert::IsTrue(fs::is_directory(StagingAreaPath(config.workDirPath)),
            L"The staging area is in <upper>\\.overlay");
        std::unique_ptr<WorkDirectory> second;
        std::wstring error;
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BUSY),
            WorkDirectory::Open(env.MakeConfig(), &second, error),
            L"The overlay holds the upper while its work directory is in the upper");
    }

    TEST_METHOD(Open_LeftoverThatCannotBeDeleted_FailsNamingItAndReleasesTheLock) {
        TempLayerEnvironment env(0);
        const std::wstring held = env.Staging() + L"\\#1.2.3.4.tmp\\held.txt";
        env.WriteFile(env.Staging(), L"#1.2.3.4.tmp\\held.txt", "staged");
        std::unique_ptr<WorkDirectory> workDirectory;
        std::wstring error;
        HRESULT hr = S_OK;
        {
            const ScopedHandle holder = HoldOpen(held, FILE_SHARE_READ | FILE_SHARE_WRITE);
            hr = WorkDirectory::Open(env.MakeConfig(), &workDirectory, error);
        }

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), hr,
            L"A leftover that cannot be deleted fails the open with the delete's error");
        Assert::IsTrue(workDirectory == nullptr, L"The failed open returns no work directory");
        Assert::IsTrue(error.find(held) != std::wstring::npos,
            (L"The error names the leftover: " + error).c_str());
        const auto reopened = OpenWorkDirectory(env.MakeConfig());
        AssertStagingAreaEmpty(env, L"An open after the holder closes deletes the leftover");
    }
};

}
