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

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"", MAXIMUM_ALLOWED,
                                 /*createOptions*/ 0u, /*originatorPid*/ 0u,
                                 &fh, &info),
            L"the root open with MAXIMUM_ALLOWED succeeds");
        Assert::IsNotNull(fh, L"the root open returns a handle");
        Assert::IsTrue((info.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the root open reports a directory");

        LM_FILE_INFO queried{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetFileInfo(fh, &queried),
                                  L"a get-file-info on the root handle succeeds");
        Assert::IsTrue((queried.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the get-file-info reports a directory");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(CreateFile_WithMaximumAllowed_WritesAndReadsThroughHandle) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\max.txt",
                                   /*createOptions*/ 0u, MAXIMUM_ALLOWED,
                                   FILE_ATTRIBUTE_NORMAL,
                                   /*securityDescriptor*/ nullptr, 0u,
                                   /*allocationSize*/ 0u, /*originatorPid*/ 0u,
                                   &fh, &info),
            L"the create with MAXIMUM_ALLOWED succeeds");
        Assert::IsNotNull(fh, L"the create returns a handle");

        const char   payload[]  = "maximum allowed";
        const UINT32 payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32       written    = 0;
        LM_FILE_INFO postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, payload, /*offset*/ 0, payloadLen,
                                  /*writeToEnd*/ FALSE, /*constrainedIo*/ FALSE,
                                  &written, &postWrite),
            L"a write through the created handle succeeds");
        Assert::AreEqual<UINT32>(payloadLen, written, L"the write is complete");

        char   readBuf[32] = {};
        UINT32 readCount   = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(fh, readBuf, /*offset*/ 0, sizeof(readBuf),
                                 &readCount),
            L"a read through the created handle succeeds");
        Assert::AreEqual<UINT32>(payloadLen, readCount, L"the read returns the written bytes");
        Assert::IsTrue(std::memcmp(readBuf, payload, payloadLen) == 0,
                       L"the read returns the written content");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(CreateDirectory_WithMaximumAllowed_ReturnsDirectoryHandle) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\maxdir",
                                   FILE_DIRECTORY_FILE, MAXIMUM_ALLOWED,
                                   FILE_ATTRIBUTE_DIRECTORY,
                                   /*securityDescriptor*/ nullptr, 0u,
                                   /*allocationSize*/ 0u, /*originatorPid*/ 0u,
                                   &fh, &info),
            L"the directory create with MAXIMUM_ALLOWED succeeds");
        Assert::IsNotNull(fh, L"the directory create returns a handle");
        Assert::IsTrue((info.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the create reports a directory");

        LM_FILE_INFO queried{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetFileInfo(fh, &queried),
                                  L"a get-file-info on the directory handle succeeds");
        Assert::IsTrue((queried.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the get-file-info reports a directory");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(TrackerDeniesWrites_OpenWithMaximumAllowed_SucceedsWhileGenericWriteIsDenied) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"tracked.txt", "content");
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerEnable(mount.Get(), TRUE));
        const std::wstring rulesPath = WriteDenyWriteRules(env);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerSetRules(mount.Get(), rulesPath.c_str()),
            L"the deny-write rule set loads");

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\tracked.txt", MAXIMUM_ALLOWED,
                                 /*createOptions*/ 0u, /*originatorPid*/ 0u,
                                 &fh, &info),
            L"an open with MAXIMUM_ALLOWED is an Open operation and passes the rule");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        LM_FILE_HANDLE denied = nullptr;
        LM_FILE_INFO   deniedInfo{};
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED),
            ::LayerMountOpenFile(mount.Get(), L"\\tracked.txt", GENERIC_WRITE,
                                 /*createOptions*/ 0u, /*originatorPid*/ 0u,
                                 &denied, &deniedInfo),
            L"an open with GENERIC_WRITE is a Write operation and the rule denies it");
        Assert::IsNull(denied, L"the denied open returns no handle");
    }
};

}
