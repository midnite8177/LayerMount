#include "pch.h"
#include "AbiTestFixture.h"
#include "DeepPathAbiHelpers.h"

#include <sddl.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

bool HostDirectoryExists(const std::wstring& extendedPath) {
    const DWORD attributes = ::GetFileAttributesW(extendedPath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void MakeDeepLowerDirectory(const TempLayerEnv& env, const std::wstring& name,
                            const std::vector<std::wstring>& children) {
    const std::wstring directory = ExtendedHostPath(env.Lower(0), name);
    std::filesystem::create_directories(directory);
    for (const std::wstring& child : children) {
        WriteText(directory + L"\\" + child, "child");
    }
}

HRESULT LM_CALL CollectName(PCWSTR name, const LM_FILE_INFO*, void* userContext) {
    static_cast<std::vector<std::wstring>*>(userContext)->push_back(name);
    return S_OK;
}

std::vector<std::wstring> SortedMergedNames(LM_HANDLE mount, const std::wstring& mountPath) {
    std::vector<std::wstring> names;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountMergeDirectory(mount, mountPath.c_str(), &CollectName, &names),
        L"The listing of the deep directory succeeds");
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::wstring& name) {
                                   return name == L"." || name == L"..";
                               }),
                names.end());
    std::sort(names.begin(), names.end());
    return names;
}

UINT64 IndexNumberThroughMount(LM_HANDLE mount, const std::wstring& mountPath) {
    OpenedFile opened;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, mountPath.c_str(), GENERIC_READ, kNoCreateOptions, opened),
        L"The read open of the deep file succeeds");
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    return opened.info.indexNumber;
}

}

