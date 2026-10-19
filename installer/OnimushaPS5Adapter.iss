#ifndef MyAppVersion
  #define MyAppVersion "2.3"
#endif
#define MyAppExeName "OnimushaPS5Adapter.Manager.exe"
#ifndef ReleaseStaging
  #define ReleaseStaging "..\release\staging"
#endif
#ifndef MyOutputName
  #ifdef DevelopmentBuild
    #define MyOutputName "Onimusha-PS5-Adapter-Setup-" + MyAppVersion + "-DEV"
  #else
    #define MyOutputName "Onimusha-PS5-Adapter-Setup-" + MyAppVersion
  #endif
#endif

[Setup]
AppId={{F1B1EF8B-82E6-4F54-B77A-4833D250619E}
AppName={cm:AppName}
AppVersion={#MyAppVersion}
AppVerName={cm:AppName} {#MyAppVersion}
AppPublisher={cm:Publisher}
DefaultDirName={autopf}\OnimushaPS5Adapter
DefaultGroupName={cm:ShortcutName}
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename={#MyOutputName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\assets\branding\onimusha-ps5-adapter.ico
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#MyAppVersion}.0.0
VersionInfoDescription=Onimusha PS5 Controller Adapter (Unofficial)
VersionInfoProductName=Onimusha PS5 Controller Adapter
VersionInfoProductVersion={#MyAppVersion}
ShowLanguageDialog=yes
UsePreviousLanguage=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "..\third_party\inno-zh\ChineseSimplified.isl"; LicenseFile: "..\legal\SOURCE_AVAILABLE_LICENSE_zh-CN.txt"; InfoBeforeFile: "..\legal\FREE_EDITION_NOTICE_zh-CN.txt"
Name: "english"; MessagesFile: "compiler:Default.isl"; LicenseFile: "..\legal\SOURCE_AVAILABLE_LICENSE_en-US.txt"; InfoBeforeFile: "..\legal\FREE_EDITION_NOTICE_en-US.txt"

[CustomMessages]
chinesesimplified.AppName=鬼武者 PS5 手柄适配工具（非官方）
chinesesimplified.ShortcutName=鬼武者 PS5 手柄适配工具
chinesesimplified.Publisher=独立开发者
chinesesimplified.UninstallShortcut=卸载鬼武者 PS5 手柄适配工具
chinesesimplified.InstallBridge=安装游戏手柄适配组件
chinesesimplified.OpenManager=打开手柄适配管理器
chinesesimplified.CloseGameBeforeUninstall=请先退出《鬼武者 剑之道》，再重新运行卸载。
chinesesimplified.LanguageSaveFailed=无法保存安装语言。请确认安装目录可写，然后重新运行安装程序。
english.AppName=Onimusha PS5 Controller Adapter (Unofficial)
english.ShortcutName=Onimusha PS5 Controller Adapter
english.Publisher=Independent Developer
english.UninstallShortcut=Uninstall Onimusha PS5 Controller Adapter
english.InstallBridge=Install the game controller bridge
english.OpenManager=Open the controller adapter manager
english.CloseGameBeforeUninstall=Please close Onimusha: Way of the Sword before running the uninstaller again.
english.LanguageSaveFailed=The installation language could not be saved. Please check that the installation directory is writable and run Setup again.

[Dirs]
Name: "{commonappdata}\OnimushaPS5Adapter"; Permissions: users-modify

[InstallDelete]
Type: files; Name: "{app}\legal\COMMERCIAL_LICENSE_zh-CN.txt"
Type: files; Name: "{app}\legal\COMMERCIAL_LICENSE_en-US.txt"
Type: files; Name: "{app}\legal\RELEASE_RIGHTS_DECLARATION.md"
Type: files; Name: "{app}\使用说明.md"
Type: files; Name: "{app}\User Guide.md"
Type: files; Name: "{app}\USER_GUIDE_2.2_en-US.md"
Type: files; Name: "{group}\Onimusha DualSense Adapter.lnk"
Type: files; Name: "{group}\Uninstall Onimusha DualSense Adapter.lnk"
Type: files; Name: "{autodesktop}\Onimusha DualSense Adapter.lnk"

[Files]
Source: "{#ReleaseStaging}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{cm:ShortcutName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallShortcut}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{cm:ShortcutName}"; Filename: "{app}\{#MyAppExeName}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--auto-install --language {code:GetManagerLanguage}"; Description: "{cm:InstallBridge}"; Flags: waituntilterminated runascurrentuser
Filename: "{app}\{#MyAppExeName}"; Parameters: "--language {code:GetManagerLanguage}"; Description: "{cm:OpenManager}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--uninstall-bridge-quiet"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveGameBridge"

[UninstallDelete]
Type: files; Name: "{app}\ui-language.txt"

[Code]
function GetManagerLanguage(Param: String): String;
begin
  if ActiveLanguage() = 'english' then
    Result := 'en-US'
  else
    Result := 'zh-CN';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if not SaveStringToFile(ExpandConstant('{app}\ui-language.txt'),
        GetManagerLanguage('') + #13#10, False) then
      RaiseException(CustomMessage('LanguageSaveFailed'));
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
  ManagerPath: String;
begin
  Result := True;
  ManagerPath := ExpandConstant('{app}\{#MyAppExeName}');
  if FileExists(ManagerPath) then
  begin
    if (not Exec(ManagerPath, '--can-uninstall', '', SW_HIDE,
        ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      MsgBox(CustomMessage('CloseGameBeforeUninstall'),
        mbError, MB_OK);
      Result := False;
    end;
  end;
end;
