#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::AccessDenied;

namespace LayerMountAbiTests {

namespace {

std::wstring StagingArea(const std::wstring& workDir) {
    return workDir + L"\\work";
}

bool Exists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring LastErrorMessage(HRESULT hr) {
    std::vector<wchar_t> message(4096);
    SIZE_T required = 0;
    if (FAILED(::LayerMountGetLastErrorMessage(hr, message.data(), message.size(), &required))) {
        return {};
    }
    return message.data();
}

HRESULT CreateWithPaths(const TempLayerEnv& env, const std::wstring& upper,
                        const std::wstring& workDir, LM_HANDLE* handle) {
    ConfigBuilder builder(env);
    LM_CONFIG config = *builder.Ptr();
    config.upperPath = upper.c_str();
    config.workDirPath = workDir.c_str();
    return ::LayerMountCreate(&config, handle);
}

HRESULT CreateWithLower(const TempLayerEnv& env, const std::wstring& lower,
                        const std::wstring& workDir, LM_HANDLE* handle) {
    ConfigBuilder builder(env);
    LM_CONFIG config = *builder.Ptr();
    const PCWSTR lowers[] = {lower.c_str()};
    config.lowerPathCount = 1;
    config.lowerPaths = lowers;
    config.workDirPath = workDir.c_str();
    return ::LayerMountCreate(&config, handle);
}

void WriteText(const std::wstring& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

void MakeReadOnly(const std::wstring& path) {
    Assert::IsTrue(::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
        (L"The test must make " + path + L" read-only").c_str());
}

}

TEST_CLASS(AbiWorkDirectoryTests) {
public:
    TEST_METHOD(Create_LeftoverStagedTree_EmptiesTheStagingArea) {
        TempLayerEnv env(1);
        const std::wstring staging = StagingArea(env.Work());
        const std::wstring container = staging + L"\\#1.2.3.4.tmp";
        const std::wstring entry = container + L"\\entry";
        WriteText(entry + L"\\data.txt", "staged");
        WriteText(staging + L"\\#1.2.3.5.tmp", "staged");
        MakeReadOnly(entry + L"\\data.txt");
        MakeReadOnly(staging + L"\\#1.2.3.5.tmp");
        const AccessDenied entryDeniesDelete(entry, DELETE);
        const AccessDenied containerDeniesChildDelete(container, FILE_DELETE_CHILD);

        LayerMountHolder mount = CreateLayerMount(env);

        Assert::IsTrue(std::filesystem::is_directory(staging),
            L"The staging area exists after the create");
        Assert::IsTrue(SortedNamesIn(staging).empty(),
            L"The create deletes every leftover in the staging area");
    }

    TEST_METHOD(Create_WhileAnotherOverlayHoldsTheWorkDirectory_FailsAsBusy) {
        TempLayerEnv env(1);
        LayerMountHolder first = CreateLayerMount(env);

        LM_HANDLE second = nullptr;
        const HRESULT hr =
            CreateWithPaths(env, env.Upper(), L"\\\\?\\" + env.Work() + L"\\", &second);
        const std::wstring message = LastErrorMessage(hr);
        if (second != nullptr) {
            (void)::LayerMountDestroy(second);
        }

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BUSY), hr,
            L"A second create on a held work directory fails as busy");
        Assert::IsNull(second);
        Assert::IsFalse(message.empty(), L"The failed create says why");

        first.Reset();
        LayerMountHolder afterDestroy = CreateLayerMount(env);
    }