TEST_CLASS(AbiDeepLowerPathTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        LayerMountTestShared::AssertHostRefusesPlainPathsPastMaxPath();
    }

    TEST_METHOD(WriteFile_DeepLowerFile_CopiesTheFileUpAndKeepsTheLower) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(name).c_str(),
                            GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened),
            L"The write open of the deep lower file succeeds");
        UINT32 written = 0;
        LM_FILE_INFO info{};
        const HRESULT writeHr = WriteFromStart(opened.handle, "upper", 5, &written, &info);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        Assert::AreEqual<HRESULT>(S_OK, writeHr);
        Assert::AreEqual<std::string>("upper",
            ReadAllBytes(ExtendedHostPath(env.Upper(), name)),
            L"The copy-up writes the deep upper file");
        Assert::AreEqual<std::string>("lower",
            ReadAllBytes(ExtendedHostPath(env.Lower(0), name)),
            L"The copy-up leaves the deep lower file as it was");
        Assert::AreEqual<std::string>("upper",
            ReadThroughMount(mount.Get(), MountPathOf(name), GENERIC_READ),
            L"The mount reads the copied-up deep file");
    }

    TEST_METHOD(OpenFile_DeepLowerMetacopyShell_FillsTheShellWithTheLowerData) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        const std::string content(static_cast<size_t>(kAboveMetacopyThresholdBytes), 'L');
        WriteText(ExtendedHostPath(env.Lower(0), name), content);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile staged;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(name).c_str(), kAttributeOnlyAccess,
                            kNoCreateOptions, staged),
            L"The attribute-only open of the deep lower file succeeds");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(staged.handle));
        const std::wstring upperPath = ExtendedHostPath(env.Upper(), name);
        Assert::IsTrue((::GetFileAttributesW(upperPath.c_str()) & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
            L"Precondition: the attribute-only open staged a metacopy shell");

        Assert::AreEqual<std::string>(content.substr(0, 64),
            ReadThroughMount(mount.Get(), MountPathOf(name), GENERIC_READ),
            L"The read open fills the deep shell and reads the lower data");
        Assert::IsTrue(ReadAllBytes(upperPath) == content,
            L"The fill writes the whole lower data into the deep upper file");
    }

    TEST_METHOD(DeleteFile_DeepLowerFile_WritesAWhiteoutThatHidesIt) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), MountPathOf(name).c_str()));

        Assert::IsTrue(HostEntryExists(ExtendedHostPath(env.Upper(), L".wh." + name)),
            L"The delete writes the whiteout of the deep lower file");
        Assert::IsTrue(HostEntryExists(ExtendedHostPath(env.Lower(0), name)),
            L"The delete leaves the deep lower file");
        AssertOpenFailsNotFound(mount.Get(), MountPathOf(name).c_str(),
            L"The whiteout hides the deep lower file");
    }

    TEST_METHOD(CreateFile_OverDeletedDeepLowerFile_RemovesTheWhiteout) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), MountPathOf(name).c_str()));

        WriteThroughOverlay(mount.Get(), MountPathOf(name).c_str(), "new");

        Assert::IsFalse(HostEntryExists(ExtendedHostPath(env.Upper(), L".wh." + name)),
            L"The create removes the whiteout of the deep name");
        Assert::AreEqual<std::string>("new",
            ReadThroughMount(mount.Get(), MountPathOf(name), GENERIC_READ),
            L"The mount reads the new deep file");
    }

    TEST_METHOD(RenameFile_DeepLowerFile_MovesItAndHidesTheOldName) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring source = DeepNameSizedBy(env.Lower(0), L"Source");
        const std::wstring target = DeepNameSizedBy(env.Lower(0), L"Target");
        WriteText(ExtendedHostPath(env.Lower(0), source), "moved");
        LayerMountHolder mount = CreateLayerMount(env);

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), MountPathOf(source).c_str(),
                                   MountPathOf(target).c_str(), replaceIfExists));

        AssertOpenFailsNotFound(mount.Get(), MountPathOf(source).c_str(),
            L"The old deep name is gone from the mount");
        Assert::AreEqual<std::string>("moved",
            ReadThroughMount(mount.Get(), MountPathOf(target), GENERIC_READ),
            L"The new deep name opens the moved file");
        Assert::AreEqual<std::string>("moved",
            ReadAllBytes(ExtendedHostPath(env.Upper(), target)),
            L"The rename puts the data at the deep upper name");
    }

    TEST_METHOD(MergeDirectory_DeepLowerDirectory_ListsItsEntries) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        MakeDeepLowerDirectory(env, name, {L"a.txt", L"b.txt"});
        LayerMountHolder mount = CreateLayerMount(env);

        const std::vector<std::wstring> expected = {L"a.txt", L"b.txt"};
        Assert::IsTrue(expected == SortedMergedNames(mount.Get(), MountPathOf(name)),
            L"The listing of the deep lower directory shows its entries");
    }

    TEST_METHOD(CreateFile_InDeepLowerDirectory_CopiesTheDirectoryUp) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        MakeDeepLowerDirectory(env, name, {L"a.txt"});
        LayerMountHolder mount = CreateLayerMount(env);

        WriteThroughOverlay(mount.Get(), (MountPathOf(name) + L"\\new.txt").c_str(), "new");

        Assert::IsTrue(HostDirectoryExists(ExtendedHostPath(env.Upper(), name)),
            L"The create copies the deep lower directory up");
        Assert::AreEqual<std::string>("new",
            ReadAllBytes(ExtendedHostPath(env.Upper(), name + L"\\new.txt")),
            L"The create writes the new file in the deep upper directory");
        const std::vector<std::wstring> expected = {L"a.txt", L"new.txt"};
        Assert::IsTrue(expected == SortedMergedNames(mount.Get(), MountPathOf(name)),
            L"The listing merges the lower entry and the new entry");
    }

    TEST_METHOD(CreateDirectory_InDeepLowerDirectory_MakesTheDeepUpperDirectory) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        MakeDeepLowerDirectory(env, name, {});
        LayerMountHolder mount = CreateLayerMount(env);

        CreateDirectoryThroughMount(mount.Get(), MountPathOf(name) + L"\\sub");

        Assert::IsTrue(HostDirectoryExists(ExtendedHostPath(env.Upper(), name + L"\\sub")),
            L"The create makes the directory under the deep upper directory");
        const std::vector<std::wstring> expected = {L"sub"};
        Assert::IsTrue(expected == SortedMergedNames(mount.Get(), MountPathOf(name)),
            L"The listing shows the new directory");
    }

    TEST_METHOD(DeleteFile_DeepLowerDirectory_WritesAWhiteoutThatHidesIt) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        MakeDeepLowerDirectory(env, name, {});
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), MountPathOf(name).c_str()));

        Assert::IsTrue(HostEntryExists(ExtendedHostPath(env.Upper(), L".wh." + name)),
            L"The delete writes the whiteout of the deep lower directory");
        AssertOpenFailsNotFound(mount.Get(), MountPathOf(name).c_str(),
            L"The whiteout hides the deep lower directory");
    }

    TEST_METHOD(CreateDirectory_OverDeletedDeepLowerDirectory_MakesAnOpaqueEmptyDirectory) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        MakeDeepLowerDirectory(env, name, {L"a.txt"});
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), (MountPathOf(name) + L"\\a.txt").c_str()));
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), MountPathOf(name).c_str()));

        CreateDirectoryThroughMount(mount.Get(), MountPathOf(name));

        Assert::IsTrue(HostDirectoryExists(ExtendedHostPath(env.Upper(), name)),
            L"The create makes the deep upper directory");
        Assert::IsTrue(SortedMergedNames(mount.Get(), MountPathOf(name)).empty(),
            L"The new deep directory hides the entries of the deleted lower directory");
    }

    TEST_METHOD(RenameFile_DeepLowerDirectory_MovesItsEntriesUnderTheNewName) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring source = DeepNameSizedBy(env.Lower(0), L"Source");
        const std::wstring target = DeepNameSizedBy(env.Lower(0), L"Target");
        MakeDeepLowerDirectory(env, source, {L"a.txt", L"b.txt"});
        LayerMountHolder mount = CreateLayerMount(env);

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), MountPathOf(source).c_str(),
                                   MountPathOf(target).c_str(), replaceIfExists));

        AssertOpenFailsNotFound(mount.Get(), MountPathOf(source).c_str(),
            L"The old deep directory name is gone from the mount");
        const std::vector<std::wstring> expected = {L"a.txt", L"b.txt"};
        Assert::IsTrue(expected == SortedMergedNames(mount.Get(), MountPathOf(target)),
            L"The renamed deep directory lists the entries it had");
        Assert::AreEqual<std::string>("child",
            ReadThroughMount(mount.Get(), MountPathOf(target) + L"\\a.txt", GENERIC_READ),
            L"An entry of the renamed deep directory opens under the new name");
    }

    TEST_METHOD(SetSecurity_DeepLowerFile_WritesTheDaclOnTheCopiedUpFile) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        LayerMountHolder mount = CreateLayerMount(env);
        PSECURITY_DESCRIPTOR sd = nullptr;
        ULONG sdSize = 0;
        Assert::IsTrue(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &sd, &sdSize) != FALSE,
            L"Precondition: the descriptor converts from SDDL");

        const HRESULT setHr = ::LayerMountSetSecurity(mount.Get(), MountPathOf(name).c_str(),
            DACL_SECURITY_INFORMATION, static_cast<const BYTE*>(sd), sdSize);
        ::LocalFree(sd);

        Assert::AreEqual<HRESULT>(S_OK, setHr);
        Assert::AreEqual(std::wstring(L"D:P(A;;FA;;;WD)"),
            DaclSddlOf(ExtendedHostPath(env.Upper(), name)),
            L"The security write puts the DACL on the copied-up deep file");
    }

    TEST_METHOD(EnumerateStreams_DeepLowerFileWithStream_ReportsTheStream) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        const std::wstring lowerPath = ExtendedHostPath(env.Lower(0), name);
        WriteText(lowerPath, "lower");
        WriteRawStream(lowerPath + L":s", "x", 1);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_STREAM_INFO streams[4]{};
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), MountPathOf(name).c_str(), streams, 4,
                                         &written));

        Assert::AreEqual<UINT32>(1u, written, L"The enumeration lists the stream of the deep file");
        Assert::AreEqual(std::wstring(L":s:$DATA"), std::wstring(streams[0].streamName));
    }

    TEST_METHOD(WriteFile_DeepLowerFile_KeepsTheFileIndexAcrossTheCopyUp) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        LayerMountHolder mount = CreateLayerMount(env);
        const UINT64 indexBefore = IndexNumberThroughMount(mount.Get(), MountPathOf(name));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(name).c_str(),
                            GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened),
            L"The write open of the deep lower file succeeds");
        UINT32 written = 0;
        LM_FILE_INFO info{};
        const HRESULT writeHr = WriteFromStart(opened.handle, "upper", 5, &written, &info);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
        Assert::AreEqual<HRESULT>(S_OK, writeHr);
        Assert::IsTrue(HostEntryExists(ExtendedHostPath(env.Upper(), name)),
            L"Precondition: the write copies the deep file up");

        Assert::AreEqual(indexBefore, IndexNumberThroughMount(mount.Get(), MountPathOf(name)),
            L"The copied-up deep file keeps the file index of the lower file");
    }

    TEST_METHOD(EnsureInUpperLayer_DeepLowerFileSymlink_CopiesTheLinkUp) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Link");
        const std::wstring target = env.Root() + L"\\target.txt";
        WriteText(target, "target");
        if (!LinkCreatedOrSkipped(CreateFileSymlink, ExtendedHostPath(env.Lower(0), name),
                                  target)) {
            return;
        }
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), MountPathOf(name).c_str()),
            L"The copy-up of the deep lower link succeeds");

        const std::wstring upperLink = ExtendedHostPath(env.Upper(), name);
        const DWORD attributes = ::GetFileAttributesW(upperLink.c_str());
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, attributes,
            L"The copy-up puts the deep link in the upper");
        Assert::AreNotEqual<DWORD>(0, attributes & FILE_ATTRIBUTE_REPARSE_POINT,
            L"The deep upper entry is a link");
        Assert::AreEqual<std::string>("target", ReadAllBytes(upperLink),
            L"The deep upper link points to the target of the lower link");
    }

    TEST_METHOD(Overwrite_DeepLowerFile_WritesTheNewDataInTheUpperAndKeepsTheLower) {
        TempLayerEnv env(1);
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepNameSizedBy(env.Lower(0), L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower content");
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(name).c_str(),
                            GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened),
            L"The write open of the deep lower file succeeds");
        constexpr UINT64 allocationSize = 0u;
        const HRESULT overwriteHr = OverwriteAddingAttributes(
            opened.handle, FILE_ATTRIBUTE_NORMAL, allocationSize, nullptr);
        UINT32 written = 0;
        LM_FILE_INFO info{};
        const HRESULT writeHr = WriteFromStart(opened.handle, "new", 3, &written, &info);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        Assert::AreEqual<HRESULT>(S_OK, overwriteHr);
        Assert::AreEqual<HRESULT>(S_OK, writeHr);
        Assert::AreEqual<std::string>("new",
            ReadAllBytes(ExtendedHostPath(env.Upper(), name)),
            L"The overwrite puts only the new data in the deep upper file");
        Assert::AreEqual<std::string>("lower content",
            ReadAllBytes(ExtendedHostPath(env.Lower(0), name)),
            L"The overwrite leaves the deep lower file as it was");
        Assert::AreEqual<std::string>("new",
            ReadThroughMount(mount.Get(), MountPathOf(name), GENERIC_READ),
            L"The mount reads the new data of the deep file");
    }
};

}
