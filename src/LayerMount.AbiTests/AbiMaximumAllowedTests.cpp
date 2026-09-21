#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

std::wstring WriteDenyWriteRules(const TempLayerEnv& env) {
    return WriteTrackerRules(env,
        R"({"rules":[{"processName":"*","pathPattern":"*","allowWrite":false}]})");
}

}

TEST_CLASS(AbiMaximumAllowedTests) {
public:
    TEST_METHOD(OpenRoot_WithMaximumAllowed_ReturnsDirectoryHandle) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"", MAXIMUM_ALLOWED, kNoCreateOptions, opened),
            L"the root open with MAXIMUM_ALLOWED succeeds");
        Assert::IsNotNull(opened.handle, L"the root open returns a handle");
        Assert::IsTrue((opened.info.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the root open reports a directory");

        LM_FILE_INFO queried{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetFileInfo(opened.handle, &queried),
                                  L"a get-file-info on the root handle succeeds");
        Assert::IsTrue((queried.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the get-file-info reports a directory");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    }

    TEST_METHOD(CreateFile_WithMaximumAllowed_WritesAndReadsThroughHandle) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\max.txt", MAXIMUM_ALLOWED,
                              kNoCreateOptions, FILE_ATTRIBUTE_NORMAL, opened),
            L"the create with MAXIMUM_ALLOWED succeeds");
        Assert::IsNotNull(opened.handle, L"the create returns a handle");

        const char   payload[]  = "maximum allowed";
        const UINT32 payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32       written    = 0;
        LM_FILE_INFO postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, payload, payloadLen, &written, &postWrite),
            L"a write through the created handle succeeds");
        Assert::AreEqual<UINT32>(payloadLen, written, L"the write is complete");

        char   readBuf[32] = {};
        UINT32 readCount   = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ReadFromStart(opened.handle, readBuf, sizeof(readBuf), &readCount),
            L"a read through the created handle succeeds");
        Assert::AreEqual<UINT32>(payloadLen, readCount, L"the read returns the written bytes");
        Assert::IsTrue(std::memcmp(readBuf, payload, payloadLen) == 0,
                       L"the read returns the written content");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    }

    TEST_METHOD(CreateDirectory_WithMaximumAllowed_ReturnsDirectoryHandle) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\maxdir", MAXIMUM_ALLOWED,
                              FILE_DIRECTORY_FILE, FILE_ATTRIBUTE_DIRECTORY, opened),
            L"the directory create with MAXIMUM_ALLOWED succeeds");
        Assert::IsNotNull(opened.handle, L"the directory create returns a handle");
        Assert::IsTrue((opened.info.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the create reports a directory");

        LM_FILE_INFO queried{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetFileInfo(opened.handle, &queried),
                                  L"a get-file-info on the directory handle succeeds");
        Assert::IsTrue((queried.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the get-file-info reports a directory");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    }

    TEST_METHOD(TrackerDeniesWrites_OpenWithMaximumAllowed_SucceedsWhileGenericWriteIsDenied) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"tracked.txt", "content");
        LayerMountHolder mount = CreateLayerMount(env);

        constexpr BOOL enable = TRUE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerEnable(mount.Get(), enable));
        const std::wstring rulesPath = WriteDenyWriteRules(env);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerSetRules(mount.Get(), rulesPath.c_str()),
            L"the deny-write rule set loads");

        OpenedFile allowed;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\tracked.txt", MAXIMUM_ALLOWED,
                            kNoCreateOptions, allowed),
            L"an open with MAXIMUM_ALLOWED is an Open operation and passes the rule");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(allowed.handle));

        OpenedFile denied;
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED),
            OpenOverlayFile(mount.Get(), L"\\tracked.txt", GENERIC_WRITE,
                            kNoCreateOptions, denied),
            L"an open with GENERIC_WRITE is a Write operation and the rule denies it");
        Assert::IsNull(denied.handle, L"the denied open returns no handle");
    }
};

}
