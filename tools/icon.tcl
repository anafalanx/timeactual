#!/usr/bin/env tclsh
# tools/icon.tcl -- render the application icon from a palette and a few
# numbers, so the colours change by editing one block (or passing flags) and
# rerunning `kuu.exe run icon`. Pure Tcl: a signed-distance rasteriser with one-pixel
# anti-aliasing, size-dependent simplification (the minute track, the twelve
# hour ticks and the inner ring drop out where they would no longer fit), a
# PNG encoder and an ICO packer. No Tk, no external tools.
#
#   kuu.exe run icon ?--face #hex? ?--ink #hex? ?--accent #hex?
#       -> assets/icon.ico (16..256) + assets/icon.png (256 preview)
#          + build/icon/*.png (every size, plus nearest-neighbour blow-ups
#            of the small ones for checking the pixel fit)
#
# The design is concept N, "the observatory clock": a double-ring stamp
# around a dial with a sixty-tick minute track, hands at ten past ten and
# the second hand at four. Geometry is written in a 64-unit box -- the same
# numbers as the concept page -- and scaled per size.

# ---- palette: edit here -------------------------------------------------------
# Neutral Moresnet: black over white over blue. The blue is the one the
# Wikimedia Commons flag file draws (#0F47AF), not the electric value the
# colour-code sites quote.
array set PALETTE {
    face   #FFFFFF
    ink    #000000
    accent #0F47AF
}

# ---- geometry (64-unit box, centre 32,32; hw = half-width) --------------------
array set G {
    face_r    30.0
    outer_r   28.3   outer_hw 1.2
    inner_r   24.2   inner_hw 0.6
    min_r0    21.5   min_r1   23.2   min_hw  0.45
    hour_r0   20.0   hour_r1  23.2   hour_hw 0.8
    card_r0   16.4   card_r1  20.8   card_hw 1.3
    hhand_deg -60    hhand_u0 -2.5   hhand_u1 14.5  hhand_hw 2.2
    mhand_deg  60    mhand_u0 -2.5   mhand_u1 20.2  mhand_hw 1.55
    shand_deg 125    shand_u0 -4.5   shand_u1 19.8  shand_hw0 0.9  shand_hw1 2.1
    hub_r      3.1
}

# ---- sizes, and what survives at each -----------------------------------------
set SIZES {256 128 64 48 40 32 24 20 16}
proc features {S} {
    # minute: the sixty-tick track; hours: the twelve; inner: the chapter ring.
    if {$S >= 96} { return {minute 1 hours 1 inner 1} }
    if {$S >= 32} { return {minute 0 hours 1 inner 1} }
    return {minute 0 hours 0 inner 0}
}

# ---- rasteriser ------------------------------------------------------------------
proc hex2rgb {hex} {
    if {[scan $hex "#%02x%02x%02x" r g b] != 3} { error "bad colour: $hex (want #rrggbb)" }
    return [list $r $g $b]
}

