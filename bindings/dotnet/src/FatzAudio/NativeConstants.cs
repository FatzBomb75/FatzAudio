namespace FatzAudio;

/// <summary>
/// The numbers the C header defines (<c>fa_event_type</c>, <c>fa_curve_kind</c>, <c>FA_CAPTURE_*</c>), named once. A
/// test (<c>HeaderConstantsTests</c>) reads the header and checks every one, so a renumbered enum fails the build's tests.
/// </summary>
internal static class NativeConstants
{
    // fa_event_type (the ones the binding delivers as stream events)
    public const int EventEnded = 0, EventPosition = 1, EventMarker = 2, EventMetadata = 3, EventStalled = 4,
        EventResumed = 5, EventDecodeError = 6, EventDeviceChanged = 7, EventLoudnessFinal = 8, EventRampDone = 9,
        EventStarted = 10, EventMidiText = 11, EventLooped = 12, EventCdgChanged = 13, EventSeekFailed = 14,
        EventTrackerRow = 15, EventTrackerInstrument = 21, EventTrackerSync = 22;

    // fa_curve_kind
    public const int CurveLinear = 0, CurveEqualPower = 1, CurveLog = 2, CurveExp = 3, CurveSCurve = 4, CurveBezier = 5, CurvePoints = 6;

    // FA_CAPTURE_*
    public const uint CaptureOsProcessing = 1u << 0, CaptureLoopback = 1u << 1;
}
