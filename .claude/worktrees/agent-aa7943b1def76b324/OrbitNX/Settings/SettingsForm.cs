namespace OrbitNX.Settings;

/// <summary>
/// Minimal settings dialog opened from the tray icon: lets the user set/change the client's IP
/// (and ports, if they ever differ from OrbitNX's defaults) without rebuilding the app.
/// </summary>
public sealed class SettingsForm : Form
{
    private readonly TextBox _ipBox = new();
    private readonly NumericUpDown _videoPort = new();
    private readonly NumericUpDown _hidPort = new();
    private readonly NumericUpDown _audioPort = new();

    public Config Result { get; private set; }

    public SettingsForm(Config current)
    {
        Result = current;

        Text = "OrbitNX - Настройки";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(320, 200);

        var ipLabel = new Label { Text = "IP устройства:", Location = new Point(12, 15), AutoSize = true };
        _ipBox.Location = new Point(140, 12);
        _ipBox.Width = 160;
        _ipBox.Text = current.ClientIp;

        var videoLabel = new Label { Text = "Порт видео:", Location = new Point(12, 50), AutoSize = true };
        _videoPort.Location = new Point(140, 47);
        _videoPort.Width = 160;
        _videoPort.Minimum = 1; _videoPort.Maximum = 65535;
        _videoPort.Value = current.VideoPort;

        var hidLabel = new Label { Text = "Порт HID:", Location = new Point(12, 85), AutoSize = true };
        _hidPort.Location = new Point(140, 82);
        _hidPort.Width = 160;
        _hidPort.Minimum = 1; _hidPort.Maximum = 65535;
        _hidPort.Value = current.HidPort;

        var audioLabel = new Label { Text = "Порт звука:", Location = new Point(12, 120), AutoSize = true };
        _audioPort.Location = new Point(140, 117);
        _audioPort.Width = 160;
        _audioPort.Minimum = 1; _audioPort.Maximum = 65535;
        _audioPort.Value = current.AudioPort;

        var saveButton = new Button { Text = "Сохранить", Location = new Point(140, 155), DialogResult = DialogResult.OK };
        var cancelButton = new Button { Text = "Отмена", Location = new Point(225, 155), DialogResult = DialogResult.Cancel };

        Controls.AddRange(new Control[]
        {
            ipLabel, _ipBox, videoLabel, _videoPort, hidLabel, _hidPort,
            audioLabel, _audioPort, saveButton, cancelButton,
        });

        AcceptButton = saveButton;
        CancelButton = cancelButton;

        FormClosing += (_, e) =>
        {
            if (DialogResult != DialogResult.OK) return;
            Result = new Config
            {
                ClientIp = _ipBox.Text.Trim(),
                VideoPort = (int)_videoPort.Value,
                HidPort = (int)_hidPort.Value,
                AudioPort = (int)_audioPort.Value,
                TargetWidth = current.TargetWidth,
                TargetHeight = current.TargetHeight,
                Fps = current.Fps,
                BitrateKbps = current.BitrateKbps,
            };
        };
    }
}
