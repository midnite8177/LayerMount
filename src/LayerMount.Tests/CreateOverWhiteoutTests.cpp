#include "pch.h"
#include "TestFixture.h"

#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

TEST_CLASS(CreateOverWhiteoutTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerDirectory_IsEmptyUpperFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d", kNoCreateOptions),
            L"A file create-new over a whited-out lower directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"d"),
            L"The create must write the new file in the upper");
        Assert::IsFalse(HasAttribute(env.Upper() + L"\\d", FILE_ATTRIBUTE_DIRECTORY),
            L"The create must write a file, not a directory");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"d")),
            L"The create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"d"),
            L"The mount must show the new empty file");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"d\\inner.txt"),
            L"The create must leave the lower directory's child as it was");
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerFile_IsNotOpaque) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f"), "");
        const auto config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f", FILE_DIRECTORY_FILE),
            L"A directory create-new over a whited-out lower file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\f", FILE_ATTRIBUTE_DIRECTORY),
            L"The create must write the new directory in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f")),
            L"The create must remove the whiteout");
        Assert::IsFalse(env.FileExists(env.Upper(), OpaqueMarkerPath(L"f")),
            L"A directory over a lower file has no lower children to hide, so it must not be opaque");
        Assert::IsFalse(::LayerMount::MetadataADS::HasOpaqueADS(env.Upper() + L"\\f", &config),
            L"The new directory must have no opaque metadata marker");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f"),
            L"The create must leave the lower file as it was");
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerFileInNestedDirectory_IsEmptyUpperFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a\\b\\c.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"a\\b\\c.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"a\\b\\c.txt", kNoCreateOptions),
            L"A create-new over a whited-out lower file in a nested directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"a\\b\\c.txt"),
            L"The create must write the new file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"a\\b\\c.txt")),
            L"The create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"a\\b\\c.txt"),
            L"The mount must show the new empty file, not the lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"a\\b\\c.txt"),
            L"The create must leave the lower file as it was");
    }

    TEST_METHOD(CreateDeleteCreate_OverWhitedOutLowerFile_EndsWithEmptyUpperFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt", kNoCreateOptions),
            L"The first create-new over the whited-out lower file must succeed");
        AssertStatus(STATUS_SUCCESS, mount.Delete(L"f.txt", kNoCallerPid),
            L"The delete of the new upper file must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f.txt"),
            L"The delete must remove the upper file");
        Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f.txt")),
            L"The delete must write a whiteout, because the lower still holds the file");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"f.txt"),
            L"After the delete the mount must hide the lower file");

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt", kNoCreateOptions),
            L"The second create-new over the whited-out lower file must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f.txt")),
            L"The second create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"f.txt"),
            L"The mount must show the new empty file, not the lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f.txt"),
            L"Creates and deletes through the mount must leave the lower file as it was");
    }
};

}
