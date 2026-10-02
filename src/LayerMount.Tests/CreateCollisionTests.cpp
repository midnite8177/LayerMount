#include "pch.h"
#include "TestFixture.h"

#include "MetadataStore.h"

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

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"sub\\f.txt", kNoCreateOptions),
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

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE),
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

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE),
            L"A create-new over a directory that the upper holds must collide");
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerFile_SucceedsAndRemovesWhiteout) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt", kNoCreateOptions),
            L"A create-new over a whited-out lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"f.txt"),
            L"The create must write the new file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f.txt")),
            L"The create must remove the whiteout");
    }

    TEST_METHOD(CreateFile_UnderOpaqueAncestorOverLowerFile_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\f.txt", kNoCreateOptions),
            L"A create-new under an opaque directory must succeed over a hidden lower file");
        Assert::IsTrue(env.FileExists(env.Upper(), L"d\\f.txt"),
            L"The create must write the new file in the upper");
    }

    TEST_METHOD(CreateStream_NewStreamOnLowerOnlyFile_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions),
            L"A create-new of a new stream on a lower-only file must succeed");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Upper(), L"f.txt"),
            L"The stream create must copy the host file up with its data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f.txt"),
            L"The stream create must leave the lower file as it was");
    }

    TEST_METHOD(CreateStream_OnWhitedOutLowerFile_GivesEmptyHostAndRemovesWhiteout) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt:s", kNoCreateOptions),
            L"A create-new of a stream on a whited-out lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"f.txt"),
            L"The stream create must write the host file in the upper");
        Assert::AreEqual(std::string(), env.ReadFile(env.Upper(), L"f.txt"),
            L"The stream create must not copy the deleted lower file up");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f.txt")),
            L"The stream create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"f.txt"),
            L"The mount must show an empty host file, not the deleted lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f.txt"),
            L"The stream create must leave the lower file as it was");
    }

    TEST_METHOD(CreateStream_UnderOpaqueAncestorOnLowerFile_GivesEmptyHost) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\f.txt:s", kNoCreateOptions),
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
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE),
            L"A create-new of a directory over a whited-out lower directory must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"d")),
            L"The create must remove the whiteout");
        Assert::IsTrue(env.FileExists(env.Upper(), OpaqueMarkerPath(L"d")),
            L"The new directory must be opaque");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\c.txt"),
            L"The new directory must hide the children of the deleted lower directory");
    }

    TEST_METHOD(CreateStream_OnWhitedOutLowerDirectory_GivesEmptyHostFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d:s", kNoCreateOptions),
            L"A create-new of a stream on a whited-out lower directory must succeed");
        Assert::AreEqual(std::string(), env.ReadFile(env.Upper(), L"d"),
            L"The stream create must write an empty host file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"d")),
            L"The stream create must remove the whiteout");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\f.txt"),
            L"The host file must hide the children of the deleted lower directory");
    }

    TEST_METHOD(CreateStream_ExistingStreamOnLowerOnlyFile_CollidesAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Lower(0), L"f.txt:extra", "stream");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions),
            L"A create-new of a stream that the lower-only host already has must collide");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f.txt"),
            L"A colliding stream create must not copy the host file up");
    }

    TEST_METHOD(CreateFile_OverLowerOnlyDirectory_Collides) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\child.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"d", kNoCreateOptions),
            L"A file create-new over a directory that only a lower holds must collide");
        Assert::IsFalse(env.FileExists(env.Upper(), L"d"),
            L"A colliding create must write nothing in the upper");
    }

    TEST_METHOD(CreateDirectory_OverLowerOnlyFile_Collides) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"f", FILE_DIRECTORY_FILE),
            L"A directory create-new over a file that only a lower holds must collide");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f"),
            L"A colliding create must write nothing in the upper");
    }

    TEST_METHOD(CreateFile_OverFileInUpperAndLower_CollidesAndKeepsUpperData) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), L"f.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"f.txt", kNoCreateOptions),
            L"A create-new over a file that both layers hold must collide");
        Assert::AreEqual(std::string("upper"), env.ReadFile(env.Upper(), L"f.txt"),
            L"A colliding create must leave the upper file as it was");
    }

    TEST_METHOD(CreateStream_NewStreamOnUpperFile_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"f.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions),
            L"A create-new of a new stream on an upper file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"f.txt:extra"),
            L"The stream create must write the stream on the upper file");
        Assert::AreEqual(std::string("upper"), env.ReadFile(env.Upper(), L"f.txt"),
            L"The stream create must leave the upper file's data as it was");
    }

    TEST_METHOD(CreateDirectory_UnderOpaqueAncestorOverLowerDirectory_IsNotOpaqueAndHidesLowerChildren) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\e\\c.txt", "lower");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\e", FILE_DIRECTORY_FILE),
            L"A directory create-new under an opaque directory must succeed over a hidden lower directory");
        Assert::IsFalse(env.FileExists(env.Upper(), OpaqueMarkerPath(L"d\\e")),
            L"The new directory must not be opaque, because the opaque ancestor already hides the lower directory");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\d\\e", nullptr),
            L"The new directory must carry no opaque stream");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\e\\c.txt"),
            L"The new directory must hide the children of the hidden lower directory");
    }

    TEST_METHOD(CreateStream_ExistingStreamOnMetacopyShellOrigin_CollidesAndLeavesShellUnfilled) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Lower(0), L"f.txt:extra", "stream");
        {
            CopyUpRig rig(env.MakeConfig());
            Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"f.txt")),
                L"Preconditions: the copy-up must stage a metacopy shell");
        }
        const std::wstring shellPath = env.Upper() + L"\\f.txt";
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_COLLISION, CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions),
            L"A create-new of a stream that the shell's origin has must collide");
        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(shellPath, nullptr).metacopy,
            L"A colliding stream create must not fill the shell");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f.txt:extra"),
            L"A colliding stream create must not bring the origin's stream into the upper");
    }

    TEST_METHOD(CreateStream_NewStreamOnMetacopyShell_FillsShellAndSucceeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        {
            CopyUpRig rig(env.MakeConfig());
            Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"f.txt")),
                L"Preconditions: the copy-up must stage a metacopy shell");
        }
        const std::wstring shellPath = env.Upper() + L"\\f.txt";
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt:extra", kNoCreateOptions),
            L"A create-new of a new stream on a metacopy shell must succeed");
        Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(shellPath, nullptr).metacopy,
            L"The stream create must fill the shell");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Upper(), L"f.txt"),
            L"The filled shell must hold the origin's data");
    }
};

}
