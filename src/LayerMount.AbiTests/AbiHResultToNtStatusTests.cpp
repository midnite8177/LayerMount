#include "pch.h"

#pragma warning(push)
#pragma warning(disable: 4005)   // macro redefinition (windows.h already defines a few)
#include <ntstatus.h>
#pragma warning(pop)

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// LayerMountHResultToNtStatus is the canonical translation hosts call to
// bridge the DLL's HRESULT returns onto an NTSTATUS-shaped callback
// surface. These tests pin the contract any host adapter depends on.
TEST_CLASS(AbiHResultToNtStatusTests) {
public:
    TEST_METHOD(Success_MapsToStatusSuccess) {
        NTSTATUS s = STATUS_UNSUCCESSFUL;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountHResultToNtStatus(S_OK, &s));
        Assert::AreEqual<LONG>(STATUS_SUCCESS, s);
    }

    TEST_METHOD(NullOutPointer_ReturnsEPointer) {
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountHResultToNtStatus(E_HANDLE, nullptr));
    }

    TEST_METHOD(LayerMountComCodes_MapToCanonicalNtStatus) {
        struct Case { HRESULT hr; NTSTATUS expected; };
        const Case cases[] = {
            { E_HANDLE,              STATUS_INVALID_HANDLE       },
            { E_ILLEGAL_METHOD_CALL, STATUS_INVALID_DEVICE_STATE },
            { E_INVALIDARG,          STATUS_INVALID_PARAMETER    },
            { E_OUTOFMEMORY,         STATUS_NO_MEMORY            },
            { E_ACCESSDENIED,        STATUS_ACCESS_DENIED        },
            { E_POINTER,             STATUS_INVALID_PARAMETER    },
            { E_NOTIMPL,             STATUS_NOT_IMPLEMENTED      },
            { E_ABORT,               STATUS_CANCELLED            },
            { E_FAIL,                STATUS_UNSUCCESSFUL         },
        };
        for (const auto& c : cases) {
            NTSTATUS s = 0;
            Assert::AreEqual<HRESULT>(S_OK,
                ::LayerMountHResultToNtStatus(c.hr, &s));
            Assert::AreEqual<LONG>(c.expected, s);
        }
    }

    TEST_METHOD(FacilityWin32_MapsThroughInternalTable) {
        struct Case { DWORD win32; NTSTATUS expected; };
        const Case cases[] = {
            { ERROR_FILE_NOT_FOUND,      STATUS_OBJECT_NAME_NOT_FOUND },
            { ERROR_PATH_NOT_FOUND,      STATUS_OBJECT_PATH_NOT_FOUND },
            { ERROR_ACCESS_DENIED,       STATUS_ACCESS_DENIED         },
            { ERROR_INVALID_HANDLE,      STATUS_INVALID_HANDLE        },
            { ERROR_SHARING_VIOLATION,   STATUS_SHARING_VIOLATION     },
            { ERROR_DIR_NOT_EMPTY,       STATUS_DIRECTORY_NOT_EMPTY   },
            { ERROR_INSUFFICIENT_BUFFER, STATUS_BUFFER_TOO_SMALL      },
            { ERROR_DISK_FULL,           STATUS_DISK_FULL             },
        };
        for (const auto& c : cases) {
            NTSTATUS s = 0;
            Assert::AreEqual<HRESULT>(S_OK,
                ::LayerMountHResultToNtStatus(HRESULT_FROM_WIN32(c.win32), &s));
            Assert::AreEqual<LONG>(c.expected, s);
        }
    }

    TEST_METHOD(FacilityNtBit_RoundTrips) {
        // HRESULT_FROM_NT(ntstatus) sets bit 0x10000000; the translator
        // must invert it to recover the original NTSTATUS.
        const NTSTATUS originals[] = {
            STATUS_OBJECT_NAME_NOT_FOUND,
            STATUS_DIRECTORY_NOT_EMPTY,
            STATUS_INVALID_DEVICE_STATE,
            STATUS_ACCESS_DENIED,
        };
        for (NTSTATUS original : originals) {
            const HRESULT wrapped = HRESULT_FROM_NT(original);
            NTSTATUS s = 0;
            Assert::AreEqual<HRESULT>(S_OK,
                ::LayerMountHResultToNtStatus(wrapped, &s));
            Assert::AreEqual<LONG>(original, s);
        }
    }

    TEST_METHOD(UnknownHresult_FallsBackToUnsuccessful) {
        // FACILITY_ITF code that is not in any of the known tables.
        const HRESULT hr = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0xBEEF);
        NTSTATUS s = STATUS_SUCCESS;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountHResultToNtStatus(hr, &s));
        Assert::AreEqual<LONG>(STATUS_UNSUCCESSFUL, s);
    }
};

} // namespace LayerMountAbiTests
