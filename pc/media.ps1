# Prints the current Windows media session once per second (read-only):
#   status<TAB>title<TAB>artist<TAB>position_s<TAB>duration_s
# or the single word NONE when nothing is registered. Used by agent.py.
param([int]$ParentPid = 0)   # exit when the agent that started us is gone
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(-1) | Out-Null; $t.Result
}
[void][Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$mgrType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]
$propType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties]
$mgr = Await ($mgrType::RequestAsync()) $mgrType
while ($true) {
    if ($ParentPid -and -not (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue)) { exit }
    try {
        $s = $mgr.GetCurrentSession()
        if (-not $s) { 'NONE' }
        else {
            $p = Await ($s.TryGetMediaPropertiesAsync()) $propType
            $i = $s.GetPlaybackInfo(); $t = $s.GetTimelineProperties()
            ('{0}' + [char]9 + '{1}' + [char]9 + '{2}' + [char]9 + '{3}' + [char]9 + '{4}') -f `
                $i.PlaybackStatus, $p.Title, $p.Artist, [int]$t.Position.TotalSeconds, [int]$t.EndTime.TotalSeconds
        }
    } catch { 'NONE' }
    [Console]::Out.Flush()
    Start-Sleep -Seconds 1
}
