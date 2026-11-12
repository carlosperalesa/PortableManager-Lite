#Requires -Version 5.1
param(
    [ValidateSet("Prepare", "Verify", "Contain")]
    [string]$Action = "Prepare",
    [string]$UsbTestDir = "E:\PM_Lite_Fase03_Test",
    [string]$StateJsonPath = "D:\OneDrive\GitHub\PML\PortableManager-Lite\build\usb_physical_session_active.json",
    [string]$ReportJsonPath = "D:\OneDrive\GitHub\PML\PortableManager-Lite\reports\fase03_usb_physical_disconnect_20261111.json"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$InternalHelper = Join-Path $ProjectRoot "build\test_phase3_guard.exe"

if ($Action -eq "Prepare") {
    $vol = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='E:'"
    if (-not $vol -or $vol.DriveType -ne 2) {
        throw "La unidad E: no esta disponible o no es una unidad extraible (DriveType=2)."
    }
    $usbPm = Join-Path $UsbTestDir "pm.exe"
    $usbController = Join-Path $UsbTestDir "test_phase3_guard.exe"
    if (-not (Test-Path $usbPm) -or -not (Test-Path $usbController)) {
        throw "Faltan ejecutables de prueba en $UsbTestDir."
    }

    $sessionId = [guid]::NewGuid().ToString("D")
    $sessionDir = Join-Path $env:TEMP "PortableManager\Session\$sessionId"

    $outsiderCmd = "`"$InternalHelper`" --leaf"
    $outsiderWmi = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine      = $outsiderCmd
        CurrentDirectory = $env:TEMP
    }
    if ($outsiderWmi.ReturnValue -ne 0) {
        throw "Error creando proceso outsider via WMI (ReturnValue=$($outsiderWmi.ReturnValue))"
    }
    $outsiderPid = [int]$outsiderWmi.ProcessId

    $ownerCmd = "`"$usbController`" --controller `"$sessionId`" `"$usbPm`" physical-usb"
    $ownerWmi = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine      = $ownerCmd
        CurrentDirectory = $env:TEMP
    }
    if ($ownerWmi.ReturnValue -ne 0) {
        Stop-Process -Id $outsiderPid -Force -ErrorAction SilentlyContinue
        throw "Error creando proceso owner via WMI (ReturnValue=$($ownerWmi.ReturnValue))"
    }
    $ownerPid = [int]$ownerWmi.ProcessId

    $infoFile = Join-Path $sessionDir "test-info.txt"
    $manifestFile = Join-Path $sessionDir "session.ini"
    $deadline = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $infoFile) -and (Test-Path $manifestFile)) {
            $manifestRaw = [System.IO.File]::ReadAllText($manifestFile)
            if ($manifestRaw -match "State=ACTIVE") { break }
        }
        Start-Sleep -Milliseconds 100
    }

    if (-not (Test-Path $infoFile)) {
        throw "El controlador no publico test-info.txt a tiempo."
    }
    $pids = ([System.IO.File]::ReadAllText($infoFile)).Trim() -split "\s+"
    $guardPid = [int]$pids[0]
    $workerPid = [int]$pids[1]
    $leafProc = Get-CimInstance Win32_Process -Filter "ParentProcessId=$workerPid" -ErrorAction SilentlyContinue | Select-Object -First 1
    $leafPid = if ($leafProc) { [int]$leafProc.ProcessId } else { 0 }

    $stateObj = [ordered]@{
        prepared_at_utc = (Get-Date).ToUniversalTime().ToString("o")
        session_id      = $sessionId
        session_dir     = $sessionDir
        manifest_path   = $manifestFile
        diagnostic_path = (Join-Path $sessionDir "controller-diagnostic.txt")
        usb_pm_path     = $usbPm
        owner_pid       = $ownerPid
        guard_pid       = $guardPid
        worker_pid      = $workerPid
        leaf_pid        = $leafPid
        outsider_pid    = $outsiderPid
        job_name        = "Local\PM_Job_36_${sessionId}_synthetic"
    }
    $stateObj | ConvertTo-Json -Depth 4 | Set-Content -Path $StateJsonPath -Encoding UTF8
    Write-Host "[READY] Sesion activa preparada en E: | SessionId=$sessionId | OwnerPID=$ownerPid | GuardPID=$guardPid | WorkerPID=$workerPid | LeafPID=$leafPid | OutsiderPID=$outsiderPid"
    Write-Host "[READY] Es seguro retirar fisicamente el pendrive E: ahora."
}
elseif ($Action -eq "Verify") {
    if (-not (Test-Path $StateJsonPath)) {
        throw "No existe archivo de sesion preparada en $StateJsonPath"
    }
    if (Test-Path $ReportJsonPath) {
        throw "El informe ya existe; usa una ruta nueva para preservar la evidencia historica: $ReportJsonPath"
    }
    $state = [System.IO.File]::ReadAllText($StateJsonPath) | ConvertFrom-Json
    $usbPresent = Test-Path "E:\"

    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $ownerAlive = [bool](Get-Process -Id $state.owner_pid -ErrorAction SilentlyContinue)
        $guardAlive = [bool](Get-Process -Id $state.guard_pid -ErrorAction SilentlyContinue)
        $workerAlive = [bool](Get-Process -Id $state.worker_pid -ErrorAction SilentlyContinue)
        $leafAlive = if ($state.leaf_pid -gt 0) { [bool](Get-Process -Id $state.leaf_pid -ErrorAction SilentlyContinue) } else { $false }
        if (-not $ownerAlive -and -not $guardAlive -and -not $workerAlive -and -not $leafAlive) { break }
        Start-Sleep -Milliseconds 100
    }

    $ownerAlive = [bool](Get-Process -Id $state.owner_pid -ErrorAction SilentlyContinue)
    $guardAlive = [bool](Get-Process -Id $state.guard_pid -ErrorAction SilentlyContinue)
    $workerAlive = [bool](Get-Process -Id $state.worker_pid -ErrorAction SilentlyContinue)
    $leafAlive = if ($state.leaf_pid -gt 0) { [bool](Get-Process -Id $state.leaf_pid -ErrorAction SilentlyContinue) } else { $false }
    $outsiderAlive = [bool](Get-Process -Id $state.outsider_pid -ErrorAction SilentlyContinue)

    $manifestText = if (Test-Path $state.manifest_path) { [System.IO.File]::ReadAllText($state.manifest_path) } else { "" }
    $diagText = if (Test-Path $state.diagnostic_path) { [System.IO.File]::ReadAllText($state.diagnostic_path) } else { "" }

    if ($outsiderAlive) {
        Stop-Process -Id $state.outsider_pid -Force -ErrorAction SilentlyContinue
    }

    $originLossDetected = [bool]($diagText -match "step=wait_origin_loss status=passed actual=1 expected=1")
    $singleCleanupVerified = [bool](($diagText -match "step=cleanup_origin_loss_first status=passed") -and ($diagText -match "step=cleanup_origin_loss_repeat status=passed"))
    $manifestCleaned = [bool]($manifestText -match "State=CLEANED")
    $jobRolledBack = [bool]($manifestText -match "OpState=ROLLED_BACK")
    $passed = (-not $usbPresent) -and (-not $ownerAlive) -and (-not $guardAlive) -and (-not $workerAlive) -and (-not $leafAlive) -and $outsiderAlive -and $manifestCleaned -and $jobRolledBack -and $originLossDetected -and $singleCleanupVerified
    $resultObj = [ordered]@{
        timestamp_utc           = (Get-Date).ToUniversalTime().ToString("o")
        session_id              = [string]$state.session_id
        job_name                = [string]$state.job_name
        pids                    = [ordered]@{
            owner_pid    = [int]$state.owner_pid
            guard_pid    = [int]$state.guard_pid
            worker_pid   = [int]$state.worker_pid
            leaf_pid     = [int]$state.leaf_pid
            outsider_pid = [int]$state.outsider_pid
        }
        usb_still_mounted       = [bool]$usbPresent
        origin_loss_detected    = $originLossDetected
        owner_terminated        = (-not $ownerAlive)
        guard_terminated        = (-not $guardAlive)
        worker_terminated       = (-not $workerAlive)
        leaf_terminated         = (-not $leafAlive)
        job_drained             = ((-not $workerAlive) -and (-not $leafAlive))
        outsider_survived       = $outsiderAlive
        manifest_cleaned        = $manifestCleaned
        job_rolled_back         = $jobRolledBack
        single_cleanup_verified = $singleCleanupVerified
        controller_diagnostics  = $diagText
        manifest_content        = $manifestText
        status                  = if ($passed) { "passed" } else { "failed" }
    }
    $resultObj | ConvertTo-Json -Depth 4 | Set-Content -Path $ReportJsonPath -Encoding UTF8
    Write-Host "[VERIFY] status=$($resultObj.status) | Report=$ReportJsonPath"
}
elseif ($Action -eq "Contain") {
    if (Test-Path $StateJsonPath) {
        $state = [System.IO.File]::ReadAllText($StateJsonPath) | ConvertFrom-Json
        foreach ($p in @($state.owner_pid, $state.guard_pid, $state.worker_pid, $state.leaf_pid, $state.outsider_pid)) {
            if ($p -and $p -gt 0) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
        }
        Write-Host "[CONTAIN] Procesos auxiliares de la sesion $($state.session_id) contenidos."
    }
}

