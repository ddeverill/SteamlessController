# Builds the application's icons, all from the one gamepad glyph:
#
#   SteamlessController.ico   the application: the exe, its shortcuts, the installer
#   SteamControllerON.ico     the tray icon while Steamless mode is on   (blue)
#   SteamControllerOFF.ico    ... while it is off                        (grey)
#   SteamControllerSHARED.ico ... while on but sharing the pad with Steam (amber)
#
#   powershell -ExecutionPolicy Bypass -File resources\MakeIcons.ps1
#
# The tray icons are the application's tile in a state colour, not a recolour of a
# flat pad: a badge shape is what keeps them apart from the other small gamepads in
# the tray. Grey is for "off" because off is mostly an ordinary state (an auto mode
# yielding to Steam, no controller connected), and red would read as a fault; it is
# a mid grey, not the panel's dark one, so it holds up on a dark taskbar.
#
# Windows chooses between the sizes in an .ico by DPI and by where the icon is
# shown, so every size is drawn on its own rather than scaled from one master. Each
# is rendered at 4x and scaled down, which is what keeps the edges clean at 16
# pixels. Frames are stored as PNG.
#
# The outline below is the same path as TrayPanel.cpp (BuildPadGeometry) and the
# SVG in the Customize window's title bar: a 256-unit box, body spanning x 20..236
# and y 40..202, ring centred at 128,98. Change one, change them all.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Color([string]$hex) {
    [Drawing.Color]::FromArgb(255, [Convert]::ToInt32($hex.Substring(0, 2), 16),
        [Convert]::ToInt32($hex.Substring(2, 2), 16), [Convert]::ToInt32($hex.Substring(4, 2), 16))
}

function New-PadPath {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddBezier(78,44, 100,40, 170,40, 192,44)
    $p.AddBezier(192,44, 220,50, 236,80, 236,110)
    $p.AddBezier(236,110, 236,140, 232,165, 226,185)
    $p.AddBezier(226,185, 222,198, 212,202, 204,198)
    $p.AddBezier(204,198, 196,194, 192,180, 186,168)
    $p.AddBezier(186,168, 182,160, 176,158, 168,158)
    $p.AddLine(168,158, 88,158)
    $p.AddBezier(88,158, 80,158, 74,160, 70,168)
    $p.AddBezier(70,168, 64,180, 60,194, 52,198)
    $p.AddBezier(52,198, 44,202, 34,198, 30,186)
    $p.AddBezier(30,186, 24,165, 20,140, 20,110)
    $p.AddBezier(20,110, 20,80, 36,50, 78,44)
    $p.CloseFigure()
    $p
}

function New-RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $d = $r * 2
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    $p
}

# What differs between the icons: the tile's colours (top-left to bottom-right), the
# glyph's, and the ring's, which is cut from the glyph in the tile's own dark end.
$Blue  = @{ Top = '1a9fff'; Bottom = '0a4a78'; Glyph = 'bfe3ff'; Ring = '0a4a78' }
$Grey  = @{ Top = '93a0ad'; Bottom = '5d6a77'; Glyph = 'f1f5f9'; Ring = '5d6a77' }
$Amber = @{ Top = 'f2b53a'; Bottom = 'b4730a'; Glyph = 'fff4d9'; Ring = 'b4730a' }

# How big the glyph is on its tile, by size. The application's is a comfortable three
# fifths. The tray's is drawn at 16 to 24 pixels, where a tile that size leaves a
# pad too small to read, so it fills more of the tile there and loses the ring,
# which at that size is a smudge rather than a hole.
$App  = { param($s) @{ Margin = 0.03; Glyph = 0.62; Ring = $true } }
$Tray = { param($s)
    if     ($s -le 20) { @{ Margin = 0.02; Glyph = 0.78; Ring = $false } }
    elseif ($s -le 24) { @{ Margin = 0.03; Glyph = 0.72; Ring = $true } }
    else               { @{ Margin = 0.03; Glyph = 0.66; Ring = $true } }
}

