#!/usr/bin/env tclsh
# tools/genres.tcl -- generate the native build's PE resource inputs (Time Actual).
# Ported from els/tools/genres.tcl. Emits <outdir>/lunar.exe.manifest and
# <outdir>/lunar.rc. The version comes from the top-level VERSION file (the
# same single source the C engine's build.py reads). windres compiles
# lunar.rc with --include-dir <outdir> so it finds the sibling manifest +
# lunar.ico (copied there from assets/icon.ico by task_build).
#
#   tclsh90.exe tools/genres.tcl [outdir]   (default: <root>/build)

proc script_root {} {
    set s [info script]
    if {[file pathtype $s] ne "absolute"} { set s [file join [pwd] $s] }
    return [file dirname [file dirname $s]]
}
set ROOT [script_root]
set OUT  [lindex $argv 0]
if {$OUT eq ""} { set OUT [file join $ROOT build] }
file mkdir $OUT

# --- version, straight from the VERSION file ---------------------------------
set fh [open [file join $ROOT VERSION] r] ; set ver [string trim [read $fh]] ; close $fh
if {![regexp {^[0-9][0-9.]*$} $ver]} { error "genres: bad VERSION: '$ver'" }
set parts [split $ver .]
while {[llength $parts] < 4} { lappend parts 0 }
set fv4  [join [lrange $parts 0 3] ,]     ;# FILEVERSION 0,20,1,0
set vdot [join [lrange $parts 0 3] .]     ;# manifest "version" 0.20.1.0

proc emit {path text} {
    set fh [open $path w] ; fconfigure $fh -translation lf
    puts -nonewline $fh $text ; close $fh
}

# --- application manifest -----------------------------------------------------
# system DPI-aware (Tk 9.0.3 ignores WM_DPICHANGED, so NOT PerMonitorV2),
# common-controls v6, UTF-8 code page, long-path aware.
set manifest [string map [list @VER@ $vdot] {<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"
	xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
    <assemblyIdentity version="@VER@" processorArchitecture="AMD64" name="anafalanx.timeactual" type="win32"/>
    <description>Time Actual - trustworthy network time for Windows</description>
    <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
	<security>
	    <requestedPrivileges>
		<requestedExecutionLevel level="asInvoker" uiAccess="false"/>
	    </requestedPrivileges>
	</security>
    </trustInfo>
    <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
	<application>
	    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
	    <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
	    <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
	    <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
	</application>
    </compatibility>
    <asmv3:application>
	<asmv3:windowsSettings>
	    <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
	    <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
	    <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
	</asmv3:windowsSettings>
    </asmv3:application>
    <dependency>
	<dependentAssembly>
	    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="AMD64" publicKeyToken="6595b64144ccf1df" language="*"/>
	</dependentAssembly>
    </dependency>
</assembly>
}]
emit [file join $OUT lunar.exe.manifest] $manifest

# --- resource script: icon + manifest + version info --------------------------
set rc [string map [list @FV4@ $fv4 @VER@ $ver] {#include <windows.h>

/* app icon (a single named icon group; Explorer shows the first one) */
lunar ICON "lunar.ico"

/* application manifest: CREATEPROCESS_MANIFEST_RESOURCE_ID (1), RT_MANIFEST (24) */
1 24 "lunar.exe.manifest"

VS_VERSION_INFO VERSIONINFO
  FILEVERSION    @FV4@
  PRODUCTVERSION @FV4@
  FILEFLAGSMASK  0x3fL
  FILEFLAGS      0x0L
  FILEOS         VOS_NT_WINDOWS32
  FILETYPE       VFT_APP
  FILESUBTYPE    VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName",      "anafalanx"
            VALUE "FileDescription",  "Time Actual"
            VALUE "FileVersion",      "@VER@"
            VALUE "InternalName",     "TimeActual"
            VALUE "LegalCopyright",   "Copyright (C) 2026 the Time Actual authors. MIT licensed."
            VALUE "OriginalFilename", "TimeActual.exe"
            VALUE "ProductName",      "Time Actual"
            VALUE "ProductVersion",   "@VER@"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
}]
emit [file join $OUT lunar.rc] $rc

puts "generated lunar.rc + lunar.exe.manifest (v$ver) in [file nativename $OUT]"
