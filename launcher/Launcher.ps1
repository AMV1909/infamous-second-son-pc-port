param([switch]$Check, [switch]$Preview)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$settingsFile = Join-Path $PSScriptRoot 'settings.json'
$profiles = [ordered]@{
    'Aprovada' = 'Start-Upload-Fix-Test.cmd'
    'Precompiled' = 'Start-Precompiled.cmd'
    'Fast' = 'Start-Fast.cmd'
    'Standard' = 'Start-Standard.cmd'
}
function Get-LaunchPlan([string]$profile, [string]$display) {
    if (!$profiles.Contains($profile)) { throw 'Unknown launch profile.' }
    if ($display -notin @('Windowed','Fullscreen')) { throw 'Unknown display mode.' }
    $script = Join-Path $root ('scripts/play/' + $profiles[$profile])
    foreach ($relative in @('pc-port/out/iss-probe.exe','pc-port/out/iss-gpu.dll','pc-port/out/SDL3.dll','pc-port/out/boot-linked.bin','pc-port/out/link.json','pc-port/scripts/run_probe.py')) {
        if (!(Test-Path -LiteralPath (Join-Path $root $relative))) { throw "Required file missing: $relative" }
    }
    if (!(Test-Path -LiteralPath $script)) { throw "Launch script missing: $script" }
    if (!(Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python is required. Install Python and add it to PATH.' }
    if ($profile -in @('Aprovada','Precompiled') -and !(Test-Path -LiteralPath (Join-Path $root 'pc-port/out/pipeline-library/CUSA00004-packed/manifest.json'))) {
        throw 'The pipeline library is missing. Select Fast or Standard, or prepare the library first.'
    }
    if ($profile -eq 'Aprovada') {
        $pythonw = Join-Path (Split-Path (Get-Command python).Source) 'pythonw.exe'
        if (!(Test-Path -LiteralPath $pythonw)) { throw "Python GUI runner missing: $pythonw" }
        foreach ($relative in @('pc-port/out/releases/20261011-upload-fix/iss-probe.exe',
                                'pc-port/out/releases/20261011-upload-fix/iss-gpu.dll',
                                'pc-port/out/pipeline-library-runtime-trusted/CUSA00004/profile.bin')) {
            if (!(Test-Path -LiteralPath (Join-Path $root $relative))) { throw "Approved build file missing: $relative" }
        }
    }
    [pscustomobject]@{Script=$script;Profile=$profile;Display=$display;Fullscreen=$(if($display -eq 'Fullscreen'){'1'}else{'0'});Icon=(Join-Path $PSScriptRoot 'assets/game.bmp')}
}
if ($Check) {
    foreach ($profile in $profiles.Keys) {
        foreach ($display in @('Windowed','Fullscreen')) { Get-LaunchPlan $profile $display }
    }
    exit 0
}
[System.Windows.Forms.Application]::EnableVisualStyles()
$form = New-Object Windows.Forms.Form
$form.Text = 'inFAMOUS Second Son'
$form.ClientSize = New-Object Drawing.Size(580,510)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.BackColor = [Drawing.Color]::FromArgb(23,23,25)
$form.ForeColor = [Drawing.Color]::WhiteSmoke
$form.Font = New-Object Drawing.Font('Segoe UI',10)
$form.Icon = New-Object Drawing.Icon((Join-Path $PSScriptRoot 'assets/game.ico'))
$picture = New-Object Windows.Forms.PictureBox
$picture.SetBounds(0,0,580,190)
$picture.BackColor = [Drawing.Color]::FromArgb(15,15,15)
$picture.SizeMode = 'Zoom'
$picture.Image = [Drawing.Image]::FromFile((Join-Path $PSScriptRoot 'assets/game.png'))
$form.Controls.Add($picture)
function Add-Label($text,$x,$y,$width,$height) {
    $label = New-Object Windows.Forms.Label
    $label.Text=$text; $label.SetBounds($x,$y,$width,$height)
    $form.Controls.Add($label)
}
Add-Label 'PC PORT  /  EXPERIMENTAL' 24 207 520 24
Add-Label 'Launch profile' 24 245 260 24
Add-Label 'Display mode' 310 245 240 24
$profileBox = New-Object Windows.Forms.ComboBox
$profileBox.SetBounds(24,272,260,30); $profileBox.DropDownStyle='DropDownList'
[void]$profileBox.Items.AddRange([object[]]@($profiles.Keys))
$profileBox.SelectedIndex=0
$displayBox = New-Object Windows.Forms.ComboBox
$displayBox.SetBounds(310,272,246,30); $displayBox.DropDownStyle='DropDownList'
[void]$displayBox.Items.AddRange([object[]]@('Windowed','Fullscreen'))
$displayBox.SelectedIndex=0
$form.Controls.AddRange(@($profileBox,$displayBox))
if (Test-Path -LiteralPath $settingsFile) {
    try {
        $saved = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json
        if ($profileBox.Items.Contains($saved.Profile)) { $profileBox.SelectedItem=$saved.Profile }
        if ($displayBox.Items.Contains($saved.Display)) { $displayBox.SelectedItem=$saved.Display }
    } catch { }
}
Add-Label 'Aprovada: cidade, shaders, oclusão real e logs desligados.' 24 315 532 24
Add-Label 'Alt+Enter: fullscreen   /   F9: FPS   /   F10: performance marker' 24 341 532 24
function Add-Button($text,$x,$y,$width,$handler) {
    $button=New-Object Windows.Forms.Button
    $button.Text=$text; $button.SetBounds($x,$y,$width,42)
    $button.FlatStyle='Flat'; $button.BackColor=[Drawing.Color]::FromArgb(40,40,43)
    $button.Add_Click($handler); $form.Controls.Add($button)
    return $button
}
$play = Add-Button 'PLAY' 24 384 532 {
    try {
        $plan=Get-LaunchPlan ([string]$profileBox.SelectedItem) ([string]$displayBox.SelectedItem)
        $running=Get-Process -Name 'iss-probe' -ErrorAction SilentlyContinue
        if ($running) { throw 'The port is already running. Close it before starting another session.' }
        @{Profile=$plan.Profile;Display=$plan.Display} | ConvertTo-Json | Set-Content -LiteralPath $settingsFile -Encoding utf8
        $env:ISS_FULLSCREEN=$plan.Fullscreen
        $env:ISS_WINDOW_ICON=$plan.Icon
        foreach ($key in @('ISS_PIPELINE_LIBRARY','ISS_PIPELINE_CACHE_ROOT','ISS_PIPELINE_EVENTS','ISS_FAULT_WINDOW_KB','ISS_UPLOAD_WORKER','ISS_TUNING','ISS_BARRIER_POOL','SHADPS4_FAST_STAGE_LOOKUP','SHADPS4_REUSE_DESCRIPTOR_STORAGE','ISS_TLS_PATCH','ISS_THREAD_PRIORITY','ISS_MAPPING_INDEX','ISS_MINIMAL_LOGS','ISS_CP_SPLIT','ISS_RING_BARRIERS','ISS_REAL_OCCLUSION','ISS_PREWARM_LARGE_UPLOAD')) {
            [Environment]::SetEnvironmentVariable($key,$null,'Process')
        }
        if ($plan.Profile -eq 'Aprovada') {
            # Run the approved runner without a console host. Windows reported
            # intermittent 0xc0000142 while starting both cmd.exe and python.exe
            # from the button; pythonw.exe has no console initialization step.
            $python = (Get-Command python -ErrorAction Stop).Source
            $pythonw = Join-Path (Split-Path $python) 'pythonw.exe'
            if (!(Test-Path -LiteralPath $pythonw)) { throw "Python GUI runner missing: $pythonw" }
            Start-Process -FilePath $pythonw -ArgumentList @('pc-port/scripts/run_probe.py','--linked','--entry','--gpu','--timeout','0','--approved') -WorkingDirectory $root -WindowStyle Hidden
        } else {
            Start-Process -FilePath $env:ComSpec -ArgumentList ('/d /c ""'+$plan.Script+'""') -WorkingDirectory $root -WindowStyle Normal
        }
        $form.Close()
    } catch { [void][Windows.Forms.MessageBox]::Show($_.Exception.Message,'Unable to start','OK','Error') }
}
$play.BackColor=[Drawing.Color]::FromArgb(246,190,30)
$play.ForeColor=[Drawing.Color]::FromArgb(20,20,20)
$play.Font=New-Object Drawing.Font('Segoe UI',12,[Drawing.FontStyle]::Bold)
[void](Add-Button 'Logs' 24 446 164 { Start-Process explorer.exe -ArgumentList ('"'+(Join-Path $root 'pc-port/out/runs')+'"') })
[void](Add-Button 'Saves' 208 446 164 { Start-Process explorer.exe -ArgumentList ('"'+(Join-Path $root 'pc-port/out/user/city-savedata')+'"') })
[void](Add-Button 'Handoffs' 392 446 164 { Start-Process explorer.exe -ArgumentList ('"'+(Join-Path $root 'docs/handoffs')+'"') })
if ($Preview) {
    $form.Show(); [Windows.Forms.Application]::DoEvents()
    $bitmap=New-Object Drawing.Bitmap($form.Width,$form.Height)
    $form.DrawToBitmap($bitmap,(New-Object Drawing.Rectangle(0,0,$form.Width,$form.Height)))
    $bitmap.Save((Join-Path $PSScriptRoot 'preview.png'))
    $bitmap.Dispose();$form.Close()
} else { [void]$form.ShowDialog() }
$picture.Image.Dispose();$form.Icon.Dispose();$form.Dispose()
