# Tutorial: a WPF player

A window with Play/Pause, a seek bar, a volume slider, the song's title and artist, and a level meter. The pattern is the same for Avalonia, WinUI or MAUI: FatzAudio does the sound on its own threads and you update the UI on yours.

## 1. The engine lives as long as the app

```csharp
public partial class MainWindow : Window
{
    readonly AudioEngine _engine = AudioEngine.Create();
    readonly OutputDevice _output;
    AudioStream? _song;
    readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(50) };

    public MainWindow()
    {
        InitializeComponent();
        _output = _engine.OpenOutput();
        _timer.Tick += (_, _) => UpdateUi();
        Closed += (_, _) => { _song?.Dispose(); _output.Dispose(); _engine.Dispose(); };
    }
}
```

Close the song, then the output, then the engine.

## 2. Open a song

```csharp
void Open(string path)
{
    _song?.Dispose();
    _song = _engine.OpenFile(path);                       // for a URL, use OpenUrl off the UI thread
    var tags = Tags.Read(path, TagReadOptions.NoPictures);
    TitleText.Text = $"{tags.Artist} - {tags.Title}";
    Seek.Maximum = _song.Info.Duration.TotalSeconds;

    // Events come from an engine thread: hop to the UI thread.
    _song.Ended += (_, _) => Dispatcher.BeginInvoke(() => PlayButton.Content = "Play");

    _song.GetLevels();              // the first call starts the meters
    _song.PlayOn(_output);
    _timer.Start();
}
```

## 3. Buttons, seek bar and volume

```csharp
void PlayPause_Click(object s, RoutedEventArgs e)
{
    if (_song is null) return;
    if (_song.State == StreamState.Playing) _song.Pause(); else _song.Play();
}

void Seek_Drag(object s, DragCompletedEventArgs e) =>
    _song?.RequestSeek(TimeSpan.FromSeconds(Seek.Value));     // does not block the UI thread

void Volume_Changed(object s, RoutedPropertyChangedEventArgs<double> e)
{
    if (_song is not null) _song.Volume = (float)e.NewValue;  // 0..1, linear
}
```

## 4. Update the position and the meter

```csharp
void UpdateUi()
{
    if (_song is null) return;
    if (!Seek.IsMouseCaptureWithin) Seek.Value = _song.Position.TotalSeconds;
    var levels = _song.GetLevels(TimeSpan.FromMilliseconds(50));
    LeftMeter.Value  = levels.Peak[0];
    RightMeter.Value = levels.Peak.Length > 1 ? levels.Peak[1] : levels.Peak[0];   // mono files give one value
}
```

## Where next

[Karaoke app](karaoke-app.md) adds graphics, [common tasks](../common-tasks.md), [levels and spectrum](../features/levels-spectrum.md) for a spectrum visualizer, [tags](../features/tags.md) for cover art.
