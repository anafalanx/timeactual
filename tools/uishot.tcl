# tools/uishot.tcl -- render a deterministic (stubbed-engine) Time Actual UI state
# for screenshot-based UI review, so the layout can be evaluated without a live
# network sync. Driven by shot.tcl, which launches wish on this script:
#
#   kuu.exe run uishot <out.png> ?state?
#   tools/shot.tcl <wish> tools/uishot.tcl <out.png> ?state?
#
# ?state?: trusted (default) | degraded | wide | stopped | acquiring --
# stages the uncertainty-fan second hand at a representative bound. The
# display has exactly two states (time-with-fan, always signature red;
# or a bare dial):
#   trusted   ok        ±207 ms  (near-hairline fan)
#   degraded  holdover  ±300 ms  (slightly wider fan; legacy stage name)
#   wide      holdover  ±2.5 s   (fan visibly open)
#   stopped   holdover  ±6.5 s   (> default 5 s ceiling: no time shown)
#   acquiring inop      no time  (bare dial; word only in the status bar)
# Dialog stages (set LUNAR_SHOT_TITLE to the window title to capture):
#   settings          -- "Time Actual Settings", Clock tab
#   eventlog          -- "Time Actual — Event Log" over a seeded representative store
#   eventlog-filtered -- the log with search "pin" + level Warn+
#   eventlog-sorted   -- the log sorted by Category
#
# It sources lunar.tcl (which builds the real UI via lunar::main), then replaces
# the engine commands with fixed sample data so every capture is identical.
# Modelled on els's tools/readme_shots.tcl staged-scene approach.

set ::LUNAR_ROOT [file dirname [file dirname [file normalize [info script]]]]
set ::UISHOT_STATE [expr {[llength $::argv] ? [lindex $::argv 0] : "trusted"}]
set ::argv {} ; set ::argc 0

# Isolate BEFORE sourcing: lunar::main persists real events (the session
# marker, canvas-fallback notes) the moment it runs -- without this, every
# staging run would append fake rows to the user's %APPDATA% events.log
# and slurp their real history into the staged captures.
if {![info exists ::env(LUNAR_DATA_DIR)] || $::env(LUNAR_DATA_DIR) eq ""} {
    set ::env(LUNAR_DATA_DIR) [file join $::env(TEMP) "lunar-uishot-[pid]"]
}

source [file join $::LUNAR_ROOT lunar.tcl]   ;# runs lunar::main -> builds the UI

# --- deterministic engine stubs (no network) --------------------------------
switch -- $::UISHOT_STATE {
    degraded  { set ::UISHOT_TRUST holdover ; set ::UISHOT_BOUND 300 ; set ::UISHOT_HASTIME 1 }
    wide      { set ::UISHOT_TRUST holdover ; set ::UISHOT_BOUND 2500 ; set ::UISHOT_HASTIME 1 }
    stopped   { set ::UISHOT_TRUST holdover ; set ::UISHOT_BOUND 6500 ; set ::UISHOT_HASTIME 1 }
    acquiring { set ::UISHOT_TRUST inop     ; set ::UISHOT_BOUND 0    ; set ::UISHOT_HASTIME 0 }
    eventlog -
    eventlog-filtered -
    eventlog-sorted -
    settings {
        set ::UISHOT_TRUST ok ; set ::UISHOT_BOUND 207 ; set ::UISHOT_HASTIME 1
    }
    default   { set ::UISHOT_TRUST ok       ; set ::UISHOT_BOUND 207  ; set ::UISHOT_HASTIME 1 }
}
proc ::lunar::status {} {
    return [dict create state $::UISHOT_TRUST \
        synced $::UISHOT_HASTIME hasTime $::UISHOT_HASTIME \
        utcMs 1751731872000 boundMs $::UISHOT_BOUND \
        sysDeltaValid $::UISHOT_HASTIME sysDeltaMs 420]
}
proc ::lunar::localtime {ms zone} {
    return [dict create hour 17 minute 51 second 12 \
        wday 0 day 5 month 7 year 2026 offSec 7200 abbr CEST]
}
# Render immediately with the stubs; twice, so the stop rule's two-read
# debounce settles when staging the stopped state.
after 150 {
    set ::lunar::tz "Europe/Brussels"
    catch { lunar::render [::lunar::status] }
    catch { lunar::render [::lunar::status] }
    if {[string match "eventlog*" $::UISHOT_STATE]} { uishot_stage_eventlog }
    if {$::UISHOT_STATE eq "settings"} { lunar::settings_dlg }
    update idletasks ; update
}

