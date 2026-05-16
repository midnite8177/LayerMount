#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

TEST_CLASS(PlaceholderTests) {
public:
    TEST_METHOD(BuildVerification) {
        Assert::IsTrue(true, L"Build verification passed");
    }
};

} // namespace LayerMountTests
