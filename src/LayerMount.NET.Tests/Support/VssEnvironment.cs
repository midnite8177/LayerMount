namespace LayerMount.Tests.Support;

internal static class VssEnvironment
{
    /// <summary>
    /// True when the HRESULT a VSS call surfaced corresponds to an
    /// environmental condition (service down, no provider registered,
    /// caller not elevated) rather than a wrapper-level defect. Tests
    /// that exercise the VSS roundtrip on every host use this allowlist
    /// to tolerate the call failing without skipping the assertion that
    /// the wrapper itself round-trips the failure correctly.
    /// </summary>
    public static bool IsEnvironmentalHResult(int hr)
    {
        switch (unchecked((uint)hr))
        {
            case 0x80070005u: // E_ACCESSDENIED
            case 0x80070490u: // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
            case 0x800704DDu: // HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE)
            case 0x800703E3u: // HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)
            case 0x8004230Fu: // VSS_E_UNEXPECTED_PROVIDER_ERROR
            case 0x80042301u: // VSS_E_BAD_STATE
            case 0x80042302u: // VSS_E_PROVIDER_ALREADY_REGISTERED
            case 0x80042305u: // VSS_E_PROVIDER_NOT_REGISTERED
            case 0x80042306u: // VSS_E_PROVIDER_VETO
            case 0x8004230Cu: // VSS_E_INSUFFICIENT_STORAGE
                return true;
            default:
                return false;
        }
    }
}
