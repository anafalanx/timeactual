#!/usr/bin/env tclsh
# tools/tasks.tcl - Time Actual's task runner, ported from els/tools/tasks.tcl.
# Builds the native Tcl/Tk shell exe against z's shared Tcl/Tk 9 + UCRT64
# gcc payloads. Invoked as `z <task>` (via z.json) or directly:
#   <z>/.z/r/tcltk/9.0.3/tcl9/bin/tclsh90.exe tools/tasks.tcl <task>
#
# The build links the C timing engine, Tcl/Tk shell glue, Direct2D clock
# widget, generated resources, and the statically linked Tcl/Tk runtime.

proc script_root {} {
    set s [info script]
    if {[file pathtype $s] ne "absolute"} { set s [file join [pwd] $s] }
    return [file dirname [file dirname $s]]
}
proc zmal_paths {root args} {
    set out {}
    if {[info exists ::env(Z_HOME)] && $::env(Z_HOME) ne ""} {
        lappend out [file join $::env(Z_HOME) {*}$args]
    } elseif {[info exists ::env(Z_ROOT)] && $::env(Z_ROOT) ne ""} {
        lappend out [file join $::env(Z_ROOT) .z {*}$args]
    }
    lappend out [file join [file dirname $root] .z {*}$args]
    return $out
}
proc discover_payload {root envs rel marker missingPath} {
    set candidates {}
    foreach var $envs {
        if {[info exists ::env($var)] && $::env($var) ne ""} { lappend candidates $::env($var) }
    }
    lappend candidates {*}[zmal_paths $root {*}$rel]
    foreach p $candidates {
        set p [file normalize $p]
        if {[file exists [file join $p {*}$marker]]} { return $p }
    }
    return [file normalize $missingPath]
}

set ROOT [script_root]
set TC    [discover_payload $ROOT Z_TCLTK {r tcltk 9.0.3} {tcl9 bin tclsh90.exe} \
              [file join [file dirname $ROOT] .z r tcltk 9.0.3]]
set MSYS2 [discover_payload $ROOT Z_MSYS2 {r msys2} {ucrt64 bin gcc.exe} \
              [file join [file dirname $ROOT] .z r msys2]]
set TWAPI [discover_payload $ROOT Z_TWAPI {r twapi 5.2.0} {pkgIndex.tcl} \
              [file join [file dirname $ROOT] .z r twapi 5.2.0]]
set ::env(Z_TCLTK) [file nativename $TC]
set ::env(Z_MSYS2) [file nativename $MSYS2]
set ::env(Z_TWAPI) [file nativename $TWAPI]

foreach {var rel marker} {
    TCL_LIBRARY {tcllib tcl_library} init.tcl
    TK_LIBRARY  {tcllib tk_library}  tk.tcl
} {
    set p [file join $TC {*}$rel]
    if {[file exists [file join $p $marker]]} { set ::env($var) [file nativename $p] }
}

# Make twapi (and build/, for cap.dll's pkgIndex) resolvable by the screenshot
# tool: prepend to both TCLLIBPATH (for child tclsh) and this interp's auto_path.
set pkgpaths {}
foreach p [list $TWAPI [file join $ROOT build]] {
    if {[file isdirectory $p]} { lappend pkgpaths $p }
}
if {[llength $pkgpaths]} {
    if {[info exists ::env(TCLLIBPATH)] && $::env(TCLLIBPATH) ne ""} {
        set ::env(TCLLIBPATH) [concat $pkgpaths $::env(TCLLIBPATH)]
    } else {
        set ::env(TCLLIBPATH) $pkgpaths
    }
    set auto_path [concat $pkgpaths $auto_path]
}

# z runtime wins on PATH: Tcl/Tk 9 BEFORE MSYS2 (which ships its own 8.6).
set vbins {}
foreach b [list [file join $TC tcl9 bin] [file join $MSYS2 ucrt64 bin] [file join $MSYS2 usr bin]] {
    if {[file isdirectory $b]} { lappend vbins [file nativename $b] }
}
if {[llength $vbins]} { set ::env(PATH) "[join $vbins {;}];$::env(PATH)" }
if {![info exists ::env(MSYSTEM)]} { set ::env(MSYSTEM) UCRT64 }

