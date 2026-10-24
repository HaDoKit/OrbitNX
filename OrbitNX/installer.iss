#define MyAppName "OrbitNX"
#ifndef MyAppVersion
  ; Single source of truth for the version: ../VERSION (also read by OrbitNX.csproj and Client/Makefile).
  #define VersionFile FileOpen(AddBackslash(SourcePath) + "..\VERSION")
  #define MyAppVersion Trim(FileRead(VersionFile))
  #expr FileClose(VersionFile)
#endif
#define MyAppPublisher "OrbitNX"
#define MyAppExeName "OrbitNX.exe"
#define MyPublishDir "publish\win-x64"

[Setup]
AppId={{6E7B6E1B-6E0B-4C9E-9D3B-5F6C6E7B6E1B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=Setup
OutputBaseFilename=OrbitNX-Setup
SetupIconFile=Assets\icon.ico
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=yes
RestartApplications=yes
CloseApplicationsFilter=*.exe

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; focushook.dll (Steam focus-spoofing hook) was removed from the app - drop the copy older versions
; installed. Silently skipped if a still-running Steam from an old session has it loaded.
Type: files; Name: "{app}ocushook.dll"

[Files]
Source: "{#MyPublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
// Audio capture no longer needs a driver at all (see AudioCaptureSender - native WASAPI loopback),
// so this page only covers the gamepad driver now. Was a two-driver page (audio + input) before the
// audio pipeline rewrite dropped the virtual-audio-capturer dependency entirely.
var
  DriverPage: TWizardPage;
  InputGroup: TPanel;
  InputHeader: TNewStaticText;
  InputInstalled: Boolean;
  InputNone, InputAction, InputRemove: TNewRadioButton;

// ViGEmBus registers a kernel driver service under this key regardless of installer version -
// far more reliable than matching against its uninstall registry entry's display name/GUID.
function DetectInputDriver(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\ViGEmBus');
end;

function FindUninstallStringIn(const RootPath, NameSubstring: String): String;
var
  Names: TArrayOfString;
  I: Integer;
  DisplayName, UninstallStr, SubKey: String;
begin
  Result := '';
  if not RegGetSubkeyNames(HKLM, RootPath, Names) then Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    SubKey := RootPath + '\' + Names[I];
    if RegQueryStringValue(HKLM, SubKey, 'DisplayName', DisplayName) then
    begin
      if Pos(Lowercase(NameSubstring), Lowercase(DisplayName)) > 0 then
      begin
        if RegQueryStringValue(HKLM, SubKey, 'UninstallString', UninstallStr) then
        begin
          Result := UninstallStr;
          Exit;
        end;
      end;
    end;
  end;
end;

function FindUninstallString(const NameSubstring: String): String;
begin
  Result := FindUninstallStringIn('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', NameSubstring);
  if Result = '' then
    Result := FindUninstallStringIn('SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall', NameSubstring);
end;

procedure ExecCommandLine(const CmdLine, FailureMessage: String);
var
  exe, params: String;
  p, resultCode: Integer;
begin
  if (Length(CmdLine) > 0) and (CmdLine[1] = '"') then
  begin
    p := Pos('"', Copy(CmdLine, 2, Length(CmdLine) - 1));
    if p > 0 then
    begin
      exe := Copy(CmdLine, 2, p - 1);
      params := Trim(Copy(CmdLine, p + 2, Length(CmdLine)));
    end
    else
    begin
      exe := CmdLine;
      params := '';
    end;
  end
  else
  begin
    p := Pos(' ', CmdLine);
    if p > 0 then
    begin
      exe := Copy(CmdLine, 1, p - 1);
      params := Trim(Copy(CmdLine, p + 1, Length(CmdLine)));
    end
    else
    begin
      exe := CmdLine;
      params := '';
    end;
  end;
  if not Exec(exe, params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, resultCode) then
    MsgBox(FailureMessage, mbError, MB_OK);
end;

procedure RunGuiInstaller(const ExePath, FailureMessage: String);
var
  resultCode: Integer;
begin
  if not FileExists(ExePath) then
  begin
    MsgBox('Не найден: ' + ExePath, mbError, MB_OK);
    Exit;
  end;
  if not Exec(ExePath, '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, resultCode) then
    MsgBox(FailureMessage, mbError, MB_OK);
end;

procedure RunUninstall(const NameSubstring, Description: String);
var
  uninstallStr: String;
begin
  uninstallStr := FindUninstallString(NameSubstring);
  if uninstallStr = '' then
  begin
    MsgBox('Не удалось найти установщик для удаления "' + Description + '". Удалите вручную через Параметры Windows -> Приложения.', mbInformation, MB_OK);
    Exit;
  end;
  ExecCommandLine(uninstallStr, 'Не удалось запустить удаление: ' + Description);
end;

function MakeRadio(Parent: TWinControl; ATop: Integer; ACaption: String): TNewRadioButton;
begin
  Result := TNewRadioButton.Create(Parent);
  Result.Parent := Parent;
  Result.Left := 12;
  Result.Top := ATop;
  Result.Width := 380;
  Result.Caption := ACaption;
end;

procedure CreateDriverPage;
var
  inputActionCaption: String;
begin
  DriverPage := CreateCustomPage(wpSelectTasks, 'Драйвер геймпада', 'ViGEmBus нужен для эмуляции геймпада');

  InputInstalled := DetectInputDriver();

  InputHeader := TNewStaticText.Create(DriverPage);
  InputHeader.Parent := DriverPage.Surface;
  InputHeader.Left := 0;
  InputHeader.Top := 0;
  InputHeader.AutoSize := True;
  InputHeader.Font.Style := [fsBold];
  if InputInstalled then
    InputHeader.Caption := 'Драйвер геймпада (ViGEmBus) — уже установлен'
  else
    InputHeader.Caption := 'Драйвер геймпада (ViGEmBus) — не найден';

  InputGroup := TPanel.Create(DriverPage);
  InputGroup.Parent := DriverPage.Surface;
  InputGroup.Left := 0;
  InputGroup.Top := InputHeader.Top + InputHeader.Height + 6;
  InputGroup.Width := DriverPage.SurfaceWidth;
  InputGroup.Height := 78;
  InputGroup.BevelOuter := bvNone;

  InputNone := MakeRadio(InputGroup, 0, 'Не трогать');
  InputNone.Checked := True;

  if InputInstalled then inputActionCaption := 'Восстановить/переустановить'
  else inputActionCaption := 'Установить';
  InputAction := MakeRadio(InputGroup, 22, inputActionCaption);

  InputRemove := MakeRadio(InputGroup, 44, 'Удалить');
  InputRemove.Visible := InputInstalled;
  InputRemove.Enabled := InputInstalled;
end;

procedure InitializeWizard;
begin
  CreateDriverPage;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  appDir: String;
begin
  if CurStep = ssPostInstall then
  begin
    appDir := ExpandConstant('{app}');

    if InputAction.Checked then
      RunGuiInstaller(appDir + '\vigembus-setup.exe', 'Не удалось запустить установку драйвера геймпада.');
    if InputRemove.Checked then
      RunUninstall('ViGEmBus', 'драйвер геймпада (ViGEmBus)');
  end;
end;
