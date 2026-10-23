#ifndef PayloadDir
  #define PayloadDir "..\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\setup"
#endif

[Setup]
AppId={{83A5F9E7-438A-4C64-9AF4-61FE9628735D}
AppName=TaiJiAura
AppVersion=1.0
AppVerName=TaiJiAura 1.0
AppPublisher=TaiJiAura
DefaultDirName={localappdata}\Programs\TaiJiAura
DefaultGroupName=TaiJiAura
DisableDirPage=no
DisableWelcomePage=no
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=TaiJiAura-1.0-x64-Setup
SetupIconFile=..\resources\DesktopBall.ico
UninstallDisplayIcon={app}\TaiJiAura.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
Uninstallable=yes
AppMutex=Local\DesktopBall.Application.1
SetupMutex=TaiJiAura.Setup
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=yes
VersionInfoVersion=1.0.0.0

[Languages]
Name: "zhcn"; MessagesFile: "languages\ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
zhcn.SelectDirDesc=请选择 TaiJiAura 的安装位置。
zhcn.SelectDirLabel3=推荐使用默认目录，也可点击“浏览”选择其他磁盘的 TaiJiAura 专用文件夹。程序设置保存在安装目录，请选择当前用户可写的位置，避免 Windows、Program Files 等受保护目录。
en.SelectDirLabel3=Use the recommended per-user location, or click Browse to choose a dedicated TaiJiAura folder on another drive. Settings are saved beside the application. Choose a writable location outside Windows and Program Files.

[CustomMessages]
zhcn.DesktopIcon=创建桌面快捷方式
zhcn.LaunchApp=启动 TaiJiAura
zhcn.UninstallApp=卸载 TaiJiAura
zhcn.PathError=无法写入所选目录，请选择当前用户有写入权限的 TaiJiAura 专用文件夹。
zhcn.RootError=请选择 TaiJiAura 专用文件夹，不要直接选择磁盘根目录。
zhcn.RemoveSettings=是否同时删除 TaiJiAura 的个人设置？选择“否”将保留 TaiJiAura.ini，方便以后重新安装。
en.DesktopIcon=Create a desktop shortcut
en.LaunchApp=Launch TaiJiAura
en.UninstallApp=Uninstall TaiJiAura
en.PathError=Cannot write to the selected location. Choose a dedicated TaiJiAura folder writable by the current user.
en.RootError=Select a dedicated TaiJiAura folder rather than a drive root.
en.RemoveSettings=Also remove your TaiJiAura settings? Choose No to preserve TaiJiAura.ini for a future installation.

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\TaiJiAura.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\uninstall.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\TaiJiAura"; Filename: "{app}\TaiJiAura.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallApp}"; Filename: "{app}\uninstall.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\TaiJiAura"; Filename: "{app}\TaiJiAura.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\TaiJiAura.exe"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  AppDir, ProbeFile: String;
begin
  Result := '';
  AppDir := RemoveBackslash(ExpandConstant('{app}'));
  if CompareText(AppDir, RemoveBackslash(ExtractFileDrive(AppDir))) = 0 then
  begin
    Result := CustomMessage('RootError');
    Exit;
  end;
  if not ForceDirectories(AppDir) then
  begin
    Result := CustomMessage('PathError');
    Exit;
  end;
  ProbeFile := GenerateUniqueName(AppDir, '.desktopball-write-check-');
  if not SaveStringToFile(ProbeFile, 'TaiJiAura write check', False) then
    Result := CustomMessage('PathError')
  else
    DeleteFile(ProbeFile);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  StartupValue, ExpectedValue: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    ExpectedValue := '"' + ExpandConstant('{app}\TaiJiAura.exe') + '"';
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'TaiJiAura', StartupValue) then
      if CompareText(Trim(StartupValue), ExpectedValue) = 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'TaiJiAura');
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'DesktopBall', StartupValue) then
      if (CompareText(Trim(StartupValue), ExpectedValue) = 0) or
         (CompareText(Trim(StartupValue), '"' + ExpandConstant('{app}\DesktopBall.exe') + '"') = 0) then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'DesktopBall');
    if FileExists(ExpandConstant('{app}\TaiJiAura.ini')) or FileExists(ExpandConstant('{app}\DesktopBall.ini')) then
      if SuppressibleMsgBox(CustomMessage('RemoveSettings'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
      begin
        DeleteFile(ExpandConstant('{app}\TaiJiAura.ini'));
        DeleteFile(ExpandConstant('{app}\DesktopBall.ini'));
      end;
  end;
end;
