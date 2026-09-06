# tools/package.tcl - append the Time Actual zipfs payload onto a wrapper exe.
#
# Runs under the STATIC tclsh90s so zipfs can lmkimg-append the staged app
# payload - main.tcl (= lunar.tcl), resources/, tcl_library/, tk_library/ at
# the archive root - onto a Tk-capable wrapper, AFTER the PE image so a
# baked-in icon/manifest survives.
#
#   tclsh90s.exe tools/package.tcl [out.exe] [--wrapper W]
#
# Ported verbatim from els/tools/package.tcl (Lunar uses the same z shared
# Tcl/Tk 9 payload at <z>/.z/r/tcltk/9.0.3).

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
proc discover_tcltk {root} {
    set cands {}
    if {[info exists ::env(Z_TCLTK)] && $::env(Z_TCLTK) ne ""} { lappend cands $::env(Z_TCLTK) }
    lappend cands {*}[zmal_paths $root r tcltk 9.0.3]
    foreach p $cands {
        set p [file normalize $p]
        if {[file exists [file join $p tcl9 bin tclsh90.exe]]} { return $p }
    }
    error "z Tcl/Tk payload not found (r/tcltk/9.0.3) - restore z's runtime payloads"
}
set ROOT [script_root]
set TC   [discover_tcltk $ROOT]
proc TCp {args} { return [file join $::TC {*}$args] }

proc copy_tree {src dst} {
    file mkdir $dst
    foreach item [glob -nocomplain [file join $src *]] {
        set target [file join $dst [file tail $item]]
        if {[file isdirectory $item]} {
            copy_tree $item $target
        } else {
            file copy -force $item $target
        }
    }
}
proc zip_entries {root {rel ""}} {
    set out {}
    foreach item [glob -nocomplain [file join $root $rel *]] {
        set name [file tail $item]
        if {$rel eq ""} { set zrel $name } else { set zrel [file join $rel $name] }
        if {[file isdirectory $item]} {
            lappend out {*}[zip_entries $root $zrel]
        } else {
            lappend out $item [string map {\\ /} $zrel]
        }
    }
    return $out
}

set positional {}
set wrapperOverride ""
for {set i 0} {$i < [llength $argv]} {incr i} {
    switch -- [lindex $argv $i] {
        --wrapper  { incr i ; set wrapperOverride [lindex $argv $i] }
        default    { lappend positional [lindex $argv $i] }
    }
}
set out [lindex $positional 0]
if {$out eq ""} { set out [file join $ROOT dist TimeActual.exe] }

set wish [TCp tcl9s bin wish90s.exe]
if {![file exists $wish]} { error "static wish missing: $wish" }
if {$wrapperOverride ne ""} { set mkimgWrapper $wrapperOverride } else { set mkimgWrapper $wish }
if {[file isdirectory //zipfs:/app/tcl_library]} {
    set tclLibrary //zipfs:/app/tcl_library
} elseif {[file isdirectory [TCp tcllib tcl_library]]} {
    set tclLibrary [TCp tcllib tcl_library]
} else {
    error "tcl_library not found in //zipfs:/app or the bundle's tcllib"
}

set stage [file join $ROOT build _pkg_stage]
file delete -force $stage
file mkdir $stage

copy_tree $tclLibrary [file join $stage tcl_library]

set copiedTk 0
if {![catch {zipfs mount $wish Wt}]} {
    if {[file isdirectory //zipfs:/Wt/tk_library]} {
        copy_tree //zipfs:/Wt/tk_library [file join $stage tk_library]
        set copiedTk 1
    }
    zipfs unmount Wt
}
if {!$copiedTk && [file isdirectory [TCp tcllib tk_library]]} {
    copy_tree [TCp tcllib tk_library] [file join $stage tk_library]
    set copiedTk 1
}
if {!$copiedTk} { error "tk_library not found in wish90s.exe or the bundle's tcllib" }

file copy -force [file join $ROOT lunar.tcl] [file join $stage main.tcl]
if {[file isdirectory [file join $ROOT resources]]} {
    copy_tree [file join $ROOT resources] [file join $stage resources]
}

file delete -force $out
set entries [zip_entries $stage]
if {![llength $entries]} { error "package stage is empty: $stage" }
zipfs lmkimg $out $entries {} $mkimgWrapper
file delete -force $stage
puts "built [file nativename $out]  ([file size $out] bytes)"
