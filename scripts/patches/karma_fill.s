# SPDX-License-Identifier: GPL-2.0-or-later
# Source for RuntimeCheats karma_fill_update (arena 0x320), CUSA00309 01.07 only.
# Runs at the existing hero update call site; the native setter handles unlocks,
# threshold, consumption timer and HUD notifications. No action/XP is awarded.
.intel_syntax noprefix
.text
karma_fill_update:
    push rbx
    push r12
    push r13
    mov rbx, rdi
    call original_recovery
    movabs r12, offset control_data
    mov r13d, dword ptr [r12 + 0x44]
    test r13d, r13d
    je done
    movabs rax, offset hero_pointer
    cmp qword ptr [rax], rbx
    jne done
    # Reject stale/non-player update objects before touching their streak state.
    cmp qword ptr [rbx + 0x1ef48], rbx
    jne done
    movabs rax, offset progress
    cmp dword ptr [rax + 0xc4], 1
    ja done
    cmp r13d, dword ptr [r12 + 0x48]
    je sample
    lea rdi, [rbx + 0x1ef30]
    mov esi, 0x7fffffff
    xor edx, edx
    call streak_setter
sample:
    # After an explicit request, expose the live count to prove later consumption.
    mov eax, dword ptr [rbx + 0x1ef58]
    mov dword ptr [r12 + 0x4c], eax
    # Publish completion only after the native setter has returned.
    mov dword ptr [r12 + 0x48], r13d
done:
    pop r13
    pop r12
    pop rbx
    ret