# Stage the Event Log window on a representative store: seeded directly
# into ::lunar::events (bypassing lunar::ev so the scratch events.log
# stays empty and the staged rows are the only content). Capture with
# LUNAR_SHOT_TITLE set to "Time Actual — Event Log" so shot.tcl targets the
# log window, not the face.
proc uishot_stage_eventlog {} {
    set B 1751731872000  ;# same staged instant the face uses
    # The face's localtime stub returns one fixed instant, which would
    # render every row with the same stamp; the dialog wants real, varied,
    # still-deterministic times -- true UTC breakdown, no tz dependency.
    proc ::lunar::localtime {ms zone} {
        set s [expr {$ms / 1000}]
        if {$ms % 1000 < 0} { incr s -1 }
        lassign [clock format $s -format {%Y %m %d %H %M %S} -timezone :UTC] Y M D h m sec
        return [dict create hour [scan $h %d] minute [scan $m %d] \
            second [scan $sec %d] wday 0 day [scan $D %d] month [scan $M %d] \
            year [scan $Y %d] offSec 0 abbr UTC]
    }
    set ::lunar::events {}
    set i 0
    foreach {off trusted sev cat msg} [list \
        -93000000 0 info  app      {session start (Time Actual 0.54)} \
        -92990000 0 info  dns      {resolver ready, 4 pinned providers} \
        -92980000 0 info  ntp      {cycle: 4/4 replies, spread 31 ms} \
        -92970000 1 info  clock    {anchored, rate -2 ppm, anchorErr 44 ms} \
        -92960000 1 info  nts      {time.cloudflare.com: 8 cookies, rtt 18 ms} \
        -86000000 1 warn  ntp      {ptbtime1.ptb.de: timeout, retry in 8 s} \
        -85990000 1 info  ntp      {cycle: 3/4 replies, spread 55 ms} \
        -49000000 1 info  chime    {armed marks: 12 3 6 9} \
        -48000000 1 info  chime    {chime fired for mark 3} \
        -21600000 1 warn  nts      {cookie jar stale (67 min), forcing NTS-KE} \
        -21590000 1 info  nts      {NTS-KE ok, 8 fresh cookies} \
        -7200000  1 info  update   {release check: 0.55 is current} \
        -3600000  1 warn  pinstore {pin set nearing rotation window} \
        -3500000  1 error clock    {*** TIME STEP *** system clock moved 2.1 s} \
        -3490000  1 info  clock    {re-anchored, rate -2 ppm, anchorErr 47 ms} \
        -3480000  1 info  ntp      {cycle: 4/4 replies, spread 29 ms} \
        -1800000  1 info  ui       {settings saved} \
        -900447   1 error app     {bgerror: can't read "nosuchvar": no such variable · while executing · "demo trace line"} \
        -600212   1 info  tz       {display zone set to Europe/Brussels} \
        -300841   1 info  ntp      {cycle: 4/4 replies, spread 24 ms} \
        -240133   1 info  clock    {holdover drift 0.4 ms over 60 s} \
        -180029   1 warn  ntp      {NOTE pin rotation observed for time.nist.gov} \
        -120776   1 info  ntp      {cycle: 4/4 replies, spread 26 ms} \
        -60415    1 info  nts      {time.cloudflare.com: rtt 17 ms, offset +3 ms} \
        -30268    0 info  app      {session start (Time Actual 0.55)} \
        -20097    0 info  ntp      {cycle: 4/4 replies, spread 31 ms} \
        -10354    1 info  clock    {anchored, rate -2 ppm, anchorErr 45 ms} \
    ] {
        lappend ::lunar::events \
            [list [expr {$B + $off}] $trusted $sev $cat $msg]
        incr i
    }
    lunar::log_dlg
    set r $::lunar::log_root
    switch -- $::UISHOT_STATE {
        eventlog-filtered {
            lunar::log_q_set "pin"
            $r.top.lvl set Warn+
            lunar::log_refresh
        }
        eventlog-sorted {
            lunar::log_sortby cat
        }
    }
}