# ---- path helpers -------------------------------------------------------
proc P      {args} { return [file join $::ROOT  {*}$args] }
proc TCp    {args} { return [file join $::TC    {*}$args] }
proc MSYSp  {args} { return [file join $::MSYS2 {*}$args] }
proc TWAPIp {args} { return [file join $::TWAPI {*}$args] }
proc tclsh    {} { return [TCp tcl9 bin tclsh90.exe] }
proc wish     {} { return [TCp tcl9 bin wish90.exe] }
proc tclshs   {} { return [TCp tcl9s bin tclsh90s.exe] }
proc gcc      {} { return [MSYSp ucrt64 bin gcc.exe] }
proc windres  {} { return [MSYSp ucrt64 bin windres.exe] }
proc strip-exe {} { return [MSYSp ucrt64 bin strip.exe] }

proc stream {args} {
    if {[catch {exec {*}$args >@ stdout 2>@ stderr} err opts]} {
        if {[lindex [dict get $opts -errorcode] 0] eq "CHILDSTATUS"} {
            set code [lindex [dict get $opts -errorcode] 2]
            return -code error -errorcode [list STREAM CHILD $code] \
                "child exited with status $code"
        }
        return -options $opts $err
    }
}
proc run_capture {args} {
    set ch [file tempfile tmp]
    set rc [catch {exec {*}$args >@ $ch 2>@ $ch} err opts]
    close $ch
    set f [open $tmp r] ; set text [read $f] ; close $f
    file delete -force $tmp
    if {$rc} {
        set ec [dict get $opts -errorcode]
        if {[lindex $ec 0] eq "CHILDSTATUS"} { return [list [lindex $ec 2] $text] }
        return [list 1 [string trim "$text\n$err"]]
    }
    return [list 0 $text]
}
proc need {args} {
    foreach tool $args {
        set p ""
        switch $tool {
            gcc     { set p [gcc] }
            tclsh   { set p [tclsh] }
            tclshs  { set p [tclshs] }
            windres { set p [windres] }
        }
        if {$p eq "" || ![file exists $p]} {
            error "required tool '$tool' is missing - restore z's runtime payloads (r/tcltk, r/msys2)"
        }
    }
}

