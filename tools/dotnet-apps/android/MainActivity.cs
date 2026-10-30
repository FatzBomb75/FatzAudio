using Android.App;
using Android.OS;
using Android.Util;
using FatzAudio.AppChecks;

namespace FatzAudio.AndroidApp;

// Runs the checks on a worker thread when started (am start --es file ... --es url ...) and logs the
// result to logcat under "FatzAudioApp".
[Activity(Name = "net.fatzaudio.ciapp.MainActivity", Label = "FatzAudio CI", MainLauncher = true, Exported = true)]
public class MainActivity : Activity
{
    private const string Tag = "FatzAudioApp";

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        base.OnCreate(savedInstanceState);
        string? file = Intent?.GetStringExtra("file");
        string? url = Intent?.GetStringExtra("url");
        Task.Run(() => AppCheck.Run(file, url, line => Log.Info(Tag, line)));
    }
}
