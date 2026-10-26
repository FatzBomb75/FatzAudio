using FatzAudio.Native;

namespace FatzAudio;

/// <summary>The shape of an equalizer band.</summary>
public enum EqualizerBandType
{
    /// <summary>A bell around the frequency; <see cref="EqualizerBand.Q"/> sets its width (1.41 = one octave).</summary>
    Peak = 0,
    /// <summary>Everything below the frequency; Q 0.71 is the usual slope.</summary>
    LowShelf = 1,
    /// <summary>Everything above the frequency.</summary>
    HighShelf = 2,
}

/// <summary>One band of a stream's equalizer (<see cref="AudioStream.SetEqualizer(System.Collections.Generic.IReadOnlyList{EqualizerBand})"/>).</summary>
/// <param name="Type">Peak or shelf.</param>
/// <param name="Frequency">Centre (peak) or corner (shelf) frequency in Hz; 10 Hz to just under half the device rate.</param>
/// <param name="Q">Width: 0.1 (wide) to 20 (narrow).</param>
/// <param name="GainDb">Boost or cut in dB, -24 to +24.</param>
public readonly record struct EqualizerBand(EqualizerBandType Type, float Frequency, float Q, float GainDb)
{
    /// <summary>A peak band, the building block of a graphic EQ.</summary>
    public static EqualizerBand Peak(float frequency, float gainDb, float q = 1.41f) => new(EqualizerBandType.Peak, frequency, q, gainDb);
}

/// <summary>Built-in presets: ten peak bands on the octave frequencies (31 Hz to 16 kHz), so they all move the same sliders.</summary>
public enum EqualizerPreset
{
    /// <summary>All bands at 0 dB.</summary>
    Flat = 0,
    /// <summary>Lows and highs up, mids slightly down.</summary>
    Rock,
    /// <summary>Low mids up, lows and highs slightly down.</summary>
    Pop,
    /// <summary>A gentle smile.</summary>
    Jazz,
    /// <summary>Highs gently down.</summary>
    Classical,
    /// <summary>Strong lows, mids down.</summary>
    Dance,
    /// <summary>Strong lows.</summary>
    HipHop,
    /// <summary>Warm and present.</summary>
    Acoustic,
    /// <summary>Mids up for singing.</summary>
    Vocal,
    /// <summary>Speech range up, lows cut.</summary>
    SpokenWord,
    /// <summary>Lows up.</summary>
    BassBoost,
    /// <summary>Highs up.</summary>
    TrebleBoost,
    /// <summary>Lows and highs up, for quiet listening.</summary>
    Loudness,
    /// <summary>A live singing mic: less boom and mud, more presence and air.</summary>
    LiveVocal,
}

/// <summary>Equalizer presets.</summary>
public static class Equalizer
{
    /// <summary>Every built-in preset.</summary>
    public static IReadOnlyList<EqualizerPreset> Presets { get; } = Array.AsReadOnly(Enum.GetValues<EqualizerPreset>());

    /// <summary>The bands of a preset (ten peaks, 31 Hz to 16 kHz).</summary>
    /// <exception cref="ArgumentOutOfRangeException">Unknown preset.</exception>
    public static unsafe EqualizerBand[] GetPreset(EqualizerPreset preset)
    {
        var bands = stackalloc FaEqBand[10];
        uint n;
        if (NativeMethods.fa_eq_get_preset((int)preset, bands, &n) < 0) throw new ArgumentOutOfRangeException(nameof(preset));
        var result = new EqualizerBand[n];
        for (int i = 0; i < n; i++) result[i] = bands[i].ToBand();
        return result;
    }

    /// <summary>English display name ("Rock").</summary>
    /// <exception cref="ArgumentOutOfRangeException">Unknown preset.</exception>
    public static unsafe string GetPresetName(EqualizerPreset preset)
    {
        byte* p = NativeMethods.fa_eq_preset_name((int)preset);
        if (p == null) throw new ArgumentOutOfRangeException(nameof(preset));
        return NativeText.Read(p);
    }
}
