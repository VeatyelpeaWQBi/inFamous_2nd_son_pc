# SPDX-License-Identifier: GPL-2.0-or-later
# Pure policy: importing this file never launches a process or writes a file.
function Get-V3ExperimentCatalog {
    return [ordered]@{
        'descriptor-delta'=@{v3_descriptor_delta=1}
        'layout-sharing'=@{v3_layout_sharing=1}
        'lighting-reduction'=@{v3_lighting_reduction=1}
        'lighting-cse'=@{v3_lighting_cse=1}
        'lighting-vector'=@{v3_lighting_vector=1}
        'lighting-schedule'=@{v3_lighting_schedule=1}
        'particle-schedule'=@{v3_particle_schedule=1}
        'particle-async'=@{v3_particle_async=1}
        'demand-dependencies'=@{v3_demand_readback=1}
        'page-batches'=@{v3_prefetch_batches=1}
        'window-prefetch'=@{v3_window_prefetch=1}
        'sort-write-bounds'=@{v3_sort_write_bounds=1}
        'submit-prepare'=@{v3_submit_prepare=1}
        'recording-2'=@{v3_recording_workers=2}
        'recording-4'=@{v3_recording_workers=4}
        'copy-coalesce'=@{v3_copy_coalesce=1}
        'completion-elision'=@{v3_completion_elision=1}
        # These are ablations of already active baseline functionality, not new gains.
        'params-lookup-off'=@{v3_disable_params_lookup=1}
        'flatbuf-reuse-off'=@{v3_disable_flatbuf_reuse=1}
    }
}

function Get-V3CandidateOverrides([string[]]$Candidates=@()) {
    if($null -eq $Candidates){$Candidates=@()}
    $catalog=Get-V3ExperimentCatalog
    $overrides=[ordered]@{}
    foreach($item in $catalog.Values){foreach($key in $item.Keys){$overrides[$key]=0}}
    if(@($Candidates|Select-Object -Unique).Count -ne $Candidates.Count){throw 'Duplicate V3 candidate selection.'}
    if($Candidates -contains 'recording-2' -and $Candidates -contains 'recording-4'){
        throw 'Select one recording worker count per process.'
    }
    foreach($name in $Candidates){
        if(-not $catalog.Contains($name)){throw ('Unknown V3 candidate: '+$name)}
        foreach($key in $catalog[$name].Keys){$overrides[$key]=$catalog[$name][$key]}
    }
    return $overrides
}

function Get-V3CombinedCandidates([ValidateSet(2,4)][int]$Workers=2) {
    return @((Get-V3ExperimentCatalog).Keys|Where-Object {$_ -notmatch '-off$|^recording-'})+@('recording-'+$Workers)
}
