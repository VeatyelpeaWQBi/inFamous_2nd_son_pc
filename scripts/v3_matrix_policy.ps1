# SPDX-License-Identifier: GPL-2.0-or-later
# Pure state/identity helpers. Importing this file starts no process.
function ConvertTo-MatrixData($Value){
    if($null -eq $Value){return $null}
    if($Value -is [Collections.IDictionary]){
        $taskResult=[ordered]@{}
        foreach($taskKey in $Value.Keys){$taskResult[$taskKey]=ConvertTo-MatrixData $Value[$taskKey]}
        return $taskResult
    }
    if($Value -is [pscustomobject]){
        $taskResult=[ordered]@{}
        foreach($taskProperty in $Value.PSObject.Properties){$taskResult[$taskProperty.Name]=ConvertTo-MatrixData $taskProperty.Value}
        return $taskResult
    }
    if($Value -is [Collections.IList]){
        $taskResult=@();foreach($taskItem in $Value){$taskResult+=,(ConvertTo-MatrixData $taskItem)}
        return ,$taskResult
    }
    return $Value
}
function Get-MatrixFrozenFiles([string]$Root,[string[]]$Paths){
    return @($Paths|Sort-Object -Unique|ForEach-Object {
        $taskPath=Resolve-ProjectChild $Root $_
        if(-not(Test-Path -LiteralPath $taskPath -PathType Leaf)){throw ('Frozen input missing: '+$_)}
        @{path=$_;sha256=(Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
}
function Assert-MatrixFrozenFiles([string]$Root,$Files){
    foreach($taskFile in $Files){
        $taskPath=Resolve-ProjectChild $Root $taskFile.path
        if(-not(Test-Path -LiteralPath $taskPath -PathType Leaf) -or
           (Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskFile.sha256){
            throw ('Frozen experimental input changed: '+$taskFile.path)
        }
    }
}
function Reset-InterruptedMatrixGroups($State){
    foreach($taskCase in @($State.rows.case|Select-Object -Unique)){
        $taskGroup=@($State.rows|Where-Object {$_.case -eq $taskCase})
        $taskRepeat=($State.repeat_required_cases -contains $taskCase)
        if(-not $taskRepeat -and @($taskGroup|Where-Object {$_.status -ne 'validated'}).Count -eq 0){continue}
        if(@($taskGroup|Where-Object {$_.status -ne 'pending'}).Count){
            $State.history+=@{case=$taskCase;reason=if($taskRepeat){'condition-review-repeat'}else{'interrupted-group-repeat'};rows=(ConvertTo-MatrixData $taskGroup)}
            foreach($taskRow in $taskGroup){
                $taskRow.status='pending';$taskRow.session=$null;$taskRow.launcher_exit_code=$null;$taskRow.attempt++
            }
            if($State.Contains('reference_images') -and $State.reference_images.Contains($taskCase)){$State.reference_images.Remove($taskCase)}
        }
    }
    $State.repeat_required_cases=@();$State.failure=$null;$State.complete=$false;$State.analysis_status='pending'
}
