#!/usr/bin/env tclsh
# tools/shot.tcl -- launch a Tk app and screenshot ITS window to PNG, robustly.
#
# Window-finding via twapi (by PID); the capture is the cap extension's
# PrintWindow (occlusion-proof) -> a DIB, converted to PNG here. No clipboard,
# no foreground, no full-screen crop -- it grabs only the one window, even if
# covered or in the background. Ported from els/tools/shot.tcl.
#
#   tclsh90.exe tools/shot.tcl <wish.exe> <lunar.tcl> <out.png> [file ...]
#   tclsh90.exe tools/shot.tcl <TimeActual.exe> - <out.png> [file ...]   # single-exe
#   tclsh90.exe tools/shot.tcl --selftest        ;# headless converter checks
#
# Set LUNAR_SHOT_TITLE to capture a specific toplevel/dialog by title.
# Requires build/cap.dll (`z build-ext`). twapi is provided on auto_path by
# tools/tasks.tcl (or via the Z_TWAPI env var when run directly).

package require Tk
wm withdraw .

proc script_root {} {
    set s [info script]
    if {[file pathtype $s] ne "absolute"} { set s [file join [pwd] $s] }
    return [file dirname [file dirname $s]]
}
set ::SHOT_ROOT [script_root]
# twapi is normally on auto_path already (tasks.tcl exports TCLLIBPATH). When
# shot.tcl is run standalone, honor Z_TWAPI.
if {[info exists ::env(Z_TWAPI)] && $::env(Z_TWAPI) ne ""} {
    lappend auto_path $::env(Z_TWAPI)
}

# ---- DIB (BITMAPINFOHEADER) -> Tk photo ---------------------------------
proc dib_to_photo {dib} {
    binary scan $dib iiissiiiiii \
        biSize biWidth biHeight biPlanes biBitCount biCompression \
        biSizeImage bppmX bppmY biClrUsed biClrImportant
    set width  $biWidth
    set height [expr {abs($biHeight)}]
    set topDown [expr {$biHeight < 0}]
    set bpp [expr {$biBitCount & 0xffff}]
    if {$bpp != 24 && $bpp != 32} {
        error "unsupported DIB bit depth: $bpp (need 24 or 32)"
    }
    if {$biCompression != 0 && $biCompression != 3} {
        error "unsupported DIB compression: $biCompression (need BI_RGB or BI_BITFIELDS)"
    }
    set bytesPP [expr {$bpp / 8}]
    set rowStride [expr {(($width * $bpp + 31) / 32) * 4}]
    set maskBytes [expr {$biCompression == 3 ? 12 : 0}]
    set pixelStart [expr {$biSize + ($biClrUsed * 4) + $maskBytes}]

    set rows [list "P6\n$width $height\n255\n"]
    set rowLen [expr {$width * $bytesPP}]
    for {set oy 0} {$oy < $height} {incr oy} {
        set sy [expr {$topDown ? $oy : $height - 1 - $oy}]
        set off [expr {$pixelStart + $sy * $rowStride}]
        binary scan [string range $dib $off [expr {$off + $rowLen - 1}]] cu* bs
        set rgb {}
        if {$bytesPP == 4} {
            foreach {b g r a} $bs { lappend rgb $r $g $b }
        } else {
            foreach {b g r} $bs { lappend rgb $r $g $b }
        }
        lappend rows [binary format c* $rgb]
    }
    set ppm [join $rows ""]

    set tmp [file join [::shot_tmpdir] _shot_[pid].ppm]
    set fh [::open $tmp w]
    try {
        fconfigure $fh -translation binary
        puts -nonewline $fh $ppm
    } finally {
        close $fh
    }
    try {
        set img [image create photo -file $tmp]
    } finally {
        file delete -force $tmp
    }
    return $img
}

proc ::shot_tmpdir {} {
    set d [file join $::SHOT_ROOT build]
    file mkdir $d
    return $d
}

# ---- live capture -------------------------------------------------------
proc window_for_pid {pid timeoutMs {title ""}} {
    set deadline [expr {[clock milliseconds] + $timeoutMs}]
    while {[clock milliseconds] < $deadline} {
        foreach hwin [twapi::find_windows -toplevel 1 -visible 1] {
            if {[catch {twapi::get_window_process $hwin} wp]} { continue }
            if {$wp != $pid} { continue }
            if {$title eq ""} { return $hwin }
            if {[catch {twapi::get_window_text $hwin} wt]} { continue }
            if {$wt eq $title || [string match $title $wt]} { return $hwin }
        }
        after 120
    }
    return ""
}

