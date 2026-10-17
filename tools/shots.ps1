# Launch an exe, follow a script of "wait;keys" steps, screenshot after each,
# and tile the shots into one contact sheet.
# Usage: shots.ps1 -Exe <path> [-Args ""] -Out <sheet.png> -Steps "4;","1;{F1}","3;"
param([string]$Exe, [string]$ExeArgs = "", [string]$Out, [string[]]$Steps, [int]$Cols = 3)
$tools = Split-Path $MyInvocation.MyCommand.Path
$dir = Split-Path $Out
$wd = Split-Path (Split-Path $Exe)
if ($ExeArgs) { $p = Start-Process -FilePath $Exe -ArgumentList $ExeArgs -WorkingDirectory $wd -PassThru }
else { $p = Start-Process -FilePath $Exe -WorkingDirectory $wd -PassThru }
$shots = @()
$i = 0
foreach ($s in $Steps) {
    $wait, $keys = $s.Split(';', 2)
    Start-Sleep -Milliseconds ([int]([double]$wait * 1000))
    $f = Join-Path $dir ("step{0}.png" -f $i)
    & "$tools\winshot.ps1" -ProcId $p.Id -Out $f -Keys $keys -Wait 0.3
    $shots += $f
    $i++
}
Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Drawing
$imgs = $shots | % { [System.Drawing.Image]::FromFile($_) }
$w = [int]($imgs[0].Width / 2); $h = [int]($imgs[0].Height / 2)
$rows = [math]::Ceiling($imgs.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($w * [math]::Min($Cols, $imgs.Count)), ($h * $rows)
$g = [System.Drawing.Graphics]::FromImage($sheet)
for ($k = 0; $k -lt $imgs.Count; $k++) { $g.DrawImage($imgs[$k], ($k % $Cols) * $w, [math]::Floor($k / $Cols) * $h, $w, $h) }
$sheet.Save($Out)
$imgs | % { $_.Dispose() }
