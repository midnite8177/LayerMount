#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

std::string TrackerNameOfThisProcess() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring full(path);
    const size_t lastSlash = full.find_last_of(L'\\');
    const std::wstring base =
        lastSlash == std::wstring::npos ? full : full.substr(lastSlash + 1);
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, base.c_str(), -1,
                                             nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, base.c_str(), -1, utf8.data(), needed,
                          nullptr, nullptr);
    utf8.resize(static_cast<size_t>(needed) - 1);
    return utf8;
}

std::wstring WriteAllowOpenerDenyOthersRules(const TempLayerEnv& env) {
    return WriteTrackerRules(env,
        R"({"rules":[{"processName":")" + TrackerNameOfThisProcess() +
        R"(","pathPattern":"*","allowRead":true},)"
        R"({"processName":"*","pathPattern":"*","allowRead":false}]})");
}

std::wstring WriteDenyOpenerReadRules(const TempLayerEnv& env) {
    return WriteTrackerRules(env,
        R"({"rules":[{"processName":")" + TrackerNameOfThisProcess() +
        R"(","pathPattern":"*","allowRead":false}]})");
}

std::wstring WriteDenySystemAndUnknownProcessRules(const TempLayerEnv& env) {
    return WriteTrackerRules(env,
        R"({"rules":[{"processName":"<unknown>","pathPattern":"*","allowRead":false},)"
        R"({"processName":"System","pathPattern":"*","allowRead":false},)"
        R"({"processName":"*","pathPattern":"*"}]})");
}

std::wstring WriteAllowSystemDenyOpenerDeleteRules(const TempLayerEnv& env) {
    return WriteTrackerRules(env,
        R"({"rules":[{"processName":"<unknown>","pathPattern":"*"},)"
        R"({"processName":"System","pathPattern":"*"},)"
        R"({"processName":")" + TrackerNameOfThisProcess() +
        R"(","pathPattern":"*","allowDelete":false}]})");
}

LayerMountHolder CreateTrackedMount(const TempLayerEnv& env,
                                    const std::wstring& rulesPath) {
    LayerMountHolder mount = CreateLayerMount(env);
    constexpr BOOL enable = TRUE;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountProcessTrackerEnable(mount.Get(), enable));
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountProcessTrackerSetRules(mount.Get(), rulesPath.c_str()),
        L"the rule set loads");
    return mount;
}

}

TEST_CLASS(AbiTrackerOpenerTests) {
public:
    TEST_METHOD(RulesAllowOpenerAndDenyOthers_ReadOnOpenedHandle_ReturnsBytes) {
        TempLayerEnv env(0);
        env.WriteUpperFile(L"tracked.txt", "opener bytes");
        LayerMountHolder mount =
            CreateTrackedMount(env, WriteAllowOpenerDenyOthersRules(env));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\tracked.txt", GENERIC_READ,
                            kNoCreateOptions, opened),
            L"the open by the allowed process succeeds");

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(opened.handle, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead,
            L"the engine checks the read against the opener, so it succeeds");
        Assert::AreEqual(std::string("opener bytes"), bytes,
            L"the read returns the file's bytes");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    }

    TEST_METHOD(RulesDenyOpenerRead_Open_FailsWithAccessDenied) {
        TempLayerEnv env(0);
        env.WriteUpperFile(L"tracked.txt", "opener bytes");
        LayerMountHolder mount =
            CreateTrackedMount(env, WriteDenyOpenerReadRules(env));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED),
            OpenOverlayFile(mount.Get(), L"\\tracked.txt", GENERIC_READ,
                            kNoCreateOptions, opened),
            L"the rule denies the open itself");
        Assert::IsNull(opened.handle, L"the denied open returns no handle");
    }

    TEST_METHOD(RulesDenySystemProcess_ReadOnOpenedHandle_ReturnsBytes) {
        TempLayerEnv env(0);
        env.WriteUpperFile(L"tracked.txt", "opener bytes");
        LayerMountHolder mount =
            CreateTrackedMount(env, WriteDenySystemAndUnknownProcessRules(env));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\tracked.txt", GENERIC_READ,
                            kNoCreateOptions, opened),
            L"the open by this process passes the catch-all rule");

        HRESULT hrRead = E_FAIL;
        const std::string bytes = ReadThroughHandle(opened.handle, &hrRead);
        Assert::AreEqual<HRESULT>(S_OK, hrRead,
            L"a rule that denies the system process does not fail the read");
        Assert::AreEqual(std::string("opener bytes"), bytes,
            L"the read returns the file's bytes");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    }

    TEST_METHOD(RulesDenyOpenerDelete_CanDeleteOnHandleOpenedBySystem_Succeeds) {
        TempLayerEnv env(0);
        env.WriteUpperFile(L"tracked.txt", "opener bytes");
        LayerMountHolder mount =
            CreateTrackedMount(env, WriteAllowSystemDenyOpenerDeleteRules(env));
        constexpr DWORD systemPid = 4;

        OpenedFile bySystem;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFileAs(mount.Get(), L"\\tracked.txt", DELETE, kNoCreateOptions,
                              systemPid, bySystem),
            L"the open by the system process passes its rule");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCanDeleteOpenFile(bySystem.handle),
            L"the delete check uses the opener, not the process that calls it");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(bySystem.handle));

        OpenedFile byThisProcess;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\tracked.txt", DELETE, kNoCreateOptions,
                            byThisProcess),
            L"the open by this process passes; only its delete is denied");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED),
            ::LayerMountCanDeleteOpenFile(byThisProcess.handle),
            L"the rule denies the delete for the opener");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(byThisProcess.handle));
    }
};

}
