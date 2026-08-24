<#
.SYNOPSIS
    Renders src\BatteryTray.ico from the Segoe Fluent Icons battery glyph.

.DESCRIPTION
    Design-time tool, not part of the build: run it once (Windows PowerShell 5.1
    or PowerShell 7 on Windows), then commit the .ico. The build must not depend
    on the font, both because rc.exe only ever consumes the finished file and
    because the CI image is Windows Server, which ships Segoe MDL2 Assets rather
    than Segoe Fluent Icons.

    The glyph is BatteryCharging7 (U+E861): a battery at roughly 70% with the
    charging symbol on it. Levels 0 to 8 (U+E85A to U+E862) sit at the same
    codepoints in both symbol fonts -- only 9 and 10 were moved -- so the MDL2
    fallback below renders the identical shape on Windows 10.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
#>
[CmdletBinding()]
param(
    [string]$Glyph = 'E861',
    [string[]]$FontFamily = @('Segoe Fluent Icons', 'Segoe MDL2 Assets'),
    # Mid-tone green: the exe icon sits on both a light Explorer background and a
    # dark Task Manager row, so it has to carry its own contrast either way.
    [string]$Color = '#107C10',
    # 20/64/128 are dropped: every shell surface picks one of these and scales,
    # and each extra frame is dead weight inside the exe.
    [int[]]$Sizes = @(16, 24, 32, 48, 256),
    # Rendering large and downsampling keeps the stroke weight even at sizes the
    # font's hinting was never tuned for.
    [ValidateRange(1, 16)]
    [int]$Supersample = 4,
    [string]$OutFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

if (-not $OutFile) {
    $OutFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\BatteryTray.ico'
}

$family = $null
foreach ($name in $FontFamily) {
    try { $family = New-Object System.Drawing.FontFamily($name); break } catch { }
}
if (-not $family) {
    throw "None of these fonts are installed: $($FontFamily -join ', ')"
}
Write-Host "Font:  $($family.Name)"

$text = [string][char][Convert]::ToInt32($Glyph, 16)
$rgb = [System.Drawing.ColorTranslator]::FromHtml($Color)

# GenericTypographic drops the padding GenericDefault adds around the string, so
# the draw origin stays a fixed reference point for the ink measurement below.
# NoClip keeps GDI+ from trimming the glyph when the em box overflows the canvas,
# which it does by design here: the ink is scaled up until it fills the frame.
$format = [System.Drawing.StringFormat]::GenericTypographic
$format.FormatFlags = $format.FormatFlags -bor [System.Drawing.StringFormatFlags]::NoClip -bor [System.Drawing.StringFormatFlags]::NoWrap

function New-GlyphBitmap([int]$pixels, [single]$em, [single]$x, [single]$y) {
    $bmp = New-Object System.Drawing.Bitmap($pixels, $pixels, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        # Grayscale AA, never ClearType: the icon is composited over an unknown
        # background through its alpha channel and subpixel coverage would show
        # up as colored fringes on the glyph edges.
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
        $font = New-Object System.Drawing.Font($family, $em, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
        try {
            # White now, tinted after downsampling: interpolating color across the
            # transparent border would drag the edge pixels toward black.
            $g.DrawString($text, $font, [System.Drawing.Brushes]::White, $x, $y, $format)
        } finally { $font.Dispose() }
    } finally { $g.Dispose() }
    $bmp
}

function Get-PixelBytes([System.Drawing.Bitmap]$bmp) {
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $bits = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $bytes = New-Object byte[] ($bits.Stride * $bmp.Height)
        [System.Runtime.InteropServices.Marshal]::Copy($bits.Scan0, $bytes, 0, $bytes.Length)
        [pscustomobject]@{ Bytes = $bytes; Stride = $bits.Stride }
    } finally { $bmp.UnlockBits($bits) }
}

function Set-PixelBytes([System.Drawing.Bitmap]$bmp, [byte[]]$bytes) {
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $bits = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $bits.Scan0, $bytes.Length)
    } finally { $bmp.UnlockBits($bits) }
}

# Ink box of the glyph, in em units relative to the draw origin. The symbol fonts
# leave typographic padding around their drawings and the battery is much wider
# than it is tall, so laying the icon out from these metrics is what makes it
# fill the frame instead of floating in the middle of it.
function Measure-Ink {
    $em = 256
    $probe = New-GlyphBitmap ($em * 2) $em ($em * 0.5) ($em * 0.5)
    try {
        $pixels = Get-PixelBytes $probe
        $left = $probe.Width; $top = $probe.Height; $right = -1; $bottom = -1
        for ($y = 0; $y -lt $probe.Height; $y++) {
            $row = $y * $pixels.Stride
            for ($x = 0; $x -lt $probe.Width; $x++) {
                if ($pixels.Bytes[$row + $x * 4 + 3] -eq 0) { continue }
                if ($x -lt $left) { $left = $x }
                if ($x -gt $right) { $right = $x }
                if ($y -lt $top) { $top = $y }
                if ($y -gt $bottom) { $bottom = $y }
            }
        }
        if ($right -lt 0) { throw "Glyph U+$Glyph is blank in $($family.Name)" }
        [pscustomobject]@{
            Left   = ($left - $em * 0.5) / $em
            Top    = ($top - $em * 0.5) / $em
            Width  = ($right - $left + 1) / $em
            Height = ($bottom - $top + 1) / $em
        }
    } finally { $probe.Dispose() }
}

