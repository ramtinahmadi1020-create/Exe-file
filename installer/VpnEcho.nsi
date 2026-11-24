; VPN Echo 1.1.0 - NSIS installer script (Windows 10/11 x64)
; Build: makensis VpnEcho.nsi  (run from this folder)
Unicode True
!include "MUI2.nsh"
!include "LogicLib.nsh"

!define APPNAME "VPN Echo"
!define APPVERSION "1.1.0"
!define PUBLISHER "Ramtin Ahmadi"
!define APPEXE "VPN Echo.exe"

Name "${APPNAME} ${APPVERSION}"
OutFile "VPN-Echo-Setup.exe"
InstallDir "$PROGRAMFILES64\VPN Echo"
InstallDirRegKey HKLM "Software\VPN Echo" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "VPN Echo"

VIProductVersion "1.1.0.0"
VIAddVersionKey "ProductName" "VPN Echo"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "VPN Echo Setup"
VIAddVersionKey "FileVersion" "${APPVERSION}"
VIAddVersionKey "LegalCopyright" "developer: Ramtin Ahmadi"

!define MUI_ICON "../Assets/echo.ico"
!define MUI_UNICON "../Assets/echo.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APPEXE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "VPN Echo (required)" SecMain
  SectionIn RO
  SetRegView 64
  SetOutPath "$INSTDIR"
  File "../publish/VPN Echo.exe"
  File "../publish/sing-box.exe"
  File "../publish/libcronet.dll"
  File "../publish/servers.json"
  File "../publish/echo.ico"
  File "../publish/sing-box-LICENSE.txt"
  File "../publish/GPL-3.0.txt"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\VPN Echo"
  CreateShortcut "$SMPROGRAMS\VPN Echo\VPN Echo.lnk" "$INSTDIR\${APPEXE}" "" "$INSTDIR\echo.ico"
  CreateShortcut "$SMPROGRAMS\VPN Echo\Uninstall VPN Echo.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\VPN Echo.lnk" "$INSTDIR\${APPEXE}" "" "$INSTDIR\echo.ico"

  WriteRegStr HKLM "Software\VPN Echo" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "DisplayName" "VPN Echo"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "DisplayVersion" "${APPVERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "DisplayIcon" "$INSTDIR\echo.ico"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  Delete "$INSTDIR\VPN Echo.exe"
  Delete "$INSTDIR\sing-box.exe"
  Delete "$INSTDIR\libcronet.dll"
  Delete "$INSTDIR\servers.json"
  Delete "$INSTDIR\echo.ico"
  Delete "$INSTDIR\sing-box-LICENSE.txt"
  Delete "$INSTDIR\GPL-3.0.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\VPN Echo\VPN Echo.lnk"
  Delete "$SMPROGRAMS\VPN Echo\Uninstall VPN Echo.lnk"
  RMDir "$SMPROGRAMS\VPN Echo"
  Delete "$DESKTOP\VPN Echo.lnk"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VPN Echo"
  DeleteRegKey HKLM "Software\VPN Echo"
SectionEnd