function New-IconBitmap([int]$size, $palette, $tune) {
    $ss  = 4
    $big = $size * $ss
    $bmp = New-Object Drawing.Bitmap $big, $big, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)

    # The tile fills the canvas with a hair of margin so the corner anti-aliasing is
    # not clipped, and rounds by about a fifth of its width, as the panel's does.
    $m    = $big * $tune.Margin
    $side = $big - 2 * $m
    $tile = New-RoundedRect $m $m $side $side ($side * 0.22)
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush ((New-Object Drawing.PointF 0, 0), (New-Object Drawing.PointF $big, $big),
        (Color $palette.Top), (Color $palette.Bottom))
    $g.FillPath($grad, $tile)

    # A faint sheen over the top half, so the tile reads as a surface at large sizes.
    # Clipped to the tile; invisible at small ones, where it would only be mud.
    if ($size -ge 32) {
        $g.SetClip($tile)
        $sheen = New-Object Drawing.Drawing2D.LinearGradientBrush ((New-Object Drawing.PointF 0, 0), (New-Object Drawing.PointF 0, ($big * 0.55)),
            [Drawing.Color]::FromArgb(46, 255, 255, 255), [Drawing.Color]::FromArgb(0, 255, 255, 255))
        $g.FillRectangle($sheen, 0, 0, $big, $big * 0.55)
        $g.ResetClip()
        $sheen.Dispose()
    }

    # The glyph, centred. Its box is x 20..236 by y 40..202, so (128, 121) is its
    # middle.
    $scale = ($side * $tune.Glyph) / 216
    $g.TranslateTransform([single]($big / 2), [single]($big / 2))
    $g.ScaleTransform([single]$scale, [single]$scale)
    $g.TranslateTransform(-128, -121)
    $g.FillPath((New-Object Drawing.SolidBrush (Color $palette.Glyph)), (New-PadPath))

    # Heavier at small sizes, where the proportional width would be a fraction of a
    # pixel.
    if ($tune.Ring) {
        $ringW = if ($size -le 32) { 17 } else { 10 }
        $ringR = if ($size -le 32) { 24 } else { 25 }
        $pen = New-Object Drawing.Pen (Color $palette.Ring), $ringW
        $g.DrawEllipse($pen, (128 - $ringR), (98 - $ringR), (2 * $ringR), (2 * $ringR))
    }
    $g.Dispose()

    $final = New-Object Drawing.Bitmap $size, $size, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $fg = [Drawing.Graphics]::FromImage($final)
    $fg.InterpolationMode = 'HighQualityBicubic'
    $fg.PixelOffsetMode   = 'HighQuality'
    $fg.CompositingMode   = 'SourceCopy'
    $fg.DrawImage($bmp, 0, 0, $size, $size)
    $fg.Dispose(); $bmp.Dispose()
    $final
}

# ICONDIR, one ICONDIRENTRY per frame, then the frames. A width or height of 256 is
# stored as 0.
function Write-Ico([string]$name, [int[]]$sizes, $palette, $tuning) {
    $frames = foreach ($s in $sizes) {
        $bmp = New-IconBitmap $s $palette (& $tuning $s)
        $ms  = New-Object IO.MemoryStream
        $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
    }
    $ico = New-Object IO.MemoryStream
    $w   = New-Object IO.BinaryWriter $ico
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($f in $frames) {
        $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]$f.Data.Length); $w.Write([uint32]$offset)
        $offset += $f.Data.Length
    }
    foreach ($f in $frames) { $w.Write($f.Data) }
    $w.Flush()
    $path = Join-Path $PSScriptRoot $name
    [IO.File]::WriteAllBytes($path, $ico.ToArray())
    "wrote $name ($((Get-Item $path).Length) bytes: $($sizes -join ', '))"
}

# The application: every size Windows asks for, up to the 256 the shell shows large.
Write-Ico 'SteamlessController.ico'   (16, 20, 24, 32, 40, 48, 64, 128, 256) $Blue $App

# The tray asks for 16 pixels at 100% scaling and 1.25, 1.5, 1.75, 2.5 and 3 times
# that on denser displays; nothing larger is ever drawn there.
$traySizes = 16, 20, 24, 28, 32, 40, 48
Write-Ico 'SteamControllerON.ico'     $traySizes $Blue  $Tray
Write-Ico 'SteamControllerOFF.ico'    $traySizes $Grey  $Tray
Write-Ico 'SteamControllerSHARED.ico' $traySizes $Amber $Tray
