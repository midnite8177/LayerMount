#include "pch.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

void AssertMapsTo(HRESULT hr, NTSTATUS expected)
{
    NTSTATUS s = 0;
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountHResultToNtStatus(hr, &s));
    Assert::AreEqual<LONG>(expected, s);
}

}

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
            AssertMapsTo(c.hr, c.expected);
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
            AssertMapsTo(HRESULT_FROM_WIN32(c.win32), c.expected);
        }
    }

    TEST_METHOD(FacilityWin32CloudFileCodes_MapToTheirCloudFileStatuses) {
        struct Case { DWORD win32; NTSTATUS expected; };
        const Case cases[] = {
            { ERROR_CLOUD_FILE_SYNC_ROOT_METADATA_CORRUPT,      STATUS_CLOUD_FILE_SYNC_ROOT_METADATA_CORRUPT      },
            { ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING,            STATUS_CLOUD_FILE_PROVIDER_NOT_RUNNING            },
            { ERROR_CLOUD_FILE_METADATA_CORRUPT,                STATUS_CLOUD_FILE_METADATA_CORRUPT                },
            { ERROR_CLOUD_FILE_METADATA_TOO_LARGE,              STATUS_CLOUD_FILE_METADATA_TOO_LARGE              },
            { ERROR_CLOUD_FILE_PROPERTY_BLOB_TOO_LARGE,         STATUS_CLOUD_FILE_PROPERTY_BLOB_TOO_LARGE         },
            { ERROR_CLOUD_FILE_PROPERTY_BLOB_CHECKSUM_MISMATCH, STATUS_CLOUD_FILE_PROPERTY_BLOB_CHECKSUM_MISMATCH },
            { ERROR_CLOUD_FILE_TOO_MANY_PROPERTY_BLOBS,         STATUS_CLOUD_FILE_TOO_MANY_PROPERTY_BLOBS         },
            { ERROR_CLOUD_FILE_PROPERTY_VERSION_NOT_SUPPORTED,  STATUS_CLOUD_FILE_PROPERTY_VERSION_NOT_SUPPORTED  },
            { ERROR_NOT_A_CLOUD_FILE,                           STATUS_NOT_A_CLOUD_FILE                           },
            { ERROR_CLOUD_FILE_NOT_IN_SYNC,                     STATUS_CLOUD_FILE_NOT_IN_SYNC                     },
            { ERROR_CLOUD_FILE_ALREADY_CONNECTED,               STATUS_CLOUD_FILE_ALREADY_CONNECTED               },
            { ERROR_CLOUD_FILE_NOT_SUPPORTED,                   STATUS_CLOUD_FILE_NOT_SUPPORTED                   },
            { ERROR_CLOUD_FILE_INVALID_REQUEST,                 STATUS_CLOUD_FILE_INVALID_REQUEST                 },
            { ERROR_CLOUD_FILE_READ_ONLY_VOLUME,                STATUS_CLOUD_FILE_READ_ONLY_VOLUME                },
            { ERROR_CLOUD_FILE_CONNECTED_PROVIDER_ONLY,         STATUS_CLOUD_FILE_CONNECTED_PROVIDER_ONLY         },
            { ERROR_CLOUD_FILE_VALIDATION_FAILED,               STATUS_CLOUD_FILE_VALIDATION_FAILED               },
            { ERROR_CLOUD_FILE_AUTHENTICATION_FAILED,           STATUS_CLOUD_FILE_AUTHENTICATION_FAILED           },
            { ERROR_CLOUD_FILE_INSUFFICIENT_RESOURCES,          STATUS_CLOUD_FILE_INSUFFICIENT_RESOURCES          },
            { ERROR_CLOUD_FILE_NETWORK_UNAVAILABLE,             STATUS_CLOUD_FILE_NETWORK_UNAVAILABLE             },
            { ERROR_CLOUD_FILE_UNSUCCESSFUL,                    STATUS_CLOUD_FILE_UNSUCCESSFUL                    },
            { ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT,             STATUS_CLOUD_FILE_NOT_UNDER_SYNC_ROOT             },
            { ERROR_CLOUD_FILE_IN_USE,                          STATUS_CLOUD_FILE_IN_USE                          },
            { ERROR_CLOUD_FILE_PINNED,                          STATUS_CLOUD_FILE_PINNED                          },
            { ERROR_CLOUD_FILE_REQUEST_ABORTED,                 STATUS_CLOUD_FILE_REQUEST_ABORTED                 },
            { ERROR_CLOUD_FILE_PROPERTY_CORRUPT,                STATUS_CLOUD_FILE_PROPERTY_CORRUPT                },
            { ERROR_CLOUD_FILE_ACCESS_DENIED,                   STATUS_CLOUD_FILE_ACCESS_DENIED                   },
            { ERROR_CLOUD_FILE_INCOMPATIBLE_HARDLINKS,          STATUS_CLOUD_FILE_INCOMPATIBLE_HARDLINKS          },
            { ERROR_CLOUD_FILE_PROPERTY_LOCK_CONFLICT,          STATUS_CLOUD_FILE_PROPERTY_LOCK_CONFLICT          },
            { ERROR_CLOUD_FILE_REQUEST_CANCELED,                STATUS_CLOUD_FILE_REQUEST_CANCELED                },
            { ERROR_CLOUD_FILE_PROVIDER_TERMINATED,             STATUS_CLOUD_FILE_PROVIDER_TERMINATED             },
            { ERROR_NOT_A_CLOUD_SYNC_ROOT,                      STATUS_NOT_A_CLOUD_SYNC_ROOT                      },
            { ERROR_CLOUD_FILE_REQUEST_TIMEOUT,                 STATUS_CLOUD_FILE_REQUEST_TIMEOUT                 },
            { ERROR_CLOUD_FILE_DEHYDRATION_DISALLOWED,          STATUS_CLOUD_FILE_DEHYDRATION_DISALLOWED          },
            { ERROR_CLOUD_FILE_US_MESSAGE_TIMEOUT,              STATUS_CLOUD_FILE_US_MESSAGE_TIMEOUT              },
        };
        for (const auto& c : cases) {
            AssertMapsTo(HRESULT_FROM_WIN32(c.win32), c.expected);
        }
    }

    TEST_METHOD(FacilityNtBit_RoundTrips) {
        const NTSTATUS originals[] = {
            STATUS_OBJECT_NAME_NOT_FOUND,
            STATUS_DIRECTORY_NOT_EMPTY,
            STATUS_INVALID_DEVICE_STATE,
            STATUS_ACCESS_DENIED,
        };
        for (NTSTATUS original : originals) {
            AssertMapsTo(HRESULT_FROM_NT(original), original);
        }
    }

    TEST_METHOD(UnknownHresult_FallsBackToUnsuccessful) {
        const HRESULT hr = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0xBEEF);
        NTSTATUS s = STATUS_SUCCESS;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountHResultToNtStatus(hr, &s));
        Assert::AreEqual<LONG>(STATUS_UNSUCCESSFUL, s);
    }
};

}