# A twapi handle is a {address TYPE} list; lunarcap::window wants the integer.
proc hwnd_int {h} {
    set a [lindex $h 0]
    if {[regexp {(0x[0-9a-fA-F]+|[0-9]+)} $a -> n]} { return $n }
    return $a
}

proc main {argv} {
    if {[lindex $argv 0] eq "--selftest"} { selftest ; return }
    package require twapi
    load [file join $::SHOT_ROOT build cap.dll] Cap

    lassign $argv app script out
    set files [lrange $argv 3 end]
    if {$app eq "" || $script eq "" || $out eq ""} {
        puts stderr "usage: shot.tcl <wish.exe> <lunar.tcl> <out.png> \[file ...\]"
        puts stderr "       shot.tcl <TimeActual.exe> - <out.png> \[file ...\]   ;# single-exe"
        exit 2
    }
    set ::env(LUNAR_NO_SINGLE_INSTANCE) 1
    if {$script eq "-"} {
        set pid [exec $app {*}$files &]            ;# self-contained TimeActual.exe
    } else {
        set pid [exec $app $script {*}$files &]    ;# wish + lunar.tcl
    }
    set title [expr {[info exists ::env(LUNAR_SHOT_TITLE)] ? $::env(LUNAR_SHOT_TITLE) : ""}]
    set hwin [window_for_pid $pid 12000 $title]
    if {$hwin eq ""} {
        catch {twapi::end_process $pid -force}
        puts stderr "window for pid $pid never appeared"
        exit 3
    }
    # let Tk finish painting; LUNAR_SHOT_SETTLE_MS can extend this so the
    # engine has time to reach a synced state before the frame is grabbed.
    set settle [expr {[info exists ::env(LUNAR_SHOT_SETTLE_MS)] ? $::env(LUNAR_SHOT_SETTLE_MS) : 700}]
    after $settle
    try {
        set img [dib_to_photo [lunarcap::window [hwnd_int $hwin]]]
        $img write $out -format png
        puts "wrote $out ([image width $img]x[image height $img])"
    } finally {
        catch {twapi::send_message $hwin 0x10 0 0}   ;# WM_CLOSE
        after 400
        catch {twapi::end_process $pid -force}
    }
}

# ---- headless converter self-test ---------------------------------------
proc make_dib {width height bpp pixels} {
    set bytesPP [expr {$bpp / 8}]
    set rowStride [expr {(($width * $bpp + 31) / 32) * 4}]
    set pad [expr {$rowStride - $width * $bytesPP}]
    set hdr [binary format iiissiiiiii 40 $width $height 1 $bpp 0 0 2835 2835 0 0]
    set body ""
    for {set y [expr {$height - 1}]} {$y >= 0} {incr y -1} {
        for {set x 0} {$x < $width} {incr x} {
            set i [expr {($y * $width + $x) * $bytesPP}]
            append body [binary format c* [lrange $pixels $i [expr {$i + $bytesPP - 1}]]]
        }
        if {$pad > 0} { append body [binary format x$pad] }
    }
    return $hdr$body
}

proc selftest {} {
    set fails 0
    set px {
        0 0 255 0   0 255 0 0   255 0 0 0
        255 255 255 0   0 0 0 0   128 128 128 0
    }
    set dib [make_dib 3 2 32 $px]
    set img [dib_to_photo $dib]
    foreach {x y want} {0 0 {255 0 0}  1 0 {0 255 0}  2 0 {0 0 255}
                        0 1 {255 255 255}  1 1 {0 0 0}  2 1 {128 128 128}} {
        set got [lrange [$img get $x $y] 0 2]
        if {$got ne $want} { puts "FAIL 32bpp ($x,$y): got {$got} want {$want}"; incr fails }
    }
    if {[image width $img] != 3 || [image height $img] != 2} { incr fails }
    image delete $img
    if {$fails == 0} { puts "shot.tcl converter selftest: OK" ; exit 0 }
    puts "shot.tcl converter selftest: $fails FAILURE(S)" ; exit 1
}

if {[catch {main $argv} err]} {
    puts stderr $err
    exit 1
}
exit 0
