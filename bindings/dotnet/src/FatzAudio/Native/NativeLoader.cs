using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.InteropServices;

namespace FatzAudio.Native;

/// <summary>
/// Resolves the <c>fatzaudio</c> native library. The default .NET probing (which understands the
/// <c>runtimes/&lt;rid&gt;/native</c> layout of NuGet packages) runs first; if it fails the application
/// directory and the assembly directory are checked as well.
/// </summary>
internal static class NativeLoader
{
    private static readonly object Gate = new();
    private static volatile bool _loaded, _versionChecked;
    private static string? _versionError;

    /// <summary>
    /// Sets up loading and, on first use, checks that the native library has this binding's
    /// major.minor version (before 1.0 a minor version may change the ABI, see CHANGELOG.md).
    /// Every entry point that calls into the library goes through here.
    /// </summary>
    /// <exception cref="InvalidOperationException">The native library is another major.minor version.</exception>
    public static void EnsureInitialized()
    {
        EnsureLoaded();
        if (!_versionChecked)
        {
            lock (Gate)
            {
                if (!_versionChecked)
                {
                    // A missing library throws DllNotFoundException here and is checked again next time.
                    _versionError = VersionMismatch(Unpack(NativeMethods.fa_version()), BindingVersion);
                    _versionChecked = true;
                }
            }
        }
        if (_versionError is not null) throw new InvalidOperationException(_versionError);
    }

    /// <summary>Sets up loading without the version check, for reporting the native version or an error text.</summary>
    public static void EnsureLoaded()
    {
        if (_loaded) return;
        lock (Gate)
        {
            if (_loaded) return;
            NativeLibrary.SetDllImportResolver(typeof(NativeLoader).Assembly, Resolve);
            if (OperatingSystem.IsAndroid()) SetJavaVmFromRuntime();
            _loaded = true;
        }
    }

    /// <summary>The version this binding was built for: the assembly version, which comes from FA_VERSION_*.</summary>
    public static Version BindingVersion
    {
        get
        {
            var v = typeof(NativeLoader).Assembly.GetName().Version ?? new Version(0, 0, 0);
            return new Version(v.Major, v.Minor, Math.Max(0, v.Build));
        }
    }

    public static Version Unpack(uint v) => new((int)(v >> 16), (int)((v >> 8) & 0xFF), (int)(v & 0xFF));

    /// <summary>Null when <paramref name="native"/> can be used with <paramref name="binding"/>, else the error text.</summary>
    internal static string? VersionMismatch(Version native, Version binding) =>
        native.Major == binding.Major && native.Minor == binding.Minor ? null
        : $"The native fatzaudio library is version {native}, but this FatzAudio package needs {binding.Major}.{binding.Minor}.x. Use the native library from the same package.";

    /// <summary>
    /// HTTPS on Android runs on the app's Java VM. The native library finds it by itself from
    /// Android 12; for older versions this hands it over from .NET for Android's own runtime
    /// (looked up by reflection, so the binding needs no reference to Mono.Android).
    /// </summary>
    [DynamicDependency(DynamicallyAccessedMemberTypes.PublicProperties, "Java.Interop.JniRuntime", "Java.Interop")]
    private static void SetJavaVmFromRuntime()
    {
        try
        {
            var runtimeType = Type.GetType("Java.Interop.JniRuntime, Java.Interop", throwOnError: false);
            var runtime = runtimeType?.GetProperty("CurrentRuntime", BindingFlags.Public | BindingFlags.Static)?.GetValue(null);
            var vm = runtime?.GetType().GetProperty("InvocationPointer", BindingFlags.Public | BindingFlags.Instance)?.GetValue(runtime);
            if (vm is nint p && p != 0) NativeMethods.fa_android_set_java_vm(p);
        }
        catch (Exception)
        {
            // Not .NET for Android's runtime: the app can call AudioEngine.SetAndroidJavaVM.
        }
    }

    private static nint Resolve(string name, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (name != "fatzaudio") return 0;

        if (NativeLibrary.TryLoad(name, assembly, searchPath, out var handle)) return handle;

        string file = OperatingSystem.IsWindows() ? "fatzaudio.dll"
                    : OperatingSystem.IsMacOS() ? "libfatzaudio.dylib"
                    : "libfatzaudio.so";

        foreach (var dir in CandidateDirectories(assembly))
        {
            var path = Path.Combine(dir, file);
            if (File.Exists(path) && NativeLibrary.TryLoad(path, out handle)) return handle;
        }

        // .NET for iOS links the native framework into the app bundle (Frameworks/fatzaudio.framework).
        if (OperatingSystem.IsIOS() && NativeLibrary.TryLoad("@rpath/fatzaudio.framework/fatzaudio", out handle)) return handle;
        return 0; // let the runtime raise DllNotFoundException
    }

    private static IEnumerable<string> CandidateDirectories(Assembly assembly)
    {
        var bases = new List<string> { AppContext.BaseDirectory };
        var asmDir = Path.GetDirectoryName(assembly.Location);
        if (!string.IsNullOrEmpty(asmDir)) bases.Add(asmDir);

        string os = OperatingSystem.IsWindows() ? "win" : OperatingSystem.IsMacOS() ? "osx" : "linux";
        string arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.X64 => "x64",
            Architecture.X86 => "x86",
            Architecture.Arm64 => "arm64",
            Architecture.Arm => "arm",
            _ => "x64",
        };

        foreach (var b in bases)
        {
            yield return b;
            yield return Path.Combine(b, "runtimes", $"{os}-{arch}", "native");
        }
    }
}
