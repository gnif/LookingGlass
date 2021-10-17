/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

;Include
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"

;Settings
Name "Looking Glass (host)"
OutFile "looking-glass-host-setup.exe"
Unicode true
RequestExecutionLevel admin
ShowInstDetails "show"
ShowUninstDetails "show"

!ifndef BUILD_32BIT
Target AMD64-Unicode
InstallDir "$PROGRAMFILES\Looking Glass (host)"
!else
InstallDir "$PROGRAMFILES64\Looking Glass (host)"
!endif

!define MUI_ICON "icon.ico"
!define MUI_UNICON "icon.ico"
!define MUI_LICENSEPAGE_BUTTON "Agree"
!define MUI_BGCOLOR "3c046c"
!define MUI_TEXTCOLOR "ffffff"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${NSISDIR}\Contrib\Graphics\Wizard\nsis3-grey.bmp"
!define /file VERSION "../../VERSION"

!define MUI_WELCOMEPAGE_TEXT "You are about to install $(^Name) version ${VERSION}.$\r$\n$\r$\nWhen upgrading, you don't need to close your Looking Glass client, but should install the ${VERSION} client after installation is complete.$\r$\n$\r$\nPress Next to continue."

;Install and uninstall pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"


Function ShowHelpMessage
  !define line1 "Command line options:$\r$\n$\r$\n"
  !define line2 "/S - silent install (must be uppercase)$\r$\n"
  !define line3 "/D=path\to\install\folder - Change install directory$\r$\n"
  !define line4 "   (Must be uppercase, the last option given and no quotes)$\r$\n$\r$\n"
  !define line5 "/startmenu - create start menu shortcut$\r$\n"
  !define line6 "/desktop - create desktop shortcut$\r$\n"
  !define line7 "/noservice - do not create a service to auto start and elevate the host"
  MessageBox MB_OK "${line1}${line2}${line3}${line4}${line5}${line6}${line7}"
  Abort
FunctionEnd

Function .onInit

  var /GLOBAL cmdLineParams
  Push $R0
  ${GetParameters} $cmdLineParams
  ClearErrors

  ${GetOptions} $cmdLineParams '/?' $R0
  IfErrors +2 0
  Call ShowHelpMessage

  ${GetOptions} $cmdLineParams '/H' $R0
  IfErrors +2 0
  Call ShowHelpMessage

  Pop $R0


  Var /GLOBAL option_startMenu
  Var /GLOBAL option_desktop
  Var /GlOBAL option_noservice
  StrCpy $option_startMenu     0
  StrCpy $option_desktop       0
  StrCpy $option_noservice     0

  Push $R0
    
  ${GetOptions} $cmdLineParams '/startmenu' $R0
  IfErrors +2 0
  StrCpy $option_startMenu 1

  ${GetOptions} $cmdLineParams '/desktop' $R0
  IfErrors +2 0
  StrCpy $option_desktop 1

  ${GetOptions} $cmdLineParams '/noservice' $R0
  IfErrors +2 0
  StrCpy $option_noservice 1

  Pop $R0

FunctionEnd

!macro StopLookingGlassService
  ;Attempt to stop existing LG service only if it exists

  nsExec::Exec 'sc.exe query "Looking Glass (host)"'
  Pop $0 ; SC.exe error level

  ${If} $0 == 0 ; If error level is 0, service exists
    DetailPrint "Stop service: Looking Glass (host)"
    nsExec::ExecToLog 'net.exe STOP "Looking Glass (host)"'
  ${EndIf}

!macroend

;Install 
Section "-Install" Section1

  !insertmacro StopLookingGlassService

  SetOutPath $INSTDIR
  File ..\..\looking-glass-host.exe
  File /nonfatal ..\..\looking-glass-host.pdb
  File LICENSE.txt
  WriteUninstaller $INSTDIR\uninstaller.exe

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "EstimatedSize" "$0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "DisplayName" "Looking Glass (host)"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "UninstallString" "$\"$INSTDIR\uninstaller.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "QuietUninstallString" "$\"$INSTDIR\uninstaller.exe$\" /S"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "Publisher" "Geoffrey McRae"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "DisplayIcon" "$\"$INSTDIR\looking-glass-host.exe$\""
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "NoRepair" "1"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "NoModify" "1"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)" \
  "DisplayVersion" ${VERSION}

SectionEnd

Section "Looking Glass (host) Service" Section2

  ${If} $option_noservice == 0
    DetailPrint "Install service: Looking Glass (host)"
    nsExec::Exec '"$INSTDIR\looking-glass-host.exe" UninstallService'
    nsExec::ExecToLog '"$INSTDIR\looking-glass-host.exe" InstallService'
  ${EndIf}

SectionEnd

Section /o "Desktop Shortcut" Section3
  StrCpy $option_desktop 1
SectionEnd

Section "Start Menu Shortcut" Section4
  StrCpy $option_startMenu 1
SectionEnd

Section "-Hidden Start Menu" Section5
  SetShellVarContext all
  
  ${If} $option_startMenu == 1
    CreateDirectory "$APPDATA\Looking Glass (host)"
    CreateDirectory "$SMPROGRAMS\Looking Glass (host)"
    CreateShortCut "$SMPROGRAMS\Looking Glass (host)\Looking Glass (host).lnk" $INSTDIR\looking-glass-host.exe
    CreateShortCut "$SMPROGRAMS\Looking Glass (host)\Looking Glass Logs.lnk" "$APPDATA\Looking Glass (host)"
  ${EndIf}

  ${If} $option_desktop == 1
    CreateShortCut "$DESKTOP\Looking Glass (host).lnk" $INSTDIR\looking-glass-host.exe
  ${EndIf}
  
SectionEnd

Section "Uninstall" Section6
  SetShellVarContext all

  !insertmacro StopLookingGlassService

  DetailPrint "Uninstall service: Looking Glass (host)"
  nsExec::ExecToLog '"$INSTDIR\looking-glass-host.exe" UninstallService'

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (host)"
  Delete "$SMPROGRAMS\Looking Glass (host).lnk"
  Delete "$DESKTOP\Looking Glass (host).lnk"
  Delete "$INSTDIR\uninstaller.exe"
  Delete "$INSTDIR\looking-glass-host.exe"
  Delete "$INSTDIR\LICENSE.txt"

  RMDir $INSTDIR
SectionEnd

;Description text for selection of install items
LangString DESC_Section1 ${LANG_ENGLISH} "Install Files into $INSTDIR"
LangString DESC_Section2 ${LANG_ENGLISH} "Install service to automatically start Looking Glass (host)."
LangString DESC_Section3 ${LANG_ENGLISH} "Create desktop shortcut icon."
LangString DESC_Section4 ${LANG_ENGLISH} "Create start menu shortcut."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Section1} $(DESC_Section1)
  !insertmacro MUI_DESCRIPTION_TEXT ${Section2} $(DESC_Section2)
  !insertmacro MUI_DESCRIPTION_TEXT ${Section3} $(DESC_Section3)
  !insertmacro MUI_DESCRIPTION_TEXT ${Section4} $(DESC_Section4)
!insertmacro MUI_FUNCTION_DESCRIPTION_END
