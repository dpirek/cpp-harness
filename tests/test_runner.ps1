param(
    [switch]$ValidateOnly,
    [switch]$VerifyExecution,
    [string]$TestExecutable='build\chat-tests.exe',
    [string]$AppExecutable='build\chat.exe'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$script:root = Split-Path $PSScriptRoot -Parent
$script:queue = New-Object 'System.Collections.Generic.Queue[int]'
$script:active = $null
$script:results = @()
$script:suites = @(
    @{ Name='Core and integration'; Description='C++ checks: formatting, configuration, history, streaming, cancellation, PowerShell, MCP and agent tools. Includes desktop capture and input.' },
    @{ Name='Window smoke test'; Description='Launch the app and check caption controls, resizing, minimizing, maximizing and window actions.' },
    @{ Name='UI and mock API'; Description='Launch the app with a local API fixture; check conversations, model selection, attachments and MCP settings.' }
)

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Harness test runner'
$form.Size = New-Object System.Drawing.Size(1120,820)
$form.MinimumSize = New-Object System.Drawing.Size(880,650)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI',10)
$layout = New-Object System.Windows.Forms.TableLayoutPanel
$layout.Dock = 'Fill'
$layout.Padding = New-Object System.Windows.Forms.Padding(16)
$layout.ColumnCount = 1
$layout.RowCount = 8
foreach ($height in @(38,32,32,44,155,34)) {
    [void]$layout.RowStyles.Add((New-Object System.Windows.Forms.RowStyle('Absolute',$height)))
}
[void]$layout.RowStyles.Add((New-Object System.Windows.Forms.RowStyle('Percent',55)))
[void]$layout.RowStyles.Add((New-Object System.Windows.Forms.RowStyle('Percent',45)))
$form.Controls.Add($layout)
$title = New-Object System.Windows.Forms.Label
$title.Text = 'Test automation'
$title.Font = New-Object System.Drawing.Font('Segoe UI',18,[System.Drawing.FontStyle]::Bold)
$title.AutoSize = $true
$layout.Controls.Add($title,0,0)

function Add-PathRow($label,$value,$row) {
    $panel = New-Object System.Windows.Forms.TableLayoutPanel
    $panel.Dock = 'Fill'; $panel.ColumnCount = 2
    [void]$panel.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute',160)))
    [void]$panel.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Percent',100)))
    $caption = New-Object System.Windows.Forms.Label
    $caption.Text = $label; $caption.AutoSize = $true
    $box = New-Object System.Windows.Forms.TextBox
    $box.Text = $value; $box.Dock = 'Fill'
    $panel.Controls.Add($caption,0,0); $panel.Controls.Add($box,1,0)
    $layout.Controls.Add($panel,0,$row)
    return $box
}
$testPath = Add-PathRow 'C++ test executable' $TestExecutable 1
$appPath = Add-PathRow 'App executable' $AppExecutable 2
$buttons = New-Object System.Windows.Forms.FlowLayoutPanel
$buttons.Dock = 'Fill'
function Add-Button($text) {
    $button = New-Object System.Windows.Forms.Button
    $button.Text = $text; $button.AutoSize = $true; $button.Height = 32
    $buttons.Controls.Add($button)
    return $button
}
$runSelected = Add-Button 'Run selected suite'
$runAll = Add-Button 'Run all suites'
$stop = Add-Button 'Stop'
$save = Add-Button 'Save report'
$stop.Enabled = $false
$layout.Controls.Add($buttons,0,3)

function New-Grid {
    $grid = New-Object System.Windows.Forms.DataGridView
    $grid.Dock = 'Fill'; $grid.ReadOnly = $true
    $grid.AllowUserToAddRows = $false; $grid.AllowUserToDeleteRows = $false
    $grid.RowHeadersVisible = $false; $grid.MultiSelect = $false
    $grid.SelectionMode = 'FullRowSelect'
    $grid.AutoSizeColumnsMode = 'Fill'
    $grid.BackgroundColor = [System.Drawing.Color]::White
    return $grid
}
$suitesGrid = New-Grid
foreach ($column in @('Suite','Description','Status','Seconds')) { [void]$suitesGrid.Columns.Add($column,$column) }
$suitesGrid.Columns[0].FillWeight=22; $suitesGrid.Columns[1].FillWeight=65
$suitesGrid.Columns[2].FillWeight=13; $suitesGrid.Columns[3].FillWeight=10
$suitesGrid.AutoSizeRowsMode = 'AllCells'
$suitesGrid.Columns[1].DefaultCellStyle.WrapMode = 'True'
foreach ($suite in $script:suites) { [void]$suitesGrid.Rows.Add($suite.Name,$suite.Description,'Not run','') }
$layout.Controls.Add($suitesGrid,0,4)
$summary = New-Object System.Windows.Forms.Label
$summary.Text = 'Ready. Select a suite or run all. Desktop tests need an unlocked desktop and may move the pointer.'
$summary.Dock = 'Fill'; $summary.TextAlign = 'MiddleLeft'
$layout.Controls.Add($summary,0,5)
$checks = New-Grid
foreach ($column in @('Suite','Result','Check description')) { [void]$checks.Columns.Add($column,$column) }
$checks.Columns[0].FillWeight=22; $checks.Columns[1].FillWeight=12; $checks.Columns[2].FillWeight=80
$layout.Controls.Add($checks,0,6)
$log = New-Object System.Windows.Forms.RichTextBox
$log.Dock = 'Fill'; $log.ReadOnly = $true
$log.Font = New-Object System.Drawing.Font('Consolas',9)
$log.BackColor = [System.Drawing.Color]::FromArgb(25,28,34)
$log.ForeColor = [System.Drawing.Color]::FromArgb(225,230,238)
$layout.Controls.Add($log,0,7)

function Set-Status($index,$status) {
    $cell = $suitesGrid.Rows[$index].Cells[2]
    $cell.Value = $status
    $cell.Style.ForeColor = switch ($status) {
        'Passed' { [System.Drawing.Color]::ForestGreen }
        'Failed' { [System.Drawing.Color]::Firebrick }
        'Running' { [System.Drawing.Color]::RoyalBlue }
        default { [System.Drawing.Color]::DimGray }
    }
}
function Write-OutputLine($line) {
    $log.AppendText($line + [Environment]::NewLine)
    if ($script:active) { [void]$script:active.Lines.Add($line) }
    if ($line -match '^(PASS|FAIL):\s*(.*)') {
        $result = $Matches[1]; $description = $Matches[2]
        $row = $checks.Rows.Add($script:suites[$script:active.Index].Name,$result,$description)
        $checks.Rows[$row].Cells[1].Style.ForeColor = if ($result -eq 'PASS') { [System.Drawing.Color]::ForestGreen } else { [System.Drawing.Color]::Firebrick }
        $checks.FirstDisplayedScrollingRowIndex = $row
    }
}
function Resolve-Executable($value) {
    $candidate = if ([IO.Path]::IsPathRooted($value)) { $value } else { Join-Path $script:root $value }
    if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { throw "Executable missing: $candidate. Build it first or update the path above." }
    return (Resolve-Path -LiteralPath $candidate).Path
}
function Set-Busy($busy) {
    $runSelected.Enabled = !$busy; $runAll.Enabled = !$busy
    $testPath.Enabled = !$busy; $appPath.Enabled = !$busy
    $stop.Enabled = $busy; $save.Enabled = !$busy
}
function Start-Next {
    if (!$script:queue.Count) {
        Set-Busy $false
        $passed = @($script:results | Where-Object Status -eq 'Passed').Count
        $failed = @($script:results | Where-Object Status -eq 'Failed').Count
        $stopped = @($script:results | Where-Object Status -eq 'Stopped').Count
        $summary.Text = "Finished: $passed passed, $failed failed, $stopped stopped. Select a suite to run again."
        return
    }
    $index = $script:queue.Dequeue()
    $script:active = @{ Index=$index; Process=$null; Started=[DateTime]::UtcNow; Lines=(New-Object 'System.Collections.Generic.List[string]'); Stopped=$false; TimedOut=$false }
    Set-Status $index 'Running'
    $summary.Text = "Running: $($script:suites[$index].Name). Desktop tests may take focus."
    Write-OutputLine "--- $($script:suites[$index].Name) ---"
    try {
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.WorkingDirectory = $script:root
        $info.UseShellExecute = $false; $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
        if ($index -eq 1) {
            $exe = Resolve-Executable $appPath.Text
            $info.FileName = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
            $info.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $PSScriptRoot 'ui_smoke.ps1') + '" -Executable "' + $exe + '"'
        } else {
            $info.FileName = (Get-Command node -CommandType Application -ErrorAction Stop).Source
            $exe = if ($index -eq 0) { Resolve-Executable $testPath.Text } else { Resolve-Executable $appPath.Text }
            $info.Arguments = '"' + (Join-Path $PSScriptRoot 'mock_server.cjs') + '" --test-executable "' + $exe + '"'
            if ($index -eq 2) { $info.Arguments += ' --ui' }
        }
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $info
        [void]$process.Start()
        $script:active.Process = $process
        $script:active.OutTask = $process.StandardOutput.ReadLineAsync()
        $script:active.ErrTask = $process.StandardError.ReadLineAsync()
    } catch {
        Write-OutputLine ('FAIL: ' + $_.Exception.Message)
        Complete-Active 'Failed'
    }
}
function Complete-Active($status) {
    $seconds = [Math]::Round(([DateTime]::UtcNow - $script:active.Started).TotalSeconds,2)
    $index = $script:active.Index
    Set-Status $index $status
    $suitesGrid.Rows[$index].Cells[3].Value = $seconds
    $script:results += [pscustomobject]@{ Suite=$script:suites[$index].Name; Description=$script:suites[$index].Description; Status=$status; Seconds=$seconds; Output=@($script:active.Lines.ToArray()) }
    if ($script:active.Process) { $script:active.Process.Dispose() }
    $script:active = $null
}
function Stop-ProcessTree {
    if ($script:active -and $script:active.Process -and !$script:active.Process.HasExited) {
        $kill = New-Object System.Diagnostics.ProcessStartInfo
        $kill.FileName = "$env:SystemRoot\System32\taskkill.exe"
        $kill.Arguments = "/PID $($script:active.Process.Id) /T /F"
        $kill.UseShellExecute = $false; $kill.CreateNoWindow = $true
        $killer = [System.Diagnostics.Process]::Start($kill)
        $killer.Dispose()
    }
}
function Start-Run($indices) {
    $script:results = @(); $checks.Rows.Clear(); $log.Clear()
    foreach ($i in 0..2) { Set-Status $i 'Not run'; $suitesGrid.Rows[$i].Cells[3].Value='' }
    foreach ($i in $indices) { $script:queue.Enqueue($i); Set-Status $i 'Queued' }
    Set-Busy $true
    Start-Next
}
$runSelected.Add_Click({ if ($suitesGrid.SelectedRows.Count) { Start-Run @([int]$suitesGrid.SelectedRows[0].Index) } })
$runAll.Add_Click({ Start-Run @(0,1,2) })
$stop.Add_Click({
    foreach ($i in $script:queue.ToArray()) { Set-Status $i 'Not run' }
    $script:queue.Clear()
    if ($script:active) { $script:active.Stopped=$true; Stop-ProcessTree }
})
$save.Add_Click({
    $dialog = New-Object System.Windows.Forms.SaveFileDialog
    $dialog.Filter='JSON report (*.json)|*.json'; $dialog.FileName='test-report.json'
    if ($dialog.ShowDialog() -eq 'OK') {
        try { ConvertTo-Json -InputObject @($script:results) -Depth 5 | Set-Content -LiteralPath $dialog.FileName -Encoding UTF8 }
        catch { [void][System.Windows.Forms.MessageBox]::Show($_.Exception.Message,'Could not save report') }
    }
    $dialog.Dispose()
})
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval=80
$timer.Add_Tick({
    if (!$script:active) { if ($stop.Enabled) { Start-Next }; return }
    $process = $script:active.Process
    if (!$process) { return }
    foreach ($stream in @('Out','Err')) {
        $key = $stream + 'Task'
        $reader = if ($stream -eq 'Out') { $process.StandardOutput } else { $process.StandardError }
        $count=0
        while ($script:active[$key] -and $script:active[$key].IsCompleted -and $count -lt 200) {
            $line = $script:active[$key].GetAwaiter().GetResult()
            if ($null -eq $line) { $script:active[$key]=$null; break }
            Write-OutputLine $line
            $script:active[$key] = $reader.ReadLineAsync()
            $count++
        }
    }
    if (!$process.HasExited -and !$script:active.TimedOut -and ([DateTime]::UtcNow-$script:active.Started).TotalSeconds -gt 90) {
        $script:active.TimedOut=$true
        Write-OutputLine 'FAIL: Suite exceeded the 90 second deadline.'
        Stop-ProcessTree
    }
    if ($process.HasExited -and !$script:active.OutTask -and !$script:active.ErrTask) {
        $status = if ($script:active.Stopped) { 'Stopped' } elseif ($process.ExitCode -eq 0 -and !$script:active.TimedOut) { 'Passed' } else { 'Failed' }
        Write-OutputLine "Suite $status (exit code $($process.ExitCode))"
        Complete-Active $status
        Start-Next
    }
})
$form.Add_FormClosing({ $script:queue.Clear(); Stop-ProcessTree; $timer.Stop() })
if ($ValidateOnly) {
    if ($suitesGrid.Rows.Count -ne 3 -or $stop.Enabled -or !$runAll.Enabled) { throw 'Runner initial state is invalid.' }
    Set-Status 0 'Passed'
    if ($suitesGrid.Rows[0].Cells[2].Value -ne 'Passed') { throw 'Status update failed.' }
    $testPath.Text = 'build\missing-runner-validation.exe'
    $appPath.Text = 'build\missing-runner-validation.exe'
    Start-Run @(0,1,2)
    while ($stop.Enabled) { Start-Next }
    if ($script:results.Count -ne 3 -or @($script:results | Where-Object Status -ne 'Failed').Count -ne 0 -or $checks.Rows.Count -ne 3) {
        throw 'Sequential execution or missing-executable reporting failed.'
    }
    $form.Dispose(); $timer.Dispose()
    Write-Output 'Runner UI construction, status, sequential queue and failure reporting validation passed.'
    exit 0
}
$timer.Start()
if ($VerifyExecution) {
    Start-Run @(0)
    while ($stop.Enabled) {
        [System.Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 20
    }
    $report = @($script:results)
    $timer.Dispose(); $form.Dispose()
    ConvertTo-Json -InputObject $report -Depth 5
    if ($report.Count -ne 1 -or $report[0].Status -ne 'Passed') { exit 1 }
    exit 0
}
[void]$form.ShowDialog()
$timer.Dispose(); $form.Dispose()
