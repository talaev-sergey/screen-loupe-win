; Установщик Screen Loupe: ставит на текущего пользователя без прав администратора,
; прописывает автозапуск и сразу запускает лупу в фоне.
; Сборка: iscc /DAppVersion=0.1.0 installer\ScreenLoupe.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{C80D2901-2296-42A3-931D-9D819121837D}
AppName=Screen Loupe
AppVersion={#AppVersion}
AppPublisher=talaev-sergey
AppPublisherURL=https://github.com/talaev-sergey/screen-loupe-win
DefaultDirName={autopf}\Screen Loupe
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
MinVersion=10.0.22000
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\ScreenLoupe.exe
WizardStyle=modern
OutputDir=..\dist
OutputBaseFilename=ScreenLoupe-Setup

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "..\build\Release\ScreenLoupe.exe"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
  ValueName: "ScreenLoupe"; ValueData: """{app}\ScreenLoupe.exe"""; Flags: uninsdeletevalue

[Run]
Filename: "{app}\ScreenLoupe.exe"; Description: "{cm:LaunchProgram,Screen Loupe}"; Flags: nowait postinstall

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM ScreenLoupe.exe"; Flags: runhidden; RunOnceId: "StopApp"

[Code]
// Обновление поверх работающей лупы: exe занят, пока процесс жив.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM ScreenLoupe.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;
