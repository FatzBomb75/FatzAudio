using System.Text.RegularExpressions;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>The numbers the binding repeats (event types, curve kinds, capture flags) against the C header.</summary>
public class HeaderConstantsTests
{
    private static readonly string Header = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "fatzaudio.h"));

    /// <summary>The values of <c>typedef enum name { ... }</c>, counting up from the last explicit value as C does.</summary>
    private static Dictionary<string, long> Enum(string name)
    {
        var m = Regex.Match(Header, (name == "" ? @"\nenum \{(.*?)\}" : @"typedef enum " + name + @" \{(.*?)\}"), RegexOptions.Singleline);
        Assert.True(m.Success, name);
        var body = Regex.Replace(m.Groups[1].Value, @"/\*.*?\*/", "", RegexOptions.Singleline);
        var result = new Dictionary<string, long>();
        long next = 0;
        foreach (var item in body.Split(','))
        {
            var t = item.Trim();
            if (t.Length == 0) continue;
            var parts = t.Split('=');
            if (parts.Length == 2) next = Convert.ToInt64(parts[1].Trim(), parts[1].Trim().StartsWith("0x") ? 16 : 10);
            result[parts[0].Trim()] = next++;
        }
        return result;
    }

    [Fact]
    public void EventTypes_MatchTheHeader()
    {
        var e = Enum("fa_event_type");
        Assert.Equal(NativeConstants.EventEnded, e["FA_EVENT_ENDED"]);
        Assert.Equal(NativeConstants.EventPosition, e["FA_EVENT_POSITION"]);
        Assert.Equal(NativeConstants.EventMarker, e["FA_EVENT_MARKER"]);
        Assert.Equal(NativeConstants.EventMetadata, e["FA_EVENT_METADATA"]);
        Assert.Equal(NativeConstants.EventStalled, e["FA_EVENT_STALLED"]);
        Assert.Equal(NativeConstants.EventResumed, e["FA_EVENT_RESUMED"]);
        Assert.Equal(NativeConstants.EventDecodeError, e["FA_EVENT_DECODE_ERROR"]);
        Assert.Equal(NativeConstants.EventDeviceChanged, e["FA_EVENT_DEVICE_CHANGED"]);
        Assert.Equal(NativeConstants.EventLoudnessFinal, e["FA_EVENT_LOUDNESS_FINAL"]);
        Assert.Equal(NativeConstants.EventRampDone, e["FA_EVENT_RAMP_DONE"]);
        Assert.Equal(NativeConstants.EventStarted, e["FA_EVENT_STARTED"]);
        Assert.Equal(NativeConstants.EventMidiText, e["FA_EVENT_MIDI_TEXT"]);
        Assert.Equal(NativeConstants.EventLooped, e["FA_EVENT_LOOPED"]);
        Assert.Equal(NativeConstants.EventCdgChanged, e["FA_EVENT_CDG_CHANGED"]);
        Assert.Equal(NativeConstants.EventSeekFailed, e["FA_EVENT_SEEK_FAILED"]);
        Assert.Equal(NativeConstants.EventTrackerRow, e["FA_EVENT_TRACKER_ROW"]);
        Assert.Equal(NativeConstants.EventTrackerInstrument, e["FA_EVENT_TRACKER_INSTRUMENT"]);
        Assert.Equal(NativeConstants.EventTrackerSync, e["FA_EVENT_TRACKER_SYNC"]);
    }

    [Fact]
    public void CurveKinds_MatchTheHeader()
    {
        var e = Enum("fa_curve");
        Assert.Equal(NativeConstants.CurveLinear, e["FA_CURVE_LINEAR"]);
        Assert.Equal(NativeConstants.CurveEqualPower, e["FA_CURVE_EQUAL_POWER"]);
        Assert.Equal(NativeConstants.CurveLog, e["FA_CURVE_LOG"]);
        Assert.Equal(NativeConstants.CurveExp, e["FA_CURVE_EXP"]);
        Assert.Equal(NativeConstants.CurveSCurve, e["FA_CURVE_SCURVE"]);
        Assert.Equal(NativeConstants.CurveBezier, e["FA_CURVE_BEZIER"]);
        Assert.Equal(NativeConstants.CurvePoints, e["FA_CURVE_POINTS"]);
    }

    [Fact]
    public void CaptureFlags_MatchTheHeader()
    {
        Assert.Equal(NativeConstants.CaptureOsProcessing, Flag("FA_CAPTURE_OS_PROCESSING"));
        Assert.Equal(NativeConstants.CaptureLoopback, Flag("FA_CAPTURE_LOOPBACK"));
    }

    [Fact]
    public void ErrorCodes_MatchTheHeader()
    {
        var header = Enum("").Where(kv => kv.Key.StartsWith("FA_ERROR_") || kv.Key == "FA_OK").Select(kv => kv.Value).Order().ToArray();
        var binding = System.Enum.GetValues<ErrorCode>().Select(c => (long)c).Order().ToArray();
        Assert.Equal(header, binding);
    }

    private static uint Flag(string name)
    {
        var m = Regex.Match(Header, @"#define " + name + @"\s+\(1u << (\d+)\)");
        Assert.True(m.Success, name);
        return 1u << int.Parse(m.Groups[1].Value);
    }
}
