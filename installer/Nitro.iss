; Nitro installer (Inno Setup 6). Built by CI:
;   ISCC.exe /DAppVersion=0.2.0 /DNumericVersion=0.2.0.0 /DSourceDir=..\package\Nitro installer\Nitro.iss
;
; Installs per user (no admin prompt) into %LOCALAPPDATA%\Programs\Nitro,
; because Nitro keeps its settings and imported shaders next to Nitro.exe.

#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.2.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\package\Nitro"
#endif

[Setup]
AppId={{7C1E9B4A-5D2F-4E8B-9A61-3F0B2C7D8E15}
AppName=Nitro
AppVersion={#AppVersion}
AppVerName=Nitro {#AppVersion}
AppPublisher=RJ22256
AppPublisherURL=https://github.com/RJ22256/Nitro
AppSupportURL=https://github.com/RJ22256/Nitro/issues
AppUpdatesURL=https://github.com/RJ22256/Nitro/releases
VersionInfoVersion={#NumericVersion}
VersionInfoDescription=Nitro Setup
DefaultDirName={autopf}\Nitro
DefaultGroupName=Nitro
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\package
OutputBaseFilename=Nitro-Setup
SetupIconFile=..\assets\nitro.ico
UninstallDisplayIcon={app}\Nitro.exe
UninstallDisplayName=Nitro
WizardStyle=modern
WizardImageFile=wizard-164.bmp,wizard-328.bmp
WizardSmallImageFile=small-55.bmp,small-110.bmp
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install Nitro {#AppVersion} on your computer.%n%nNitro adds Lossless Scaling's frame generation to your single-player games. You need your own copy of Lossless Scaling from Steam; Nitro's first-run setup imports its shaders.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\Nitro.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroHook.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroVk.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroVk.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroHook32.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroVk32.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroVk32.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NitroInject32.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Fonts\*"; DestDir: "{app}\Fonts"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Nitro"; Filename: "{app}\Nitro.exe"
Name: "{autodesktop}\Nitro"; Filename: "{app}\Nitro.exe"; Tasks: desktopicon

[Registry]
; Nitro.exe registers its Vulkan layer here when you enable a Vulkan game.
; Remove those entries on uninstall (nothing is created at install time).
Root: HKCU; Subkey: "Software\Khronos\Vulkan\ImplicitLayers"; ValueType: none; ValueName: "{app}\NitroVk.json"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Khronos\Vulkan\ImplicitLayers"; ValueType: none; ValueName: "{app}\NitroVk32.json"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\Nitro.exe"; Description: "{cm:LaunchProgram,Nitro}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Settings, the shaders imported from your Lossless Scaling, and logs
Type: files; Name: "{app}\Nitro.ini"
Type: files; Name: "{app}\Nitro.shaders"
Type: files; Name: "{app}\*.log"
Type: dirifempty; Name: "{app}\Fonts"
Type: dirifempty; Name: "{app}"

[Code]
function InitializeUninstall(): Boolean;
begin
  Result := True;
  if not UninstallSilent then
    Result := MsgBox('Before uninstalling, open Nitro and use "Remove from game" (or Remove in the Library) ' +
                     'for every game you installed it into. Uninstalling Nitro doesn''t touch your game folders.' + #13#10 + #13#10 +
                     'Uninstall Nitro now?', mbConfirmation, MB_YESNO) = IDYES;
end;
