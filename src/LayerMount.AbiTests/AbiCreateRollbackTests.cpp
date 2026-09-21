#include "pch.h"
#include "AbiTestFixture.h"
#include "AclTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

// The deny names FILE_DELETE_CHILD on purpose. The engine opens the upper
// with backup semantics and SE_BACKUP_NAME / SE_RESTORE_NAME enabled, and
// neither privilege grants FILE_DELETE_CHILD, so the kernel checks the ACL.
void DenyDeleteChildInChildDirectories(const std::wstring& path) {
    LayerMountTestShared::AddInheritableDenyAce(
        path, FILE_DELETE_CHILD, CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE);
}

HRESULT CreateDirectoryForReadAndDeleteChild(LayerMountHolder& mount, PCWSTR relativePath,
                                             OpenedFile& out) {
    return CreateOverlayFile(mount.Get(), relativePath, GENERIC_READ | FILE_DELETE_CHILD,
                             FILE_DIRECTORY_FILE, FILE_ATTRIBUTE_DIRECTORY, out);
}

}

TEST_CLASS(AbiCreateRollbackTests) {
public:
    TEST_METHOD(CreateDirectory_WhenHandleOpenFails_RemovesTheDirectoryItMade) {
        TempLayerEnv     env(0);
        const std::wstring parent = env.Upper() + L"\\denied";
        Assert::IsTrue(::CreateDirectoryW(parent.c_str(), nullptr) != FALSE,
                       L"CreateDirectoryW makes the parent in the upper");
        DenyDeleteChildInChildDirectories(parent);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile    created;
        const HRESULT hr = CreateDirectoryForReadAndDeleteChild(mount, L"\\denied\\newdir", created);
        Assert::IsTrue(FAILED(hr), L"the directory create fails");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED), hr,
                                  L"the directory create reports access denied");
        Assert::IsNull(created.handle, L"the failed create returns no handle");

        const std::wstring newDir = env.Upper() + L"\\denied\\newdir";
        const DWORD attrs = ::GetFileAttributesW(newDir.c_str());
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES, attrs,
                                L"the upper has no directory at the failed path");
    }

    TEST_METHOD(CreateDirectory_WhenHandleOpenFailsOnExistingUpperDirectory_LeavesIt) {
        TempLayerEnv     env(0);
        const std::wstring parent = env.Upper() + L"\\denied";
        Assert::IsTrue(::CreateDirectoryW(parent.c_str(), nullptr) != FALSE,
                       L"CreateDirectoryW makes the parent in the upper");
        DenyDeleteChildInChildDirectories(parent);
        const std::wstring existing = parent + L"\\existing";
        Assert::IsTrue(::CreateDirectoryW(existing.c_str(), nullptr) != FALSE,
                       L"CreateDirectoryW makes the existing directory in the upper");
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile    created;
        const HRESULT hr = CreateDirectoryForReadAndDeleteChild(mount, L"\\denied\\existing", created);
        Assert::IsTrue(FAILED(hr), L"the directory create fails");
        Assert::IsNull(created.handle, L"the failed create returns no handle");

        const DWORD attrs = ::GetFileAttributesW(existing.c_str());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, attrs,
                                   L"a directory the engine did not make stays in the upper");
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       L"the upper entry is still a directory");
    }
};

}
