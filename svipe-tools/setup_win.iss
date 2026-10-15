; Svipe Desktop for Windows: the installer, built by .github/workflows/svipe-win.yml.
;
; Derived from tdesktop's Telegram/build/setup.iss, with Svipe's own identity everywhere Windows
; keys an app by it: AppId (the uninstall entry Updater.exe keeps current, see updater_win.cpp),
; the Start menu folder and shortcut (windows_app_user_model_id.cpp looks for
; "Svipe Desktop\Svipe.lnk"), and the data folder (%APPDATA%\Svipe Desktop, core/version.h AppName).
; Telegram's values here would make this installer upgrade, or uninstall, Telegram Desktop.
;
;   iscc /dMyAppVersion=1.0.14 /dReleasePath=<out\Release> /dOutputDir=<dir> svipe-tools\setup_win.iss

#define MyAppShortName "Svipe"
#define MyAppName "Svipe Desktop"
#define MyAppPublisher "Svipe"
#define MyAppURL "https://svipe.uz/desktop"
#define MyAppExeName "Svipe.exe"
#define MyAppId "42B58ADA-3B86-47F7-899D-E80AE491D7EF"
#define CurrentYear GetDateTimeString('yyyy','','')
#define ModulesFolder "modules\x64"

[Setup]
AppId={{{#MyAppId}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppCopyright={#MyAppPublisher} {#CurrentYear}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={userappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir={#OutputDir}
OutputBaseFilename=svipe-desktop-win64-setup
SetupIconFile={#SourcePath}..\Telegram\Resources\art\icon256.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
DisableStartupPrompt=yes
PrivilegesRequired=lowest
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
CloseApplications=force
DisableDirPage=auto
DisableProgramGroupPage=auto
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "ru";      MessagesFile: "compiler:Languages\Russian.isl"
Name: "ua";      MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "tr";      MessagesFile: "compiler:Languages\Turkish.isl"
Name: "de";      MessagesFile: "compiler:Languages\German.isl"
Name: "es";      MessagesFile: "compiler:Languages\Spanish.isl"
Name: "fr";      MessagesFile: "compiler:Languages\French.isl"
Name: "it";      MessagesFile: "compiler:Languages\Italian.isl"
Name: "pt_BR";   MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#ReleasePath}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleasePath}\Updater.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ReleasePath}\{#ModulesFolder}\d3d\d3dcompiler_47.dll"; DestDir: "{app}\{#ModulesFolder}\d3d"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppShortName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppShortName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppShortName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\data"
Type: files; Name: "{app}\data_config"
Type: files; Name: "{app}\log.txt"
Type: filesandordirs; Name: "{app}\DebugLogs"
Type: filesandordirs; Name: "{app}\tupdates"
Type: filesandordirs; Name: "{app}\tdata"
Type: filesandordirs; Name: "{app}\tcache"
Type: filesandordirs; Name: "{app}\tdumps"
Type: filesandordirs; Name: "{app}\modules"
Type: dirifempty; Name: "{app}"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    Exec(ExpandConstant('{app}\{#MyAppExeName}'), '-cleanup', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
