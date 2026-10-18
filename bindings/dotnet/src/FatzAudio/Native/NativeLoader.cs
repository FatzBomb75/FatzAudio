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
    private static int _initialized;

    public static void EnsureInitialized()
    {
        if (Interlocked.Exchange(ref _initialized, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeLoader).Assembly, Resolve);
        if (OperatingSystem.IsAndroid()) SetJavaVmFromRuntime();
    }

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
