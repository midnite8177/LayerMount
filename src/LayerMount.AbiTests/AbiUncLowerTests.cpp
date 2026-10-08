#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::AdminSharePathOf;
using LayerMountTestShared::ExtendedFormOf;
using LayerMountTestShared::SharePathReachableOrSkipped;

namespace LayerMountAbiTests {
namespace {

enum class UncForm { Plain, Extended };

std::wstring UncLowerPath(const TempLayerEnv& env, UncForm form) {
    const std::wstring plain = AdminSharePathOf(env.Lower(0));
    return form == UncForm::Plain ? plain : ExtendedFormOf(plain);
}

LayerMountHolder CreateWithLower(const TempLayerEnv& env, const std::wstring& lower) {
    const PCWSTR lowers[] = {lower.c_str()};
    LM_CONFIG cfg{};
    cfg.structSize            = sizeof(LM_CONFIG);
    cfg.abiVersion            = LM_ABI_VERSION;
    cfg.hostCapabilities      = LM_CAP_ADS | LM_CAP_REPARSE_POINTS | LM_CAP_SPARSE_FILES |
                                LM_CAP_MULTIPLE_STREAMS | LM_CAP_NTFS_ACLS;
    cfg.accessLogCapacity     = 256;
    cfg.pathCacheCapacity     = 256;
    cfg.enableProcessTracking = FALSE;
    cfg.lowerPathCount        = 1;
    cfg.lowerPaths            = lowers;
    cfg.upperPath             = env.Upper().c_str();
    cfg.workDirPath           = env.Work().c_str();
    LM_HANDLE handle = nullptr;
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCreate(&cfg, &handle),
        L"LayerMountCreate with a lower on a share");
    return LayerMountHolder(handle);
}

void AssertOpenCopiesUp(UncForm form, UINT32 grantedAccess) {
    TempLayerEnv env(1);
    env.WriteLowerFile(0, L"a.txt", "lower-bytes");
    const std::wstring lower = UncLowerPath(env, form);
    if (!SharePathReachableOrSkipped(lower)) {
        return;
    }
    LayerMountHolder mount = CreateWithLower(env, lower);

    OpenedFile opened;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount.Get(), L"\\a.txt", grantedAccess, kNoCreateOptions, opened),
        L"an open for write of a file in a lower on a share copies it up");
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

    Assert::AreEqual<std::string>("lower-bytes", ReadAllBytes(env.Upper() + L"\\a.txt"),
        L"the upper copy carries the bytes of the lower file");
}

void AssertSetAttributesCopiesUp(UncForm form) {
    TempLayerEnv env(1);
    env.WriteLowerFile(0, L"a.txt", "lower-bytes");
    const std::wstring lower = UncLowerPath(env, form);
    if (!SharePathReachableOrSkipped(lower)) {
        return;
    }
    LayerMountHolder mount = CreateWithLower(env, lower);
    const LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\a.txt", GENERIC_READ);

    FileInfoChange change;
    change.fileAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN;
    LM_FILE_INFO info{};
    const HRESULT hr = SetFileInfo(fh, change, &info);
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

    Assert::AreEqual<HRESULT>(S_OK, hr,
        L"an attribute change of a file in a lower on a share copies it up");
    Assert::IsTrue((::GetFileAttributesW((env.Upper() + L"\\a.txt").c_str()) &
                    FILE_ATTRIBUTE_HIDDEN) != 0,
        L"the upper copy carries the new attributes");
}

}

TEST_CLASS(AbiUncLowerTests) {
public:
    TEST_METHOD(OpenForWriteAttributes_FileInPlainUncLower_CopiesUp) {
        AssertOpenCopiesUp(UncForm::Plain, FILE_WRITE_ATTRIBUTES);
    }

    TEST_METHOD(OpenForWriteAttributes_FileInExtendedUncLower_CopiesUp) {
        AssertOpenCopiesUp(UncForm::Extended, FILE_WRITE_ATTRIBUTES);
    }

    TEST_METHOD(OpenForGenericWrite_FileInPlainUncLower_CopiesUp) {
        AssertOpenCopiesUp(UncForm::Plain, GENERIC_READ | GENERIC_WRITE);
    }

    TEST_METHOD(OpenForGenericWrite_FileInExtendedUncLower_CopiesUp) {
        AssertOpenCopiesUp(UncForm::Extended, GENERIC_READ | GENERIC_WRITE);
    }

    TEST_METHOD(SetFileInfo_FileInPlainUncLower_CopiesUp) {
        AssertSetAttributesCopiesUp(UncForm::Plain);
    }

    TEST_METHOD(SetFileInfo_FileInExtendedUncLower_CopiesUp) {
        AssertSetAttributesCopiesUp(UncForm::Extended);
    }
};

}
