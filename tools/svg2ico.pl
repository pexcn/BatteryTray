#!/usr/bin/perl
#
# Builds ui/StatMeter.ico from the SVGs beside this script. It parses the path
# data, rasterises it with a nonzero-winding scanline fill, and packs the
# result as an .ico. Nothing in the build calls it: the .ico is committed, and
# this exists so the next person can regenerate it instead of guessing how it
# was made.
#
# Plain Perl with no modules outside core, because the machine this was written
# on has no rasteriser, no ImageMagick and no Python, and the project takes no
# third-party dependencies (AGENTS.md).
#
# Usage:  svg2ico.pl OUT.ico RRGGBB SIZE=FILE.svg [SIZE=FILE.svg ...]
#
# The command that produced the icon in the tree, run from the repo root:
#
#   perl tools/svg2ico.pl ui/StatMeter.ico 0078D4 \
#        16=tools/icon_16.svg 20=tools/icon_20.svg 24=tools/icon_24.svg \
#        32=tools/icon_32.svg 48=tools/icon_32.svg 64=tools/icon_32.svg \
#        256=tools/icon_32.svg
#
# Fluent ships a separate design per size up to 32 (the small ones are drawn on
# their own pixel grid), so each of those gets its own file; 48 and up are the
# 32 design scaled, since there is no larger source.
#
# The colour is an argument rather than the #212121 the assets carry, because
# the glyph has no plate and sits straight on the system background: #212121
# measures 1.01:1 against the dark-mode #202020, which is not dim but absent.
# Clearing 3:1 on both white and #202020 confines the colour to a relative
# luminance between 0.143 and 0.30. #0078D4 sits at 0.18 — 4.53:1 and 3.60:1 —
# and is the system's own accent. Any replacement has to survive the same two
# measurements; a colour picked on a white background alone will not.
use strict; use warnings;
use IO::Compress::Deflate qw(deflate);

my $SS = 16;   # sub-scanlines per pixel row; x coverage is analytic

# ---------- SVG path -> polygon edge list ----------
sub flatten_cubic {
    my ($pts, $x0,$y0,$x1,$y1,$x2,$y2,$x3,$y3, $depth) = @_;
    # flat enough? distance of control points from the chord
    my $dx = $x3-$x0; my $dy = $y3-$y0;
    my $d1 = abs(($x1-$x3)*$dy - ($y1-$y3)*$dx);
    my $d2 = abs(($x2-$x3)*$dy - ($y2-$y3)*$dx);
    if ($depth > 16 || ($d1+$d2)**2 <= 0.02 * ($dx*$dx + $dy*$dy)) {
        push @$pts, [$x3,$y3]; return;
    }
    my ($x01,$y01) = (($x0+$x1)/2, ($y0+$y1)/2);
    my ($x12,$y12) = (($x1+$x2)/2, ($y1+$y2)/2);
    my ($x23,$y23) = (($x2+$x3)/2, ($y2+$y3)/2);
    my ($xa,$ya)   = (($x01+$x12)/2, ($y01+$y12)/2);
    my ($xb,$yb)   = (($x12+$x23)/2, ($y12+$y23)/2);
    my ($xm,$ym)   = (($xa+$xb)/2, ($ya+$yb)/2);
    flatten_cubic($pts,$x0,$y0,$x01,$y01,$xa,$ya,$xm,$ym,$depth+1);
    flatten_cubic($pts,$xm,$ym,$xb,$yb,$x23,$y23,$x3,$y3,$depth+1);
}