# ---- tasks --------------------------------------------------------------
proc task_help {args} {
    puts {Time Actual task runner
  z build [out]   build the native Tcl/Tk shell exe -> dist/TimeActual.exe
  z check [exe]   run the exe's --selftest and report pass/fail
  z run           launch the shell under wish (dev, no build)
  z sign [exe]    code-sign the release exe (Certum/SimplySign) + verify
  z icon [--face #hex] [--ink #hex] [--accent #hex]
                  render assets/icon.ico + icon.png from tools/icon.tcl
  z env           print resolved toolchain paths}
}

# icon -- regenerate the application icon from tools/icon.tcl (palette and
# geometry live at the top of that file; flags override the palette).
proc task_icon {args} {
    need tclsh
    stream [tclsh] [P tools icon.tcl] --out [P assets] --preview [P build icon] {*}$args
}

proc task_env {args} {
    puts "ROOT  = $::ROOT"
    puts "TCLTK = $::TC"
    puts "MSYS2 = $::MSYS2"
    foreach {label path} [list tclsh [tclsh] tclshs [tclshs] gcc [gcc]] {
        puts [format "  %-7s %s  (%s)" $label $path \
            [expr {[file exists $path] ? "ok" : "MISSING"}]]
    }
    catch {puts "  gcc     [exec [gcc] -dumpversion]"}
}

proc task_run {args} {
    exec [wish] [P lunar.tcl] {*}$args &
    puts "launched lunar.tcl under wish"
}

# Time Actual system libraries. els's 15 (the wish90s import table) plus the
# engine's crypto/net imports (crypt32/bcrypt/wtsapi32); ws2_32 is already
# in els's set. Unused imports are harmless while the engine isn't linked.
set ::SYSLIBS {
    -lnetapi32 -lkernel32 -luser32 -ladvapi32 -luserenv -lws2_32
    -lgdi32 -lcomdlg32 -limm32 -lcomctl32 -lshell32 -luuid -lole32
    -loleaut32 -lwinspool -lcrypt32 -lbcrypt -lwtsapi32 -lwinmm -ld2d1 -ldwrite
}

# The C engine (every engine src/*.c). The Tk shell glue -- entry point
# lunar_main.c and the ::lunar command wrapper lunarx.c -- is compiled
# separately below.
set ::ENGINE_SRCS {
    app_paths sysvol netutil ntp clock logbuf tz tzif tz_embed tz_winmap
    tz_winmap_gen siv nts_ke nts_ef pinned_tls cert_verify_win pin_store
    update_check nts dns h2
}

# engine_prereqs -- reuse build.py's cached mbedTLS archive + version.h
# (built with the same UCRT64 gcc 16.1, so ABI-compatible with our link).
# Returns the archive path.
proc engine_prereqs {} {
    set py [MSYSp ucrt64 bin python.exe]
    if {![file exists $py]} { error "python not found: $py" }
    set sp [string map {\\ /} [P scripts]]
    set code "import sys; sys.path.insert(0, r'$sp'); import build; g=build.find_tool('gcc'); build.write_version_header(build.BUILD, build.read_version()); print(build.build_mbedtls_archive(g))"
    set out [exec $py -c $code]
    set arch [string map {\\ /} [string trim [lindex [split [string trim $out] \n] end]]]
    if {![file exists $arch]} { error "mbedTLS archive not produced (got '$arch')\n$out" }
    return $arch
}

proc task_build {args} {
    need gcc tclsh tclshs windres
    set out [lindex $args 0] ; if {$out eq ""} { set out [P dist TimeActual.exe] }
    if {[string match -* $out]} { error "z build takes no flags; usage: z build ?outfile?" }
    set inc  [TCp tcl9 include]
    set libd [TCp tcl9s lib]
    set tp   [P third_party]
    file mkdir [P build]

    puts "prereqs: version.h + mbedTLS archive (via build.py)"
    set mbedtls [engine_prereqs]

    puts "gen  build/lunar.rc + lunar.exe.manifest + lunar.ico"
    file copy -force [P assets icon.ico] [P build lunar.ico]
    stream [tclsh] [P tools genres.tcl] [P build]
    puts "windres build/lunar.rc -> build/lunar.res"
    stream [windres] --include-dir [P build] --include-dir $inc \
        [P build lunar.rc] -O coff -o [P build lunar.res]

    # LUNAR_DEBUG=1 -> -g + no strip, for gdb backtraces.
    set dbg [expr {[info exists ::env(LUNAR_DEBUG)] && $::env(LUNAR_DEBUG) ne ""}]
    set gf [expr {$dbg ? "-g" : ""}]

    # Engine include/config flags (mirror build.py's app compile).
    set eflags [list -std=c23 -O2 -ffunction-sections -fdata-sections \
        -I[file join $tp mbedtls-3.6.6 include] -I$tp -I[P build] \
        -DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>]
    if {$dbg} { lappend eflags -g }

    set objs {}
    puts "cc  engine ([llength $::ENGINE_SRCS] units)"
    foreach name $::ENGINE_SRCS {
        set o [P build $name.o]
        stream [gcc] {*}$eflags -c [P src $name.c] -o $o
        lappend objs $o
    }

    puts "cc  src/lunarx.c"
    stream [gcc] -std=c23 -O2 {*}$gf -DSTATIC_BUILD=1 -ffunction-sections -fdata-sections \
        -I$inc -I[P src] -I[P build] -c [P src lunarx.c] -o [P build lunarx.o]

    puts "cc  src/lunarclock.c"
    stream [gcc] -std=c23 -O2 {*}$gf -DSTATIC_BUILD=1 -ffunction-sections -fdata-sections \
        -I$inc -I[P src] -I[P build] -c [P src lunarclock.c] -o [P build lunarclock.o]

    puts "cc  src/lunar_main.c"
    stream [gcc] -std=c23 -O2 {*}$gf -municode -DUNICODE -D_UNICODE -DSTATIC_BUILD=1 \
        -DLUNAR_STATIC_LUNARX -ffunction-sections -fdata-sections \
        -c [P src lunar_main.c] -o [P build lunar_main.o] -I$inc

    puts "ld  -> build/lunar-bare.exe"
    set bare [P build lunar-bare.exe]
    stream [gcc] -municode -mwindows -static-libgcc -Wl,--gc-sections \
        [P build lunar_main.o] [P build lunarx.o] [P build lunarclock.o] {*}$objs [P build lunar.res] \
        [file join $libd libtcl9tk90.a] [file join $libd libtcl90.a] \
        [file join $libd libtclstub.a] $mbedtls {*}$::SYSLIBS -o $bare
    if {!$dbg} { catch {stream [strip-exe] $bare} }

    file mkdir [file dirname $out]
    set staged "$out.new"
    stream [tclshs] [P tools package.tcl] --wrapper $bare $staged
    catch {file delete -force "$out.old"}
    if {[catch {file rename -force $staged $out}]} {
        if {[catch {
            file rename -force $out "$out.old"
            file rename -force $staged $out
        } e]} {
            catch {file delete -force $staged}
            error "cannot place $out (locked?): $e"
        }
        puts "note: $out was in use; parked the running copy as [file tail $out.old]"
    }
    puts "placed $out ([file size $out] bytes)"
}

# z repackage -- re-append the zipfs (main.tcl/resources) onto the EXISTING
# build/lunar-bare.exe, without recompiling. Fast iteration on lunar.tcl.
proc task_repackage {args} {
    need tclshs
    set bare [P build lunar-bare.exe]
    if {![file exists $bare]} { error "no build/lunar-bare.exe; run z build first" }
    set out [P dist TimeActual.exe]
    set staged "$out.new"
    stream [tclshs] [P tools package.tcl] --wrapper $bare $staged
    catch {file delete -force "$out.old"}
    if {[catch {file rename -force $staged $out}]} {
        catch {file rename -force $out "$out.old"}
        file rename -force $staged $out
    }
    puts "repackaged $out ([file size $out] bytes)"
}

proc task_check {args} {
    set exe [lindex $args 0] ; if {$exe eq ""} { set exe [P dist TimeActual.exe] }
    if {![file exists $exe]} { error "not found: $exe (run z build first)" }
    set report [file join [file dirname $exe] timeactual-selftest.txt]
    file delete -force $report
    # The selftest fires a chime, arms a marker and ingests events: run it
    # against a scratch data dir so none of that lands in the user's real
    # %APPDATA%\TimeActual (events.log, settings, pin store).
    set saveEnv [expr {[info exists ::env(LUNAR_DATA_DIR)] ? $::env(LUNAR_DATA_DIR) : ""}]
    set tmp [file join $::env(TEMP) "lunar-check-[pid]"]
    file mkdir $tmp
    set ::env(LUNAR_DATA_DIR) $tmp
    lassign [run_capture $exe --selftest $report] rc out
    # GUI subsystem: the exe detaches; poll briefly for the report file.
    for {set i 0} {$i < 50 && ![file exists $report]} {incr i} { after 100 }
    if {$saveEnv ne ""} { set ::env(LUNAR_DATA_DIR) $saveEnv } else { unset -nocomplain ::env(LUNAR_DATA_DIR) }
    catch { file delete -force $tmp }
    if {![file exists $report]} { error "selftest produced no report (exe failed to run?)\n$out" }
    set fh [open $report r] ; set txt [read $fh] ; close $fh
    puts $txt
    if {[string match "*status=ok*" $txt]} {
        puts "CHECK OK"
    } else {
        error "CHECK FAILED"
    }
}

proc find_signtool {} {
    set sdk [lsort [glob -nocomplain {C:/Program Files (x86)/Windows Kits/10/bin/*/x64/signtool.exe}]]
    if {[llength $sdk]} { return [lindex $sdk end] }
    set zhome [expr {[info exists ::env(Z_HOME)] && $::env(Z_HOME) ne "" ? $::env(Z_HOME) : [file join [file dirname $::ROOT] .z]}]
    set j [file join $zhome r winsdk 10.0.26100.0 signtool.exe]
    if {[file exists $j]} { return $j }
    set p [auto_execok signtool]
    if {[llength $p]} { return [lindex $p 0] }
    error "signtool.exe not found - install the Windows SDK Signing Tools"
}
proc task_sign {args} {
    set signtool [find_signtool]
    set exe [lindex $args 0] ; if {$exe eq ""} { set exe [P dist TimeActual.exe] }
    if {![file exists $exe]} { error "file not found: $exe" }
    set exe [file normalize $exe]
    puts "file:     $exe"
    puts "signtool: $signtool"
    puts "signing (SimplySign Desktop must be connected)..."
    lassign [run_capture $signtool sign /a /tr http://time.certum.pl /td sha256 /fd sha256 /v $exe] rc out
    puts [string trim $out]
    if {$rc != 0} {
        if {[string match {*No certificates were found*} $out]} {
            error "no code-signing certificate - SimplySign is not connected. Refusing to emit an unsigned binary."
        }
        error "signtool sign failed (exit $rc)"
    }
    lassign [run_capture $signtool verify /pa /all /v $exe] vrc vout
    if {$vrc != 0} { error "signature verify FAILED:\n$vout" }
    if {![string match {*timestamp*} $vout]} { error "signature NOT timestamped - aborting." }
    lassign [run_capture certutil -hashfile $exe SHA256] hrc hout
    set sum ""
    foreach line [split $hout \n] {
        set t [string map {" " ""} [string trim $line]]
        if {[regexp {^[0-9a-fA-F]{64}$} $t]} { set sum [string tolower $t] ; break }
    }
    puts "\nOK - signed, verified, timestamped."
    if {$sum ne ""} { puts "  sha256  $sum" }
}

proc task_build-ext {args} {
    need gcc tclsh
    set inc [TCp tcl9 include]
    set lib [TCp tcl9 lib]
    file mkdir [P build]
    puts "cc  src/cap.c -> build/cap.dll"
    stream [gcc] -std=c23 -O2 -Wall -shared -DUSE_TCL_STUBS \
        -I$inc [P src cap.c] -o [P build cap.dll] -L$lib -ltclstub -static-libgcc \
        -luser32 -lgdi32
    set idx [open [P build pkgIndex.tcl] w]
    puts $idx "package ifneeded lunarcap 0.1 \[list load \[file join \$dir cap.dll\] Cap\]"
    close $idx
    puts "built build/cap.dll"
}

# z shot <out.png> [--dev]
#   default: screenshot the built dist/TimeActual.exe (single-exe mode)
#   --dev  : screenshot wish + lunar.tcl (fast UI iteration, no build)
proc task_shot {args} {
    need gcc tclsh
    if {[lindex $args 0] eq "--selftest"} { stream [tclsh] [P tools shot.tcl] --selftest ; return }
    if {![llength $args]} { error "usage: z shot <out.png> \[--dev]" }
    if {![file exists [P build cap.dll]]} { puts "building capture extension..." ; task_build-ext }
    set out [lindex $args 0]
    if {[lsearch -exact $args --dev] >= 0} {
        stream [tclsh] [P tools shot.tcl] [wish] [P lunar.tcl] $out
    } else {
        set exe [P dist TimeActual.exe]
        if {![file exists $exe]} { error "not found: $exe (run z build first, or: z shot <out> --dev)" }
        stream [tclsh] [P tools shot.tcl] $exe - $out
    }
}

# uishot -- capture the deterministic (stubbed-engine) analog UI for review.
proc task_uishot {args} {
    need gcc tclsh
    if {[llength $args] < 1 || [llength $args] > 2} {
        error "usage: z uishot <out.png> ?trusted|degraded|wide|stopped|acquiring|settings|eventlog|eventlog-filtered|eventlog-sorted?\n(dialog stages need LUNAR_SHOT_TITLE: \"Time Actual Settings\" / \"Time Actual — Event Log\")"
    }
    if {![file exists [P build cap.dll]]} { puts "building capture extension..." ; task_build-ext }
    stream [tclsh] [P tools shot.tcl] [wish] [P tools uishot.tcl] {*}$args
}

# ---- dispatch -----------------------------------------------------------
set cmd [lindex $argv 0]
if {$cmd eq ""} { set cmd help }
set proc "task_$cmd"
if {[llength [info commands $proc]] == 0} {
    puts stderr "lunar tasks: unknown command '$cmd' (try: z help)"
    exit 2
}
if {[catch {$proc {*}[lrange $argv 1 end]} err opts]} {
    set ec [dict get $opts -errorcode]
    if {[lindex $ec 0] eq "STREAM" && [lindex $ec 1] eq "CHILD"} { exit [lindex $ec 2] }
    puts stderr "z $cmd: $err"
    exit 1
}
