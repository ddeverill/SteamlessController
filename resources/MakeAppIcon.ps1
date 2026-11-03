# Builds resources\SteamlessController.ico, the application's icon: the gamepad
# glyph on a blue tile, as in the tray panel's header and the Customize window.
#
#   powershell -ExecutionPolicy Bypass -File resources\MakeAppIcon.ps1
#
# Windows chooses between the sizes in an .ico by DPI and by where the icon is
# shown, so every size is drawn on its own rather than scaled from one master.
# Each is rendered at 4x and scaled down, which is what keeps the edges clean at
# 16 pixels. Frames are stored as PNG, as the existing icons already are.
#
# The outline below is the same path as TrayPanel.cpp (BuildPadGeometry) and the
# SVG in the Customize window's title bar: a 256-unit box, body spanning x 20..236
# and y 40..202, ring centred at 128,98. Change one, change them all.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes   = 16, 20, 24, 32, 40, 48, 64, 128, 256
$out     = Join-Path $PSScriptRoot 'SteamlessController.ico'

$tileTop = [Drawing.Color]::FromArgb(255, 0x1a, 0x9f, 0xff)
$tileBot = [Drawing.Color]::FromArgb(255, 0x0a, 0x4a, 0x78)
$glyph   = [Drawing.Color]::FromArgb(255, 0xbf, 0xe3, 0xff)

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

function New-IconBitmap([int]$size) {
    $ss  = 4
    $big = $size * $ss
    $bmp = New-Object Drawing.Bitmap $big, $big, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)

    # The tile fills the canvas with a hair of margin so the corner anti-aliasing
    # is not clipped, and rounds by about a fifth of its width, as the panel's does.
    $m    = $big * 0.03
    $side = $big - 2 * $m
    $tile = New-RoundedRect $m $m $side $side ($side * 0.22)
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush ((New-Object Drawing.PointF 0, 0), (New-Object Drawing.PointF $big, $big), $tileTop, $tileBot)
    $g.FillPath($grad, $tile)

    # A faint sheen over the top half, so the tile reads as a surface at large
    # sizes. Clipped to the tile; invisible at 16 where it would only be mud.
    if ($size -ge 32) {
        $g.SetClip($tile)
        $sheen = New-Object Drawing.Drawing2D.LinearGradientBrush ((New-Object Drawing.PointF 0, 0), (New-Object Drawing.PointF 0, ($big * 0.55)),
            [Drawing.Color]::FromArgb(46, 255, 255, 255), [Drawing.Color]::FromArgb(0, 255, 255, 255))
        $g.FillRectangle($sheen, 0, 0, $big, $big * 0.55)
        $g.ResetClip()
        $sheen.Dispose()
    }

    # The glyph, centred, at about three fifths of the tile's width. Its box is
    # x 20..236 by y 40..202, so (128, 121) is its middle.
    $scale = ($side * 0.62) / 216
    $g.TranslateTransform([single]($big / 2), [single]($big / 2))
    $g.ScaleTransform([single]$scale, [single]$scale)
    $g.TranslateTransform(-128, -121)
    $pad = New-PadPath
    $g.FillPath((New-Object Drawing.SolidBrush $glyph), $pad)

    # The ring is cut from the glyph in the tile's own dark end. Heavier at small
    # sizes, where the proportional width would be a fraction of a pixel.
    $ringW = if ($size -le 32) { 17 } else { 10 }
    $ringR = if ($size -le 32) { 24 } else { 25 }
    $pen = New-Object Drawing.Pen $tileBot, $ringW
    $g.DrawEllipse($pen, (128 - $ringR), (98 - $ringR), (2 * $ringR), (2 * $ringR))
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

$frames = foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $ms  = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
}

# ICONDIR, one ICONDIRENTRY per frame, then the frames. A width or height of 256 is
# stored as 0.
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
[IO.File]::WriteAllBytes($out, $ico.ToArray())
"wrote $out ($((Get-Item $out).Length) bytes, $($frames.Count) sizes: $($sizes -join ', '))"
