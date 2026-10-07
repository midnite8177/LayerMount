#include "pch.h"
#include "TestFixture.h"
#include "ExtendedAttributeTestHelpers.h"

#include "CopyUp.h"
#include "WhiteoutManager.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::DeepLeafName;
using LayerMountTestShared::ExtendedFormOf;
using LayerMountTestShared::ExtendedPathUnder;

namespace LayerMountTests {

namespace {

const std::wstring kParentName = L"dir";

std::wstring DeepNameUnderLowerParent(const TempLayerEnvironment& env, const std::wstring& prefix) {
    return DeepLeafName(env.Lower(0) + L"\\" + kParentName, prefix);
}

std::wstring RelativePathOf(const std::wstring& name) {
    return kParentName + L"\\" + name;
}

DWORD HostAttributes(const std::wstring& layer, const std::wstring& relativePath) {
    return ::GetFileAttributesW(ExtendedPathUnder(layer, relativePath).c_str());
}

}

TEST_CLASS(DeepLowerPathTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
        LayerMountTestShared::AssertHostRefusesPlainPathsPastMaxPath();
    }

    TEST_METHOD(CopyUpFile_DeepLowerFile_CopiesContentToTheDeepUpperPath) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep"));
        env.WriteFile(ExtendedFormOf(env.Lower(0)), relative, "lower content");
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(relative),
            L"The copy-up of the deep lower file succeeds");

        Assert::AreEqual(std::string("lower content"),
            env.ReadFile(ExtendedFormOf(env.Upper()), relative),
            L"The copy-up writes the content at the deep upper path");
    }

    TEST_METHOD(CopyUpFile_DeepLowerFileWithExtendedAttributes_CopiesTheExtendedAttributes) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep"));
        env.WriteFile(ExtendedFormOf(env.Lower(0)), relative, "lower");
        const auto attributes = WslAndUserExtendedAttributes(kWslFileMode);
        SetExtendedAttributes(ExtendedPathUnder(env.Lower(0), relative), attributes);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(relative),
            L"The copy-up of the deep lower file succeeds");

        AssertHasExtendedAttributes(ExtendedPathUnder(env.Upper(), relative), attributes);
    }

    TEST_METHOD(CompleteLazyCopyUp_DeepMetacopyShell_FillsTheLowerData) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep"));
        const std::string content(64 * 1024, 'L');
        env.WriteFile(ExtendedFormOf(env.Lower(0)), relative, content);
        CopyUpAndRenameRig rig(env.MakeConfig());
        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(relative).status),
            L"Precondition: the metacopy of the deep lower file succeeds");

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CompleteLazyCopyUp(relative),
            L"The fill of the deep shell succeeds");

        Assert::IsTrue(content == env.ReadFile(ExtendedFormOf(env.Upper()), relative),
            L"The fill writes the lower data into the deep upper file");
        Assert::AreEqual<DWORD>(0, HostAttributes(env.Upper(), relative) &
                                       FILE_ATTRIBUTE_SPARSE_FILE,
            L"The filled deep file is not sparse");
    }

    TEST_METHOD(CopyUpDirectory_DeepLowerDirectory_CreatesTheDeepUpperDirectory) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep"));
        env.CreateDir(ExtendedFormOf(env.Lower(0)), relative);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(relative),
            L"The copy-up of the deep lower directory succeeds");

        const DWORD attributes = HostAttributes(env.Upper(), relative);
        Assert::IsTrue(attributes != INVALID_FILE_ATTRIBUTES &&
                           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"The copy-up makes the deep upper directory");
    }

    TEST_METHOD(CreateWhiteout_FileInDeepLowerDirectory_WritesTheMarkerUnderTheDeepUpperDirectory) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep") + L"\\f.txt");
        env.WriteFile(ExtendedFormOf(env.Lower(0)), relative, "lower");
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.whiteouts.CreateWhiteout(relative, WhiteoutType::File),
            L"The whiteout of the file in the deep directory succeeds");

        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            HostAttributes(env.Upper(), WhiteoutManager::GetWhiteoutFileName(relative)),
            L"The whiteout writes the marker under the deep upper directory");
        Assert::IsTrue(rig.whiteouts.HasWhiteout(relative, env.Upper()),
            L"The deep marker hides the lower file");
    }

    TEST_METHOD(RemoveWhiteout_DeepMarker_DeletesTheMarker) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep"));
        env.WriteFile(ExtendedFormOf(env.Lower(0)), relative, "lower");
        CopyUpAndRenameRig rig(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS, rig.whiteouts.CreateWhiteout(relative, WhiteoutType::File),
            L"Precondition: the whiteout of the deep lower file succeeds");

        AssertStatus(STATUS_SUCCESS, rig.whiteouts.RemoveWhiteout(relative),
            L"The removal of the deep whiteout succeeds");

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            HostAttributes(env.Upper(), WhiteoutManager::GetWhiteoutFileName(relative)),
            L"The removal deletes the deep marker");
        Assert::IsFalse(rig.whiteouts.HasWhiteout(relative, env.Upper()),
            L"The deep lower file is no longer hidden");
    }

    TEST_METHOD(SetOpaque_DeepDirectory_MarksTheDeepUpperDirectoryOpaque) {
        TempLayerEnvironment env(1);
        const std::wstring relative = RelativePathOf(DeepNameUnderLowerParent(env, L"Deep"));
        env.CreateDir(ExtendedFormOf(env.Lower(0)), relative);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.whiteouts.SetOpaque(relative),
            L"The opaque mark of the deep directory succeeds");

        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            HostAttributes(env.Upper(), relative + L"\\" + kOpaqueMarkerFile),
            L"The opaque mark writes the marker file in the deep upper directory");
        Assert::IsTrue(rig.whiteouts.IsOpaque(relative),
            L"The deep upper directory is opaque");
    }
};

}