function New-Frame([int]$size, $ink) {
    # One pixel of breathing room at 16px, scaled up from there.
    $margin = [math]::Max(1, [int][math]::Round($size / 16))
    $available = $size - 2 * $margin
    $scale = $Supersample
    $canvas = $size * $scale
    $em = [single]([math]::Min($available / $ink.Width, $available / $ink.Height) * $scale)
    $x = [single](($canvas - $ink.Width * $em) / 2 - $ink.Left * $em)
    $y = [single](($canvas - $ink.Height * $em) / 2 - $ink.Top * $em)

    $large = New-GlyphBitmap $canvas $em $x $y
    try {
        $frame = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($frame)
        try {
            $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.DrawImage($large, (New-Object System.Drawing.Rectangle(0, 0, $size, $size)))
        } finally { $g.Dispose() }
    } finally { $large.Dispose() }

    # Alpha is the glyph coverage; the color is flat everywhere, including under
    # fully transparent pixels, so no interpolation can tint the edges.
    $pixels = Get-PixelBytes $frame
    for ($i = 0; $i -lt $pixels.Bytes.Length; $i += 4) {
        $pixels.Bytes[$i] = $rgb.B
        $pixels.Bytes[$i + 1] = $rgb.G
        $pixels.Bytes[$i + 2] = $rgb.R
    }
    Set-PixelBytes $frame $pixels.Bytes
    $frame
}

function ConvertTo-IcoBitmap([System.Drawing.Bitmap]$bmp) {
    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter($stream)
    try {
        # BITMAPINFOHEADER, with the doubled height an icon directory expects: the
        # XOR bits are followed by an AND mask of the same dimensions.
        $writer.Write([int]40)
        $writer.Write([int]$bmp.Width)
        $writer.Write([int]($bmp.Height * 2))
        $writer.Write([int16]1)
        $writer.Write([int16]32)
        $writer.Write([int]0)  # BI_RGB
        $writer.Write([int]0)  # biSizeImage, ignored for BI_RGB
        $writer.Write([int]0); $writer.Write([int]0)
        $writer.Write([int]0); $writer.Write([int]0)

        # Bottom-up BGRA with straight (non-premultiplied) alpha, which is what
        # the shell expects from a 32bpp icon frame.
        $pixels = Get-PixelBytes $bmp
        for ($y = $bmp.Height - 1; $y -ge 0; $y--) {
            $writer.Write($pixels.Bytes, $y * $pixels.Stride, $bmp.Width * 4)
        }

        # Zeroed AND mask, rows padded to 32 bits: visibility is the alpha channel's job.
        $stride = [int]([math]::Floor(($bmp.Width + 31) / 32) * 4)
        $writer.Write((New-Object byte[] ($stride * $bmp.Height)), 0, $stride * $bmp.Height)
        $writer.Flush()
        $stream.ToArray()
    } finally { $writer.Dispose() }
}

function ConvertTo-IcoPng([System.Drawing.Bitmap]$bmp) {
    $stream = New-Object System.IO.MemoryStream
    try {
        $bmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        $stream.ToArray()
    } finally { $stream.Dispose() }
}

$ink = Measure-Ink
$frames = @()
foreach ($size in ($Sizes | Sort-Object)) {
    $bmp = New-Frame $size $ink
    try {
        # PNG compression is only universally understood at 256x256; the small
        # frames stay uncompressed so nothing has to guess.
        $bytes = if ($size -ge 256) { ConvertTo-IcoPng $bmp } else { ConvertTo-IcoBitmap $bmp }
    } finally { $bmp.Dispose() }
    $frames += [pscustomobject]@{ Size = $size; Bytes = $bytes }
    Write-Host ("Frame: {0,3}x{1,-3} {2,7:N0} bytes" -f $size, $size, $bytes.Length)
}

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([int16]0)                  # reserved
    $writer.Write([int16]1)                  # ICO
    $writer.Write([int16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        # 256 is written as 0: the directory stores each side in a single byte.
        $side = if ($frame.Size -ge 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$side)
        $writer.Write([byte]$side)
        $writer.Write([byte]0)               # palette entries
        $writer.Write([byte]0)               # reserved
        $writer.Write([int16]1)              # color planes
        $writer.Write([int16]32)             # bits per pixel
        $writer.Write([int]$frame.Bytes.Length)
        $writer.Write([int]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write($frame.Bytes, 0, $frame.Bytes.Length) }
    $writer.Flush()
    [System.IO.File]::WriteAllBytes($OutFile, $stream.ToArray())
} finally { $writer.Dispose() }

Write-Host ("Wrote: {0} ({1:N0} bytes)" -f $OutFile, (Get-Item $OutFile).Length)
