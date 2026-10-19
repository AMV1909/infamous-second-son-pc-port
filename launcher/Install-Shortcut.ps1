$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
$source=Join-Path $root 'pc-port/out/user/savedata/1/CUSA00004/infamousssAuto0.sce_sys/icon0.png'
$assets=Join-Path $PSScriptRoot 'assets'
if (!(Test-Path -LiteralPath (Join-Path $assets 'game.png'))) {
    Copy-Item -LiteralPath $source -Destination (Join-Path $assets 'game.png')
}
# Encode the original save artwork for SDL and Windows; preserve its proportions.
$original=[Drawing.Image]::FromFile((Join-Path $assets 'game.png'))
$original.Save((Join-Path $assets 'game.bmp'),[Drawing.Imaging.ImageFormat]::Bmp)
$frames=@()
foreach($size in @(16,24,32,48,64,128,256)) {
    $bitmap=New-Object Drawing.Bitmap($size,$size)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $height=[int]($size*$original.Height/$original.Width)
    $graphics.DrawImage($original,0,[int](($size-$height)/2),$size,$height)
    $stream=New-Object IO.MemoryStream
    $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $frames+=,[pscustomobject]@{size=$size;bytes=$stream.ToArray()}
    $stream.Dispose();$graphics.Dispose();$bitmap.Dispose()
}
$file=[IO.File]::Create((Join-Path $assets 'game.ico'))
$writer=New-Object IO.BinaryWriter($file)
$writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$frames.Count)
$offset=6+16*$frames.Count
foreach($frame in $frames) {
    $dimension=if($frame.size -eq 256){0}else{$frame.size}
    $writer.Write([byte]$dimension);$writer.Write([byte]$dimension)
    $writer.Write([byte]0);$writer.Write([byte]0)
    $writer.Write([uint16]1);$writer.Write([uint16]32)
    $writer.Write([uint32]$frame.bytes.Length);$writer.Write([uint32]$offset)
    $offset+=$frame.bytes.Length
}
foreach($frame in $frames){$writer.Write([byte[]]$frame.bytes)}
$writer.Dispose();$original.Dispose()
$shell=New-Object -ComObject WScript.Shell
$shortcut=$shell.CreateShortcut((Join-Path $root 'inFAMOUS Second Son.lnk'))
$shortcut.TargetPath=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$shortcut.Arguments='-NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+(Join-Path $PSScriptRoot 'Launcher.ps1')+'"'
$shortcut.WorkingDirectory=$root
$shortcut.IconLocation=(Join-Path $assets 'game.ico')+',0'
$shortcut.Description='Launch inFAMOUS Second Son PC Port'
$shortcut.Save()
