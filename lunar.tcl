# lunar.tcl -- Lunar's Tcl/Tk shell (startup script, packaged as main.tcl).
#
# Analog clock shell for Lunar's disciplined C timescale. Time and trust come
# from the engine via ::lunar::* (see src/lunarx.c); the networking stays on
# its worker threads while Tk owns the window chrome and interaction.

package require Tk

namespace eval lunar {
    variable version "0.58"
    variable poll_ms   60000 ;# BASE cadence: re-sync about this often when OK
    variable poll_min   8000 ;# FAST floor while acquiring / re-anchoring
    variable poll_max 600000 ;# RELAXED ceiling once well disciplined (10 min)
    variable poll_good     0 ;# consecutive converged cycles (drives the relax ramp)
    variable poll_bad      0 ;# consecutive unhealthy cycles (drives the fast backoff)
    variable poll_cur  60000 ;# current interval actually scheduled (for the log/About)
    variable repoll_after {} ;# pending [after] token, so poll_now can cancel it
    variable poll_forced_at 0 ;# [clock milliseconds] of the last forced poll
    variable stopped_prev 0  ;# last rendered stop flag (recover-fast edge)
    variable hastime_prev 1  ;# last rendered hasTime (recover-fast edge)
    variable log_active 0    ;# reentry latch so logging can't recurse into bgerror
    variable bgerror_count 0 ;# uncaught async errors this run (selftest gate)
    variable events {}       ;# unified event store: {wallMs trusted sev cat msg} each
    variable events_seq 0    ;# last engine ring seq ingested (lunar::log_events cursor)
    variable events_mem_max  10000   ;# in-memory history ceiling (events kept)
    variable events_file_max 4194304 ;# rotate events.log past this (4 MiB; +.1 => ~21h of sustained-unhealthy engine)
    variable events_lastcat app ;# engine continuation lines inherit the previous category
    variable events_dirty 0  ;# a new event arrived since the dialog last rebuilt
    variable log_sort {time 0}  ;# dialog sort: column + descending flag
    variable log_filter_after {} ;# debounce token for live search
    variable log_root ""     ;# widget path of the Event Log panel (settings tab)
    variable log_view {}     ;# rows currently shown, in display order (Copy source)
    variable log_q_ph 0      ;# search entry is showing its placeholder
    variable log_loop_after {} ;# refresh-loop token (cancelled on dialog close)
    variable log_mapped 0    ;# first-<Map> latch: snap to newest row once visible
    variable log_fit_after {} ;# resize-refit debounce token
    variable log_fixedW 0    ;# summed width of the fixed (non-message) columns
    variable log_maxlen 0    ;# longest message currently shown (chars)
    variable log_chw 8       ;# mono char advance (message-width fitting)
    variable log_msg_fit 300 ;# message column width that exactly fills the viewport
    variable square_extra_w 0
    variable square_extra_h 0
    variable square_work_x 0
    variable square_work_y 0
    variable square_work_w 0
    variable square_work_h 0
    variable square_last_w 0
    variable square_last_h 0
    variable square_fixing 0
    variable square_pending 0
    variable square_dragging 0
    variable settings_preview_after ""
}

# ---- diagnostics: unified event store + non-modal bgerror -------------------
# One store carries both log domains: UI-side events (appended directly via
# lunar::event) and the C engine's in-memory ring (drained incrementally via
# ::lunar::log_events). Every event is {wallMs trusted sev cat msg}. The
# store persists to %APPDATA%\TimeActual\events.log -- one Tcl list per line, so
# arbitrary message text round-trips exactly -- rotating to events.log.1 at
# events_file_max (4 MiB; the audit.log precedent: bounded disk, no
# daemon). The in-memory tail (last events_mem_max) is what the Event Log
# tab shows, reloaded across sessions, so history survives restarts. An
# OS-shutdown session keeps everything up to the last 1 s drain here; the
# engine's own final entries land in last-session.log (WM_ENDSESSION has
# no budget for a Tcl drain).
proc lunar::datadir {} {
    if {[info exists ::env(LUNAR_DATA_DIR)] && $::env(LUNAR_DATA_DIR) ne ""} { return $::env(LUNAR_DATA_DIR) }
    if {[info exists ::env(APPDATA)] && $::env(APPDATA) ne ""} {
        set new [file join $::env(APPDATA) TimeActual]
        set old [file join $::env(APPDATA) Lunar]
        # The product was Lunar before 0.58: rename its folder once so pins,
        # settings and logs carry over (the engine's app_paths.c makes the
        # same check; whichever runs first does the move).
        if {![file exists $new] && [file isdirectory $old]} { catch { file rename $old $new } }
        return $new
    }
    return [pwd]
}
proc lunar::events_path {} { return [file join [lunar::datadir] events.log] }

# Severity from message text. The engine's 85 call sites carry no explicit
# level -- severity is lexical by convention there, so one classifier owns
# the convention. UI-side events pass an explicit sev and skip this.
proc lunar::events_classify {msg} {
    # Known-healthy templates first: they contain words the heuristics
    # below would misread ("core timeout 6000ms" in the cycle banner,
    # "anchorErr=" in every TRUST_OK line, "INOP" in the recovery line,
    # "expired-renewal" as a pin's historical status).
    if {[regexp {^(cycle start|cycle TRUST_OK|trust .* → OK|display .* → OK|loaded (doh|nts):|first anchor acquired|sync — residual|PIN ROTATION ACCEPTED)} $msg]} {
        return info
    }
    if {[regexp -nocase {\*\*\*|unhandled exception|crash|TRUST_INOP|INOP latch|pin mismatch|NOT accepted|CA validation rejected|unvalidated rotation refused} $msg]} {
        return error
    }
    if {[regexp -nocase {\mfail|\merror|rejected|refused|\merr=|timeout|expired|invalid|mismatch|deferr|\mNOTE\M|giving up|stale|ROTATION observed|backing off|too uncertain} $msg]} {
        return warn
    }
    return info
}

# One-line invariant: the store is line-oriented and the table shows one
# row per event. Fold newlines visibly, strip remaining control chars --
# a raw \n inside a braced list element would span two physical lines and
# desync the framing; remote-derived text (cert subjects, release tags)
# is not guaranteed printable.
proc lunar::events_clean {msg} {
    regsub -all {[\r\n]+} $msg { · } msg
    regsub -all "\[\x01-\x1F\x7F\]+" $msg { } msg
    return $msg
}

# Append one event to the store (memory + disk) and mark the dialog stale.
# NAMED ev, NOT event: a lunar::event proc would shadow Tk's [event] for
# every proc in this namespace (the ::lunar::clock/[clock] incident).
# wallMs "" means "stamp it now": the disciplined clock when it has time
# (trusted stamp), the system clock otherwise (untrusted, rendered with ~).
proc lunar::ev {sev cat msg {wallMs ""} {trusted 0}} {
    if {$::lunar::log_active} return
    set ::lunar::log_active 1
    catch {
        if {$wallMs eq ""} {
            set wallMs [clock milliseconds] ; set trusted 0
            catch {
                set st [::lunar::status]
                if {[dict get $st hasTime]} {
                    set wallMs [dict get $st utcMs] ; set trusted 1
                }
            }
        }
        set rec [list $wallMs $trusted $sev $cat [lunar::events_clean $msg]]
        lappend ::lunar::events $rec
        if {[llength $::lunar::events] > $::lunar::events_mem_max} {
            set ::lunar::events [lrange $::lunar::events \
                end-[expr {$::lunar::events_mem_max - 1}] end]
        }
        # every UI-side event persists synchronously (bgerror traces must
        # reach disk before a follow-on crash, like lunar-ui.log did)
        lunar::events_persist [list $rec]
        set ::lunar::events_dirty 1
    }
    set ::lunar::log_active 0
    # an error event often precedes worse: pull the engine ring forward
    # too, outside the latch (drain is seq-idempotent, never nests -- it
    # schedules nothing and enters no event loop)
    if {$sev eq "error"} { catch { lunar::events_drain } }
}

# Batch-append records to events.log. Rotation first (audit.log's order),
# by an actual stat, never a tracked counter -- multi-byte UTF-8 and a
# possible second instance both drift a counter. A failed rename (the
# other instance or an AV scanner holding the file) is skipped and simply
# retried on a later append; a brief overshoot past the cap is fine, the
# C precedent ignores MoveFileW failure identically. Two instances
# sharing a data dir may still interleave lines or double-rotate -- the
# tolerant loader contains that; the missing single-instance guard is a
# pre-existing app-level gap (settings.dat has the same exposure).
# Failures drop silently (audit.log's contract: logging must never take
# the clock down with it).
proc lunar::events_persist {recs} {
    catch {
        set path [lunar::events_path]
        catch {
            if {[file size $path] > $::lunar::events_file_max} {
                file delete -force "$path.1"
                file rename $path "$path.1"
            }
        }
        file mkdir [lunar::datadir]
        set out ""
        foreach rec $recs { append out $rec \n }
        # a hard kill can leave the file without its trailing newline; a
        # batch appended onto that torn line would merge two records into
        # one corrupt line, so re-open the seam before appending
        catch {
            if {[file size $path] > 0} {
                set fh0 [open $path r] ; fconfigure $fh0 -translation binary
                seek $fh0 -1 end
                set last [read $fh0 1] ; close $fh0
                if {$last ne "\n"} { set out "\n$out" }
            }
        }
        set fh [open $path a]
        # -translation lf + explicit utf-8: the platform defaults (crlf,
        # ANSI codepage) would mangle the engine's UTF-8 and bloat lines;
        # one big buffered write per batch shrinks the torn-line window
        fconfigure $fh -encoding utf-8 -translation lf -buffersize 65536
        puts -nonewline $fh $out
        close $fh
    }
}

proc lunar::events_read_file {path} {
    if {[catch { open $path r } fh]} { return {} }
    # -eofchar {}: the Windows default treats a stray 0x1A byte as EOF,
    # which would silently truncate the rest of the history at load
    fconfigure $fh -encoding utf-8 -translation lf -eofchar {}
    set raw [read $fh] ; close $fh
    set out {}
    foreach line [split $raw \n] {
        set line [string trimright $line \r]
        if {$line eq ""} continue
        # malformed lines (torn tail after a hard kill, dual-instance
        # interleave, hand edits) skip individually; \n-split re-syncs
        # at the next record. Every consumed field is validated -- one
        # hostile trusted/sev value would otherwise throw inside the
        # dialog's render path.
        if {[catch { llength $line } n] || $n < 5} continue
        lassign [lrange $line 0 4] wallMs trusted sev cat msg
        if {![string is wideinteger -strict $wallMs]} continue
        if {![string is boolean -strict $trusted]} continue
        if {$sev ni {info warn error}} continue
        lappend out [list $wallMs [expr {$trusted ? 1 : 0}] $sev $cat $msg]
    }
    return $out
}

# Load persisted history: current file first, the rotated generation only
# when the current one doesn't fill the in-memory cap. Boot-time events
# (lunar::log fires before this runs) were persisted synchronously, so
# they are already IN the file -- the store is simply replaced by the
# loaded history; only records whose persist failed (unwritable data
# dir) are re-appended, never duplicated.
proc lunar::events_load {} {
    set cur [lunar::events_read_file [lunar::events_path]]
    set hist {}
    if {[llength $cur] < $::lunar::events_mem_max} {
        set hist [lunar::events_read_file "[lunar::events_path].1"]
    }
    set all [concat $hist $cur]
    foreach rec $::lunar::events {
        if {$rec ni $cur} { lappend all $rec }
    }
    if {[llength $all] > $::lunar::events_mem_max} {
        set all [lrange $all end-[expr {$::lunar::events_mem_max - 1}] end]
    }
    set ::lunar::events $all
    set ::lunar::events_dirty 1
}

# Drain the engine ring incrementally: everything after the last seq seen.
# Engine stamps: disciplined utcMs when trusted; otherwise approximate the
# wall time as (system now - entry age) -- absolute enough to read across
# sessions, honestly marked untrusted. `module:` prefixes become the
# category; two-space continuation lines inherit the previous category.
proc lunar::events_drain {} {
    if {![llength [info commands ::lunar::log_events]]} return
    if {[catch { ::lunar::log_events $::lunar::events_seq } batch]} return
    if {![llength $batch]} return
    set nowMs [clock milliseconds]
    set recs {}
    # seq gap = ring entries lost between drains (burst overwrite or 24h
    # eviction outran us). Rare and pathological, but never silent.
    set first [dict get [lindex $batch 0] seq]
    if {$::lunar::events_seq > 0 && $first > $::lunar::events_seq + 1} {
        set lost [expr {$first - $::lunar::events_seq - 1}]
        lappend recs [list $nowMs 0 warn app \
            "engine ring gap: $lost events evicted before they could be stored"]
    }
    foreach e $batch {
        set seq [dict get $e seq]
        if {$seq > $::lunar::events_seq} { set ::lunar::events_seq $seq }
        set msg [dict get $e msg]
        set trusted [dict get $e trusted]
        if {$trusted} {
            set wallMs [dict get $e utcMs]
        } else {
            set wallMs [expr {$nowMs - [dict get $e ageMs]}]
        }
        if {[regexp {^([a-z]+): (.*)$} $msg -> cat rest]} {
            set ::lunar::events_lastcat $cat
            set msg $rest
        } elseif {[string match "  *" $msg]} {
            set cat $::lunar::events_lastcat
        } else {
            set cat app
        }
        set msg [lunar::events_clean $msg]
        lappend recs [list $wallMs $trusted [lunar::events_classify $msg] $cat $msg]
    }
    foreach rec $recs { lappend ::lunar::events $rec }
    if {[llength $::lunar::events] > $::lunar::events_mem_max} {
        set ::lunar::events [lrange $::lunar::events \
            end-[expr {$::lunar::events_mem_max - 1}] end]
    }
    lunar::events_persist $recs
    set ::lunar::events_dirty 1
}

# Schedules strictly at its own tail and never enters the event loop, so
# drains cannot nest -- not even through lunar::quit's nested-modal loop.
proc lunar::events_drain_loop {} {
    catch { lunar::events_drain }
    after 1000 lunar::events_drain_loop
}

