#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// File-primitive round-trip (Create/Write/Read/Close) plus one error
// path (Open a nonexistent file).
TEST_CLASS(AbiFileOpsTests) {
public:
    TEST_METHOD(CreateWriteReadClose_RoundTrip) {
        TempLayerEnv   env(0);
        LayerMountHolder  mount = CreateLayerMount(env);

        const wchar_t* path      = L"\\hello.txt";
        LM_FILE_HANDLE fh       = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hr = ::LayerMountCreateFile(
            mount.Get(), path,
            /*createOptions*/ 0u,
            /*grantedAccess*/ GENERIC_READ | GENERIC_WRITE,
            /*fileAttributes*/ FILE_ATTRIBUTE_NORMAL,
            /*securityDescriptor*/ nullptr, 0u,
            /*allocationSize*/ 0u,
            /*originatorPid*/ 0u,
            &fh, &info);
        Assert::AreEqual<HRESULT>(S_OK, hr, L"LayerMountCreateFile");
        Assert::IsNotNull(fh, L"file handle should be non-null");

        const char     payload[] = "hello overlay";
        const UINT32   payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32         written    = 0;
        LM_FILE_INFO  postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, payload, /*offset*/ 0, payloadLen,
                               /*writeToEnd*/ FALSE, /*constrainedIo*/ FALSE,
                               /*originatorPid*/ 0u, &written, &postWrite));
        Assert::AreEqual<UINT32>(payloadLen, written);

        // Rewind-ish: LayerMountReadFile takes an explicit offset.
        char     readBuf[32] = {};
        UINT32   readCount   = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(fh, readBuf, /*offset*/ 0, sizeof(readBuf),
                              /*originatorPid*/ 0u, &readCount));
        Assert::AreEqual<UINT32>(payloadLen, readCount);
        Assert::IsTrue(std::memcmp(readBuf, payload, payloadLen) == 0,
                       L"Readback should match writeback byte-for-byte");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        // File should now be visible via ResolvePath in the upper layer.
        LM_RESOLVED_PATH rp{};
        std::vector<wchar_t> absBuf(MAX_PATH);
        rp.absolutePath      = absBuf.data();
        rp.absolutePathChars = absBuf.size();
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountResolvePath(mount.Get(), path, &rp));
        Assert::AreEqual<int>(LM_LAYER_UPPER, static_cast<int>(rp.source));
    }

    TEST_METHOD(OpenFile_NonExistent_ReturnsFileNotFound) {
        TempLayerEnv   env(0);
        LayerMountHolder  mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh   = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hr = ::LayerMountOpenFile(
            mount.Get(), L"\\nope.txt",
            /*grantedAccess*/ GENERIC_READ, /*createOptions*/ 0u,
            /*originatorPid*/ 0u, &fh, &info);
        Assert::IsTrue(IsFileNotFoundHr(hr),
            L"Opening a non-existent file should surface as ERROR_FILE_NOT_FOUND "
            L"(Win32 or NT-status encoding)");
        Assert::IsNull(fh, L"out handle must remain null on failure");
    }

    TEST_METHOD(DeleteFile_AfterCreate_RemovesFile) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const wchar_t* path = L"\\gone.txt";
        {
            LM_FILE_HANDLE fh = nullptr;
            LM_FILE_INFO   info{};
            Assert::AreEqual<HRESULT>(S_OK,
                ::LayerMountCreateFile(mount.Get(), path, 0u,
                    GENERIC_READ | GENERIC_WRITE | DELETE,
                    FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u,
                    &fh, &info));
            Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
        }

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), path));

        // Subsequent Open should fail.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hrOpen = ::LayerMountOpenFile(
            mount.Get(), path, GENERIC_READ, 0u, 0u, &fh, &info);
        Assert::IsTrue(IsFileNotFoundHr(hrOpen),
            L"Opening a deleted file must surface as FileNotFound");
    }
};

} // namespace LayerMountAbiTests