    TEST_METHOD(Create_WhileAnotherOverlayHoldsTheUpper_FailsAsBusyNamingTheUpper) {
        TempLayerEnv env(1);
        LayerMountHolder first = CreateLayerMount(env);
        const std::wstring otherWorkDir = env.Root() + L"\\other-work";

        LM_HANDLE second = nullptr;
        const HRESULT hr = CreateWithPaths(env, env.Upper(), otherWorkDir, &second);
        const std::wstring message = LastErrorMessage(hr);
        LayerMountHolder secondHolder(second);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BUSY), hr,
            L"A second create on a held upper fails as busy, whatever its work directory");
        Assert::IsNull(second);
        Assert::IsTrue(message.find(env.Upper()) != std::wstring::npos,
            (L"The message names the upper: " + message).c_str());

        first.Reset();
        LM_HANDLE afterDestroy = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateWithPaths(env, env.Upper(), otherWorkDir, &afterDestroy),
            L"The upper is free once the first overlay is destroyed");
        LayerMountHolder afterDestroyHolder(afterDestroy);
    }

    TEST_METHOD(Create_LeftoverThatCannotBeDeleted_FailsNamingIt) {
        TempLayerEnv env(1);
        const std::wstring held = StagingArea(env.Work()) + L"\\#1.2.3.4.tmp";
        WriteText(held, "staged");
        const HANDLE holder = ::CreateFileW(held.c_str(), GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::IsTrue(holder != INVALID_HANDLE_VALUE, L"The test must hold the leftover open");

        LM_HANDLE handle = nullptr;
        const HRESULT hr = CreateWithPaths(env, env.Upper(), env.Work(), &handle);
        const std::wstring message = LastErrorMessage(hr);
        ::CloseHandle(holder);
        if (handle != nullptr) {
            (void)::LayerMountDestroy(handle);
        }

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), hr,
            L"A leftover that cannot be deleted fails the create");
        Assert::IsTrue(message.find(held) != std::wstring::npos,
            (L"The message names the leftover: " + message).c_str());
    }

    TEST_METHOD(Create_WorkDirectoryAndUpperNested_FailsWithEInvalidArg) {
        TempLayerEnv env(0);
        const std::wstring upper = env.Upper();
        const struct {
            std::wstring upper;
            std::wstring workDir;
            const wchar_t* what;
        } layouts[] = {
            {upper, upper, L"the upper as its own work directory"},
            {upper, upper + L"\\", L"the upper with a trailing separator"},
            {upper, L"\\\\?\\" + upper, L"the upper in extended form"},
            {upper, env.Root() + L"\\UPPER", L"the upper in another case"},
            {upper, upper + L"\\work", L"a work directory inside the upper"},
            {upper, upper + L"\\.overlay\\work", L"a work directory below the sidecar store"},
            {env.Work() + L"\\upper", env.Work(), L"an upper inside the work directory"},
        };
        std::filesystem::create_directories(env.Work() + L"\\upper");

        for (const auto& layout : layouts) {
            LM_HANDLE handle = nullptr;
            const HRESULT hr = CreateWithPaths(env, layout.upper, layout.workDir, &handle);
            const std::wstring message = LastErrorMessage(hr);
            if (handle != nullptr) {
                (void)::LayerMountDestroy(handle);
            }
            Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
                (std::wstring(L"The create refuses ") + layout.what).c_str());
            Assert::IsFalse(message.empty(),
                (std::wstring(L"The failed create says why for ") + layout.what).c_str());
        }
        Assert::IsFalse(Exists(upper + L"\\work"),
            L"A refused create makes no work directory in the upper");
    }

    TEST_METHOD(Create_WorkDirectoryIsAJunctionToTheUppersParent_FailsAndKeepsTheUpper) {
        TempLayerEnv env(0);
        const std::wstring parent = env.Root() + L"\\parent";
        const std::wstring upper = parent + L"\\work";
        const std::wstring alias = env.Root() + L"\\alias";
        WriteText(upper + L"\\keep.txt", "upper");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, alias, parent)) {
            return;
        }

        LM_HANDLE handle = nullptr;
        const HRESULT hr = CreateWithPaths(env, upper, alias, &handle);
        const std::wstring message = LastErrorMessage(hr);
        LayerMountHolder mount(handle);

        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"The create refuses a work directory whose staging area is the upper");
        Assert::IsFalse(message.empty(), L"The failed create says why");
        Assert::IsTrue(Exists(upper + L"\\keep.txt"), L"The upper keeps its files");
    }

    TEST_METHOD(Create_WorkDirectoryAndALowerOverlap_FailsWithEInvalidArgNamingTheLower) {
        TempLayerEnv env(1);
        const std::wstring lower = env.Lower(0);
        const std::wstring lowerInWork = env.Work() + L"\\lower";
        std::filesystem::create_directories(lowerInWork);
        const struct {
            std::wstring lower;
            std::wstring workDir;
            const wchar_t* what;
        } layouts[] = {
            {lower, lower, L"the lower as the work directory"},
            {lower, lower + L"\\work", L"a work directory inside the lower"},
            {lowerInWork, env.Work(), L"a lower inside the work directory"},
        };

        for (const auto& layout : layouts) {
            LM_HANDLE handle = nullptr;
            const HRESULT hr = CreateWithLower(env, layout.lower, layout.workDir, &handle);
            const std::wstring message = LastErrorMessage(hr);
            LayerMountHolder mount(handle);
            Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
                (std::wstring(L"The create refuses ") + layout.what).c_str());
            Assert::IsTrue(message.find(layout.lower) != std::wstring::npos,
                (std::wstring(L"The message names the lower for ") + layout.what + L": " +
                 message).c_str());
        }
        Assert::IsFalse(Exists(lower + L"\\work"),
            L"A refused create makes no work directory in the lower");
    }

    TEST_METHOD(Create_WorkDirectoryIsTheSidecarStoreOfTheUpper_Succeeds) {
        TempLayerEnv env(0);
        const std::wstring workDir = env.Upper() + L"\\.overlay";

        LM_HANDLE handle = nullptr;
        const HRESULT hr = CreateWithPaths(env, env.Upper(), workDir, &handle);
        LayerMountHolder mount(handle);

        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"The create accepts <upper>\\.overlay as the work directory");
        Assert::IsTrue(std::filesystem::is_directory(StagingArea(workDir)),
            L"The staging area is in <upper>\\.overlay");
    }

    TEST_METHOD(CreateTransient_StagesInTheSidecarStoreAndHoldsIt) {
        TempLayerEnv env(0);
        const std::wstring workDir = env.Root() + L"\\transient";

        LM_HANDLE first = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateTransient(workDir.c_str(), LM_CAP_NONE, &first),
            L"The first transient overlay is created");
        LayerMountHolder firstHolder(first);
        LM_HANDLE second = nullptr;
        const HRESULT secondHr = ::LayerMountCreateTransient(workDir.c_str(), LM_CAP_NONE, &second);
        LayerMountHolder secondHolder(second);

        Assert::IsTrue(std::filesystem::is_directory(StagingArea(workDir + L"\\.overlay")),
            L"A transient overlay stages in <workDir>\\.overlay\\work");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BUSY), secondHr,
            L"A second transient overlay on a held directory fails as busy");
        const std::vector<std::wstring> rootNames = SortedNamesIn(workDir);
        Assert::IsTrue(rootNames == std::vector<std::wstring>{L".overlay"},
            L"The root of a transient overlay's upper holds only .overlay");
    }
};

}