# Compat shim: existing UI call sites log free text; categorize [tag]
# prefixes and classify the rest, so every path feeds the one store.
proc lunar::log {msg} {
    set sev "" ; set cat ui
    # tags may be multi-word ("[settings sync]") or underscored
    # ("[engine_start]"); spaces become dashes so the category stays one
    # scannable token
    if {[regexp {^\[([a-z_ ]+)\] (.*)$} $msg -> tag rest]} {
        set cat [string map {" " -} $tag] ; set msg $rest
        if {$tag in {bgerror tick engine_start}} { set sev error }
    }
    if {$sev eq ""} { set sev [lunar::events_classify $msg] }
    lunar::ev $sev $cat $msg
}
# Keep the status bar's trust field stable. Short-lived interaction feedback
# uses the system-clock witness field and is replaced by the next render.
proc lunar::status_note {m} { catch { .sb.sys configure -text $m -fg $::lunar::ACCENT } }
proc lunar::bgerror {msg args} {
    if {$::lunar::log_active} return
    incr ::lunar::bgerror_count   ;# the selftest fails on any nonzero count
    set trace $msg
    if {[llength $args]} { catch { set trace [dict get [lindex $args 0] -errorinfo] } }
    catch { lunar::ev error app $trace }
    catch { lunar::status_note "internal error (see event log)" }
}

# ---- look: els's visual identity --------------------------------------------
# The palette is that of the flag of Neutral Moresnet -- black, white and
# its blue (#0F47AF, as the Wikimedia Commons flag file draws it) -- with
# grey for whatever must recede. No other hue anywhere in the shell; the
# dial widget (lunarclock.c) and the icon (tools/icon.tcl) use the same
# three values.
set ::lunar::PAGE   "#FFFFFF"   ;# white page
set ::lunar::INK    "#000000"   ;# black ink
set ::lunar::ACCENT "#0F47AF"   ;# the flag's blue: second hand, armed marks, alerts
set ::lunar::MUTED  "#6E6E6E"   ;# grey (chrome text, minute ticks)
set ::lunar::CHROME "#EDEDED"   ;# flat grey chrome (status bar)
set ::lunar::HAIR   "#D4D4D4"   ;# hairline separators
set ::lunar::OK     "#000000"   ;# trusted: plain ink, the settled state
set ::lunar::WARN   "#555555"   ;# holdover/reacquiring: a shade less sure
set ::lunar::CLOCK_SZ 440       ;# analog face canvas size (square, px)

option add *tearOff 0
font create lunarUI    -family {Segoe UI} -size 9
font create lunarBig   -family Consolas   -size 44
font create lunarDate  -family {Segoe UI} -size 12
font create lunarSmall -family {Segoe UI} -size 9
font create lunarState -family {Segoe UI Semibold} -size 11
font create lunarMono  -family Consolas   -size 9
font create lunarHdr   -family {Segoe UI Semibold} -size 8
font create lunarUIb   -family {Segoe UI} -size 9 -weight bold
font create lunarGear  -family {Segoe UI Symbol} -size 12
set ::lunar::ontop 0

