# Reads the text in an image with Windows' built-in OCR (Windows.Media.Ocr, runs locally, no model, no upload).
# Prints one line per text line: "<x> <y> <w> <h>  <text>" in image pixels (x/y = top-left corner of the line).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\ocr.ps1 <image.png> [-Scale 2]
# -Scale enlarges the image first (small menu text reads better at 2x); coordinates are still in original pixels.
# Needs Windows PowerShell 5.1 (WinRT types); an OCR language pack must be installed (English is by default).
param([Parameter(Mandatory)][string]$Image, [double]$Scale = 1)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Drawing
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
    Select-Object -First 1
function Await($op, [Type]$type) { $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(); $t.Result }

$path = (Resolve-Path $Image).Path
if ($Scale -ne 1) {
    # Enlarged copy next to the system temp folder (the source image is never changed).
    $src = [System.Drawing.Image]::FromFile($path)
    $bmp = New-Object System.Drawing.Bitmap ([int]($src.Width * $Scale)), ([int]($src.Height * $Scale))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($src, 0, 0, $bmp.Width, $bmp.Height); $g.Dispose(); $src.Dispose()
    $path = Join-Path $env:TEMP ("ocr_scaled_" + [guid]::NewGuid().ToString("N") + ".png")
    $bmp.Save($path); $bmp.Dispose()
}
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
$stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
$engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
$result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
$stream.Dispose()
if ($Scale -ne 1) { Remove-Item $path -ErrorAction SilentlyContinue }
foreach ($line in $result.Lines) {
    $xs = $line.Words | ForEach-Object { $_.BoundingRect }
    $x = ($xs | Measure-Object X -Minimum).Minimum; $y = ($xs | Measure-Object Y -Minimum).Minimum
    $r = ($xs | ForEach-Object { $_.X + $_.Width } | Measure-Object -Maximum).Maximum
    $b = ($xs | ForEach-Object { $_.Y + $_.Height } | Measure-Object -Maximum).Maximum
    "{0} {1} {2} {3}  {4}" -f [int]($x / $Scale), [int]($y / $Scale), [int](($r - $x) / $Scale), [int](($b - $y) / $Scale), $line.Text
}
