; Folvio installer (Inno Setup 6)
; Built by packaging\build_release.ps1 - expects the deployed app in dist\Folvio.

#ifndef AppVersion
  #define AppVersion "0.9.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Folvio"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{A36CF9A8-7919-4117-9EE2-39806CBE94B2}
AppName=Folvio
AppVersion={#AppVersion}
AppVerName=Folvio {#AppVersion}
AppPublisher=Folvio
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Folvio
; Per-user install by default (no administrator rights needed); admins may choose all users.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\Folvio
DefaultGroupName=Folvio
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=Folvio-Setup-{#AppVersion}-x64
SetupIconFile=..\..\assets\branding\folvio.ico
UninstallDisplayIcon={app}\Folvio.exe
UninstallDisplayName=Folvio
WizardStyle=modern
WizardImageFile=..\..\assets\branding\installer-wizard.bmp
WizardSmallImageFile=..\..\assets\branding\installer-small.bmp
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
LicenseFile=..\..\THIRD_PARTY_LICENSES.md

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; {autoprograms} / {autodesktop} resolve the real known folders (Start menu, Desktop),
; including a Desktop redirected to OneDrive.
Name: "{autoprograms}\Folvio"; Filename: "{app}\Folvio.exe"; IconFilename: "{app}\Folvio.exe"; Comment: "Convert and edit documents"
Name: "{autodesktop}\Folvio"; Filename: "{app}\Folvio.exe"; IconFilename: "{app}\Folvio.exe"; Comment: "Convert and edit documents"; Tasks: desktopicon

[Registry]
; "Open with" registration (no default handler is changed).
Root: HKA; Subkey: "Software\Classes\Applications\Folvio.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "Folvio"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\Folvio.exe\shell\open\command"; ValueType: string; ValueData: """{app}\Folvio.exe"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Applications\Folvio.exe\SupportedTypes"; ValueType: string; ValueName: ".pdf"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\Folvio.exe\SupportedTypes"; ValueType: string; ValueName: ".docx"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\Folvio.exe\SupportedTypes"; ValueType: string; ValueName: ".odt"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\Folvio.exe\SupportedTypes"; ValueType: string; ValueName: ".rtf"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: none; ValueName: "Applications\Folvio.exe"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Folvio.exe"; Description: "{cm:LaunchProgram,Folvio}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Program files only. User data (%LOCALAPPDATA%\Folvio: settings, backups) is intentionally kept.
Type: filesandordirs; Name: "{app}\engines"
