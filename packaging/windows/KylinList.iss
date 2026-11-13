; Compile with Unicode Inno Setup 6. Define AppVersion, SourceDir, OutputDir
; and SetupInfoDir explicitly via ISCC /D arguments (see build-windows.ps1).
#ifndef AppVersion
  #error AppVersion must be defined - pass it via /DAppVersion=<X.Y.Z>
#endif
#ifndef SourceDir
  #error SourceDir must point to the windeployqt output directory
#endif
#ifndef OutputDir
  #error OutputDir must point to the artifact directory
#endif
#ifndef SetupInfoDir
  #error SetupInfoDir must point to the directory holding the setup install-info.json
#endif

[Setup]
AppId={{DEAE89C0-8AFD-48CE-9440-843413ED60FA}
AppName=KylinList
AppVersion={#AppVersion}
AppVerName=KylinList (麒麟清单) {#AppVersion}
AppPublisher=KylinList
DefaultDirName={localappdata}\Programs\KylinList
DefaultGroupName=KylinList
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=KylinList-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\kylinlist.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "{#SourcePath}\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
; SourceDir carries the portable install-info.json; exclude it so installed
; copies get the setup identity below, reporting variant "setup" in {app}.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "install-info.json"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SetupInfoDir}\install-info.json"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\KylinList"; Filename: "{app}\kylinlist.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\KylinList"; Filename: "{app}\kylinlist.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\kylinlist.exe"; Description: "Launch KylinList"; Flags: nowait postinstall skipifsilent

; No user data directories appear in [Files] or [UninstallDelete]. In particular
; the SQLite database in LocalAppData is preserved across uninstall/reinstall.
; Autostart remains an explicit in-app preference, not an installer default.
[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Command: String;
  InstalledExe: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    InstalledExe := '"' + ExpandConstant('{app}\kylinlist.exe') + '"';
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run',
      'KylinList', Command) then
    begin
      // Do not remove another portable installation's user preference.
      if Pos(Lowercase(InstalledExe), Lowercase(Command)) = 1 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'KylinList');
    end;
  end;
end;
