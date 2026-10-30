using FatzAudio.AppChecks;
using Foundation;
using UIKit;

// Runs the checks once the app has started, prints the result to stdout (simctl launch --console shows
// it) and exits with 0 or 1.
UIApplication.Main(args, null, typeof(AppDelegate));

[Register("AppDelegate")]
public class AppDelegate : UIApplicationDelegate
{
    public override bool FinishedLaunching(UIApplication application, NSDictionary? launchOptions)
    {
        Task.Run(() =>
        {
            bool ok = AppCheck.Run(Environment.GetEnvironmentVariable("FA_APP_FILE"),
                                   Environment.GetEnvironmentVariable("FA_APP_URL"), Console.WriteLine);
            Console.Out.Flush();
            Environment.Exit(ok ? 0 : 1);
        });
        return true;
    }
}
