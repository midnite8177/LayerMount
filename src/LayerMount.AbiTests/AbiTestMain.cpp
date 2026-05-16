#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

TEST_CLASS(PlaceholderTests) {
public:
    TEST_METHOD(BuildVerification) {
        Assert::IsTrue(true, L"ABI test build verification passed");
    }
};

} // namespace LayerMountAbiTests