# Render one size. Returns a binary string of S*S RGBA pixels, rows top-down,
# straight (non-premultiplied) alpha.
proc render {S} {
    global G PALETTE
    lassign [hex2rgb $PALETTE(face)]   fr fg fb
    lassign [hex2rgb $PALETTE(ink)]    ir ig ib
    lassign [hex2rgb $PALETTE(accent)] ar ag ab
    array set F [features $S]
    set sc [expr {$S / 64.0}]          ;# pixels per design unit
    set w  [expr {1.0 / $sc}]          ;# one pixel, in design units
    # Strokes never get thinner than one pixel (a little more at the two
    # smallest sizes, where a hairline reads as dirt).
    set minhw [expr {($S <= 24 ? 0.6 : 0.5) * $w}]
    foreach k {outer_hw inner_hw min_hw hour_hw card_hw hhand_hw mhand_hw shand_hw0 shand_hw1} {
        set $k [expr {max($G($k), $minhw)}]
    }
    foreach k {face_r outer_r inner_r min_r0 min_r1 hour_r0 hour_r1 card_r0 card_r1
               hhand_u0 hhand_u1 mhand_u0 mhand_u1 shand_u0 shand_u1 hub_r} {
        set $k $G($k)
    }
    if {$S <= 24} {
        # Where a pixel is a lot: hands a pixel and a half wide (a one-pixel
        # anti-aliased hand reads as grey), cardinals a little longer, and a
        # hub that is still a dot.
        set handmin [expr {0.75 * $w}]
        foreach k {hhand_hw mhand_hw shand_hw0 shand_hw1} { set $k [expr {max([set $k], $handmin)}] }
        set card_r0 [expr {min($card_r0, 15.5)}]
        set hub_r   [expr {max($hub_r, 1.1 * $w)}]
    }
    set pi 3.14159265358979323846
    set hth [expr {$G(hhand_deg) * $pi / 180.0}]
    set mth [expr {$G(mhand_deg) * $pi / 180.0}]
    set sth [expr {$G(shand_deg) * $pi / 180.0}]
    set hsin [expr {sin($hth)}]; set hcos [expr {cos($hth)}]
    set msin [expr {sin($mth)}]; set mcos [expr {cos($mth)}]
    set ssin [expr {sin($sth)}]; set scos [expr {cos($sth)}]
    set taper [expr {($shand_hw1 - $shand_hw0) / ($shand_u1 - $shand_u0)}]

    set out {}
    for {set py 0} {$py < $S} {incr py} {
        set y  [expr {($py + 0.5) / $sc}]
        set dy [expr {$y - 32.0}]
        for {set px 0} {$px < $S} {incr px} {
            set x  [expr {($px + 0.5) / $sc}]
            set dx [expr {$x - 32.0}]
            set len [expr {sqrt($dx*$dx + $dy*$dy)}]

            # face disc: the icon's silhouette (everything else sits inside it)
            set cf [expr {0.5 - ($len - $face_r) / $w}]
            if {$cf <= 0.0} { lappend out 0 0 0 0 ; continue }
            if {$cf > 1.0} { set cf 1.0 }

            # ink: rings, ticks, hour and minute hands (union = max coverage)
            set d [expr {abs($len - $outer_r) - $outer_hw}]
            if {$F(inner)} {
                set d2 [expr {abs($len - $inner_r) - $inner_hw}]
                if {$d2 < $d} { set d $d2 }
            }
            # radial ticks: evaluate the nearest tick of each family
            set a [expr {atan2($dx, -$dy)}]                  ;# clock angle, radians
            if {$F(minute)} {
                set th [expr {round($a / ($pi/30.0)) * ($pi/30.0)}]
                set u  [expr {$dx*sin($th) - $dy*cos($th)}]
                set v  [expr {$dx*cos($th) + $dy*sin($th)}]
                set d2 [expr {max($min_r0 - $u, $u - $min_r1, abs($v) - $min_hw)}]
                if {$d2 < $d} { set d $d2 }
            }
            if {$F(hours)} {
                set th [expr {round($a / ($pi/6.0)) * ($pi/6.0)}]
                set u  [expr {$dx*sin($th) - $dy*cos($th)}]
                set v  [expr {$dx*cos($th) + $dy*sin($th)}]
                set d2 [expr {max($hour_r0 - $u, $u - $hour_r1, abs($v) - $hour_hw)}]
                if {$d2 < $d} { set d $d2 }
            }
            set th [expr {round($a / ($pi/2.0)) * ($pi/2.0)}]
            set u  [expr {$dx*sin($th) - $dy*cos($th)}]
            set v  [expr {$dx*cos($th) + $dy*sin($th)}]
            set d2 [expr {max($card_r0 - $u, $u - $card_r1, abs($v) - $card_hw)}]
            if {$d2 < $d} { set d $d2 }
            # hour hand
            set u  [expr {$dx*$hsin - $dy*$hcos}]
            set v  [expr {$dx*$hcos + $dy*$hsin}]
            set d2 [expr {max($hhand_u0 - $u, $u - $hhand_u1, abs($v) - $hhand_hw)}]
            if {$d2 < $d} { set d $d2 }
            # minute hand
            set u  [expr {$dx*$msin - $dy*$mcos}]
            set v  [expr {$dx*$mcos + $dy*$msin}]
            set d2 [expr {max($mhand_u0 - $u, $u - $mhand_u1, abs($v) - $mhand_hw)}]
            if {$d2 < $d} { set d $d2 }
            set ci [expr {0.5 - $d / $w}]
            if {$ci < 0.0} { set ci 0.0 } elseif {$ci > 1.0} { set ci 1.0 }

            # accent: the second hand (a needle widening to the tip) and the hub
            set u  [expr {$dx*$ssin - $dy*$scos}]
            set v  [expr {$dx*$scos + $dy*$ssin}]
            set t  [expr {($u - $shand_u0) / ($shand_u1 - $shand_u0)}]
            if {$t < 0.0} { set t 0.0 } elseif {$t > 1.0} { set t 1.0 }
            set hw [expr {$shand_hw0 + $taper * ($shand_u1 - $shand_u0) * $t}]
            set d  [expr {max($shand_u0 - $u, $u - $shand_u1, abs($v) - $hw)}]
            set d2 [expr {$len - $hub_r}]
            if {$d2 < $d} { set d $d2 }
            set ca [expr {0.5 - $d / $w}]
            if {$ca < 0.0} { set ca 0.0 } elseif {$ca > 1.0} { set ca 1.0 }

            # composite: face, then ink, then accent; alpha is the silhouette
            set r [expr {$fr + ($ir - $fr) * $ci}]
            set g [expr {$fg + ($ig - $fg) * $ci}]
            set b [expr {$fb + ($ib - $fb) * $ci}]
            set r [expr {int($r + ($ar - $r) * $ca + 0.5)}]
            set g [expr {int($g + ($ag - $g) * $ca + 0.5)}]
            set b [expr {int($b + ($ab - $b) * $ca + 0.5)}]
            lappend out $r $g $b [expr {int($cf * 255.0 + 0.5)}]
        }
    }
    return [binary format cu* $out]
}

