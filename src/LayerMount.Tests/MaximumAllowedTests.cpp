#include "pch.h"
#include "TestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

TEST_CLASS(MaximumAllowedTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    static void AssertResolved(const FileContext& ctx) {
        Assert::IsTrue((ctx.grantedAccess & MAXIMUM_ALLOWED) == 0,
                       L"the stored access holds no MAXIMUM_ALLOWED bit");
        Assert::IsTrue((ctx.grantedAccess & FILE_READ_ATTRIBUTES) != 0,
                       L"the stored access holds the rights the kernel granted");
    }

    TEST_METHOD(OpenRoot_WithMaximumAllowed_StoresResolvedAccess) {
        TempLayerEnvironment env(0);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"", MAXIMUM_ALLOWED,
                                             /*createOptions*/ 0u,
                                             /*callerPid*/ 0u, &ctx, &info)),
                       L"the root open with MAXIMUM_ALLOWED succeeds");
        AssertResolved(*ctx);
        mount.Close(ctx.get());
    }

    TEST_METHOD(CreateFile_WithMaximumAllowed_StoresResolvedAccess) {
        TempLayerEnvironment env(0);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Create(L"max.txt", /*createOptions*/ 0u,
                                               MAXIMUM_ALLOWED, FILE_ATTRIBUTE_NORMAL,
                                               /*securityDescriptor*/ nullptr,
                                               /*allocationSize*/ 0u,
                                               /*callerPid*/ 0u, &ctx, &info)),
                       L"the file create with MAXIMUM_ALLOWED succeeds");
        AssertResolved(*ctx);
        Assert::IsTrue((ctx->grantedAccess & FILE_WRITE_DATA) != 0,
                       L"the stored access holds the write right on a new file");
        mount.Close(ctx.get());
    }

    TEST_METHOD(CreateDirectory_WithMaximumAllowed_StoresResolvedAccess) {
        TempLayerEnvironment env(0);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Create(L"maxdir", FILE_DIRECTORY_FILE,
                                               MAXIMUM_ALLOWED, FILE_ATTRIBUTE_DIRECTORY,
                                               /*securityDescriptor*/ nullptr,
                                               /*allocationSize*/ 0u,
                                               /*callerPid*/ 0u, &ctx, &info)),
                       L"the directory create with MAXIMUM_ALLOWED succeeds");
        AssertResolved(*ctx);
        mount.Close(ctx.get());
    }
};

}