proc lunar::init_style {} {
    set s ttk::style
    catch {$s theme use clam}
    set bg $::lunar::CHROME ; set ink $::lunar::INK ; set hair $::lunar::HAIR
    $s configure . -background $bg -foreground $ink -font lunarUI \
        -borderwidth 0 -focuscolor $bg -troughcolor $::lunar::PAGE \
        -bordercolor $hair -darkcolor $bg -lightcolor $bg
    # entries: flat, page-coloured field, hairline border; focus = a firmer
    # grey, never red (els: red is reserved for the accent). Dialogs sit on
    # PAGE, so give dialog widgets a Page background variant too.
    $s configure TEntry -relief flat -borderwidth 1 -padding {6 4} \
        -fieldbackground $::lunar::PAGE -foreground $ink -insertcolor $ink \
        -bordercolor $hair -lightcolor $hair -darkcolor $hair
    $s map TEntry -bordercolor [list focus "#A0A0A0"] \
        -lightcolor [list focus "#A0A0A0"] -darkcolor [list focus "#A0A0A0"]
    # dialog buttons read as buttons even before hover (els Dialog.TButton)
    $s configure Dialog.TButton -background $::lunar::PAGE -foreground $ink \
        -borderwidth 1 -relief solid -padding {10 5} -anchor center \
        -bordercolor $hair -lightcolor $hair -darkcolor $hair
    $s map Dialog.TButton -background [list pressed $hair active "#DEDEDE"] \
        -foreground [list disabled $::lunar::MUTED]
    # The clock's only persistent control: a quiet, borderless settings icon
    # embedded in the status bar.
    $s configure Status.TButton -background $bg -foreground $::lunar::MUTED \
        -font lunarGear -borderwidth 0 -relief flat -padding {7 1} -anchor center
    $s map Status.TButton -background [list pressed "#CCCCCC" active $hair] \
        -foreground [list active $ink]
    $s configure Settings.TNotebook -background $::lunar::PAGE -borderwidth 0 \
        -tabmargins {0 0 0 0}
    $s configure Settings.TNotebook.Tab -background $bg -foreground $::lunar::MUTED \
        -padding {14 7} -borderwidth 0 -font lunarUI
    $s map Settings.TNotebook.Tab \
        -background [list selected $::lunar::PAGE active "#DEDEDE"] \
        -foreground [list selected $ink]
    # traditional scrollbars, arrow size in POINTS so it scales per-DPI;
    # both orientations dress identically (the clam default horizontal bar
    # is a ghost: page-on-page), and the grip texture goes -- it is the
    # only 3D ornament in an otherwise flat chrome
    foreach orient {Vertical Horizontal} {
        $s configure $orient.TScrollbar -troughcolor $::lunar::PAGE \
            -background #BCBCBC -arrowcolor #4A4A4A -bordercolor #9A9A9A \
            -relief raised -borderwidth 1 -arrowsize 12p -gripcount 0
        $s map $orient.TScrollbar \
            -background [list active #A4A4A4 disabled $::lunar::PAGE]
    }
    # Event Log table: mono data rows on PAGE (continuity with the mono
    # log it replaced; timestamps and ms digits align), quiet CHROME
    # headings. Row height derives from the font so it scales per-DPI.
    $s configure Log.Treeview -background $::lunar::PAGE \
        -fieldbackground $::lunar::PAGE -foreground $ink -font lunarMono \
        -borderwidth 1 -relief solid -bordercolor $hair \
        -lightcolor $::lunar::PAGE -darkcolor $::lunar::PAGE \
        -rowheight [expr {[font metrics lunarMono -linespace] + 6}]
    $s map Log.Treeview -background [list selected "#D6E2F2"] \
        -foreground [list selected $ink]
    # heading left padding matches the cells' internal text indent so the
    # captions sit flush over their column data
    $s configure Log.Treeview.Heading -background $bg \
        -foreground $::lunar::MUTED -font lunarHdr -relief flat -padding {4 3}
    $s map Log.Treeview.Heading -background [list pressed $hair active "#DEDEDE"]
    # comboboxes (the Event Log filters are the app's first): match the
    # TEntry field treatment, including the popdown list
    $s configure TCombobox -fieldbackground $::lunar::PAGE \
        -background $::lunar::PAGE -foreground $ink -arrowcolor $::lunar::MUTED \
        -bordercolor $hair -lightcolor $hair -darkcolor $hair \
        -padding {6 3} -relief flat -borderwidth 1
    $s map TCombobox -bordercolor [list focus "#A0A0A0"] \
        -lightcolor [list focus "#A0A0A0"] -darkcolor [list focus "#A0A0A0"] \
        -fieldbackground [list readonly $::lunar::PAGE] \
        -background [list readonly $::lunar::PAGE]
    option add *TCombobox*Listbox.background $::lunar::PAGE
    option add *TCombobox*Listbox.foreground $ink
    option add *TCombobox*Listbox.selectBackground "#D6E2F2"
    option add *TCombobox*Listbox.selectForeground $ink
    option add *TCombobox*Listbox.font lunarUI
}

# ---- state -> (label, colour) ----------------------------------------------
proc lunar::state_display {state synced} {
    switch $state {
        ok          { return [list "TRUSTED"     $::lunar::OK] }
        holdover    { return [list "ESTIMATED"   $::lunar::WARN] }
        reacquiring { return [list "REACQUIRING" $::lunar::WARN] }
        default {
            if {$synced} { return [list "NO SIGNAL" $::lunar::ACCENT] }
            return [list "ACQUIRING…" $::lunar::MUTED]
        }
    }
}

proc lunar::fmt_bound {ms} {
    if {$ms <= 0} { return "" }
    if {$ms < 1000} { return "±${ms} ms" }
    return [format "±%.1f s" [expr {$ms / 1000.0}]]
}

# System-clock deviation, compact: seconds carry no unit (they are the
# obvious default); minutes/hours/days get m/h/d appended directly, no space.
proc lunar::fmt_delta {ms} {
    set s [expr {$ms / 1000.0}]
    set a [expr {abs($s)}]
    if {$a < 60.0}      { return [format "%+.2f" $s] }
    if {$a < 3600.0}    { return [format "%+.1fm" [expr {$s / 60.0}]] }
    if {$a < 86400.0}   { return [format "%+.1fh" [expr {$s / 3600.0}]] }
    return [format "%+.1fd" [expr {$s / 86400.0}]]
}

# ---- settings (same file + format + semantics as the Win32 shell) -----------
# %APPDATA%\TimeActual\settings.dat, one key=value per line. Keys this shell does
# not own (legacy tray, and any future keys) are preserved verbatim so the
# two shells can be swapped without losing anything. The PRESENCE of the tz=
# key -- even empty, meaning explicit UTC -- counts as a deliberate choice;
# only then does first-run OS-zone suggestion stop.
set ::lunar::cfg [dict create fmt24 1 confirm 1 startup 0 chimes 1 unmin 0 stopms 5000]
set ::lunar::cfg_extra {}       ;# unowned lines (confirm), kept in file order
set ::lunar::tz "UTC"           ;# the active display zone
set ::lunar::tz_chosen 0
set ::lunar::armed [lrepeat 12 0]   ;# the 12 five-minute marks (:00..:55)
set ::lunar::prev_min -1            ;# last displayed integer minute (chime edge)
set ::lunar::prewarm_min -1         ;# armed mark we've already pre-warmed for
set ::lunar::stop_hits 0            ;# consecutive reads with bound above the ceiling

# The stop ceiling: boundMs above which the clock stops showing time.
# Clamped to [1 s, 30 s]: below 1 s a healthy clock could trip on a
# transient fault inflate; above 30 s the fan exceeds what the seconds
# dial can encode (±30 s wraps the full circle). Bad input -> default.
proc lunar::clamp_stopms {v} {
    if {![string is integer -strict $v]} { return 5000 }
    if {$v < 1000}  { return 1000 }
    if {$v > 30000} { return 30000 }
    return $v
}

# Stop rule: stop only after two consecutive reads above the ceiling (a
# 500 ms debounce so a single fault-inflated cycle cannot flap the face);
# resume the instant the bound is back under it -- a re-anchor collapses the
# bound to ~200 ms, so recovery is one accepted cycle.
proc lunar::stop_eval {hasTime boundMs} {
    if {!$hasTime || $boundMs <= [dict get $::lunar::cfg stopms]} {
        set ::lunar::stop_hits 0
        return 0
    }
    incr ::lunar::stop_hits
    return [expr {$::lunar::stop_hits >= 2}]
}

proc lunar::settings_load {} {
    set path [file join [lunar::datadir] settings.dat]
    if {[catch {open $path r} fh]} { return }
    set raw [read $fh] ; close $fh
    foreach line [split $raw \n] {
        set line [string trimright $line \r]
        if {$line eq ""} continue
        if {![regexp {^([a-z0-9]+)=(.*)$} $line -> k v]} continue
        switch $k {
            fmt24   { dict set ::lunar::cfg fmt24   [expr {$v ? 1 : 0}] }
            confirm { dict set ::lunar::cfg confirm [expr {$v ? 1 : 0}] }
            startup { dict set ::lunar::cfg startup [expr {$v ? 1 : 0}] }
            chimes  { dict set ::lunar::cfg chimes  [expr {$v ? 1 : 0}] }
            unmin   { dict set ::lunar::cfg unmin   [expr {$v ? 1 : 0}] }
            stopms  { dict set ::lunar::cfg stopms  [lunar::clamp_stopms $v] }
            tz      { set ::lunar::tz_chosen 1 ; dict set ::lunar::cfg tz $v }
            default { lappend ::lunar::cfg_extra $line }
        }
    }
}

proc lunar::settings_save {} {
    set dir [lunar::datadir]
    if {[catch {file mkdir $dir}]} { return }
    set lines $::lunar::cfg_extra
    lappend lines "chimes=[dict get $::lunar::cfg chimes]"
    lappend lines "unmin=[dict get $::lunar::cfg unmin]"
    lappend lines "fmt24=[dict get $::lunar::cfg fmt24]"
    lappend lines "confirm=[dict get $::lunar::cfg confirm]"
    lappend lines "startup=[dict get $::lunar::cfg startup]"
    lappend lines "stopms=[dict get $::lunar::cfg stopms]"
    lappend lines "tz=[expr {$::lunar::tz eq "UTC" ? "" : $::lunar::tz}]"
    set path [file join $dir settings.dat]
    set tmp  "$path.new"
    if {[catch {
        set fh [open $tmp w] ; fconfigure $fh -translation lf
        puts $fh [join $lines \n] ; close $fh
        file rename -force $tmp $path
    } err]} {
        catch { file delete -force $tmp }
        lunar::log "\[settings\] save failed: $err"
    }
}

# armed.dat: 12 chars '0'/'1', one per five-minute mark (:00..:55). Same file
# and format as the Win32 shell, so the two are interchangeable.
proc lunar::armed_load {} {
    if {[catch {open [file join [lunar::datadir] armed.dat] r} fh]} return
    set raw [read $fh] ; close $fh
    set a {}
    for {set i 0} {$i < 12} {incr i} {
        lappend a [expr {[string index $raw $i] eq "1" ? 1 : 0}]
    }
    set ::lunar::armed $a
}
proc lunar::armed_save {} {
    set dir [lunar::datadir]
    if {[catch {file mkdir $dir}]} return
    set s "" ; foreach v $::lunar::armed { append s [expr {$v ? "1" : "0"}] }
    set path [file join $dir armed.dat] ; set tmp "$path.new"
    if {[catch {
        set fh [open $tmp w] ; fconfigure $fh -translation binary
        puts -nonewline $fh $s ; close $fh
        file rename -force $tmp $path
    }]} { catch {file delete -force $tmp} }
}

# Just-in-time audio pre-warm: ~3 s before an armed 5-minute mark, wake the
# render endpoint so the chime renders into an awake device. Windows parks an
# idle endpoint in D3; the D3->D0 wake would otherwise clip the tone's onset
# (and the amp's power-on pop lands on this silent prime, not on the tone).
# Fires once per approaching mark, and only when a chime will actually fire
# (chimes enabled + bound chime-worthy) -- so the endpoint stays idle the rest
# of the time, unlike a continuous keep-alive stream.
proc lunar::prewarm_check {curMin curSec boundMs} {
    if {$::lunar::prewarm_min == $curMin} { set ::lunar::prewarm_min -1 }  ;# mark reached; re-arm
    # No prime once the bound is at/above the stop ceiling: a stopped clock
    # must not assert time audibly either.
    if {$curSec < 57 || $boundMs >= [dict get $::lunar::cfg stopms]} return
    if {![dict get $::lunar::cfg chimes]} return
    set nextMin [expr {($curMin + 1) % 60}]
    if {$nextMin % 5 != 0} return
    if {![lindex $::lunar::armed [expr {$nextMin / 5}]]} return
    if {$::lunar::prewarm_min == $nextMin} return                          ;# already primed
    if {![llength [info commands ::lunar::prewarm]]} return
    set ::lunar::prewarm_min $nextMin
    catch { ::lunar::prewarm }
}

# Chime edge detector (parity with the Win32 shell): fire once when the
# displayed minute crosses an armed :MM mark, only while the bound is
# chime-worthy (<30 s) and the minute-step is 1-2 (no cascade after a jump).
proc lunar::chime_check {curMin boundMs} {
    set prev $::lunar::prev_min
    set ::lunar::prev_min $curMin
    if {$prev < 0 || $curMin == $prev} return
    set delta [expr {($curMin - $prev + 60) % 60}]
    # Chime-worthy only while the bound is under the stop ceiling: a stopped
    # clock (or one about to stop) must not assert time audibly.
    if {$delta < 1 || $delta > 2 || $boundMs >= [dict get $::lunar::cfg stopms]} return
    set chimes [dict get $::lunar::cfg chimes]
    set unmin  [dict get $::lunar::cfg unmin]
    for {set k 1} {$k <= $delta} {incr k} {
        set mm [expr {($prev + $k) % 60}]
        if {$mm % 5 != 0} continue
        if {![lindex $::lunar::armed [expr {$mm / 5}]]} continue
        if {$chimes && [llength [info commands ::lunar::beep]]} { catch { ::lunar::beep } }
        if {$unmin && [wm state .] ne "normal"} { lunar::restore }
        # the log should be able to answer "did it chime at :05?"
        lunar::ev info chime [format "mark :%02d %s (bound ±%d ms)" $mm \
            [expr {$chimes ? "chimed" : "reached, chimes off"}] $boundMs]
    }
}

# Resolve the display zone at startup: an explicit (valid) choice wins, else
# suggest the OS zone by NAME (not trusting the OS clock), else UTC.
proc lunar::tz_startup {} {
    set have_engine [llength [info commands ::lunar::localtime]]
    if {$::lunar::tz_chosen} {
        set want [dict get $::lunar::cfg tz]
        if {$want eq ""} { set want "UTC" }
        if {!$have_engine || ![catch { ::lunar::localtime 0 $want }]} {
            set ::lunar::tz $want
            return
        }
        lunar::log "\[tz\] configured zone '$want' not in embedded index; using UTC"
        set ::lunar::tz "UTC"
        return
    }
    if {$have_engine && [llength [info commands ::lunar::tz_suggest]]} {
        set sug [::lunar::tz_suggest]
        if {$sug ne "" && ![catch { ::lunar::localtime 0 $sug }]} {
            set ::lunar::tz $sug
            lunar::log "\[tz\] first run: suggesting OS zone $sug"
        }
    }
}

# ---- wall-clock formatting off the embedded tzdata --------------------------
set ::lunar::DAYS   {Sunday Monday Tuesday Wednesday Thursday Friday Saturday}
set ::lunar::MONTHS {January February March April May June July \
                     August September October November December}

proc lunar::fmt_time {lt} {
    set h [dict get $lt hour]
    if {[dict get $::lunar::cfg fmt24]} {
        return [format "%02d:%02d:%02d" $h [dict get $lt minute] [dict get $lt second]]
    }
    set ap [expr {$h >= 12 ? "PM" : "AM"}]
    set h [expr {$h % 12}] ; if {$h == 0} { set h 12 }
    return [format "%d:%02d:%02d %s" $h [dict get $lt minute] [dict get $lt second] $ap]
}
proc lunar::fmt_date {lt} {
    set wd [lindex $::lunar::DAYS   [dict get $lt wday]]
    set mo [lindex $::lunar::MONTHS [expr {[dict get $lt month] - 1}]]
    return "$wd, [format %02d [dict get $lt day]] $mo [dict get $lt year]"
}
proc lunar::fmt_utcoff {offSec} {
    set sign [expr {$offSec < 0 ? "-" : "+"}]
    set a [expr {abs($offSec)}]
    set h [expr {$a / 3600}] ; set m [expr {($a % 3600) / 60}]
    if {$m} { return "UTC$sign$h:[format %02d $m]" }
    return "UTC$sign$h"
}

# ---- analog face ------------------------------------------------------------
# A plain Tk canvas: the ring + tick marks are drawn once (tag "face"); the
# three hands + hub are redrawn each tick (tag "hand") from the disciplined
# wall-clock breakdown. No anti-aliasing library needed -- thick round-capped
# lines read clean on Tk 9.

proc lunar::_hand {c cx cy len ang col w} {
    set a [expr {$ang * 3.14159265358979 / 180.0}]
    $c create line $cx $cy [expr {$cx + $len*sin($a)}] [expr {$cy - $len*cos($a)}] \
        -fill $col -width $w -capstyle round -tags hand
}

proc lunar::armed_mask {} {
    set mask ""
    foreach armed $::lunar::armed { append mask [expr {$armed ? "1" : "0"}] }
    return $mask
}

# The hit band is the same annulus and 24-degree wedge used by the old native
# clock: a click near any five-minute marker toggles its corresponding :MM
# chime.  It deliberately follows the resized face's actual geometry.
proc lunar::clock_marker_hit {c x y} {
    set w [winfo width $c] ; set h [winfo height $c]
    if {$w <= 1 || $h <= 1} { return -1 }
    set size [expr {min($w, $h)}]
    if {$size <= 40} { return -1 }
    set cx [expr {$w / 2.0}] ; set cy [expr {$h / 2.0}]
    set radius [expr {$size * .46}]
    set dx [expr {$x - $cx}] ; set dy [expr {$y - $cy}]
    set r [expr {hypot($dx, $dy)}]
    if {$r < $radius - .075 * $size || $r > $radius + .015 * $size} { return -1 }
    set angle [expr {atan2($dx, -$dy) * 180.0 / acos(-1)}]
    if {$angle < 0} { set angle [expr {$angle + 360.0}] }
    set nearest [expr {round($angle / 30.0)}]
    if {[expr {abs($angle - $nearest * 30.0)}] > 12.0} { return -1 }
    return [expr {int($nearest) % 12}]
}

proc lunar::clock_marker_click {c x y} {
    set index [lunar::clock_marker_hit $c $x $y]
    if {$index < 0} return
    lset ::lunar::armed $index [expr {![lindex $::lunar::armed $index]}]
    # If Settings is open, keep its staged mark in sync so Save cannot
    # overwrite a click made directly on the dial.
    if {[winfo exists .set] && [info exists ::lunar::set_armed($index)]} {
        set ::lunar::set_armed($index) [lindex $::lunar::armed $index]
    }
    lunar::armed_save
    # Native Direct2D and the source-only canvas both update immediately;
    # neither has to wait for the next 200 ms clock tick.
    if {[catch {$c armed [lunar::armed_mask]}]} { lunar::clock_face_static $c }
    set mark [format ":%02d" [expr {$index * 5}]]
    lunar::status_note "[expr {[lindex $::lunar::armed $index] ? "armed" : "disarmed"}] $mark"
}

proc lunar::clock_face_static {c} {
    # A native Direct2D clock receives its actual HWND size directly. This
    # canvas fallback mirrors that geometry for source-only Tk runs.
    set w [winfo width $c] ; set h [winfo height $c]
    if {$w <= 1 || $h <= 1} { set w $::lunar::CLOCK_SZ ; set h $::lunar::CLOCK_SZ }
    set sz [expr {min($w, $h)}]
    set cx [expr {$w/2.0}] ; set cy [expr {$h/2.0}] ; set R [expr {$sz*.46}]
    set PI 3.14159265358979
    $c delete face
    $c create oval [expr {$cx-$R}] [expr {$cy-$R}] [expr {$cx+$R}] [expr {$cy+$R}] \
        -outline $::lunar::HAIR -width 2 -tags face
    set minWidth [expr {max(1, round($sz * .008))}]
    for {set i 0} {$i < 60} {incr i} {
        if {$i % 5 == 0} continue
        set a [expr {$i * 6 * $PI / 180.0}]
        set r1 [expr {$R - .020 * $sz}]
        $c create line [expr {$cx+$r1*sin($a)}] [expr {$cy-$r1*cos($a)}] \
                       [expr {$cx+$R*sin($a)}]  [expr {$cy-$R*cos($a)}] \
                       -fill $::lunar::MUTED -width $minWidth -capstyle round -tags face
    }
    set hourInner [expr {$R - .050 * $sz}]
    set hourWidth [expr {max(3, round($sz * .014))}]
    for {set hour 1} {$hour < 12} {incr hour} {
        set a [expr {$hour * 30 * $PI / 180.0}]
        set col [expr {[lindex $::lunar::armed $hour] ? $::lunar::ACCENT : $::lunar::INK}]
        $c create line [expr {$cx+$hourInner*sin($a)}] [expr {$cy-$hourInner*cos($a)}] \
                       [expr {$cx+$R*sin($a)}]         [expr {$cy-$R*cos($a)}] \
                       -fill $col -width $hourWidth -capstyle round -tags face
    }
    set col [expr {[lindex $::lunar::armed 0] ? $::lunar::ACCENT : $::lunar::INK}]
    set offset [expr {$sz * .020}]
    $c create line [expr {$cx-$offset}] [expr {$cy-$hourInner}] \
                   [expr {$cx-$offset}] [expr {$cy-$R}] \
                   -fill $col -width $hourWidth -capstyle round -tags face
    $c create line [expr {$cx+$offset}] [expr {$cy-$hourInner}] \
                   [expr {$cx+$offset}] [expr {$cy-$R}] \
                   -fill $col -width $hourWidth -capstyle round -tags face
    # A resize redraws the static face but must not cover the live hands.
    $c lower face
}

# Draw the hands from a localtime dict, mirroring the native widget's policy.
# The display has exactly TWO states: the time -- with the uncertainty carried
# entirely by the second hand, an UNCERTAINTY FAN (a pie sector as wide as the
# error bound, ±boundMs -> half-angle boundMs/1000 × 6°, around a hairline
# best-estimate centerline, always the signature red) -- or no time at all
# (the caller clears the hands). The fan is the same solid signature red as
# the centerline -- one hand, wider when less certain -- lowered BENEATH the
# face ticks so they stay legible across it.
proc lunar::clock_hands {c lt milliseconds boundMs} {
    set w [winfo width $c] ; set h [winfo height $c]
    if {$w <= 1 || $h <= 1} { set w $::lunar::CLOCK_SZ ; set h $::lunar::CLOCK_SZ }
    set sz [expr {min($w, $h)}]
    set cx [expr {$w/2.0}] ; set cy [expr {$h/2.0}]
    $c delete hand
    set h [dict get $lt hour] ; set m [dict get $lt minute]
    set s [expr {[dict get $lt second] + $milliseconds / 1000.0}]
    if {$boundMs > 0} {
        set r [expr {$sz*.44}]
        set half [expr {double($boundMs) / 1000.0 * 6.0}]
        if {$half >= 180.0} {
            set id [$c create oval [expr {$cx-$r}] [expr {$cy-$r}] \
                        [expr {$cx+$r}] [expr {$cy+$r}] \
                        -fill $::lunar::ACCENT -outline "" -tags hand]
        } else {
            set secAng [expr {$s/60.0*360}]
            set id [$c create arc [expr {$cx-$r}] [expr {$cy-$r}] \
                        [expr {$cx+$r}] [expr {$cy+$r}] \
                        -start [expr {90.0 - $secAng - $half}] \
                        -extent [expr {2.0*$half}] \
                        -style pieslice -fill $::lunar::ACCENT -outline "" -tags hand]
        }
        $c lower $id     ;# beneath the face ring/ticks
    }
    lunar::_hand $c $cx $cy [expr {$sz*.28}] [expr {(($h%12)+$m/60.0)/12.0*360}] $::lunar::INK 9
    lunar::_hand $c $cx $cy [expr {$sz*.40}] [expr {($m+$s/60.0)/60.0*360}]      $::lunar::INK 5
    lunar::_hand $c $cx $cy [expr {$sz*.44}] [expr {$s/60.0*360}] $::lunar::ACCENT 2
    $c create oval [expr {$cx-6}] [expr {$cy-6}] [expr {$cx+6}] [expr {$cy+6}] \
        -fill $::lunar::INK -outline "" -tags hand
}

# Prefer the native Direct2D clock widget when packaged with Lunar.  The
# source-only Tk iteration path has no C extension, so it deliberately falls
# back to the canvas implementation above.
# This runs once per 200 ms tick, so log each distinct failure once, not
# per render (a wish/dev run falls through here every tick: the canvas
# widget has no [show] subcommand).
proc lunar::clock_display_err {err} {
    if {[info exists ::lunar::clock_display_lasterr] &&
        $::lunar::clock_display_lasterr eq $err} return
    set ::lunar::clock_display_lasterr $err
    lunar::log "Direct2D clock update failed: $err"
}

proc lunar::clock_display {lt milliseconds state hasTime synced boundMs stopped} {
    if {[llength [info commands .face.clock]]} {
        if {$hasTime && $lt ne ""} {
            if {![catch {
                .face.clock show [dict get $lt hour] [dict get $lt minute] \
                    [dict get $lt second] $milliseconds 1 $state $synced \
                    $boundMs $stopped [lunar::armed_mask]
            } err]} { return }
            lunar::clock_display_err $err
        } elseif {![catch {
            .face.clock show 0 0 0 0 0 $state $synced 0 0 [lunar::armed_mask]
        } err]} {
            return
        } else {
            lunar::clock_display_err $err
        }
    }
    if {$hasTime && $lt ne "" && !$stopped} {
        lunar::clock_hands .face.clock $lt $milliseconds $boundMs
    } else {
        # The other of the two display states: no time shown at all.
        catch { .face.clock delete hand }
    }
}

# Windows ignores Tk's `wm aspect` request.  Keep the resize in Tk's own
# geometry manager, then compensate for the measured title-frame insets so
# the visible outer window remains square without disturbing native children.
proc lunar::square_finish {} {
    set ::lunar::square_fixing 0
    set ::lunar::square_last_w [winfo width .]
    set ::lunar::square_last_h [winfo height .]
}

proc lunar::square_apply {width height} {
    lassign [wm minsize .] minW minH
    if {$width < $minW} { set width $minW }
    if {$height < $minH} { set height $minH }
    set ::lunar::square_fixing 1
    wm geometry . "${width}x${height}"
    after idle lunar::square_finish
}

proc lunar::square_correct {} {
    set ::lunar::square_pending 0
    if {$::lunar::square_fixing || $::lunar::square_dragging} return
    if {[wm state .] eq "zoomed" && $::lunar::square_work_w > 0 && $::lunar::square_work_h > 0} {
        set side [expr {min($::lunar::square_work_w, $::lunar::square_work_h)}]
        set width [expr {$side - $::lunar::square_extra_w}]
        set height [expr {$side - $::lunar::square_extra_h}]
        set x [expr {$::lunar::square_work_x + ($::lunar::square_work_w - $side) / 2}]
        set y [expr {$::lunar::square_work_y + ($::lunar::square_work_h - $side) / 2}]
        set ::lunar::square_fixing 1
        wm state . normal
        wm geometry . "${width}x${height}+$x+$y"
        after idle lunar::square_finish
        return
    }
    set w [winfo width .] ; set h [winfo height .]
    if {$::lunar::square_last_w <= 0 || $::lunar::square_last_h <= 0} {
        set ::lunar::square_last_w $w ; set ::lunar::square_last_h $h
        return
    }
    set outerW [expr {$w + $::lunar::square_extra_w}]
    set outerH [expr {$h + $::lunar::square_extra_h}]
    if {[expr {abs($outerW - $outerH)}] <= 1} {
        set ::lunar::square_last_w $w ; set ::lunar::square_last_h $h
        return
    }
    set dw [expr {abs($w - $::lunar::square_last_w)}]
    set dh [expr {abs($h - $::lunar::square_last_h)}]
    if {$dw >= $dh} {
        set targetW $w
        set targetH [expr {$outerW - $::lunar::square_extra_h}]
    } else {
        set targetH $h
        set targetW [expr {$outerH - $::lunar::square_extra_w}]
    }
    lassign [wm minsize .] minW minH
    if {$targetH < $minH} {
        set targetH $minH
        set targetW [expr {$targetH + $::lunar::square_extra_h - $::lunar::square_extra_w}]
    }
    if {$targetW < $minW} {
        set targetW $minW
        set targetH [expr {$targetW + $::lunar::square_extra_w - $::lunar::square_extra_h}]
    }
    lunar::square_apply $targetW $targetH
}

proc lunar::square_configure {} {
    if {$::lunar::square_fixing || $::lunar::square_dragging || $::lunar::square_pending} return
    set ::lunar::square_pending 1
    after idle lunar::square_correct
}

proc lunar::square_resize_start {} { set ::lunar::square_dragging 1 }
proc lunar::square_resize_end {} {
    set ::lunar::square_dragging 0
    after idle lunar::square_correct
}

proc lunar::force_square_window {} {
    if {[llength [info commands ::lunar::frame_metrics]]} {
        if {[catch {
            set metrics [::lunar::frame_metrics [lunar::hwnd]]
            set ::lunar::square_extra_w [dict get $metrics extraWidth]
            set ::lunar::square_extra_h [dict get $metrics extraHeight]
            set ::lunar::square_work_x [dict get $metrics workX]
            set ::lunar::square_work_y [dict get $metrics workY]
            set ::lunar::square_work_w [dict get $metrics workWidth]
            set ::lunar::square_work_h [dict get $metrics workHeight]
            ::lunar::watch_resize [lunar::hwnd]
            set w [winfo width .] ; set h [winfo height .]
            set side [expr {max($w + $::lunar::square_extra_w,
                                $h + $::lunar::square_extra_h)}]
            set ::lunar::square_last_w $w
            set ::lunar::square_last_h $h
            bind . <Configure> { lunar::square_configure }
            lunar::square_apply [expr {$side - $::lunar::square_extra_w}] \
                                [expr {$side - $::lunar::square_extra_h}]
        } err]} {
            lunar::log "square-window setup failed: $err"
        }
    } else {
        catch { wm aspect . 1 1 1 1 }
    }
}

# ---- the dashboard ----------------------------------------------------------
proc lunar::build {} {
    wm title . "Time Actual $::lunar::version"
    # force_square_window below adjusts Tk's own client geometry so that the
    # actual Windows frame is square, including status/title chrome.
    wm geometry . 510x510
    wm minsize . 396 396
    wm resizable . 1 1
    lunar::init_style
    . configure -background $::lunar::PAGE
    # Window icon: the sizes `z icon` renders, so the title bar gets a real
    # 16 px image and the taskbar a real 32 px one instead of a scaled 256.
    catch {
        set rd [file join [file dirname [info script]] resources]
        set imgs {}
        foreach s {16 24 32 48 256} {
            set f [file join $rd icon-$s.png]
            if {[file exists $f]} { lappend imgs [image create photo -file $f] }
        }
        if {![llength $imgs]} { lappend imgs [image create photo -file [file join $rd icon.png]] }
        wm iconphoto . -default {*}$imgs
    }

    # Keep the power-user shortcuts, but leave the clock itself free of a
    # desktop-style menu bar. All discoverable controls live behind the
    # settings icon in the status bar.
    bind . <Control-r>     { catch { ::lunar::syncnow } ; break }
    bind . <Control-c>     { lunar::copy_time ; break }
    bind . <Control-comma> { lunar::settings_dlg ; break }

    set P $::lunar::PAGE
    frame .face -bg $P
    if {[llength [info commands ::lunarclock]]} {
        ::lunarclock .face.clock -width $::lunar::CLOCK_SZ -height $::lunar::CLOCK_SZ
    } else {
        canvas .face.clock -width $::lunar::CLOCK_SZ -height $::lunar::CLOCK_SZ \
            -bg $P -highlightthickness 0
        lunar::clock_face_static .face.clock
        bind .face.clock <Configure> { after idle [list lunar::clock_face_static %W] }
    }
    pack .face.clock -fill both -expand 1 -padx 16 -pady 16
    bind .face.clock <ButtonRelease-1> { lunar::clock_marker_click %W %x %y }

    # Stable left-to-right status summary: trust, uncertainty, SYS witness,
    # then the selected display zone. The face can be read at a glance; this
    # row answers the audit questions without repeating decorative UI.
    frame .sb -bg $::lunar::CHROME
    frame .sb.hair -height 1 -bg $::lunar::HAIR
    label .sb.trust -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUIb \
        -anchor w -text ""
    label .sb.bound -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI \
        -anchor w -text ""
    label .sb.sys   -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI \
        -anchor w -text ""
    label .sb.zone  -bg $::lunar::CHROME -fg $::lunar::MUTED -font lunarUI \
        -anchor e -text ""
    ttk::button .sb.settings -style Status.TButton -text "⚙" -width 2 \
        -takefocus 1 -command lunar::settings_dlg
    foreach n {1 2 3} {
        label .sb.sep$n -bg $::lunar::CHROME -fg $::lunar::HAIR -font lunarUI \
            -text "·"
    }
    # The stretch lives in an EMPTY spacer column (6), never in a text cell:
    # every text cell keeps its natural width, so a width shortfall can never
    # clip the zone mid-glyph into a fake-looking code ("JTC+2"). Render hides
    # redundant cells (and their dots) so the row fits with room to spare.
    grid .sb.hair  -row 0 -column 0 -columnspan 9 -sticky ew
    grid .sb.trust -row 1 -column 0 -sticky w  -padx {12 6} -pady {6 7}
    grid .sb.sep1  -row 1 -column 1 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.bound -row 1 -column 2 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.sep2  -row 1 -column 3 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.sys   -row 1 -column 4 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.sep3  -row 1 -column 6 -sticky w  -padx {0 6}  -pady {6 7}
    grid .sb.zone  -row 1 -column 7 -sticky e  -padx {0 6} -pady {6 7}
    grid .sb.settings -row 1 -column 8 -sticky e -padx {0 6} -pady {3 4}
    # The stretch (col 5) sits BEFORE the zone's separator dot, so the dot
    # travels with the zone instead of dangling after the left-hand cells.
    grid columnconfigure .sb 5 -weight 1

    pack .sb   -side bottom -fill x
    pack .face -fill both -expand 1

    wm protocol . WM_DELETE_WINDOW lunar::quit
    # Let Tk map and decorate the top-level before measuring its frame insets.
    after 50 lunar::force_square_window
}

# ---- settings actions: copy time, always-on-top, log viewer -----------------
proc lunar::apply_ontop {} { catch { wm attributes . -topmost $::lunar::ontop } }

proc lunar::copy_time {} {
    if {![llength [info commands ::lunar::status]]} { return 0 }
    set st [::lunar::status]
    if {![dict get $st hasTime]} { lunar::status_note "no trusted time yet" ; return 0 }
    if {[catch { ::lunar::localtime [dict get $st utcMs] $::lunar::tz } lt]} { return 0 }
    set off [dict get $lt offSec]
    set sign [expr {$off < 0 ? "-" : "+"}] ; set a [expr {abs($off)}]
    set iso [format "%04d-%02d-%02dT%02d:%02d:%02d%s%02d:%02d" \
        [dict get $lt year] [dict get $lt month] [dict get $lt day] \
        [dict get $lt hour] [dict get $lt minute] [dict get $lt second] \
        $sign [expr {$a / 3600}] [expr {($a % 3600) / 60}]]
    clipboard clear ; clipboard append $iso
    lunar::status_note "copied $iso"
    return 1
}

# ---- Event Log dialog --------------------------------------------------------
# A sortable, filterable table over the unified event store. Columns are
# Time | Category | Message; severity is carried by row colour (error =
# ACCENT, warn = WARN) and stays filterable -- a fourth column would
# repeat what the colour already says. History is whatever the rolling
# store holds (across sessions); the view caps at the newest 2000
# matching rows so rebuilds stay instant.

set ::lunar::LOG_VIEW_MAX 2000

proc lunar::events_fmt_time {wallMs trusted} {
    set frac [expr {$wallMs % 1000}]
    set secs [expr {$wallMs / 1000}]
    if {$frac < 0} { incr frac 1000 ; incr secs -1 }
    set txt ""
    if {[llength [info commands ::lunar::localtime]] && $::lunar::tz ne ""} {
        if {![catch { ::lunar::localtime $wallMs $::lunar::tz } lt]} {
            set txt [format "%04d-%02d-%02d %02d:%02d:%02d.%03d" \
                [dict get $lt year] [dict get $lt month] [dict get $lt day] \
                [dict get $lt hour] [dict get $lt minute] [dict get $lt second] $frac]
        }
    }
    if {$txt eq ""} {
        set txt "[clock format $secs -format {%Y-%m-%d %H:%M:%S}].[format %03d $frac]"
    }
    if {!$trusted} { append txt "~" }
    return $txt
}

# The Event Log is its own resizable window (Settings -> Application ->
# "Open event log", or LUNAR_OPEN_LOG): a log wants space, and the
# Settings dialog is deliberately compact.
proc lunar::log_dlg {} {
    if {[winfo exists .log]} { raise .log ; focus .log ; lunar::log_refresh ; return }
    set P $::lunar::PAGE
    toplevel .log -bg $P
    wm title .log "Time Actual — Event Log"
    wm transient .log .
    set ::lunar::log_sort {time 0}
    # generous default -- roomy message column, ~26 rows -- and resizable;
    # all sizing from font metrics so any DPI scale gets the same shape
    set chw   [font measure lunarMono "0"]
    set rowh  [expr {[font metrics lunarMono -linespace] + 6}]
    set timeW [expr {[font measure lunarMono "2026-07-05 17:51:12.345~"] + 18}]
    set catW  [expr {[font measure lunarMono "pinstore"] + 18}]
    set lvlW  [expr {2 * $chw + 10}]
    set W [expr {$lvlW + $timeW + $catW + 72 * $chw + 3 * $rowh}]
    set H [expr {30 * $rowh}]
    wm geometry .log ${W}x${H}
    wm minsize .log [expr {$lvlW + $timeW + $catW + 24 * $chw}] [expr {14 * $rowh}]

    frame .log.inner -bg $P
    pack .log.inner -fill both -expand 1 -padx 14 -pady 12
    lunar::log_panel .log.inner [expr {$W - 28}] [expr {$H - 24}]

    # footer: legend left, actions right (the panel owns filters + table)
    frame .log.inner.bar -bg $P ; pack .log.inner.bar -fill x -pady {10 0}
    label .log.inner.bar.legend -bg $P -fg $::lunar::MUTED -font lunarSmall \
        -anchor w -text "E error · W warning · ~ approximate time"
    ttk::button .log.inner.bar.close -style Dialog.TButton -text "Close" \
        -command {destroy .log}
    ttk::button .log.inner.bar.copy -style Dialog.TButton -text "Copy" \
        -command lunar::log_copy
    pack .log.inner.bar.legend -side left
    pack .log.inner.bar.close -side right
    pack .log.inner.bar.copy -side right -padx {0 8}

    bind .log <Escape> {destroy .log}
    bind .log <Destroy> { if {"%W" eq ".log"} { lunar::log_cleanup } }
    catch { after cancel $::lunar::log_loop_after }
    set ::lunar::log_loop_after [after 1000 lunar::log_refresh_loop]
}

proc lunar::log_cleanup {} {
    set ::lunar::log_root ""
    catch { after cancel $::lunar::log_loop_after }
    set ::lunar::log_loop_after {}
    catch { after cancel $::lunar::log_fit_after }
    set ::lunar::log_fit_after {}
}

# Build the filter row + table into $parent. targetW/targetH are the
# initial pixel budget; the window is resizable, so the message-column
# fit is re-derived from the treeview's REAL width on every <Configure>.
proc lunar::log_panel {parent targetW targetH} {
    set P $::lunar::PAGE
    set ::lunar::log_root $parent
    # All sizing derives from the mono font so the table survives any DPI
    # scale (raw pixel widths clip the Time column at 150%).
    set chw   [font measure lunarMono "0"]
    set rowh  [expr {[font metrics lunarMono -linespace] + 6}]
    set lvlW  [expr {2 * $chw + 10}]
    set timeW [expr {[font measure lunarMono "2026-07-05 17:51:12.345~"] + 18}]
    set catW  [expr {[font measure lunarMono "pinstore"] + 18}]
    # the Message column is re-fitted per refresh: viewport-filling when
    # everything fits, content-wide (with the h-scrollbar re-appearing)
    # only when a shown row actually overflows
    # the true remainder, NEVER floored upward: the h-scrollbar decision
    # compares content width against this, and a floor above the real
    # viewport would hide the scrollbar while text is still clipped
    set ::lunar::log_chw $chw
    set ::lunar::log_fixedW [expr {$lvlW + $timeW + $catW}]
    set ::lunar::log_msg_fit [expr {$targetW - $::lunar::log_fixedW - 3 * $rowh}]
    if {$::lunar::log_msg_fit < 60} { set ::lunar::log_msg_fit 60 }
    set rows  [expr {($targetH - 5 * $rowh) / $rowh}]
    if {$rows < 8} { set rows 8 }
    set ::lunar::log_q_ph 0   ;# the flag describes an entry that no longer exists

    frame $parent.top -bg $P ; pack $parent.top -fill x -pady {0 10}
    ttk::entry $parent.top.q -width 14
    lunar::log_q_placeholder
    ttk::combobox $parent.top.lvl -state readonly -width 9 \
        -values [list "All levels" Warn+ Errors]
    ttk::combobox $parent.top.cat -state readonly -width 13 \
        -values [list "All categories"]
    $parent.top.lvl set "All levels" ; $parent.top.cat set "All categories"
    label $parent.top.count -bg $P -fg $::lunar::MUTED -font lunarSmall \
        -anchor e -padx 0
    pack $parent.top.q     -side left
    pack $parent.top.lvl   -side left -padx {8 0}
    pack $parent.top.cat   -side left -padx {8 0}
    pack $parent.top.count -side right -fill x -expand 1
    bind $parent.top.q <FocusIn>  lunar::log_q_focus
    bind $parent.top.q <FocusOut> lunar::log_q_placeholder
    bind $parent.top.q <KeyRelease> {
        after cancel $::lunar::log_filter_after
        set ::lunar::log_filter_after [after 150 lunar::log_refresh]
    }
    bind $parent.top.lvl <<ComboboxSelected>> lunar::log_refresh
    bind $parent.top.cat <<ComboboxSelected>> lunar::log_refresh

    frame $parent.f -bg $P ; pack $parent.f -fill both -expand 1
    ttk::treeview $parent.f.tv -style Log.Treeview -show headings \
        -columns {lvl time cat msg} -selectmode extended -height $rows \
        -yscrollcommand [list $parent.f.vs set] \
        -xscrollcommand [list $parent.f.hs set]
    $parent.f.tv column lvl  -width $lvlW  -minwidth $lvlW  -stretch 0 -anchor w
    $parent.f.tv column time -width $timeW -minwidth $timeW -stretch 0 -anchor w
    $parent.f.tv column cat  -width $catW  -minwidth $catW  -stretch 0 -anchor w
    $parent.f.tv column msg  -width $::lunar::log_msg_fit \
        -minwidth [expr {24 * $chw}] -stretch 0 -anchor w
    foreach {c t} {lvl "" time Time cat Category msg Message} {
        $parent.f.tv heading $c -text $t -anchor w \
            -command [list lunar::log_sortby $c]
    }
    $parent.f.tv tag configure error -foreground $::lunar::ACCENT
    $parent.f.tv tag configure warn  -foreground $::lunar::INK -background "#E6E6E6"
    ttk::scrollbar $parent.f.vs -orient vertical   -command [list $parent.f.tv yview]
    ttk::scrollbar $parent.f.hs -orient horizontal -command [list $parent.f.tv xview]
    grid $parent.f.tv -row 0 -column 0 -sticky nsew
    grid $parent.f.vs -row 0 -column 1 -sticky ns
    grid $parent.f.hs -row 1 -column 0 -sticky ew
    grid rowconfigure    $parent.f 0 -weight 1
    grid columnconfigure $parent.f 0 -weight 1

    # snap to the newest row once the tree first becomes visible (the
    # refresh's deferred [see] no-ops against unmapped geometry), and
    # re-derive the message fit whenever the window is resized
    set ::lunar::log_mapped 0
    bind $parent.f.tv <Map> {+ if {!$::lunar::log_mapped} {
        set ::lunar::log_mapped 1 ; after idle lunar::log_see_end } }
    bind $parent.f.tv <Configure> {
        after cancel $::lunar::log_fit_after
        set ::lunar::log_fit_after [after 80 lunar::log_refit]
    }
    lunar::log_refresh
}

# Recompute the viewport-filling message width from the treeview's real
# width (the dialog is resizable), then re-apply the fit decision.
proc lunar::log_refit {} {
    set r $::lunar::log_root
    if {$r eq "" || ![winfo exists $r.f.tv]} return
    set w [winfo width $r.f.tv]
    if {$w < 80} return
    set fit [expr {$w - $::lunar::log_fixedW - 4}]
    if {$fit < 60} { set fit 60 }
    if {$fit == $::lunar::log_msg_fit} return
    set ::lunar::log_msg_fit $fit
    lunar::log_fit_msg
}

# Width + h-scrollbar decision from the widest row currently shown:
# viewport-filling when everything fits (no scrollbar), content-wide
# when a row overflows (scrollbar reaches the tail).
proc lunar::log_fit_msg {} {
    set r $::lunar::log_root
    if {$r eq "" || ![winfo exists $r.f.tv]} return
    set needW [expr {$::lunar::log_maxlen * $::lunar::log_chw + 12}]
    if {$needW > $::lunar::log_msg_fit} {
        $r.f.tv column msg -width $needW
        grid $r.f.hs
    } else {
        $r.f.tv column msg -width $::lunar::log_msg_fit
        grid remove $r.f.hs
    }
}

proc lunar::log_see_end {} {
    set r $::lunar::log_root
    if {$r eq "" || ![winfo exists $r.f.tv]} return
    lassign $::lunar::log_sort col desc
    if {$col ne "time" || $desc} return
    set kids [$r.f.tv children {}]
    if {[llength $kids]} { catch { $r.f.tv see [lindex $kids end] } }
}

# Slate placeholder in the search entry: cleared on focus, restored when
# it loses focus empty. log_q reads "" while the placeholder shows.
proc lunar::log_q_focus {} {
    set q $::lunar::log_root.top.q
    if {$::lunar::log_q_ph} {
        set ::lunar::log_q_ph 0
        $q delete 0 end
        $q configure -foreground $::lunar::INK
    }
}
proc lunar::log_q_placeholder {} {
    set q $::lunar::log_root.top.q
    if {![winfo exists $q]} return
    if {!$::lunar::log_q_ph && [$q get] eq ""} {
        set ::lunar::log_q_ph 1
        $q configure -foreground $::lunar::MUTED
        $q insert 0 "filter messages"
    }
}
proc lunar::log_q {} {
    set q $::lunar::log_root.top.q
    if {$::lunar::log_q_ph || ![winfo exists $q]} { return "" }
    return [$q get]
}
proc lunar::log_q_set {text} {
    set q $::lunar::log_root.top.q
    set ::lunar::log_q_ph 0
    $q configure -foreground $::lunar::INK
    $q delete 0 end
    $q insert 0 $text
}

# Column-header sort: click toggles direction on the active column.
proc lunar::log_sortby {col} {
    lassign $::lunar::log_sort cur desc
    set ::lunar::log_sort [list $col [expr {$col eq $cur ? !$desc : 0}]]
    lunar::log_refresh
}

# The current filtered+sorted view: a list of {wallMs trusted sev cat msg}
# in display order, capped at the newest LOG_VIEW_MAX matches. Kept in
# ::lunar::log_view so Copy exports exactly what the table shows.
proc lunar::log_refresh {} {
    set r $::lunar::log_root
    if {$r eq "" || ![winfo exists $r.f.tv]} return
    set q   [string tolower [lunar::log_q]]
    set lvl [$r.top.lvl get]
    set fcat [$r.top.cat get]
    set minrank [expr {$lvl eq "Errors" ? 2 : ($lvl eq "Warn+" ? 1 : 0)}]
    set ranks [dict create info 0 warn 1 error 2]

    set match {}
    set cats [dict create]
    foreach rec $::lunar::events {
        lassign $rec wallMs trusted sev cat msg
        dict set cats $cat 1
        if {[dict getdef $ranks $sev 0] < $minrank} continue
        if {$fcat ne "All categories" && $cat ne $fcat} continue
        if {$q ne "" && [string first $q [string tolower "$cat $msg"]] < 0} continue
        lappend match $rec
    }
    set total [llength $match]
    set clipped 0
    if {$total > $::lunar::LOG_VIEW_MAX} {
        set match [lrange $match end-[expr {$::lunar::LOG_VIEW_MAX - 1}] end]
        set clipped 1
    }

    # sort (stable, so equal keys keep honest insertion order)
    lassign $::lunar::log_sort col desc
    set keyed {}
    foreach rec $match {
        lassign $rec wallMs trusted sev cat msg
        switch $col {
            lvl     { set key [dict getdef $ranks $sev 0] }
            cat     { set key $cat }
            msg     { set key $msg }
            default { set key $wallMs }
        }
        lappend keyed [list $key $rec]
    }
    set opts [expr {$col in {time lvl} ? "-integer" : "-dictionary"}]
    if {$desc} { lappend opts -decreasing }
    set keyed [lsort {*}$opts -index 0 $keyed]

    # heading arrows on the active column only
    foreach {c t} {lvl "" time Time cat Category msg Message} {
        set mark [expr {$c eq $col ? ($desc ? "▼" : "▲") : ""}]
        set cap [string trim "$t $mark"]
        $r.f.tv heading $c -text $cap
    }

    # lazily refresh the category filter's choices
    set vals [linsert [lsort [dict keys $cats]] 0 "All categories"]
    $r.top.cat configure -values $vals
    if {$fcat ni $vals} { $r.top.cat set "All categories" }

    # rebuild rows; keep the old dialog's implicit stick-to-bottom under
    # chronological ascending order. The severity glyph column survives
    # Copy/paste and colour-blindness; row colour stays the fast channel.
    set atbottom [expr {[lindex [$r.f.tv yview] 1] > 0.999}]
    $r.f.tv delete [$r.f.tv children {}]
    set ::lunar::log_view {}
    set maxlen 0
    foreach pair $keyed {
        set rec [lindex $pair 1]
        lassign $rec wallMs trusted sev cat msg
        lappend ::lunar::log_view $rec
        if {[string length $msg] > $maxlen} { set maxlen [string length $msg] }
        set glyph [expr {$sev eq "error" ? "E" : ($sev eq "warn" ? "W" : "")}]
        $r.f.tv insert {} end -tags [list $sev] -values \
            [list $glyph [lunar::events_fmt_time $wallMs $trusted] $cat $msg]
    }
    set ::lunar::log_maxlen $maxlen
    lunar::log_fit_msg
    if {$atbottom && $col eq "time" && !$desc} {
        set kids [$r.f.tv children {}]
        if {[llength $kids]} {
            # after idle: gridding/removing the h-scrollbar above changes
            # the tree's height AFTER this proc returns; a synchronous
            # [see] would scroll against stale geometry and leave the
            # newest row hidden behind the new scrollbar, permanently
            # unlatching stick-to-bottom
            after idle [list catch [list $r.f.tv see [lindex $kids end]]]
        }
    }
    set n [llength $::lunar::log_view]
    set total_store [llength $::lunar::events]
    if {$clipped} {
        $r.top.count configure -text "newest $n of $total matching"
    } elseif {$n == 0 && $total_store > 0} {
        $r.top.count configure -text "no matches · $total_store hidden by filter"
    } else {
        $r.top.count configure -text "$n of $total_store"
    }
    set ::lunar::events_dirty 0
}

# Runs while the Event Log window exists; rebuilds only when something
# new arrived. The single token keeps rapid close/reopen cycles from
# stacking parallel chains.
proc lunar::log_refresh_loop {} {
    set r $::lunar::log_root
    if {$r eq "" || ![winfo exists $r]} return
    if {$::lunar::events_dirty} { lunar::log_refresh }
    set ::lunar::log_loop_after [after 1000 lunar::log_refresh_loop]
}

# Copies the view exactly as filtered and sorted; the severity word is
# spelled out so it survives into plain text (colour doesn't paste).
proc lunar::log_copy {} {
    set out ""
    foreach rec $::lunar::log_view {
        lassign $rec wallMs trusted sev cat msg
        append out [format "%s  %-5s  %-9s  %s\n" \
            [lunar::events_fmt_time $wallMs $trusted] $sev $cat $msg]
    }
    clipboard clear ; clipboard append $out
    lunar::status_note "event log copied ([llength $::lunar::log_view] rows)"
}

# ---- Settings dialog ---------------------------------------------------------
# The status-bar gear is the clock's one persistent control. Everything that
# used to live in the desktop-style menu is gathered here. Preferences are
# staged until Save; utility actions act immediately and report inline.
proc lunar::settings_dlg {{tab ""}} {
    if {[wm state .] in {withdrawn iconic}} { lunar::restore }
    if {[winfo exists .set]} {
        if {$tab ne ""} { catch { .set.shell.tabs select .set.shell.tabs.$tab } }
        wm deiconify .set
        raise .set
        focus .set
        return
    }

    # A settings window stages every preference until Save. Remove a trace left
    # by an interrupted development run before initializing the staged values.
    catch { trace remove variable ::lunar::set_filter write ::lunar::settings_filter_changed }
    set ::lunar::set_filter  ""
    set ::lunar::set_tz      $::lunar::tz
    set ::lunar::set_fmt24   [dict get $::lunar::cfg fmt24]
    set ::lunar::set_confirm [dict get $::lunar::cfg confirm]
    set ::lunar::set_startup [dict get $::lunar::cfg startup]
    set ::lunar::set_chimes  [dict get $::lunar::cfg chimes]
    set ::lunar::set_unmin   [dict get $::lunar::cfg unmin]
    set ::lunar::set_ontop   $::lunar::ontop
    set ::lunar::set_stopsec [expr {[dict get $::lunar::cfg stopms] / 1000}]
    for {set i 0} {$i < 12} {incr i} {
        set ::lunar::set_armed($i) [lindex $::lunar::armed $i]
    }

    set P $::lunar::PAGE
    set I $::lunar::INK
    set M $::lunar::MUTED
    toplevel .set -bg $P
    wm withdraw .set
    wm title .set "Time Actual Settings"
    wm transient .set .
    wm resizable .set 0 0

    frame .set.shell -bg $P
    pack .set.shell -fill both -expand 1

    ttk::notebook .set.shell.tabs -style Settings.TNotebook
    foreach {name title} {clock Clock chimes Chimes app Application} {
        frame .set.shell.tabs.$name -bg $P
        frame .set.shell.tabs.$name.inner -bg $P
        pack .set.shell.tabs.$name.inner -fill both -expand 1 -padx 24 -pady 18
        .set.shell.tabs add .set.shell.tabs.$name -text $title
    }
    pack .set.shell.tabs -fill both -expand 1 -pady {10 0}

    # Clock -------------------------------------------------------------------
    set c .set.shell.tabs.clock.inner
    label $c.zhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Display time zone"
    label $c.zhelp -bg $P -fg $M -font lunarUI -anchor w \
        -text "Search the embedded IANA time-zone database."
    ttk::entry $c.filter -font lunarUI -width 55 -textvariable ::lunar::set_filter
    frame $c.zl -bg $P
    listbox $c.zl.list -height 8 -width 55 -font lunarUI -bg $P -fg $I \
        -selectbackground "#D6E2F2" -selectforeground $I \
        -borderwidth 1 -relief solid -highlightthickness 0 -exportselection 0 \
        -yscrollcommand [list $c.zl.vs set]
    ttk::scrollbar $c.zl.vs -orient vertical -command [list $c.zl.list yview]
    pack $c.zl.vs -side right -fill y
    pack $c.zl.list -side left -fill both -expand 1
    label $c.zprev -bg $P -fg $M -font lunarUI -anchor w -text ""
    checkbutton $c.fmt24 -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P -command lunar::settings_preview \
        -text "Use 24-hour time" -variable ::lunar::set_fmt24
    frame $c.stop -bg $P
    label $c.stop.l -bg $P -fg $I -font lunarUI -anchor w \
        -text "Stop the clock when uncertainty exceeds"
    ttk::spinbox $c.stop.n -from 1 -to 30 -increment 1 -width 4 -font lunarUI \
        -textvariable ::lunar::set_stopsec
    label $c.stop.u -bg $P -fg $M -font lunarUI -text "seconds"
    pack $c.stop.l -side left
    pack $c.stop.n -side left -padx {8 4}
    pack $c.stop.u -side left
    frame $c.rule -bg $::lunar::HAIR -height 1
    label $c.ahdr -bg $P -fg $I -font lunarUIb -anchor w -text "Clock actions"
    frame $c.actions -bg $P
    ttk::button $c.actions.sync -style Dialog.TButton -text "Sync now" \
        -command lunar::settings_sync_now
    ttk::button $c.actions.copy -style Dialog.TButton -text "Copy current time" \
        -command lunar::settings_copy_time
    pack $c.actions.sync -side left
    pack $c.actions.copy -side left -padx {8 0}
    label $c.actionnote -bg $P -fg $M -font lunarSmall -anchor w -text ""

    pack $c.zhdr -fill x
    pack $c.zhelp -fill x -pady {2 9}
    pack $c.filter -fill x -pady {0 7}
    pack $c.zl -fill x
    pack $c.zprev -fill x -pady {6 0}
    pack $c.fmt24 -fill x -pady {12 0}
    pack $c.stop -fill x -pady {8 0}
    pack $c.rule -fill x -pady {16 14}
    pack $c.ahdr -fill x -pady {0 8}
    pack $c.actions -fill x
    pack $c.actionnote -fill x -pady {6 0}

    # Chimes ------------------------------------------------------------------
    set c .set.shell.tabs.chimes.inner
    label $c.hdr -bg $P -fg $I -font lunarUIb -anchor w -text "Chimes"
    label $c.help -bg $P -fg $M -font lunarUI -anchor w -justify left \
        -text "Choose which five-minute marks should sound. You can also toggle\na chime by clicking its marker directly on the clock dial."
    checkbutton $c.enabled -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Play a chime on armed marks" -variable ::lunar::set_chimes
    checkbutton $c.unmin -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Restore the clock window on armed marks" -variable ::lunar::set_unmin
    frame $c.rule1 -bg $::lunar::HAIR -height 1
    label $c.mhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Armed five-minute marks"
    frame $c.marks -bg $P
    for {set i 0} {$i < 12} {incr i} {
        checkbutton $c.marks.m$i -bg $P -fg $I -font lunarMono \
            -activebackground $P -selectcolor $P -padx 3 \
            -text [format ":%02d" [expr {$i * 5}]] -variable ::lunar::set_armed($i)
        grid $c.marks.m$i -row [expr {$i / 6}] -column [expr {$i % 6}] \
            -sticky w -padx {0 15} -pady 2
    }
    frame $c.rule2 -bg $::lunar::HAIR -height 1
    label $c.thdr -bg $P -fg $I -font lunarUIb -anchor w -text "Sound check"
    frame $c.actions -bg $P
    ttk::button $c.actions.test -style Dialog.TButton -text "Test chime" \
        -command lunar::settings_test_chime
    pack $c.actions.test -side left
    label $c.actionnote -bg $P -fg $M -font lunarSmall -anchor w -text ""

    pack $c.hdr -fill x
    pack $c.help -fill x -pady {2 12}
    pack $c.enabled -fill x
    pack $c.unmin -fill x -pady {3 0}
    pack $c.rule1 -fill x -pady {16 14}
    pack $c.mhdr -fill x -pady {0 7}
    pack $c.marks -fill x
    pack $c.rule2 -fill x -pady {16 14}
    pack $c.thdr -fill x -pady {0 8}
    pack $c.actions -fill x
    pack $c.actionnote -fill x -pady {6 0}

    # Application -------------------------------------------------------------
    set c .set.shell.tabs.app.inner
    label $c.bhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Window and startup"
    checkbutton $c.ontop -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Keep Time Actual above other windows" -variable ::lunar::set_ontop
    checkbutton $c.confirm -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Confirm before closing" -variable ::lunar::set_confirm
    checkbutton $c.startup -bg $P -fg $I -font lunarUI -anchor w \
        -activebackground $P -selectcolor $P \
        -text "Start Time Actual when I sign in" -variable ::lunar::set_startup
    frame $c.rule1 -bg $::lunar::HAIR -height 1
    label $c.uhdr -bg $P -fg $I -font lunarUIb -anchor w -text "Diagnostics"
    frame $c.actions -bg $P
    ttk::button $c.actions.log -style Dialog.TButton -text "Open event log" \
        -command lunar::log_dlg
    pack $c.actions.log -side left
    label $c.actionnote -bg $P -fg $M -font lunarSmall -anchor w -text ""
    frame $c.rule2 -bg $::lunar::HAIR -height 1

    set about [dict create version $::lunar::version tzdata unknown]
    if {[llength [info commands ::lunar::about]]} { catch { set about [::lunar::about] } }
    label $c.abouttitle -bg $P -fg $I -font lunarUIb -anchor w \
        -text "Time Actual [dict get $about version]"
    label $c.aboutbody -bg $P -fg $M -font lunarUI -anchor w -justify left \
        -text "An analog clock disciplined by authenticated network time.\nEmbedded IANA time-zone data: [dict get $about tzdata]  ·  MIT License"
    ttk::button $c.quit -style Dialog.TButton -text "Quit Lunar" -command lunar::quit

    pack $c.bhdr -fill x -pady {0 7}
    pack $c.ontop -fill x
    pack $c.confirm -fill x -pady {3 0}
    pack $c.startup -fill x -pady {3 0}
    pack $c.rule1 -fill x -pady {16 14}
    pack $c.uhdr -fill x -pady {0 8}
    pack $c.actions -fill x
    pack $c.actionnote -fill x -pady {6 0}
    pack $c.rule2 -fill x -pady {16 14}
    pack $c.abouttitle -fill x
    pack $c.aboutbody -fill x -pady {3 12}
    pack $c.quit -anchor w

    # Shared footer -----------------------------------------------------------
    frame .set.shell.footrule -bg $::lunar::HAIR -height 1
    frame .set.shell.foot -bg $P
    ttk::button .set.shell.foot.save -style Dialog.TButton -text "Save" \
        -command lunar::settings_ok
    ttk::button .set.shell.foot.cancel -style Dialog.TButton -text "Cancel" \
        -command lunar::settings_close
    pack .set.shell.foot.cancel -side right
    pack .set.shell.foot.save -side right -padx {0 8}
    pack .set.shell.footrule -fill x
    pack .set.shell.foot -fill x -padx 24 -pady 14

    trace add variable ::lunar::set_filter write ::lunar::settings_filter_changed
    bind .set.shell.tabs.clock.inner.zl.list <<ListboxSelect>> lunar::settings_zone_selected
    bind .set <Escape> { lunar::settings_close }
    bind .set <Destroy> { if {"%W" eq ".set"} { lunar::settings_cleanup } }
    wm protocol .set WM_DELETE_WINDOW lunar::settings_close
    lunar::settings_fill
    if {$tab ne ""} { catch { .set.shell.tabs select .set.shell.tabs.$tab } }

    update idletasks
    set x [expr {[winfo rootx .] + ([winfo width .]  - [winfo reqwidth .set]) / 2}]
    set y [expr {[winfo rooty .] + ([winfo height .] - [winfo reqheight .set]) / 3}]
    wm geometry .set +$x+$y
    wm deiconify .set
    raise .set
    if {$tab ne ""} { catch { .set.shell.tabs select .set.shell.tabs.$tab } }
    switch $tab {
        chimes { focus .set.shell.tabs.chimes.inner.enabled }
        app    { focus .set.shell.tabs.app.inner.ontop }
        default { focus .set.shell.tabs.clock.inner.filter }
    }
    lunar::settings_preview_loop
}

proc lunar::settings_cleanup {} {
    if {$::lunar::settings_preview_after ne ""} {
        catch { after cancel $::lunar::settings_preview_after }
        set ::lunar::settings_preview_after ""
    }
    catch { trace remove variable ::lunar::set_filter write ::lunar::settings_filter_changed }
}

proc lunar::settings_close {} {
    lunar::settings_cleanup
    catch { destroy .set }
}

proc lunar::settings_filter_changed {args} { lunar::settings_fill }

# Repopulate the zone list from the filter while retaining the staged zone.
proc lunar::settings_fill {} {
    set lb .set.shell.tabs.clock.inner.zl.list
    if {![winfo exists $lb]} return
    set pat [string tolower [string trim $::lunar::set_filter]]
    $lb delete 0 end
    set sel -1
    set i 0
    set zones [expr {[llength [info commands ::lunar::tz_list]] ? [::lunar::tz_list] : {UTC}}]
    foreach z $zones {
        if {$pat ne "" && ![string match "*$pat*" [string tolower $z]]} continue
        $lb insert end $z
        if {$z eq $::lunar::set_tz} { set sel $i }
        incr i
    }
    if {$sel >= 0} { $lb selection set $sel ; $lb see $sel }
    lunar::settings_preview
}

proc lunar::settings_zone_selected {} {
    set lb .set.shell.tabs.clock.inner.zl.list
    if {![winfo exists $lb]} return
    set s [$lb curselection]
    if {$s ne ""} { set ::lunar::set_tz [$lb get [lindex $s 0]] }
    lunar::settings_preview
}

proc lunar::settings_sel {} {
    if {[info exists ::lunar::set_tz]} { return $::lunar::set_tz }
    return ""
}

proc lunar::settings_preview {} {
    set pv .set.shell.tabs.clock.inner.zprev
    if {![winfo exists $pv]} return
    set z [lunar::settings_sel]
    if {$z eq ""} { $pv configure -text "" ; return }
    set txt "Selected: $z  ·  Current time unavailable"
    if {[llength [info commands ::lunar::status]]} {
        set st [::lunar::status]
        if {[dict get $st hasTime] &&
            ![catch { ::lunar::localtime [dict get $st utcMs] $z } lt]} {
            set h [dict get $lt hour]
            if {$::lunar::set_fmt24} {
                set tm [format "%02d:%02d:%02d" $h [dict get $lt minute] [dict get $lt second]]
            } else {
                set ap [expr {$h >= 12 ? "PM" : "AM"}]
                set h [expr {$h % 12}] ; if {$h == 0} { set h 12 }
                set tm [format "%d:%02d:%02d %s" $h [dict get $lt minute] [dict get $lt second] $ap]
            }
            set txt "Selected: $z  ·  $tm [dict get $lt abbr] ([lunar::fmt_utcoff [dict get $lt offSec]])"
        }
    }
    $pv configure -text $txt
}

proc lunar::settings_preview_loop {} {
    set ::lunar::settings_preview_after ""
    if {![winfo exists .set]} return
    lunar::settings_preview
    set ::lunar::settings_preview_after [after 500 lunar::settings_preview_loop]
}

proc lunar::settings_note {page text} {
    set w .set.shell.tabs.$page.inner.actionnote
    if {[winfo exists $w]} { $w configure -text $text }
}

proc lunar::settings_sync_now {} {
    if {![llength [info commands ::lunar::syncnow]]} {
        lunar::settings_note clock "Sync is unavailable in this build."
        return
    }
    if {[catch { ::lunar::syncnow } err]} {
        lunar::settings_note clock "Sync could not be started."
        lunar::log "\[settings sync\] $err"
    } else {
        lunar::settings_note clock "Sync requested."
    }
}

proc lunar::settings_copy_time {} {
    if {[lunar::copy_time]} {
        lunar::settings_note clock "Current time copied to the clipboard."
    } else {
        lunar::settings_note clock "Current time is not available yet."
    }
}

proc lunar::settings_test_chime {} {
    if {![llength [info commands ::lunar::beep]] || [catch { ::lunar::beep } err]} {
        lunar::settings_note chimes "The chime could not be played."
        if {[info exists err] && $err ne ""} { lunar::log "\[settings chime\] $err" }
    } else {
        lunar::settings_note chimes "Chime played."
    }
}

proc lunar::settings_ok {} {
    # Validate the one external setting first. A registry failure leaves every
    # staged preference untouched and keeps the relevant page visible.
    set startup [expr {$::lunar::set_startup ? 1 : 0}]
    if {[llength [info commands ::lunar::run_at_startup]]} {
        if {[catch { ::lunar::run_at_startup } current] ||
            ($current != $startup && [catch { ::lunar::run_at_startup $startup } regerr]) ||
            [catch { ::lunar::run_at_startup } startup] || $startup != $::lunar::set_startup} {
            .set.shell.tabs select .set.shell.tabs.app
            focus .set.shell.tabs.app.inner.startup
            lunar::settings_note app "Could not update the sign-in startup setting."
            if {[info exists regerr] && $regerr ne ""} { lunar::log "\[settings startup\] $regerr" }
            return
        }
    }

    set z [lunar::settings_sel]
    if {$z ne ""} { set ::lunar::tz $z }
    set ::lunar::tz_chosen 1
    # Event Log timestamps render in the display zone; a zone change
    # re-renders an open dialog on its next refresh tick.
    set ::lunar::events_dirty 1
    dict set ::lunar::cfg tz [expr {$::lunar::tz eq "UTC" ? "" : $::lunar::tz}]
    dict set ::lunar::cfg fmt24  [expr {$::lunar::set_fmt24 ? 1 : 0}]
    dict set ::lunar::cfg confirm [expr {$::lunar::set_confirm ? 1 : 0}]
    dict set ::lunar::cfg chimes [expr {$::lunar::set_chimes ? 1 : 0}]
    dict set ::lunar::cfg unmin  [expr {$::lunar::set_unmin ? 1 : 0}]
    if {[string is integer -strict $::lunar::set_stopsec]} {
        dict set ::lunar::cfg stopms [lunar::clamp_stopms [expr {$::lunar::set_stopsec * 1000}]]
    }
    set ::lunar::ontop [expr {$::lunar::set_ontop ? 1 : 0}]
    lunar::apply_ontop
    set a {}
    for {set i 0} {$i < 12} {incr i} { lappend a [expr {$::lunar::set_armed($i) ? 1 : 0}] }
    set ::lunar::armed $a
    lunar::armed_save
    dict set ::lunar::cfg startup $startup
    lunar::settings_save
    lunar::settings_close
}

# nudge the engine to re-sync periodically (defined at top level, not inside
# main: a `proc lunar::x` created from within a ::lunar-namespace proc would
# resolve to ::lunar::lunar::x and fail).
proc lunar::repoll {} {
    set err [catch { ::lunar::syncnow }]
    set next [lunar::next_poll_ms $err]
    set ::lunar::poll_cur $next
    set ::lunar::repoll_after [after $next lunar::repoll]
}

# Recover as fast as possible: cancel the pending scheduled poll and fire
# one now. Called on the stop edge (interval crossed the ceiling) and on
# the time-vanishing edge (hasTime 1 -> 0), so recovery never waits out a
# relaxed interval. Rate-limited to one forced poll per FAST floor; the
# engine's g_running CAS already dedups an in-flight cycle.
proc lunar::poll_now {} {
    set now [clock milliseconds]
    if {$now - $::lunar::poll_forced_at < $::lunar::poll_min} return
    set ::lunar::poll_forced_at $now
    catch { after cancel $::lunar::repoll_after }
    set ::lunar::repoll_after [after 0 lunar::repoll]
}

# Adaptive poll cadence (this is the ENTIRE scheduler; the C engine polls only
# when syncnow calls it). Sync FAST while acquiring or re-anchoring, at the BASE
# rate while merely OK, and RELAX toward the ceiling only once the clock is well
# self-regulated -- consecutive cycles that are TRUST_OK, carry a converged drift
# rate, and where BOTH the core sources and the two NTS anchors agree tightly.
# Any state drop, sync error, or continuity break (state != ok/degraded) snaps
# straight back to FAST, then backs off toward BASE so a sustained outage does
# not hammer the servers. Returns the ms delay until the next poll.
proc lunar::next_poll_ms {syncErr} {
    set fast $::lunar::poll_min
    set base $::lunar::poll_ms
    set max  $::lunar::poll_max
    set unhealthy $syncErr
    set converged 0
    if {!$syncErr && [llength [info commands ::lunar::status]]} {
        if {[catch { ::lunar::status } st]} {
            set unhealthy 1
        } else {
            set state [dict get $st state]
            # Unhealthy = not fully trusted, or the interval has grown past
            # half the stop ceiling (recover BEFORE the face would blank).
            set b [expr {[dict exists $st boundMs] ? [dict get $st boundMs] : 0}]
            if {$state ne "ok" || $b > [dict get $::lunar::cfg stopms] / 2} {
                set unhealthy 1
            }
            if {$state eq "ok" && [dict get $st synced] && [dict get $st ratePpm] != 0} {
                set cs [dict get $st spreadMs]
                set ns [dict get $st ntsSpreadMs]
                if {$cs >= 0 && $cs <= 80 && $ns >= 0 && $ns <= 80} { set converged 1 }
            }
        }
    }
    if {$unhealthy} {
        set ::lunar::poll_good 0
        incr ::lunar::poll_bad
        set iv [expr {$fast * (1 << min($::lunar::poll_bad - 1, 4))}]  ;# 8,16,32,60,60s
        return [expr {$iv > $base ? $base : $iv}]
    }
    set ::lunar::poll_bad 0
    if {!$converged} { set ::lunar::poll_good 0 ; return $base }
    incr ::lunar::poll_good
    if {$::lunar::poll_good < 3} { return $base }                      ;# confirm before relaxing
    set iv [expr {$base * (1 << min($::lunar::poll_good - 3, 8))}]     ;# 60,120,240,480...
    return [expr {$iv > $max ? $max : $iv}]                           ;# capped at 10 min
}

# ---- window state -----------------------------------------------------------
proc lunar::hwnd {} { return [winfo id .] }

proc lunar::restore {} {
    wm deiconify .
    raise .
    catch { focus -force . }
}

# Called from the C window subclass (via ::lunar::window_event) at a safe
# point in the event loop: the native resize-gesture boundaries that drive
# the square-window dance.
proc lunar::window_event {kind} {
    switch $kind {
        resize-start { lunar::square_resize_start }
        resize-end   { lunar::square_resize_end }
    }
}

# Closing stops the disciplined clock, so it asks first (Settings toggle,
# default on). The OS-shutdown path (WM_ENDSESSION in C) never asks.
proc lunar::quit {} {
    if {[dict get $::lunar::cfg confirm]} {
        set answer [tk_messageBox -parent . -type yesno -default yes \
            -icon question -title "Time Actual" -message "Close Time Actual?"]
        if {$answer ne "yes"} return
    }
    catch { if {[llength [info commands ::lunar::shutdown]]} { ::lunar::shutdown } }
    # AFTER shutdown: Ntp_Shutdown/Clock_Shutdown append their own final
    # ring entries ("aggregator stopped", "persisted rate"); the ring
    # stays readable after Shutdown_Cmd, so this last drain captures them.
    catch { lunar::events_drain }
    destroy .
}

# ---- poll loop --------------------------------------------------------------
# The Tcl side feeds the face authoritative disciplined time at five frames
# per second, phase locked to the disciplined clock. The native widget sweeps
# the second hand at ~30 fps on its own timer by extrapolating (at most a few
# hundred ms of QPC) between feeds -- so the Tk event loop never becomes an
# animation loop, and a stalled feed pauses the hand instead of letting the
# display free-run.
proc lunar::schedule_tick {utcMs} {
    set delay 200
    if {$utcMs ne ""} {
        set phase [expr {$utcMs % 200}]
        if {$phase < 0} { set phase [expr {$phase + 200}] }
        set delay [expr {200 - $phase}]
        # Match the old native timer's small guard against a pathological
        # immediate re-entry when a frame lands just before its boundary.
        if {$delay < 10} { set delay [expr {$delay + 200}] }
    }
    after $delay lunar::tick
}

proc lunar::tick {} {
    set nextUtcMs ""
    if {[catch {
        if {[llength [info commands ::lunar::status]]} {
            set st [::lunar::status]
            lunar::render $st
            if {[dict get $st hasTime]} { set nextUtcMs [dict get $st utcMs] }
        } else {
            # no engine (dev/spike): fall back to the local clock
            set nowMs [clock milliseconds]
            set now [expr {$nowMs / 1000}]
            set milliseconds [expr {$nowMs % 1000}]
            if {$milliseconds < 0} { set milliseconds [expr {$milliseconds + 1000}] }
            set lt [dict create \
                hour   [scan [clock format $now -format %H] %d] \
                minute [scan [clock format $now -format %M] %d] \
                second [scan [clock format $now -format %S] %d]]
            # No engine, no honest bound: claim none (no fan), never stopped.
            lunar::clock_display $lt $milliseconds ok 1 1 0 0
            set nextUtcMs $nowMs
        }
    } err opts]} {
        catch { lunar::log "\[tick\] [dict get $opts -errorinfo]" }
    }
    # Always leave a timer behind, including after a transient engine error.
    lunar::schedule_tick $nextUtcMs
}

proc lunar::render {st} {
    set state   [dict get $st state]
    set synced  [dict get $st synced]
    set hasTime [dict get $st hasTime]

    set lt ""
    set milliseconds 0
    if {$hasTime} {
        # break the disciplined UTC down in the DISPLAY zone via the embedded
        # tzdata (never Tcl's [clock], which trusts the OS zone database), then
        # draw the hands; the second hand's fan width carries the uncertainty.
        set utcMs [dict get $st utcMs]
        set milliseconds [expr {$utcMs % 1000}]
        if {$milliseconds < 0} { set milliseconds [expr {$milliseconds + 1000}] }
        if {![catch { ::lunar::localtime $utcMs $::lunar::tz } lt]} {
            # The native widget owns its pixels and exposes its trust state on
            # the face; the canvas fallback preserves the same hand policy.
        }
    }
    # The uncertainty fan carries the bound; the stop rule withdraws the
    # seconds claim entirely once the bound exceeds the user's ceiling.
    set boundMs [dict get $st boundMs]
    # LUNAR_FAKE_BOUND=<ms>: dev hook for screenshot/UI review of the fan and
    # the stop rule at an arbitrary bound (the engine's real bound is hard to
    # stage). Display-only; never affects the engine or the chime edge.
    if {[info exists ::env(LUNAR_FAKE_BOUND)] &&
        [string is integer -strict $::env(LUNAR_FAKE_BOUND)]} {
        set boundMs $::env(LUNAR_FAKE_BOUND)
    }
    set running [expr {$hasTime && $lt ne ""}]
    set stopped [lunar::stop_eval $running $boundMs]
    lunar::clock_display $lt $milliseconds $state $running $synced \
        $boundMs $stopped

    # Recover as fast as possible: on the edge where the face stops showing
    # time -- the stop ceiling was crossed, or the time vanished outright --
    # fire an immediate poll instead of waiting out the scheduled interval.
    if {($stopped && !$::lunar::stopped_prev) ||
        (!$running && $::lunar::hastime_prev)} {
        lunar::poll_now
    }
    set ::lunar::stopped_prev $stopped
    set ::lunar::hastime_prev $running

    # The face has exactly two states -- time (uncertainty carried entirely
    # by the second hand's fan width) or no time -- so the bar words a state
    # ONLY when the face shows no time: STOPPED, or ACQUIRING/NO SIGNAL/
    # REACQUIRING.
    # Cells hide entirely (with their separator dots) when redundant, so the
    # zone witness is never starved into a clipped, fake-looking code: the
    # full bar's natural width exceeded the window (measured 633 vs 510 px),
    # which left-clipped "UTC+2" into "JTC+2".
    lassign [lunar::state_display $state $synced] txt col
    if {$stopped} {
        set txt "STOPPED" ; set col $::lunar::ACCENT
    }
    set word [expr {($stopped || !$running) ? $txt : ""}]
    .sb.trust configure -text $word -fg $col
    .sb.sep1  configure -text [expr {$word eq "" ? "" : "·"}]
    set bound [lunar::fmt_bound $boundMs]
    .sb.bound configure -text [expr {$running ? ($bound eq "" ? "±—" : $bound) : ""}] \
        -fg $::lunar::MUTED
    set showSys [expr {$running && !$stopped && [dict get $st sysDeltaValid]}]
    # A separator dot renders only when BOTH its neighbors are visible:
    # sep2 pairs bound|sys; sep3 is the zone's dot and pairs it with ANY
    # visible cell on the left cluster (bound when running -- with SYS hidden
    # the bound|zone pairing still needs it); in the no-time states sep1
    # (word) already provides the single dot before the zone.
    .sb.sep2  configure -text [expr {$showSys ? "·" : ""}]
    .sb.sys  configure -text [expr {$showSys ? \
        "SYS[lunar::fmt_delta [dict get $st sysDeltaMs]]" : ""}] -fg $::lunar::MUTED
    .sb.sep3 configure -text [expr {$running ? "·" : ""}]
    if {$running} {
        .sb.zone configure -text "[dict get $lt abbr] [lunar::fmt_utcoff [dict get $lt offSec]]"
    } else {
        .sb.zone configure -text $::lunar::tz
    }

    # chimes on armed marks, with a just-in-time audio pre-warm ahead of them
    if {$hasTime && [info exists lt]} {
        lunar::prewarm_check [dict get $lt minute] [dict get $lt second] [dict get $st boundMs]
        lunar::chime_check [dict get $lt minute] [dict get $st boundMs]
    } else {
        set ::lunar::prev_min -1
        set ::lunar::prewarm_min -1
    }
}

# ---- selftest (headless) ----------------------------------------------------
proc lunar::selftest {reportPath} {
    set ok 1 ; set msg ""
    if {[catch { lunar::build ; update idletasks ; update } err]} { set ok 0 ; set msg $err }
    set txt "lunar-selftest\nversion=$::lunar::version\ntk=[package present Tk]\n"
    append txt "engine=[expr {[llength [info commands ::lunar::status]] ? {yes} : {no}}]\n"
    append txt "toplevel=[winfo exists .face.clock]\n"
    set hasSettings [winfo exists .sb.settings]
    append txt "settingsbutton=[expr {$hasSettings ? {yes} : {no}}]\n"
    if {!$hasSettings} { set ok 0 ; set msg "status-bar settings button is missing" }
    set hasMenu [expr {[. cget -menu] ne ""}]
    append txt "appmenu=[expr {$hasMenu ? {yes} : {no}}]\n"
    if {$hasMenu} { set ok 0 ; set msg "application menu should not be attached" }
    if {[llength [info commands ::lunar::tz_list]]} {
        append txt "tzcount=[llength [::lunar::tz_list]]\n"
        append txt "tzversion=[::lunar::tz_version]\n"
        # a known zone must resolve and disagree with UTC in summer
        if {![catch { ::lunar::localtime 1751500000000 Europe/Brussels } lt]} {
            append txt "tzresolve=[dict get $lt abbr]/[dict get $lt offSec]\n"
        } else {
            set ok 0 ; set msg "tz resolve failed: $lt"
        }
    }
    if {[llength [info commands ::lunar::run_at_startup]]} {
        append txt "startup=[::lunar::run_at_startup]\n"
    }
    append txt "beep=[expr {[llength [info commands ::lunar::beep]] ? {yes} : {no}}]\n"
    append txt "prewarm=[expr {[llength [info commands ::lunar::prewarm]] ? {yes} : {no}}]\n"
    append txt "armed=[join $::lunar::armed {}]\n"
    # The face's ButtonRelease binding must reach the native Direct2D child
    # too, not only the canvas fallback. Stub persistence so self-test never
    # changes the user's armed.dat while exercising the real interaction.
    set savedArmed $::lunar::armed
    if {[catch {
        set ::lunar::armed [lrepeat 12 0]
        rename ::lunar::armed_save ::lunar::_armed_save
        proc ::lunar::armed_save {} {}
        set cw [winfo width .face.clock] ; set ch [winfo height .face.clock]
        set cs [expr {min($cw, $ch)}]
        event generate .face.clock <ButtonRelease-1> \
            -x [expr {$cw / 2}] -y [expr {round($ch / 2.0 - $cs * .46)}]
        update
        set clickok [expr {[lindex $::lunar::armed 0] == 1}]
        append txt "markerclick=[expr {$clickok ? {ok} : {FAIL}}]\n"
        if {!$clickok} { set ok 0 ; set msg "hour-marker click did not arm :00" }
    } markerErr]} {
        append txt "markerclick=FAIL\n"
        set ok 0 ; set msg "hour-marker click test failed: $markerErr"
    }
    catch { rename ::lunar::armed_save {} }
    catch { rename ::lunar::_armed_save ::lunar::armed_save }
    set ::lunar::armed $savedArmed
    # verify the chime edge-detector fires on an armed crossing, silently
    catch {
        set ::lunar::armed [lreplace $::lunar::armed 1 1 1]   ;# arm :05
        dict set ::lunar::cfg chimes 1
        set ::lunar::_chimes 0
        catch { rename ::lunar::beep ::lunar::_realbeep }
        proc ::lunar::beep {} { incr ::lunar::_chimes }
        set ::lunar::prev_min 4 ; lunar::chime_check 5 1000       ;# 4->5, bound ok -> fire
        set ::lunar::prev_min 5 ; lunar::chime_check 6 1000       ;# 5->6, :06 not a mark
        set ::lunar::prev_min 4 ; lunar::chime_check 5 60000      ;# bound too big -> no fire
        append txt "chimefire=$::lunar::_chimes\n"                ;# want 1
    }
    # verify the just-in-time pre-warm fires once, ~3 s before an armed mark
    catch {
        set ::lunar::armed [lreplace $::lunar::armed 1 1 1]   ;# arm :05
        dict set ::lunar::cfg chimes 1
        set ::lunar::_prewarms 0
        catch { rename ::lunar::prewarm ::lunar::_realprewarm }
        proc ::lunar::prewarm {} { incr ::lunar::_prewarms }
        set ::lunar::prewarm_min -1
        lunar::prewarm_check 4 57 1000    ;# 3 s before armed :05 -> prime
        lunar::prewarm_check 4 58 1000    ;# same mark, already primed -> no
        lunar::prewarm_check 4 50 1000    ;# too early in the minute -> no
        lunar::prewarm_check 3 57 1000    ;# next minute :04 not a mark -> no
        append txt "prewarmfire=$::lunar::_prewarms\n"           ;# want 1
    }
    # verify the adaptive poll cadence: FAST (8s) when unhealthy, BASE (60s)
    # when merely OK, RELAXED ramp (120/240/480/600s, capped) once converged.
    catch {
        catch { rename ::lunar::status ::lunar::_realstatus }
        proc ::lunar::status {} { return $::lunar::_ststub }
        set ::lunar::poll_good 0 ; set ::lunar::poll_bad 0
        set ::lunar::_ststub {state holdover synced 0 ratePpm 0 spreadMs 0 ntsSpreadMs 0 boundMs 6000}
        set fst [lunar::next_poll_ms 0]                          ;# unhealthy -> 8000
        set ::lunar::poll_good 0 ; set ::lunar::poll_bad 0
        set ::lunar::_ststub {state ok synced 1 ratePpm 0 spreadMs 20 ntsSpreadMs 20 boundMs 50}
        set bse [lunar::next_poll_ms 0]                          ;# OK, no rate -> 60000
        set ::lunar::poll_good 0 ; set ::lunar::poll_bad 0
        set ::lunar::_ststub {state ok synced 1 ratePpm 13 spreadMs 20 ntsSpreadMs 20 boundMs 50}
        for {set i 1} {$i <= 3} {incr i} { set r3 [lunar::next_poll_ms 0] } ;# confirm x3 -> 60000
        set r4 [lunar::next_poll_ms 0]                           ;# -> 120000
        set r5 [lunar::next_poll_ms 0]                           ;# -> 240000
        set r6 [lunar::next_poll_ms 0]                           ;# -> 480000
        set r7 [lunar::next_poll_ms 0]                           ;# -> capped 600000
        set good [expr {$fst==8000 && $bse==60000 && $r3==60000 && $r4==120000 \
                        && $r5==240000 && $r6==480000 && $r7==600000}]
        if {$good} { set cad ok } else { set cad "BAD $fst $bse $r3 $r4 $r5 $r6 $r7" }
        append txt "pollcadence=$cad\n"
    }
    # verify the stop rule: setting clamp, 2-read debounce, immediate resume
    catch {
        set oldStop [dict get $::lunar::cfg stopms]
        dict set ::lunar::cfg stopms 5000
        set ::lunar::stop_hits 0
        set r1 [lunar::stop_eval 1 5100]   ;# 1st read above ceiling -> not yet
        set r2 [lunar::stop_eval 1 5100]   ;# 2nd consecutive -> stopped
        set r3 [lunar::stop_eval 1 5100]   ;# stays stopped
        set r4 [lunar::stop_eval 1 300]    ;# back under -> resumes immediately
        set r5 [lunar::stop_eval 0 9999]   ;# no time -> stop path not taken
        set c1 [lunar::clamp_stopms 400]   ;# below floor  -> 1000
        set c2 [lunar::clamp_stopms 99999] ;# above ceil   -> 30000
        set c3 [lunar::clamp_stopms abc]   ;# garbage      -> default 5000
        set good [expr {!$r1 && $r2 && $r3 && !$r4 && !$r5 &&
                        $c1 == 1000 && $c2 == 30000 && $c3 == 5000}]
        dict set ::lunar::cfg stopms $oldStop
        if {$good} { set sg ok } else { set sg "BAD $r1$r2$r3$r4$r5 $c1 $c2 $c3" }
        append txt "stopgate=$sg\n"
        catch { rename ::lunar::status {} }
        catch { rename ::lunar::_realstatus ::lunar::status }
    }
    # verify close confirmation: with confirm on and the dialog answering
    # "no", quit must leave the window alive and must have asked exactly once
    catch {
        set oldConfirm [dict get $::lunar::cfg confirm]
        dict set ::lunar::cfg confirm 1
        set ::lunar::_asked 0
        rename ::tk_messageBox ::lunar::_realmsgbox
        proc ::tk_messageBox {args} { incr ::lunar::_asked ; return no }
        lunar::quit
        set alive [winfo exists .]
        rename ::tk_messageBox {}
        rename ::lunar::_realmsgbox ::tk_messageBox
        dict set ::lunar::cfg confirm $oldConfirm
        if {$alive && $::lunar::_asked == 1} { set cg ok } else { set cg "BAD $alive $::lunar::_asked" }
        append txt "confirmgate=$cg\n"
    }
    # verify the event log: store ingest, rolling rotation, dialog table,
    # live filter, and sort flip -- all under a scratch data dir so the
    # user's real events.log is never touched
    set eg "BAD gate errored"
    catch {
        set saveEnv [expr {[info exists ::env(LUNAR_DATA_DIR)] ? $::env(LUNAR_DATA_DIR) : ""}]
        set saveEvents $::lunar::events
        set saveMax $::lunar::events_file_max
        set tmp [file join [lunar::datadir] "selftest-ev-[pid]"]
        catch {
            set tmp [file join $::env(TEMP) "lunar-selftest-ev-[pid]"]
        }
        file mkdir $tmp
        set ::env(LUNAR_DATA_DIR) $tmp
        set ::lunar::events {}
        # explicit stamps: four calls can otherwise land in one millisecond,
        # making the sort-flip assertion racy
        lunar::ev info chime "selftest chime line" 1751731872000 1
        lunar::ev warn ntp "selftest warn line"    1751731873000 1
        lunar::ev error app "selftest error line"  1751731874000 1
        set stored [llength $::lunar::events]
        set ondisk [llength [lunar::events_read_file [lunar::events_path]]]
        set ::lunar::events_file_max 1        ;# next persist must rotate
        lunar::ev info app "selftest rotation trigger" 1751731875000 1
        set rotated [file exists "[lunar::events_path].1"]
        set ::lunar::events_file_max $saveMax
        lunar::log_dlg ; update idletasks
        set lr $::lunar::log_root
        set rows0 [llength [$lr.f.tv children {}]]
        lunar::log_q_set "chime"
        lunar::log_refresh
        set rows1 [llength [$lr.f.tv children {}]]
        lunar::log_q_set ""
        lunar::log_sortby time   ;# time already active -> flips descending
        set firstmsg [lindex [$lr.f.tv item [lindex [$lr.f.tv children {}] 0] -values] 3]
        destroy .log
        set good [expr {$stored == 3 && $ondisk == 3 && $rotated &&
                        $rows0 == 4 && $rows1 == 1 &&
                        $firstmsg eq "selftest rotation trigger"}]
        if {$good} { set eg ok } else {
            set eg "BAD st=$stored disk=$ondisk rot=$rotated r0=$rows0 r1=$rows1 first=$firstmsg"
        }
        if {$saveEnv ne ""} { set ::env(LUNAR_DATA_DIR) $saveEnv } else { unset -nocomplain ::env(LUNAR_DATA_DIR) }
        set ::lunar::events $saveEvents
        catch { file delete -force $tmp }
    }
    append txt "eventlog=$eg\n"
    # Any uncaught async error during the gates is a defect (the %W binding
    # bug hid behind status=ok for weeks this way): fail the selftest on it.
    append txt "bgerrors=$::lunar::bgerror_count\n"
    if {$::lunar::bgerror_count > 0 && $ok} {
        set ok 0 ; set msg "$::lunar::bgerror_count uncaught error(s) during selftest (see events.log)"
    }
    append txt "status=[expr {$ok ? {ok} : {FAIL}}]\n"
    if {!$ok} { append txt "error=$msg\n" }
    if {$reportPath ne ""} {
        catch { set fh [open $reportPath w] ; puts -nonewline $fh $txt ; close $fh }
    }
    exit [expr {$ok ? 0 : 1}]
}

proc lunar::main {} {
    set i [lsearch -exact $::argv "--selftest"]
    if {$i >= 0} {
        lunar::selftest [lindex $::argv [expr {$i + 1}]]
        return
    }
    lunar::settings_load
    lunar::armed_load
    # single-source the version from the engine (VERSION -> version.h) so the
    # title bar and About agree
    if {[llength [info commands ::lunar::about]]} {
        set ::lunar::version [dict get [::lunar::about] version]
    }
    # the registry Run key is authoritative for run-at-startup (the user may
    # have toggled it via Task Manager/msconfig outside Lunar)
    if {[llength [info commands ::lunar::run_at_startup]]} {
        dict set ::lunar::cfg startup [::lunar::run_at_startup]
    }
    lunar::tz_startup
    lunar::build
    # Route uncaught async errors to the log + a status note, never a modal
    # dialog (els's production bgerror pattern). Installed after build.
    proc ::bgerror {msg args} { lunar::bgerror $msg {*}$args }
    catch { interp bgerror {} lunar::bgerror }
    catch { proc ::tk::dialog::error::bgerror {msg args} { lunar::bgerror $msg {*}$args } }

    if {[llength [info commands ::lunar::engine_start]]} {
        if {[catch { ::lunar::engine_start } e opts]} {
            catch { lunar::log "\[engine_start\] [dict get $opts -errorinfo]" }
        }
        # engine_start fired cycle #1 now; arm the adaptive scheduler at the
        # FAST floor so a fresh boot re-polls quickly until it has anchored,
        # rather than waiting a full base interval.
        set ::lunar::repoll_after [after $::lunar::poll_min lunar::repoll]
    }
    # Event store: session marker now; history merge-load off the paint
    # path (after idle still beats the first 1 s drain tick, and boot-time
    # events ingested before it are merged, not clobbered).
    lunar::ev info app "session start (Time Actual $::lunar::version)"
    after idle lunar::events_load
    after 1000 lunar::events_drain_loop
    after 100 lunar::tick
    # dev hooks: open a dialog on launch, for screenshots/testing
    if {[info exists ::env(LUNAR_OPEN_SETTINGS)] && $::env(LUNAR_OPEN_SETTINGS) ne ""} {
        # value "1" opens the default tab; any other value names a tab
        set _tab [expr {$::env(LUNAR_OPEN_SETTINGS) eq "1" ? "" : $::env(LUNAR_OPEN_SETTINGS)}]
        after 400 [list lunar::settings_dlg $_tab]
    }
    if {[info exists ::env(LUNAR_OPEN_ABOUT)]    && $::env(LUNAR_OPEN_ABOUT)    ne ""} { after 400 {lunar::settings_dlg app} }
    if {[info exists ::env(LUNAR_OPEN_LOG)]      && $::env(LUNAR_OPEN_LOG)      ne ""} { after 400 lunar::log_dlg }
    # LUNAR_BEEP=<n>: on launch, run the real prewarm->chime sequence n times
    # (prewarm ~300ms before each chime, ~1s apart), for audio testing
    # (cold-device reliability, overlap-drop). Default 1.
    if {[info exists ::env(LUNAR_BEEP)] && $::env(LUNAR_BEEP) ne ""} {
        set n [expr {[string is integer -strict $::env(LUNAR_BEEP)] ? $::env(LUNAR_BEEP) : 1}]
        for {set b 0} {$b < $n} {incr b} {
            after [expr {300 + $b * 1000}] { catch { ::lunar::prewarm } }
            after [expr {600 + $b * 1000}] { catch { ::lunar::beep } }
        }
    }
}

lunar::main
