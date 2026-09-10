# tools/package.tcl - append the Time Actual zipfs payload onto a wrapper exe.
#
# Runs under the STATIC tclsh90s so zipfs can lmkimg-append the staged app
# payload - main.tcl (= lunar.tcl), resources/, tcl_library/, tk_library/ at
# the archive root - onto a Tk-capable wrapper, AFTER the PE image so a
# baked-in icon/manifest survives.
#
#   tclsh90s.exe tools/package.tcl [out.exe] [--wrapper W]
#
# The Tcl/Tk payload is built locally by kuu.exe run prereqs.

proc script_root {} {
    set s [info script]
    if {[file pathtype $s] ne "absolute"} { set s [file join [pwd] $s] }
    return [file dirname [file dirname $s]]
}
set ROOT [script_root]
set TC   [file join $ROOT .tools tcltk]
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
} elseif {[file isdirectory [TCp tcl9 lib tcl9.0]]} {
    set tclLibrary [TCp tcl9 lib tcl9.0]
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
if {!$copiedTk && [file isdirectory [TCp tcl9 lib tk9.0]]} {
    copy_tree [TCp tcl9 lib tk9.0] [file join $stage tk_library]
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
