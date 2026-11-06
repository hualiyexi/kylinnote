; Compile with Unicode Inno Setup 6. Define SourceDir and OutputDir explicitly.
#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#ifndef SourceDir
  #error SourceDir must point to the windeployqt output directory
#endif
#ifndef OutputDir
  #error OutputDir must point to the artifact directory
#endif

[Setup]
AppId={{4A365FB7-E064-4772-8613-2EA4279B1FB8}
AppName=KylinNote
AppVersion={#AppVersion}
AppVerName=KylinNote {#AppVersion}
AppPublisher=KylinNote
DefaultDirName={localappdata}\Programs\KylinNote
DefaultGroupName=KylinNote
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=KylinNote-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\kylinnote.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\KylinNote"; Filename: "{app}\kylinnote.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\KylinNote"; Filename: "{app}\kylinnote.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\kylinnote.exe"; Description: "Launch KylinNote"; Flags: nowait postinstall skipifsilent

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
    InstalledExe := '"' + ExpandConstant('{app}\kylinnote.exe') + '"';
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run',
      'KylinNote', Command) then
    begin
      // Do not remove another portable installation's user preference.
      if Pos(Lowercase(InstalledExe), Lowercase(Command)) = 1 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'KylinNote');
    end;
  end;
end;
