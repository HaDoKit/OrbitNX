using OrbitNX.Input;
using OrbitNX.Settings;
using OrbitNX.ShadPs4;

namespace OrbitNX;

/// <summary>
/// Main control window (start/stop, button layout, driver reinstall). Closing the window
/// minimizes to tray instead of exiting (use the tray icon's "Выход" to actually quit) - this is
/// meant to run in the background for the whole play session, not be babysat.
/// </summary>
public sealed class MainForm : Form
{
    private static readonly Color AccentGreen = Color.FromArgb(46, 160, 67);
    private static readonly Color AccentRed = Color.FromArgb(200, 60, 60);
    private static readonly Color PanelBack = Color.FromArgb(247, 247, 249);

    private readonly Config _config;
    private readonly AppOrchestrator _orchestrator;
    private readonly NotifyIcon _trayIcon;

    private readonly TextBox _ipBox = new();
    private readonly Button _toggleButton = new();
    private readonly Label _statusLabel = new();
    private readonly CheckBox _switchLayoutCheckBox = new();
    private readonly ComboBox _resolutionCombo = new();
    private readonly ComboBox _fpsCombo = new();
    private readonly NumericUpDown _bitrateUpDown = new();
    private readonly ComboBox _audioSampleRateCombo = new();
    private readonly ComboBox _audioChannelsCombo = new();
    private readonly TrackBar _audioGainSlider = new();
    private readonly Label _audioGainValueLabel = new();
    private readonly TextBox _shadPs4PathBox = new();

    public MainForm(Config config, AppOrchestrator orchestrator, NotifyIcon trayIcon)
    {
        _config = config;
        _orchestrator = orchestrator;
        _trayIcon = trayIcon;

        Text = "OrbitNX";
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application;
        Font = new Font("Segoe UI", 9.5f);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = true;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.White;
        Padding = new Padding(16);

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            AutoSize = true,
        };
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        root.Controls.Add(BuildConnectionGroup(), 0, 0);
        root.Controls.Add(BuildControlGroup(), 0, 1);
        root.Controls.Add(BuildVideoGroup(), 0, 2);
        root.Controls.Add(BuildAudioGroup(), 0, 3);
        root.Controls.Add(BuildGamepadGroup(), 0, 4);
        root.Controls.Add(BuildShadPs4Group(), 0, 5);
        root.Controls.Add(BuildDriversGroup(), 0, 6);

        Controls.Add(root);

        // Measured, not guessed: GetPreferredSize reflects the actual sum of every group's
        // AutoSize row, so this stays correct regardless of future additions instead of clipping
        // the bottom of the settings grid the way a hand-picked constant would.
        var preferredContentSize = root.GetPreferredSize(Size.Empty);
        ClientSize = new Size(preferredContentSize.Width + Padding.Horizontal, preferredContentSize.Height + Padding.Vertical);

        _orchestrator.ConnectionChanged += () => BeginInvoke(RefreshStatus);
        _orchestrator.ShadPs4NotConfigured += () => BeginInvoke(PromptShadPs4NotConfigured);
        RefreshStatus();

        FormClosing += (_, e) =>
        {
            if (e.CloseReason != CloseReason.UserClosing) return;
            e.Cancel = true;
            Hide();
        };

