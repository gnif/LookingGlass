/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

!define MUI_CUSTOMFUNCTION_GUIINIT customGUIInit

;Include
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"

;Settings
Name "Looking Glass (IDD)"
OutFile "looking-glass-idd-setup.exe"
Unicode true
RequestExecutionLevel admin
ShowInstDetails "show"
ShowUninstDetails "show"
ManifestDPIAware true

!ifndef BUILD_32BIT
Target AMD64-Unicode
InstallDir "$PROGRAMFILES\Looking Glass (IDD)"
!else
!include "x64.nsh"
InstallDir "$PROGRAMFILES64\Looking Glass (IDD)"
!endif

!define MUI_ICON "icon.ico"
!define MUI_UNICON "icon.ico"
!define MUI_LICENSEPAGE_BUTTON "Agree"
!define MUI_BGCOLOR "3c046c"
!define MUI_TEXTCOLOR "ffffff"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${NSISDIR}\Contrib\Graphics\Wizard\nsis3-grey.bmp"
!define /file VERSION "VERSION"

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
!ifdef IVSHMEM
  !define line5 "/ivshmem - install the IVSHMEM driver$\r$\n"
!else
  !define line5 ""
!endif
  MessageBox MB_OK "${line1}${line2}${line3}${line4}${line5}"
  Abort
FunctionEnd

Function .onInit
  SetShellVarContext all

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

!ifdef IVSHMEM
  Var /GlOBAL option_ivshmem
  StrCpy $option_ivshmem 0
!endif

  Push $R0

!ifdef IVSHMEM
  ${GetOptions} $cmdLineParams '/ivshmem' $R0
  IfErrors +2 0
  StrCpy $option_ivshmem 1
!endif

  Pop $R0

FunctionEnd

!macro StopLGIddHelper
  ;Attempt to stop existing LG service only if it exists

  nsExec::Exec 'sc.exe query LGIddHelper'
  Pop $0 ; SC.exe error level

  ${If} $0 == 0 ; If error level is 0, service exists
    DetailPrint "Stop service: LGIddHelper"
    nsExec::ExecToLog 'net.exe STOP LGIddHelper'
  ${EndIf}
!macroend

;Install 
!ifdef IVSHMEM
Section "IVSHMEM Driver" Section0
  StrCpy $option_ivshmem 1
SectionEnd

Section "-IVSHMEM Driver"
  ${If} $option_ivshmem == 1
    DetailPrint "Extracting IVSHMEM driver"
    SetOutPath $INSTDIR
    File ..\ivshmem\ivshmem.cat
    File ..\ivshmem\ivshmem.inf
    File ..\ivshmem\ivshmem.sys
    File /nonfatal ..\ivshmem\ivshmem.pdb

    DetailPrint "Installing IVSHMEM driver"
!ifdef BUILD_32BIT
    ${DisableX64FSRedirection}
!endif
    nsExec::ExecToLog '"$SYSDIR\pnputil.exe" /add-driver "$INSTDIR\ivshmem.inf" /install'
!ifdef BUILD_32BIT
    ${EnableX64FSRedirection}
!endif
  ${EndIf}
SectionEnd
!endif

Section /o "" Section2
  DetailPrint "Disabling the old Looking Glass host application..."
  nsExec::Exec 'net.exe STOP "Looking Glass (host)"'
  nsExec::Exec 'sc.exe config "Looking Glass (host)" start=disabled'
SectionEnd

Section "!Indirect Display Driver (IDD)" Section1
  SectionIn RO

  DetailPrint "Creating log directory"
  CreateDirectory "$APPDATA\Looking Glass (IDD)"

  DetailPrint "Extracting IDD"
  SetOutPath $INSTDIR
  File lgidd.cat
  File LGIdd.dll
  File LGIdd.inf
  File LGIddHelper.exe
  File LGIddInstall.exe

  DetailPrint "Extracting support files"
  File LICENSE.txt
  WriteUninstaller $INSTDIR\uninstaller.exe

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "EstimatedSize" "$0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "DisplayName" "Looking Glass (IDD)"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "UninstallString" "$\"$INSTDIR\uninstaller.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "QuietUninstallString" "$\"$INSTDIR\uninstaller.exe$\" /S"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "Publisher" "Geoffrey McRae"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "DisplayIcon" "$\"$INSTDIR\LGIddHelper.exe$\""
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "NoRepair" "1"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "NoModify" "1"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)" \
  "DisplayVersion" ${VERSION}

  !insertmacro StopLGIddHelper

  DetailPrint "Installing IDD"
  nsExec::ExecToLog '"$INSTDIR\LGIddInstall.exe" install'

  Pop $0
  ${If} $0 == 12
    DetailPrint "Restart is required to complete driver install."
  ${EndIf}

SectionEnd

Section "Uninstall" Section6
  !insertmacro StopLGIddHelper

  DetailPrint "Uninstalling IDD"
  nsExec::ExecToLog '"$INSTDIR\LGIddInstall.exe" uninstall'

  DetailPrint "Clean up helper service"
  nsExec::Exec 'sc.exe delete LGIddHelper'

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Looking Glass (IDD)"
  Delete "$INSTDIR\uninstaller.exe"
  Delete "$INSTDIR\ivshmem.cat"
  Delete "$INSTDIR\ivshmem.inf"
  Delete "$INSTDIR\ivshmem.sys"
  Delete "$INSTDIR\ivshmem.pdb"
  Delete "$INSTDIR\lgidd.cat"
  Delete "$INSTDIR\LGIdd.dll"
  Delete "$INSTDIR\LGIdd.inf"
  Delete "$INSTDIR\LGIddHelper.exe"
  Delete "$INSTDIR\LGIddInstall.exe"
  Delete "$INSTDIR\LICENSE.txt"

  RMDir $INSTDIR
SectionEnd

Function customGUIInit
  nsExec::Exec 'sc.exe query "Looking Glass (host)"'
  Pop $0 ; SC.exe error level

  ${If} $0 == 0 ; If error level is 0, service exists
    SectionSetText  ${Section2} "Disable old host app"
    SectionSetFlags ${Section2} ${SF_SELECTED}
  ${EndIf}
FunctionEnd

;Description text for selection of install items
LangString DESC_Section0 ${LANG_ENGLISH} "Install the IVSHMEM driver. This driver is needed for Looking Glass to function. This will replace the driver if it is already installed."
LangString DESC_Section1 ${LANG_ENGLISH} "Install the Indirect Display Driver (IDD)"
LangString DESC_Section2 ${LANG_ENGLISH} "Disables the old Looking Glass host application. You can re-enable the service if you want to use it again, or uninstall it later."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
!ifdef IVSHMEM
  !insertmacro MUI_DESCRIPTION_TEXT ${Section0} $(DESC_Section0)
!endif
!insertmacro MUI_DESCRIPTION_TEXT ${Section1} $(DESC_Section1)
!insertmacro MUI_DESCRIPTION_TEXT ${Section2} $(DESC_Section2)
!insertmacro MUI_FUNCTION_DESCRIPTION_END