sub parse_path {
    my ($d) = @_;
    my @tok;
    while ($d =~ /\G\s*(?:([A-Za-z])|(-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)|,)/gc) {
        push @tok, defined $1 ? $1 : $2;
    }
    my (@subpaths, @cur);
    my ($x,$y,$sx,$sy) = (0,0,0,0);
    my ($px,$py) = (0,0);          # last cubic control point, for S
    my ($cmd, $prev) = ('', '');
    my $i = 0;
    my $num = sub { die "path: ran out of numbers\n" if $i > $#tok; $tok[$i++] };
    while ($i <= $#tok) {
        if ($tok[$i] =~ /^[A-Za-z]$/) { $cmd = $tok[$i++]; }
        elsif ($cmd eq 'M') { $cmd = 'L' } elsif ($cmd eq 'm') { $cmd = 'l' }
        my $rel = $cmd eq lc $cmd;
        my $c = uc $cmd;
        if ($c eq 'Z') {
            push @subpaths, [@cur] if @cur > 2;
            @cur = (); ($x,$y) = ($sx,$sy); $prev = $c; next;
        }
        if ($c eq 'M') {
            push @subpaths, [@cur] if @cur > 2;
            my $nx = $num->(); my $ny = $num->();
            ($x,$y) = $rel ? ($x+$nx,$y+$ny) : ($nx,$ny);
            ($sx,$sy) = ($x,$y); @cur = ([$x,$y]);
        } elsif ($c eq 'L') {
            my $nx = $num->(); my $ny = $num->();
            ($x,$y) = $rel ? ($x+$nx,$y+$ny) : ($nx,$ny);
            push @cur, [$x,$y];
        } elsif ($c eq 'H') {
            my $nx = $num->(); $x = $rel ? $x+$nx : $nx; push @cur, [$x,$y];
        } elsif ($c eq 'V') {
            my $ny = $num->(); $y = $rel ? $y+$ny : $ny; push @cur, [$x,$y];
        } elsif ($c eq 'C' || $c eq 'S') {
            my ($x1,$y1);
            if ($c eq 'C') {
                $x1 = $num->(); $y1 = $num->();
                ($x1,$y1) = ($x+$x1,$y+$y1) if $rel;
            } else {
                ($x1,$y1) = ($prev =~ /^[CS]$/) ? (2*$x-$px, 2*$y-$py) : ($x,$y);
            }
            my $x2 = $num->(); my $y2 = $num->();
            my $x3 = $num->(); my $y3 = $num->();
            if ($rel) { ($x2,$y2,$x3,$y3) = ($x+$x2,$y+$y2,$x+$x3,$y+$y3) }
            flatten_cubic(\@cur, $x,$y,$x1,$y1,$x2,$y2,$x3,$y3, 0);
            ($px,$py) = ($x2,$y2); ($x,$y) = ($x3,$y3);
        } elsif ($c eq 'Q' || $c eq 'T') {
            my ($qx,$qy);
            if ($c eq 'Q') {
                $qx = $num->(); $qy = $num->();
                ($qx,$qy) = ($x+$qx,$y+$qy) if $rel;
            } else {
                ($qx,$qy) = ($prev =~ /^[QT]$/) ? (2*$x-$px, 2*$y-$py) : ($x,$y);
            }
            my $x3 = $num->(); my $y3 = $num->();
            ($x3,$y3) = ($x+$x3,$y+$y3) if $rel;
            flatten_cubic(\@cur, $x,$y, $x+2/3*($qx-$x), $y+2/3*($qy-$y),
                          $x3+2/3*($qx-$x3), $y3+2/3*($qy-$y3), $x3,$y3, 0);
            ($px,$py) = ($qx,$qy); ($x,$y) = ($x3,$y3);
        } else {
            die "path: unsupported command '$cmd'\n";
        }
        $prev = $c;
    }
    push @subpaths, [@cur] if @cur > 2;
    return \@subpaths;
}

sub read_svg {
    my ($file) = @_;
    open my $fh, '<', $file or die "$file: $!\n";
    local $/; my $svg = <$fh>; close $fh;
    my ($vb) = $svg =~ /viewBox\s*=\s*"([^"]+)"/ or die "$file: no viewBox\n";
    my @vb = split /[\s,]+/, $vb;
    my @paths;
    while ($svg =~ /<path\b[^>]*\bd\s*=\s*"([^"]+)"/g) { push @paths, parse_path($1) }
    die "$file: no <path>\n" unless @paths;
    # Fluent icons are one filled shape; several <path> elements just union.
    return (\@vb, [map { @$_ } @paths]);
}

# ---------- scanline rasteriser, nonzero winding ----------
sub rasterize {
    my ($subpaths, $vb, $size) = @_;
    my ($vx,$vy,$vw,$vh) = @$vb;
    my $scale = $size / $vw;                 # Fluent viewBoxes are square
    my @edges;
    for my $sp (@$subpaths) {
        for my $k (0 .. $#$sp) {
            my ($ax,$ay) = @{$sp->[$k]};
            my ($bx,$by) = @{$sp->[($k+1) % @$sp]};
            ($ax,$ay) = (($ax-$vx)*$scale, ($ay-$vy)*$scale);
            ($bx,$by) = (($bx-$vx)*$scale, ($by-$vy)*$scale);
            next if $ay == $by;
            push @edges, [$ax,$ay,$bx,$by];
        }
    }
    my @cov = (0) x ($size*$size);
    for my $py (0 .. $size-1) {
        my $row = $py * $size;
        for my $s (0 .. $SS-1) {
            my $sy = $py + ($s + 0.5) / $SS;
            my @xs;
            for my $e (@edges) {
                my ($ax,$ay,$bx,$by) = @$e;
                my ($lo,$hi) = $ay < $by ? ($ay,$by) : ($by,$ay);
                next if $sy < $lo || $sy >= $hi;
                push @xs, [$ax + ($sy-$ay) * ($bx-$ax) / ($by-$ay), $ay < $by ? 1 : -1];
            }
            next unless @xs;
            @xs = sort { $a->[0] <=> $b->[0] } @xs;
            my $wind = 0; my $start = 0;
            for my $c (@xs) {
                my $was = $wind; $wind += $c->[1];
                if ($was == 0 && $wind != 0) { $start = $c->[0] }
                elsif ($was != 0 && $wind == 0) {
                    # accumulate the span [$start, $c->[0]) into the pixel row
                    my ($xa,$xb) = ($start, $c->[0]);
                    $xa = 0 if $xa < 0; $xb = $size if $xb > $size;
                    next if $xb <= $xa;
                    my $ia = int $xa; my $ib = int $xb;
                    $ib = $size-1 if $ib >= $size;
                    if ($ia == $ib) { $cov[$row+$ia] += $xb-$xa; next }
                    $cov[$row+$ia] += $ia+1 - $xa;
                    $cov[$row+$_]  += 1 for $ia+1 .. $ib-1;
                    $cov[$row+$ib] += $xb - $ib;
                }
            }
        }
    }
    $_ = $_ / $SS for @cov;
    return \@cov;
}

