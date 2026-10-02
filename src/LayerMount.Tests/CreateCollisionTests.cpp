#include "pch.h"
#include "TestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

TEST_CLASS(CreateCollisionTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CreateFile_OverLowerOnlyFile_CollidesAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\f.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
                         static_cast<long>(CreateThroughMount(mount, L"sub\\f.txt", kNoCreateOptions)),
                         L"A create-new over a file that only a lower holds must collide");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\f.txt"),
            L"A colliding create must write no file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub"),
            L"A colliding create must write no parent directory in the upper");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"sub\\f.txt"),
            L"A colliding create must leave the lower file as it was");
        Assert::AreEqual(std::string("lower"), ReadThroughMount(mount, L"sub\\f.txt"),
            L"After a colliding create the mount must still read the lower data");
    }

    TEST_METHOD(CreateDirectory_OverLowerOnlyDirectory_CollidesAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\child.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
                         static_cast<long>(CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE)),
                         L"A create-new over a directory that only a lower holds must collide");
        Assert::IsFalse(env.FileExists(env.Upper(), L"d"),
            L"A colliding create must write no directory in the upper");
        Assert::IsTrue(NT_SUCCESS(OpenThroughMount(mount, L"d\\child.txt")),
            L"A colliding create must not hide the lower directory's children");
    }

    TEST_METHOD(CreateDirectory_OverUpperDirectory_Collides) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"d");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
                         static_cast<long>(CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE)),
                         L"A create-new over a directory that the upper holds must collide");
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerFile_SucceedsAndRemovesWhiteout) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), L".wh.f.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"f.txt", kNoCreateOptions)),
                         L"A create-new over a whited-out lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"f.txt"),
            L"The create must write the new file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), L".wh.f.txt"),
            L"The create must remove the whiteout");
    }

    TEST_METHOD(CreateFile_UnderOpaqueAncestorOverLowerFile_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\.wh..wh..opq", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"d\\f.txt", kNoCreateOptions)),
                         L"A create-new under an opaque directory must succeed over a hidden lower file");
        Assert::IsTrue(env.FileExists(env.Upper(), L"d\\f.txt"),
            L"The create must write the new file in the upper");
    }

    TEST_METHOD(CreateStream_NewStreamOnLowerOnlyFile_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions)),
                         L"A create-new of a new stream on a lower-only file must succeed");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Upper(), L"f.txt"),
            L"The stream create must copy the host file up with its data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f.txt"),
            L"The stream create must leave the lower file as it was");
    }

    TEST_METHOD(CreateStream_OnWhitedOutLowerFile_GivesEmptyHostAndRemovesWhiteout) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), L".wh.f.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"f.txt:s", kNoCreateOptions)),
                         L"A create-new of a stream on a whited-out lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"f.txt"),
            L"The stream create must write the host file in the upper");
        Assert::AreEqual(std::string(), env.ReadFile(env.Upper(), L"f.txt"),
            L"The stream create must not copy the deleted lower file up");
        Assert::IsFalse(env.FileExists(env.Upper(), L".wh.f.txt"),
            L"The stream create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"f.txt"),
            L"The mount must show an empty host file, not the deleted lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f.txt"),
            L"The stream create must leave the lower file as it was");
    }

    TEST_METHOD(CreateStream_UnderOpaqueAncestorOnLowerFile_GivesEmptyHost) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\.wh..wh..opq", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"d\\f.txt:s", kNoCreateOptions)),
                         L"A create-new of a stream under an opaque directory must succeed over a hidden lower file");
        Assert::AreEqual(std::string(), env.ReadFile(env.Upper(), L"d\\f.txt"),
            L"The stream create must not copy the hidden lower file up");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"d\\f.txt"),
            L"The mount must show an empty host file, not the hidden lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"d\\f.txt"),
            L"The stream create must leave the lower file as it was");
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerDirectory_IsOpaqueAndHidesLowerChildren) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\c.txt", "lower");
        env.WriteFile(env.Upper(), L".wh.d", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE)),
                         L"A create-new of a directory over a whited-out lower directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"d\\.wh..wh..opq"),
            L"The new directory must be opaque");
        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_NOT_FOUND),
                         static_cast<long>(OpenThroughMount(mount, L"d\\c.txt")),
                         L"The new directory must hide the children of the deleted lower directory");
    }

    TEST_METHOD(CreateStream_OnWhitedOutLowerDirectory_GivesEmptyHostFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), L".wh.d", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS),
                         static_cast<long>(CreateThroughMount(mount, L"d:s", kNoCreateOptions)),
                         L"A create-new of a stream on a whited-out lower directory must succeed");
        Assert::AreEqual(std::string(), env.ReadFile(env.Upper(), L"d"),
            L"The stream create must write an empty host file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), L".wh.d"),
            L"The stream create must remove the whiteout");
    }

    TEST_METHOD(CreateStream_ExistingStreamOnLowerOnlyFile_CollidesAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Lower(0), L"f.txt:extra", "stream");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
                         static_cast<long>(CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions)),
                         L"A create-new of a stream that the lower-only host already has must collide");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f.txt"),
            L"A colliding stream create must not copy the host file up");
    }
};

}
