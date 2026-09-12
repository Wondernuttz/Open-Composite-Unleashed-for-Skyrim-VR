$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$main = Get-Content -LiteralPath (Join-Path $project 'src/Main.cpp') -Raw
$mask = Get-Content -LiteralPath (Join-Path $project 'src/DapaPlayerMask.inl') -Raw
function Require([bool]$ok, [string]$message) { if (-not $ok) { throw $message } }
$begin = $mask.IndexOf('void NotifyUnavailableAfterLoad()')
$end = $mask.IndexOf('ID3D11DeviceContext* immediate', $begin)
Require ($begin -ge 0 -and $end -gt $begin) 'Missing notification implementation'
$notice = $mask.Substring($begin, $end - $begin)
Require ($notice.Contains('SKSE::GetTaskInterface()->AddTask')) 'Notification must run on game thread'
Require ($notice.Contains('warningShown || ready.load')) 'Do not notify repeatedly or after successful startup'
Require ($notice.Contains('warningShown=true;')) 'Notification must latch once per session'
Require ($notice.Contains('RE::DebugMessageBox')) 'Failure must be visible in game'
Require (-not $notice.Contains('g_diagnosticLogging')) 'Failures must remain visible with verbose logging off'
Require ($mask.Contains('actualModule==module')) 'Only identify CSX as owner when it actually owns the address'
Require ($mask.Contains('ModuleAt(engineHooks[i].chained)')) 'Unknown owner module must be logged'
$postLoad = $main.IndexOf('case SKSE::MessagingInterface::kPostLoadGame:')
$input = $main.IndexOf('case SKSE::MessagingInterface::kInputLoaded:', $postLoad)
Require ($postLoad -ge 0 -and $input -gt $postLoad) 'Missing game-load handler'
$handler = $main.Substring($postLoad, $input - $postLoad)
Require ($handler.Contains('IsDapaEnabledInGameIni()') -and $handler.Contains('PlayerMask::NotifyUnavailableAfterLoad();')) 'Only notify after loading a game with DAPA enabled'
Write-Output 'PASS: DAPA failure notification source contracts (load timing, game thread, once/session, logging-independent, owner attribution).'