# ---------- encoders ----------
sub bmp_entry {                              # 32bpp BGRA DIB + AND mask
    my ($cov, $size, $r,$g,$b) = @_;
    my $hdr = pack 'VllvvVVllVV', 40, $size, $size*2, 1, 32, 0, 0, 0, 0, 0, 0;
    my $xor = '';
    for my $y (reverse 0 .. $size-1) {       # DIB rows run bottom-up
        for my $x (0 .. $size-1) {
            my $a = int($cov->[$y*$size+$x] * 255 + 0.5);
            $a = 255 if $a > 255; $a = 0 if $a < 0;
            $xor .= pack 'CCCC', $b, $g, $r, $a;
        }
    }
    # 1bpp AND mask, rows padded to 4 bytes: all zero, the alpha decides.
    my $stride = int(($size + 31) / 32) * 4;
    return $hdr . $xor . ("\0" x ($stride * $size));
}

sub crc32 {
    my ($buf) = @_;
    our @T;
    unless (@T) {
        for my $n (0..255) {
            my $c = $n;
            $c = ($c & 1) ? (0xedb88320 ^ ($c >> 1)) : ($c >> 1) for 1..8;
            $T[$n] = $c;
        }
    }
    my $c = 0xffffffff;
    $c = $T[($c ^ $_) & 0xff] ^ ($c >> 8) for unpack 'C*', $buf;
    return $c ^ 0xffffffff;
}

sub png_chunk {
    my ($type, $data) = @_;
    return pack('N', length $data) . $type . $data . pack('N', crc32($type.$data));
}

sub png_entry {
    my ($cov, $size, $r,$g,$b) = @_;
    my $raw = '';
    for my $y (0 .. $size-1) {
        $raw .= "\0";                        # filter type 0
        for my $x (0 .. $size-1) {
            my $a = int($cov->[$y*$size+$x] * 255 + 0.5);
            $a = 255 if $a > 255; $a = 0 if $a < 0;
            $raw .= pack 'CCCC', $r, $g, $b, $a;
        }
    }
    my $z; deflate(\$raw, \$z, -Level => 9) or die "deflate failed\n";
    return "\x89PNG\r\n\x1a\n"
         . png_chunk('IHDR', pack 'NNCCCCC', $size, $size, 8, 6, 0, 0, 0)
         . png_chunk('IDAT', $z)
         . png_chunk('IEND', '');
}

# ---------- main ----------
my $out = shift or die "usage: svg2ico.pl out.ico RRGGBB size=svg ...\n";
my $hex = shift or die "usage: svg2ico.pl out.ico RRGGBB size=svg ...\n";
my ($R,$G,$B) = map { hex } $hex =~ /^#?(..)(..)(..)$/ or die "bad colour '$hex'\n";

my (@imgs);
for my $arg (@ARGV) {
    my ($size, $svg) = $arg =~ /^(\d+)=(.+)$/ or die "bad argument '$arg'\n";
    my ($vb, $subpaths) = read_svg($svg);
    my $cov = rasterize($subpaths, $vb, $size);
    # PNG only for 256: the size Windows documents it for, and where a DIB
    # would cost 256 KB. Everything below stays a DIB.
    my $data = $size >= 256 ? png_entry($cov, $size, $R,$G,$B)
                            : bmp_entry($cov, $size, $R,$G,$B);
    push @imgs, [$size, $data];
    printf STDERR "%3d  %-44s %6d bytes  %s\n", $size, $svg, length $data,
           $size >= 256 ? 'PNG' : 'DIB';
}

my $ico = pack 'vvv', 0, 1, scalar @imgs;
my $off = 6 + 16 * @imgs;
for my $im (@imgs) {
    my ($size, $data) = @$im;
    $ico .= pack 'CCCCvvVV', $size % 256, $size % 256, 0, 0, 1, 32, length $data, $off;
    $off += length $data;
}
$ico .= $_->[1] for @imgs;
open my $fh, '>:raw', $out or die "$out: $!\n";
print $fh $ico; close $fh;
printf STDERR "wrote %s, %d bytes\n", $out, length $ico;