        // Minimizing (as opposed to closing) would otherwise leave a useless taskbar entry for a
        // window with nothing worth looking at while minimized - hide to the tray icon instead,
        // same as closing does.
        Resize += (_, _) =>
        {
            if (WindowState == FormWindowState.Minimized) Hide();
        };
    }

    private GroupBox BuildConnectionGroup()
    {
        var group = NewGroup("Подключение", 70);

        var ipLabel = new Label { Text = "IP клиента:", AutoSize = true, Location = new Point(14, 30) };
        _ipBox.Text = _config.ClientIp;
        _ipBox.Location = new Point(110, 27);
        _ipBox.Width = 220;
        _ipBox.Leave += (_, _) => SaveClientIp();

        var advancedButton = new Button { Text = "…", Location = new Point(340, 26), Width = 30, Height = 24 };
        advancedButton.Click += (_, _) => OpenAdvancedSettings();

        group.Controls.AddRange(new Control[] { ipLabel, _ipBox, advancedButton });
        return group;
    }

    private GroupBox BuildControlGroup()
    {
        var group = NewGroup("Трансляция", 100);

        _toggleButton.Location = new Point(14, 26);
        _toggleButton.Size = new Size(374, 40);
        _toggleButton.FlatStyle = FlatStyle.Flat;
        _toggleButton.FlatAppearance.BorderSize = 0;
        _toggleButton.ForeColor = Color.White;
        _toggleButton.Font = new Font("Segoe UI Semibold", 11f);
        _toggleButton.Click += (_, _) => ToggleStreaming();

        _statusLabel.Location = new Point(14, 74);
        _statusLabel.AutoSize = true;
        _statusLabel.ForeColor = Color.DimGray;

        group.Controls.AddRange(new Control[] { _toggleButton, _statusLabel });
        return group;
    }

    private static readonly (int Width, int Height)[] ResolutionOptions =
    {
        (640, 360), (854, 480), (960, 540), (1280, 720), (1920, 1080),
    };

    private static readonly int[] FpsOptions = { 24, 30, 45, 60 };

    private static readonly int[] AudioSampleRateOptions = { 8000, 11025, 16000, 22050, 32000, 44100, 48000 };

    /// <summary>
    /// Every field here is only read once, at the start of the next VideoEncoder/AudioCaptureSender -
    /// unlike SwitchButtonLayout, changing these requires a reconnect since re-encoding mid-stream
    /// isn't possible. Lower values across the board trade quality for reliability on a constrained
    /// client/network path - exposed as free dials rather than a single "quality" slider so the two
    /// people actually using this can find their own balance.
    /// </summary>
    private GroupBox BuildVideoGroup()
    {
        var group = NewGroup("Видео", 130);
        var grid = NewSettingsGrid(3);

        var resolutionLabel = NewFieldLabel("Разрешение:");
        _resolutionCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _resolutionCombo.Width = 100;
        _resolutionCombo.Anchor = AnchorStyles.Left;
        _resolutionCombo.Margin = new Padding(0, 4, 0, 4);
        _resolutionCombo.Items.AddRange(ResolutionOptions.Select(r => (object)$"{r.Width}x{r.Height}").ToArray());
        var closestResolution = ResolutionOptions
            .OrderBy(r => Math.Abs(r.Width - _config.TargetWidth) + Math.Abs(r.Height - _config.TargetHeight))
            .First();
        _resolutionCombo.SelectedItem = $"{closestResolution.Width}x{closestResolution.Height}";
        _resolutionCombo.SelectedIndexChanged += (_, _) =>
        {
            var parts = ((string)_resolutionCombo.SelectedItem!).Split('x');
            _config.TargetWidth = int.Parse(parts[0]);
            _config.TargetHeight = int.Parse(parts[1]);
            _config.Save();
        };

        var fpsLabel = NewFieldLabel("Кадров в секунду:");
        _fpsCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _fpsCombo.Width = 100;
        _fpsCombo.Anchor = AnchorStyles.Left;
        _fpsCombo.Margin = new Padding(0, 4, 0, 4);
        _fpsCombo.Items.AddRange(FpsOptions.Cast<object>().ToArray());
        _fpsCombo.SelectedItem = FpsOptions.OrderBy(f => Math.Abs(f - _config.Fps)).First();
        _fpsCombo.SelectedIndexChanged += (_, _) =>
        {
            _config.Fps = (int)_fpsCombo.SelectedItem!;
            _config.Save();
        };

        var bitrateLabel = NewFieldLabel("Битрейт (кбит/с):");
        _bitrateUpDown.Minimum = 1000;
        _bitrateUpDown.Maximum = 50000;
        _bitrateUpDown.Increment = 500;
        _bitrateUpDown.Width = 100;
        _bitrateUpDown.Anchor = AnchorStyles.Left;
        _bitrateUpDown.Margin = new Padding(0, 4, 0, 4);
        _bitrateUpDown.Value = Math.Clamp(_config.BitrateKbps, (int)_bitrateUpDown.Minimum, (int)_bitrateUpDown.Maximum);
        _bitrateUpDown.ValueChanged += (_, _) =>
        {
            _config.BitrateKbps = (int)_bitrateUpDown.Value;
            _config.Save();
        };

        grid.Controls.Add(resolutionLabel, 0, 0);
        grid.Controls.Add(_resolutionCombo, 1, 0);
        grid.Controls.Add(fpsLabel, 0, 1);
        grid.Controls.Add(_fpsCombo, 1, 1);
        grid.Controls.Add(bitrateLabel, 0, 2);
        grid.Controls.Add(_bitrateUpDown, 1, 2);

        group.Controls.Add(grid);
        return group;
    }

    /// <summary>
    /// Audio is sent as Opus over RTP/UDP (see AudioCaptureSender) - sample rate and channel count
    /// are still real quality/bandwidth levers (they set the Opus encoder's input format and thus
    /// roughly its bitrate), but there's no longer a raw packet-size knob to expose, since framing
    /// is one Opus frame (20ms) per RTP packet rather than an arbitrary byte count. The client
    /// (OrbitNX/source/audio.c) decodes via libavcodec/libswresample, so deviating from
    /// 48000Hz/stereo only makes sense together with a matching client rebuild.
    /// </summary>
    private GroupBox BuildAudioGroup()
    {
        var group = NewGroup("Аудио", 230);
        var grid = NewSettingsGrid(5);

        var sampleRateLabel = NewFieldLabel("Частота дискретизации (Гц):");
        _audioSampleRateCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _audioSampleRateCombo.Width = 100;
        _audioSampleRateCombo.Anchor = AnchorStyles.Left;
        _audioSampleRateCombo.Margin = new Padding(0, 4, 0, 4);
        _audioSampleRateCombo.Items.AddRange(AudioSampleRateOptions.Cast<object>().ToArray());
        _audioSampleRateCombo.SelectedItem = AudioSampleRateOptions.OrderBy(r => Math.Abs(r - _config.AudioSampleRate)).First();
        _audioSampleRateCombo.SelectedIndexChanged += (_, _) =>
        {
            _config.AudioSampleRate = (int)_audioSampleRateCombo.SelectedItem!;
            _config.Save();
        };

        var channelsLabel = NewFieldLabel("Каналы:");
        _audioChannelsCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _audioChannelsCombo.Width = 100;
        _audioChannelsCombo.Anchor = AnchorStyles.Left;
        _audioChannelsCombo.Margin = new Padding(0, 4, 0, 4);
        _audioChannelsCombo.Items.AddRange(new object[] { "Моно", "Стерео" });
        _audioChannelsCombo.SelectedIndex = _config.AudioChannels >= 2 ? 1 : 0;
        _audioChannelsCombo.SelectedIndexChanged += (_, _) =>
        {
            _config.AudioChannels = _audioChannelsCombo.SelectedIndex == 0 ? 1 : 2;
            _config.Save();
        };

        var gainLabel = NewFieldLabel("Громкость:");

        var gainPanel = new Panel { Width = 160, Height = 45, Margin = new Padding(0, 2, 0, 2) };
        _audioGainSlider.Minimum = 10;
        _audioGainSlider.Maximum = 100;
        _audioGainSlider.TickFrequency = 10;
        _audioGainSlider.Width = 110;
        _audioGainSlider.Location = new Point(0, 0);
        _audioGainSlider.Value = (int)Math.Clamp(Math.Round(_config.AudioGain * 100), _audioGainSlider.Minimum, _audioGainSlider.Maximum);
        _audioGainValueLabel.AutoSize = true;
        _audioGainValueLabel.Location = new Point(118, 8);
        _audioGainValueLabel.Text = $"{_audioGainSlider.Value}%";
        // Distinct from AudioCaptureSender's peak limiter - this turns the whole signal down, not
        // just the tips of the loudest peaks. The client's decode/playback is an unmodifiable black
        // box, so this is the available lever for amplitude-dependent client-side audio artifacts.
        _audioGainSlider.ValueChanged += (_, _) =>
        {
            _audioGainValueLabel.Text = $"{_audioGainSlider.Value}%";
            _config.AudioGain = _audioGainSlider.Value / 100.0;
            _config.Save();
        };
        gainPanel.Controls.Add(_audioGainSlider);
        gainPanel.Controls.Add(_audioGainValueLabel);

        // The patched Switch client (OrbitNX/source/audio.c) expects RTP/Opus at 48000Hz/stereo (see
        // AudioCaptureSender) - deviating from those two values only makes sense together with a
        // matching client rebuild, since audio.c's decode/resample setup is compiled in around them.
        var compatibilityWarningLabel = new Label
        {
            Text = "Частота и каналы должны совпадать с клиентом (48000 Гц, стерео) - менять только вместе с пересборкой OrbitNX.",
            AutoSize = false,
            Width = 374,
            Height = 32,
            ForeColor = Color.DimGray,
            Font = new Font(Font.FontFamily, 8f),
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 4, 0, 4),
        };

        // Diagnostic-only, deliberately not persisted (see Config.AudioDebugRecordToFile) - isolates
        // this app's own capture/encode from the network and client's decode by recording locally
        // instead of streaming, to narrow down where an audible artifact is actually introduced.
        var debugRecordCheckBox = new CheckBox
        {
            Text = "Диагностика: писать в audio_debug.wav рядом с exe вместо сети",
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 8, 0, 4),
        };
        debugRecordCheckBox.CheckedChanged += (_, _) => _config.AudioDebugRecordToFile = debugRecordCheckBox.Checked;

        grid.Controls.Add(sampleRateLabel, 0, 0);
        grid.Controls.Add(_audioSampleRateCombo, 1, 0);
        grid.Controls.Add(channelsLabel, 0, 1);
        grid.Controls.Add(_audioChannelsCombo, 1, 1);
        grid.Controls.Add(gainLabel, 0, 2);
        grid.Controls.Add(gainPanel, 1, 2);
        grid.Controls.Add(compatibilityWarningLabel, 0, 3);
        grid.SetColumnSpan(compatibilityWarningLabel, 2);
        grid.Controls.Add(debugRecordCheckBox, 0, 4);
        grid.SetColumnSpan(debugRecordCheckBox, 2);

        group.Controls.Add(grid);
        return group;
    }

    private static Label NewFieldLabel(string text) =>
        new() { Text = text, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 8, 10, 6) };

    // Grid instead of hand-placed Locations: labels of very different lengths ("Битрейт" vs.
    // "Частота дискретизации") would overlap fixed-pixel control offsets. A left column sized to
    // the longest label and a right column for values sidesteps that regardless of label length.
    private static TableLayoutPanel NewSettingsGrid(int rowCount)
    {
        var grid = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = rowCount,
        };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (var i = 0; i < rowCount; i++)
        {
            grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        }
        return grid;
    }

    private GroupBox BuildGamepadGroup()
    {
        var group = NewGroup("Геймпад", 55);

        _switchLayoutCheckBox.Text = "Раскладка как на Nintendo Switch (A↔B, X↔Y)";
        _switchLayoutCheckBox.AutoSize = true;
        _switchLayoutCheckBox.Location = new Point(14, 26);
        _switchLayoutCheckBox.Checked = _config.SwitchButtonLayout;
        _switchLayoutCheckBox.CheckedChanged += (_, _) =>
        {
            _config.SwitchButtonLayout = _switchLayoutCheckBox.Checked;
            _orchestrator.SwapAbxy = _switchLayoutCheckBox.Checked;
            _config.Save();
        };

        group.Controls.Add(_switchLayoutCheckBox);
        return group;
    }

    /// <summary>
    /// shadPS4 (https://shadps4.net) is a portable, installer-less PS4 emulator - unlike Steam it
    /// writes no registry entries at all, so there's no *reliable* auto-detect the way
    /// SteamLauncher reads Steam's install path from the registry. As a best-effort convenience,
    /// <see cref="ShadPs4Launcher.TryAutoDetect"/> scans the common places a user would have
    /// extracted the portable zip to (see its own doc comment) - if that finds nothing, the user
    /// must point this at their own exe manually. Which of Big Picture/shadPS4 actually gets
    /// launched on any given connect is chosen on the Switch side (see AppOrchestrator) - this
    /// group only configures where shadPS4 lives on this PC.
    /// </summary>
    private GroupBox BuildShadPs4Group()
    {
        var group = NewGroup("shadPS4", 100);
        var grid = NewSettingsGrid(2);

        if (string.IsNullOrEmpty(_config.ShadPs4ExePath))
        {
            var detected = ShadPs4Launcher.TryAutoDetect();
            if (detected is not null)
            {
                Logger.Log("MainForm", $"shadPS4 auto-detected at {detected}");
                _config.ShadPs4ExePath = detected;
                _config.Save();
            }
        }

        var pathLabel = NewFieldLabel("Путь к shadPS4.exe:");
        _shadPs4PathBox.ReadOnly = true;
        _shadPs4PathBox.Width = 220;
        _shadPs4PathBox.Anchor = AnchorStyles.Left;
        _shadPs4PathBox.Margin = new Padding(0, 4, 8, 4);
        _shadPs4PathBox.Text = _config.ShadPs4ExePath;

        var browseButton = new Button { Text = "Обзор…", Width = 90, Anchor = AnchorStyles.Left, Margin = new Padding(0, 2, 0, 2) };
        browseButton.Click += (_, _) => BrowseForShadPs4Exe();

        var pathPanel = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0) };
        pathPanel.Controls.Add(_shadPs4PathBox);
        pathPanel.Controls.Add(browseButton);

        var downloadLink = new LinkLabel { Text = "Скачать shadPS4", AutoSize = true, Margin = new Padding(0, 8, 0, 0) };
        downloadLink.LinkClicked += (_, _) => ShadPs4Launcher.OpenDownloadPage();

        grid.Controls.Add(pathLabel, 0, 0);
        grid.Controls.Add(pathPanel, 1, 0);
        grid.SetColumnSpan(downloadLink, 2);
        grid.Controls.Add(downloadLink, 0, 1);

        group.Controls.Add(grid);
        return group;
    }

    private void BrowseForShadPs4Exe()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "shadPS4.exe|shadPS4.exe|Исполняемые файлы (*.exe)|*.exe",
            Title = "Укажите shadPS4.exe",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        _config.ShadPs4ExePath = dialog.FileName;
        _config.Save();
        _shadPs4PathBox.Text = dialog.FileName;
    }

    private void PromptShadPs4NotConfigured()
    {
        var result = MessageBox.Show(
            this,
            "shadPS4 не настроен или не найден по указанному пути. Открыть страницу загрузки?",
            "OrbitNX",
            MessageBoxButtons.YesNo,
            MessageBoxIcon.Warning);
        if (result == DialogResult.Yes) ShadPs4Launcher.OpenDownloadPage();
    }

    // Audio no longer has a driver to reinstall - see AudioCaptureSender (WASAPI loopback, built
    // into Windows, no third-party driver involved anymore).
    private GroupBox BuildDriversGroup()
    {
        var group = NewGroup("Драйверы", 62);

        var inputButton = new Button { Text = "Переустановить драйвер геймпада (ViGEmBus)", Location = new Point(14, 26), Size = new Size(374, 28) };
        inputButton.Click += (_, _) => ViGEmBusInstaller.RunInstaller();

        group.Controls.Add(inputButton);
        return group;
    }

    private static GroupBox NewGroup(string title, int height) => new()
    {
        Text = title,
        Width = 388,
        Height = height,
        Margin = new Padding(0, 0, 0, 12),
        Padding = new Padding(4),
        BackColor = PanelBack,
    };

    private void SaveClientIp()
    {
        var ip = _ipBox.Text.Trim();
        if (ip == _config.ClientIp) return;
        _config.ClientIp = ip;
        _config.Save();
    }

    private void OpenAdvancedSettings()
    {
        using var form = new SettingsForm(_config);
        if (form.ShowDialog(this) != DialogResult.OK) return;

        _config.ClientIp = form.Result.ClientIp;
        _config.VideoPort = form.Result.VideoPort;
        _config.HidPort = form.Result.HidPort;
        _config.AudioPort = form.Result.AudioPort;
        _config.Save();
        _ipBox.Text = _config.ClientIp;
    }

    private void ToggleStreaming()
    {
        SaveClientIp();
        if (_orchestrator.IsArmed)
        {
            _orchestrator.Disarm();
        }
        else
        {
            if (!_config.IsValid())
            {
                MessageBox.Show(this, "Сначала укажите IP клиента.", "OrbitNX", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            _orchestrator.Arm();
        }
        RefreshStatus();
    }

    private void RefreshStatus()
    {
        if (!_orchestrator.IsArmed)
        {
            _toggleButton.Text = "▶  Включить трансляцию";
            _toggleButton.BackColor = AccentGreen;
            _statusLabel.Text = "Статус: выключено";
        }
        else if (_orchestrator.IsConnected)
        {
            _toggleButton.Text = "■  Выключить трансляцию";
            _toggleButton.BackColor = AccentRed;
            var targetName = _orchestrator.ActiveTarget switch
            {
                AppTarget.BigPicture => "Big Picture (Steam)",
                AppTarget.ShadPs4 => "shadPS4",
                _ => null,
            };
            _statusLabel.Text = targetName is null
                ? "Статус: клиент подключён, идёт трансляция"
                : $"Статус: клиент подключён, идёт трансляция — {targetName}";
        }
        else
        {
            _toggleButton.Text = "■  Выключить трансляцию";
            _toggleButton.BackColor = AccentRed;
            _statusLabel.Text = "Статус: ожидание подключения клиента…";
        }

        _trayIcon.Text = _orchestrator.IsArmed
            ? (_orchestrator.IsConnected ? "OrbitNX — подключено" : "OrbitNX — ожидание")
            : "OrbitNX — выключено";
    }
}
