/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/


;Include  
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"

;Settings
Name "Looking Glass Host"
OutFile "..\build\Looking-Glass-Host-Setup.exe" 
Unicode true
RequestExecutionLevel admin
ShowInstDetails "show"
ShowUninstDetails "show"
InstallDir "$PROGRAMFILES64\Looking-Glass"

!define MUI_ICON "..\..\resources\icon.ico"
!define MUI_UNICON "..\..\resources\icon.ico"
!define MUI_LICENSEPAGE_BUTTON "Agree"
!define /file VERSION "..\..\VERSION"

;Install and uninstall pages
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

;Install 
Section "Install" Section1
  SectionIn RO

  SetOutPath $INSTDIR
  File ..\build\looking-glass-host.exe
  File /oname=LICENSE.txt ..\..\LICENSE
  WriteUninstaller $INSTDIR\uninstaller.exe

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"EstimatedSize" "$0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"DisplayName" "Looking-Glass"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"UninstallString" "$\"$INSTDIR\uninstaller.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"QuietUninstallString" "$\"$INSTDIR\uninstaller.exe$\" /S"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"Publisher" "Geoffrey McRae"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"DisplayIcon" "$\"$INSTDIR\looking-glass-host.exe$\""
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"NoRepair" "1"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
	"NoModify" "1"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass" \
    "DisplayVersion" ${VERSION}

SectionEnd

Section "Auto Start Looking-Glass" Section2

  nsExec::Exec 'SCHTASKS /Delete /F /TN "Looking Glass"'
  nsExec::Exec 'SCHTASKS /Create /TN "Looking Glass" /SC  ONLOGON /RL HIGHEST /TR "C:\Program Files\Looking-Glass\looking-glass-host.exe"'

SectionEnd

Section /o "Desktop Shortcut" Section3
  SetShellVarContext all
  CreateShortCut $DESKTOP\Looking-Glass-Host.lnk $INSTDIR\looking-glass-host.exe
SectionEnd

Section /o "Start Menu Shortcut" Section4
  SetShellVarContext all
  CreateShortCut $STARTMENU\Looking-Glass-Host.lnk $INSTDIR\looking-glass-host.exe
SectionEnd


Section "Uninstall" Section5
  SetShellVarContext all

  nsExec::Exec 'SCHTASKS /Delete /F /TN "Looking Glass"'

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking-Glass"
  Delete $STARTMENU\Looking-Glass-Host.lnk
  Delete $DESKTOP\Looking-Glass-Host.lnk
  Delete $INSTDIR\uninstaller.exe
  Delete $INSTDIR\looking-glass-host.exe
  Delete $INSTDIR\LICENSE

  RMDir $INSTDIR

SectionEnd

;Description text for selection of install items
LangString DESC_Section1 ${LANG_ENGLISH} "Install Files into $INSTDIR"
LangString DESC_Section2 ${LANG_ENGLISH} "Create scheduled task to automatically start Looking-Glass."
LangString DESC_Section3 ${LANG_ENGLISH} "Create desktop shortcut icon."
LangString DESC_Section4 ${LANG_ENGLISH} "Create start menu shortcut."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Section1} $(DESC_Section1)
  !insertmacro MUI_DESCRIPTION_TEXT ${Section2} $(DESC_Section2)
  !insertmacro MUI_DESCRIPTION_TEXT ${Section3} $(DESC_Section3)
  !insertmacro MUI_DESCRIPTION_TEXT ${Section4} $(DESC_Section4)
!insertmacro MUI_FUNCTION_DESCRIPTION_END