# ---- PNG ------------------------------------------------------------------------
proc png_chunk {type data} {
    set crc [zlib crc32 "$type$data"]
    return [binary format Ia*a*I [string length $data] $type $data $crc]
}
proc png_encode {w h rgba} {
    set raw ""
    set stride [expr {$w * 4}]
    for {set y 0} {$y < $h} {incr y} {
        append raw "\x00" [string range $rgba [expr {$y*$stride}] [expr {($y+1)*$stride - 1}]]
    }
    set ihdr [binary format IIccccc $w $h 8 6 0 0 0]
    return "\x89PNG\r\n\x1a\n[png_chunk IHDR $ihdr][png_chunk IDAT [zlib compress $raw 9]][png_chunk IEND {}]"
}

# Nearest-neighbour blow-up, for eyeballing the pixel fit of the small sizes.
proc upscale {S rgba k} {
    set stride [expr {$S * 4}]
    set out ""
    for {set y 0} {$y < $S} {incr y} {
        set row ""
        for {set x 0} {$x < $S} {incr x} {
            append row [string repeat [string range $rgba [expr {$y*$stride + $x*4}] [expr {$y*$stride + $x*4 + 3}]] $k]
        }
        append out [string repeat $row $k]
    }
    return $out
}

# ---- ICO ------------------------------------------------------------------------
# Every entry is a PNG (Windows has read PNG-compressed icon images since
# Vista; the previous icon shipped the same way), which keeps the file small.
proc ico_pack {entries} {
    set n [llength $entries]
    set dir [binary format sss 0 1 $n]
    set offset [expr {6 + 16*$n}]
    set body ""
    foreach e $entries {
        lassign $e w h data
        append dir [binary format ccccssii [expr {$w >= 256 ? 0 : $w}] [expr {$h >= 256 ? 0 : $h}] \
                        0 0 1 32 [string length $data] $offset]
        append body $data
        incr offset [string length $data]
    }
    return "$dir$body"
}

proc write_bytes {path data} {
    file mkdir [file dirname $path]
    set fh [open $path w]
    fconfigure $fh -translation binary
    puts -nonewline $fh $data
    close $fh
}

# ---- main --------------------------------------------------------------------------
proc main {argv} {
    global PALETTE SIZES
    set root [file dirname [file dirname [file normalize [info script]]]]
    set out [file join $root assets]
    set preview [file join $root build icon]
    set resources [file join $root resources]   ;# packaged; lunar.tcl's wm iconphoto set
    for {set i 0} {$i < [llength $argv]} {incr i} {
        set a [lindex $argv $i]
        switch -- $a {
            --face      { set PALETTE(face)   [lindex $argv [incr i]] }
            --ink       { set PALETTE(ink)    [lindex $argv [incr i]] }
            --accent    { set PALETTE(accent) [lindex $argv [incr i]] }
            --out       { set out             [lindex $argv [incr i]] }
            --preview   { set preview         [lindex $argv [incr i]] }
            --resources { set resources       [lindex $argv [incr i]] }
            default     { error "unknown option $a" }
        }
    }
    foreach k {face ink accent} { hex2rgb $PALETTE($k) }   ;# validate early
    puts "icon: face $PALETTE(face)  ink $PALETTE(ink)  accent $PALETTE(accent)"

    set entries {}
    foreach S $SIZES {
        set t0 [clock milliseconds]
        set rgba [render $S]
        set png [png_encode $S $S $rgba]
        write_bytes [file join $preview icon-$S.png] $png
        if {$S <= 48} {
            set k [expr {$S <= 24 ? 8 : 4}]
            write_bytes [file join $preview icon-$S-x$k.png] [png_encode [expr {$S*$k}] [expr {$S*$k}] [upscale $S $rgba $k]]
        }
        if {$S == 256} {
            write_bytes [file join $out icon.png] $png
            write_bytes [file join $resources icon.png] $png
        }
        if {$S in {16 24 32 48 256}} { write_bytes [file join $resources icon-$S.png] $png }
        lappend entries [list $S $S $png]
        puts [format "  %3d px  %4d ms" $S [expr {[clock milliseconds] - $t0}]]
    }
    set ico [ico_pack $entries]
    write_bytes [file join $out icon.ico] $ico
    puts "wrote [file join $out icon.ico] ([string length $ico] bytes, [llength $entries] sizes), icon.png, and the window-icon set in $resources; previews in $preview"
}

main $argv
