#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {
namespace {

UINT64 ActiveHandles(LM_HANDLE mount) {
    LM_STATS stats{};
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetStats(mount, &stats));
    return stats.activeHandles;
}

}

TEST_CLASS(AbiCleanupFileTests) {
public:
    TEST_METHOD(UpperFile_ReadAfterCleanup_ReturnsSameBytes) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"upper.txt", "upper bytes");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\upper.txt", GENERIC_READ);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(fh, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead);
        Assert::AreEqual(std::string("upper bytes"), bytes);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(UpperFile_ReadOnWriteOnlyHandleAfterCleanup_ReturnsSameBytes) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"write-only.txt", "write-only bytes");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\write-only.txt", GENERIC_WRITE);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(fh, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead,
            L"A read on a write-only handle after cleanup succeeds");
        Assert::AreEqual(std::string("write-only bytes"), bytes);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(LowerFile_ReadAfterCleanup_ReturnsSameBytes) {
        TempLayerEnv     env(1);
        env.WriteLowerFile(0, L"lower.txt", "lower bytes");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\lower.txt", GENERIC_READ);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(fh, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead);
        Assert::AreEqual(std::string("lower bytes"), bytes);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(MetacopyShell_SetSizeOnReopenAfterCleanup_SucceedsAndReadsLowerBytes) {
        TempLayerEnv      env(1);
        const std::string lowerContent = WriteLargeLowerFile(env, 0, L"big.bin");
        LayerMountHolder  mount = CreateLayerMount(env);

        LM_FILE_HANDLE first = OpenWithAccess(mount.Get(), L"\\big.bin",
            GENERIC_READ | GENERIC_WRITE);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(first));

        LM_FILE_HANDLE second = OpenWithAccess(mount.Get(), L"\\big.bin",
            GENERIC_READ | GENERIC_WRITE);

        const UINT64 newSize = 40;
        Assert::AreEqual<HRESULT>(S_OK, SetFileSize(second, newSize, nullptr),
            L"A set-size on a reopened shell after cleanup of the first handle must succeed");

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(second, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead);
        Assert::AreEqual(lowerContent.substr(0, static_cast<size_t>(newSize)), bytes,
            L"A read on the second handle returns the lower bytes up to the new size");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(second));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(first));
    }

    TEST_METHOD(UpperFile_SetSizeAfterCleanup_TruncatesFile) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"shrink.txt", "twelve bytes");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE first = OpenWithAccess(mount.Get(), L"\\shrink.txt",
            GENERIC_READ | GENERIC_WRITE);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(first));

        const UINT64 newSize = 6;
        Assert::AreEqual<HRESULT>(S_OK, SetFileSize(first, newSize, nullptr),
            L"A set-size on the same handle after cleanup must succeed");

        LM_FILE_HANDLE second = OpenWithAccess(mount.Get(), L"\\shrink.txt", GENERIC_READ);
        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(second, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead);
        Assert::AreEqual(std::string("twelve"), bytes,
            L"A read on a fresh handle returns the bytes up to the new size");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(second));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(first));
    }

    TEST_METHOD(DeletedFile_ReadAfterCleanup_FailsWithFileNotFound) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"gone.txt", "soon gone");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\gone.txt", GENERIC_READ);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));

        const std::wstring upperPath = env.Upper() + L"\\gone.txt";
        HANDLE exclusive = ::CreateFileW(upperPath.c_str(), GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::IsTrue(exclusive != INVALID_HANDLE_VALUE,
            L"An exclusive open must succeed after cleanup, so no NT handle stays open");
        ::CloseHandle(exclusive);
        Assert::IsTrue(::DeleteFileW(upperPath.c_str()) != FALSE);

        HRESULT hrRead = S_OK;
        (void)ReadThroughHandle(fh, &hrRead);
        Assert::IsTrue(IsFileNotFoundHr(hrRead),
            L"A read after cleanup on a deleted file must fail with file not found");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(Cleanup_KeepsActiveHandlesCount) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"counted.txt", "counted");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\counted.txt", GENERIC_READ);
        const UINT64 whileOpen = ActiveHandles(mount.Get());

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));
        Assert::AreEqual<UINT64>(whileOpen, ActiveHandles(mount.Get()),
            L"Cleanup must not change the open-file count");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(SecondCleanup_ReturnsSuccess) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"twice.txt", "twice");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\twice.txt", GENERIC_READ);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(fh, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead);
        Assert::AreEqual(std::string("twice"), bytes);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(CloseAfterCleanup_FreesSlotAndDropsActiveHandles) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"closed.txt", "closed");
        LayerMountHolder mount = CreateLayerMount(env);
        const UINT64 baseline = ActiveHandles(mount.Get());

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\closed.txt", GENERIC_READ);
        Assert::AreEqual<UINT64>(baseline + 1, ActiveHandles(mount.Get()));

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
        Assert::AreEqual<UINT64>(baseline, ActiveHandles(mount.Get()),
            L"Close after cleanup must drop the open-file count");

        Assert::AreEqual<HRESULT>(E_HANDLE, ::LayerMountCleanupFile(fh),
            L"The slot is free after close, so a later cleanup must reject the handle");
        HRESULT hrRead = S_OK;
        (void)ReadThroughHandle(fh, &hrRead);
        Assert::AreEqual<HRESULT>(E_HANDLE, hrRead,
            L"The slot is free after close, so a later read must reject the handle");
    }

    TEST_METHOD(NullHandle_ReturnsEHandle) {
        Assert::AreEqual<HRESULT>(E_HANDLE, ::LayerMountCleanupFile(nullptr));
    }

    TEST_METHOD(UpperFile_DeleteAfterCleanup_RemovesFromUpper) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"gone.txt", "to delete");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\gone.txt", GENERIC_READ | DELETE);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDeleteOpenFile(fh),
            L"Delete on a handle after cleanup must succeed by path");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        Assert::IsFalse(std::filesystem::exists(env.Upper() + L"\\gone.txt"),
            L"The upper file must be gone after delete");
    }

    TEST_METHOD(LowerOnlyFile_DeleteAfterCleanup_HidesLowerFile) {
        TempLayerEnv     env(1);
        env.WriteLowerFile(0, L"hidden.txt", "lower data");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\hidden.txt", GENERIC_READ | DELETE);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDeleteOpenFile(fh),
            L"Delete on a handle after cleanup must succeed by path");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        AssertOpenFailsNotFound(mount.Get(), L"\\hidden.txt",
            L"After delete, a reopen must fail with file not found");
    }

    TEST_METHOD(UpperStream_DeleteAfterCleanup_RemovesStreamAndKeepsHostFile) {
        TempLayerEnv     env(0);
        env.WriteUpperFile(L"host.txt", "host bytes");
        std::ofstream(env.Upper() + L"\\host.txt:stream") << "stream bytes";
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = OpenWithAccess(mount.Get(), L"\\host.txt:stream", GENERIC_READ | DELETE);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCleanupFile(fh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDeleteOpenFile(fh),
            L"Delete on a stream handle after cleanup must succeed by path");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        Assert::IsTrue(std::filesystem::exists(env.Upper() + L"\\host.txt"),
            L"A stream delete must leave the host file in place");
        AssertOpenFailsNotFound(mount.Get(), L"\\host.txt:stream",
            L"After delete, an open of the stream must fail with file not found");
    }
};

}
