$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$source = Get-Content -LiteralPath (Join-Path $project 'src/DapaPlayerMask.inl') -Raw
function Require([bool]$value,[string]$message) { if(-not $value) {throw $message} }
$start = $source.IndexOf('if(apiResult==DapaCsxApi::ConnectResult::Connected)')
$legacy = $source.IndexOf('const auto imageBase=REL::Module::get().base();',$start)
Require ($start -ge 0 -and $legacy -gt $start) 'API selection must precede legacy preparation'
$selected = $source.Substring($start,$legacy-$start)
Require ($selected.Contains('ready.store(true') -and $selected.Contains('return;')) 'API success must return before legacy hooks'
Require (-not ($selected -match 'HookGeometry|\.Install\(|AllocTrampoline')) 'API mode cannot install private hooks'
$start = $source.IndexOf('void __cdecl ApiAccepted(')
$end = $source.IndexOf('template<size_t Site> uintptr_t Callback()', $start)
$callback = $source.Substring($start,$end-$start)
Require ($callback.Contains('IsPlayerGeometry') -and $callback.Contains('event->sceneDepth')) 'API mask needs live ownership and canonical scene depth'
Require ($callback.Contains('event->replay(event->replayToken)')) 'API must use provider isolated replay'
Require (-not ($callback -match 'DrawIndexed\(|DrawIndexedInstanced\(')) 'API consumer must not duplicate the original native draw'
Require ($callback.Contains('replayResult!=CSXAcceptedDrawAPI::Success')) 'Failed replay must not publish coverage'
Require ($source.Contains('csxApi.Stop()')) 'Rollback must stop API subscription'
Write-Output 'PASS: API integration source contracts (exclusive routing, owned geometry, scene depth, isolated replay, failed replay and stop).'
