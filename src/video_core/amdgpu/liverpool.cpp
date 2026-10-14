// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/perf_stats.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace AmdGpu {

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])
// A yield of a compute queue task: the graphics queue runs meanwhile (asc_queue routing off).
#define YIELD_ASC_QUEUE(id)                                                                        \
    do {                                                                                           \
        if (rasterizer) {                                                                          \
            rasterizer->SetAsyncCompute(false);                                                    \
        }                                                                                          \
        YIELD_ASC(id);                                                                             \
        if (rasterizer) {                                                                          \
            rasterizer->SetAsyncCompute(true);                                                     \
        }                                                                                          \
    } while (0)

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
}

void Liverpool::ProcessCommands() {
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
        ++processed_packets;
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    gpu_id = std::this_thread::get_id();
#ifdef __linux__
    gpu_tid = gettid();
#endif

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
        }
        if (stoken.stop_requested()) {
            break;
        }
        // Awake time is split between processing packets and spinning on unsatisfied waits
        // (WAIT_REG_MEM, semaphores...). It is accumulated locally and published at least every
        // millisecond, as the thread can stay awake for many frames.
        u64 time_mark = Common::Perf::NowNs();
        u64 packets_mark = processed_packets;
        u64 work_ns = 0;
        u64 spin_ns = 0;
        const auto account_time = [&](bool publish) {
            const u64 now = Common::Perf::NowNs();
            // Sleeping on a flip label inside a packet is waiting, not work.
            const u64 blocked = std::min(blocked_ns, now - time_mark);
            blocked_ns = 0;
            spin_ns += blocked;
            (processed_packets != packets_mark ? work_ns : spin_ns) += now - time_mark - blocked;
            time_mark = now;
            packets_mark = processed_packets;
            if (publish || work_ns + spin_ns >= 1'000'000) {
                Common::Perf::Add(Common::Perf::GpuBusyNs, work_ns);
                Common::Perf::Add(Common::Perf::GpuSpinNs, spin_ns);
                work_ns = 0;
                spin_ns = 0;
            }
        };

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_submits || num_commands) {
            ProcessCommands();
            if (rasterizer) {
                // Readbacks the GPU has finished are written as soon as possible, so their pages
                // are readable before the guest touches them.
                rasterizer->RetireReadbacks();
            }

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            if (curr_qid != GfxQueueId) {
                const u64 resume_start = Common::Perf::NowNs();
                task.resume();
                Common::Perf::Add(Common::Perf::AscBusyNs, Common::Perf::NowNs() - resume_start);
            } else {
                task.resume();
            }
            account_time(false);

            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();

                if (curr_qid != GfxQueueId) {
                    asc_submits.fetch_sub(1, std::memory_order_relaxed);
                }
                --num_submits;
                submits_completed.fetch_add(1, std::memory_order_release);
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
            ++processed_packets;
        }
        account_time(true);
        Common::Perf::GpuThreadCpu().store(Common::Perf::CurrentCpu(), std::memory_order_relaxed);

        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    }
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();
        ++processed_packets;

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            if (rasterizer) {
                rasterizer->OnGpuThreadWrite(dump_const->Address<VAddr>(), dump_const->Size());
            }
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           u64 scout_seq) {
    FIBER_ENTER(dcb_task_name);

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    if (rasterizer && rasterizer->IsDumping()) [[unlikely]] {
        rasterizer->DumpNote("gfx", fmt::format("commands {:#x} {} dwords", base_addr, dcb.size()));
    }
    if (scout_seq != 0 && scout) {
        // The scout read this submission ahead (or is reading it): its entries, draw by draw.
        scout_submission = scout->Begin(scout_seq, regs);
    }
    // bind_prefetch: the commands a few draws ahead are prefetched (the hardware prefetchers do
    // not cross into the next 4 KB page, where each page of commands missed the caches).
    const bool prefetch_commands =
        Common::Perf::GetTuning().bind_prefetch.load(std::memory_order_relaxed);
    constexpr size_t CommandPrefetchDwords = 128;
    while (!dcb.empty()) {
        ProcessCommands();
        ++processed_packets;
        if (prefetch_commands && dcb.size() > CommandPrefetchDwords) {
            VideoCore::PrefetchLine(dcb.data() + CommandPrefetchDwords);
        }

        // The PS4 runs asynchronous compute queues alongside the graphics queue: the guest may
        // read their results (particle counts...) well before this queue would wait for them.
        // Give way to queued compute work regularly instead of only when blocked.
        if (const u32 yield_packets =
                Common::Perf::GetTuning().asc_yield_packets.load(std::memory_order_relaxed);
            yield_packets != 0 && asc_submits.load(std::memory_order_relaxed) != 0 &&
            ++gfx_packets_since_yield >= yield_packets) {
            gfx_packets_since_yield = 0;
            Common::Perf::Add(Common::Perf::GfxYields);
            YIELD_GFX();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0:
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    const auto marker_sz = nop->header.count.Value() * 2;
                    const std::string_view label{
                        reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    if (rasterizer && rasterizer->IsDumping()) [[unlikely]] {
                        rasterizer->DumpNote("marker+", label.substr(0, label.find('\0')));
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    const auto marker_sz = nop->header.count.Value() * 2;
                    const std::string_view label{
                        reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                    if (guest_markers_enabled) {
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    if (rasterizer && rasterizer->IsDumping()) [[unlikely]] {
                        rasterizer->DumpNote("marker*", label.substr(0, label.find('\0')));
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    if (rasterizer && rasterizer->IsDumping()) [[unlikely]] {
                        rasterizer->DumpNote("marker-", "");
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                ++context_reg_changes;
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                if (std::memcmp(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32)) !=
                    0) {
                    ++context_reg_changes;
                }
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                if (std::memcmp(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32)) !=
                    0) {
                    ++context_reg_changes;
                }
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                if (std::memcmp(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                                header + 2, (count - 1) * sizeof(u32)) != 0) {
                    ++context_reg_changes;
                }
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                break;
            }
            case PM4ItOpcode::SetPredication: {
                LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION");
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(true); });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->Draw(true, draw_index_off->index_offset); });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(false); });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_index_indirect->base_vtx_loc,
                                                 draw_index_indirect->start_inst_loc);
                    });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                BeginScoutDraw(header);
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                EndScoutDraw();
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->DispatchDirect(); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->DispatchIndirect(indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        // Real occlusion queries: the GPU counts the samples of the draws between
                        // the begin and end dumps (light flares behind walls are not visible).
                        const VAddr address = event->Address<VAddr>();
                        const auto result =
                            rasterizer ? rasterizer->OcclusionDump(address, num_counter_pairs)
                                       : Vulkan::OcclusionDumpResult::Emulated;
                        if (result == Vulkan::OcclusionDumpResult::EmulatedWithBegin) {
                            WriteEmulatedOcclusionCounters(address - sizeof(u64));
                        }
                        if (result != Vulkan::OcclusionDumpResult::Counted) {
                            WriteEmulatedOcclusionCounters(address);
                        }
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                event_eos->SignalFence([this](void* address, u64 data, u32 num_bytes) {
                    auto* memory = Core::Memory::Instance();
                    ASSERT(memory->TryWriteBacking(address, &data, num_bytes));
                    NoteCpWrite(reinterpret_cast<VAddr>(address), &data, num_bytes);
                });
                if (event_eos->command == PM4CmdEventWriteEos::Command::GdsStore) {
                    ASSERT(event_eos->size == 1);
                    if (rasterizer) {
                        rasterizer->Finish();
                        const u32 value = rasterizer->ReadDataFromGds(event_eos->gds_index);
                        *event_eos->Address() = value;
                        NoteCpWrite(reinterpret_cast<VAddr>(event_eos->Address()), &value,
                                    sizeof(value));
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                event_eop->SignalFence(
                    [this](void* address, u64 data, u32 num_bytes) {
                        auto* memory = Core::Memory::Instance();
                        ASSERT(memory->TryWriteBacking(address, &data, num_bytes));
                        NoteCpWrite(reinterpret_cast<VAddr>(address), &data, num_bytes);
                    },
                    [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                ASSERT(dma_data->command.das == 0);
                if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                                           dma_data->data, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                           dma_data->NumBytes(), true, false);
                } else if (dma_data->src_sel == DmaDataSrc::Data &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                           dma_data->data, false);
                    RecordCpLabel(dma_data->DstAddress<VAddr>(), &dma_data->data,
                                  std::min<u32>(dma_data->NumBytes(), sizeof(u32)));
                } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                           dma_data->NumBytes(), false, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                           dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                                           false, false);
                } else {
                    UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                    u32(dma_data->dst_sel));
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                if (!write_data->wr_one_addr.Value()) {
                    if (rasterizer) {
                        rasterizer->OnFence();
                    }
                    std::memcpy(address, write_data->data, data_size);
                    if (rasterizer) {
                        rasterizer->OnGpuThreadWrite(reinterpret_cast<VAddr>(address), data_size);
                    }
                    RecordCpLabel(reinterpret_cast<VAddr>(address), write_data->data, data_size);
                } else {
                    UNREACHABLE();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                    VideoCore::NoteHostWrite();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                    }
                    mem_semaphore->Decrement();
                    VideoCore::NoteHostWrite();
                    // The guest may have written memory before signaling.
                    Common::Perf::Add(Common::Perf::EpochSemaphoreWaits);
                    VideoCore::NextUploadEpoch();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                }
                // The guest CPU validated the commands that follow.
                Common::Perf::Add(Common::Perf::EpochRewinds);
                VideoCore::NextUploadEpoch();
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                if (SkipOcclusionWait(*wait_reg_mem)) {
                    break;
                }
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    const u64 wait_start = Common::Perf::NowNs();
                    // Commands from guest threads (readbacks waiting for this thread) are still
                    // served while the label is not written. Compute work submitted meanwhile
                    // runs at once (the PS4 runs it alongside): wait by yielding to it then.
                    const bool yield_to_compute =
                        Common::Perf::GetTuning().asc_yield_packets.load(
                            std::memory_order_relaxed) != 0;
                    bool satisfied = false;
                    while (!(satisfied = vo_port->WaitVoLabelFor(
                                 [&] { return wait_reg_mem->Test(regs.reg_array); },
                                 std::chrono::milliseconds{1}))) {
                        ProcessCommands();
                        if (yield_to_compute && asc_submits.load(std::memory_order_relaxed) != 0) {
                            break;
                        }
                    }
                    blocked_ns += Common::Perf::NowNs() - wait_start;
                    if (satisfied) {
                        // Written by the presentation, not by the guest CPU: no new upload epoch.
                        Common::Perf::Add(Common::Perf::EpochVoWaitsSkipped);
                        break;
                    }
                }
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    YIELD_GFX();
                }
                OnWaitRegMemSatisfied(*wait_reg_mem);
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (scout_submission) {
                    scout_submission->CheckCondition(header, skip);
                }
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }
    if (scout_seq != 0 && scout) {
        scout_submission.reset();
        scout->Finish(scout_seq);
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid, u64 submit_ns) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];
    if (submit_ns != 0) {
        // First resumed: how long the work waited for the command processor.
        Common::Perf::Add(Common::Perf::AscStarts);
        Common::Perf::Add(Common::Perf::AscStartDelayNs, Common::Perf::NowNs() - submit_ns);
    }
    if (rasterizer && rasterizer->IsDumping()) [[unlikely]] {
        rasterizer->DumpNote("asc", fmt::format("queue {} {} dwords{}", vqid, acb.size(),
                                                is_indirect ? " indirect" : ""));
    }

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();
        ++processed_packets;
        // asc_queue: the dispatches, copies and fills of this queue run on the compute queue.
        if (rasterizer) {
            rasterizer->SetAsyncCompute(true);
        }

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC_QUEUE(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
                RecordCpLabel(dma_data->DstAddress<VAddr>(), &dma_data->data,
                              std::min<u32>(dma_data->NumBytes(), sizeof(u32)));
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC_QUEUE(vqid);
            }
            // The guest CPU validated the commands that follow.
            Common::Perf::Add(Common::Perf::EpochRewinds);
            VideoCore::NextUploadEpoch();
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchDirect(); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchIndirect(ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
                if (rasterizer) {
                    rasterizer->OnGpuThreadWrite(write_data->Address<VAddr>(), data_size);
                }
                RecordCpLabel(write_data->Address<VAddr>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
                VideoCore::NoteHostWrite();
            } else {
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC_QUEUE(vqid);
                }
                mem_semaphore->Decrement();
                VideoCore::NoteHostWrite();
                Common::Perf::Add(Common::Perf::EpochSemaphoreWaits);
                VideoCore::NextUploadEpoch();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            if (SkipOcclusionWait(*wait_reg_mem)) {
                break;
            }
            while (!wait_reg_mem->Test(regs.reg_array)) {
                YIELD_ASC_QUEUE(vqid);
            }
            OnWaitRegMemSatisfied(*wait_reg_mem);
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (rasterizer) {
                // asc_queue: the work before the fence is submitted now, so the guest (and the
                // readbacks at this fence) wait for it only.
                rasterizer->FlushComputeQueue();
                rasterizer->OnFence();
            }
            release_mem->SignalFence(
                [pipe_id = queue.pipe_id] {
                    Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
                },
                [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                    rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
                });
            switch (release_mem->data_sel.Value()) {
            case DataSelect::Data32Low:
                NoteCpWrite(release_mem->Address<VAddr>(), release_mem->Address<const void*>(),
                            sizeof(u32));
                break;
            case DataSelect::Data64:
            case DataSelect::GpuClock64:
            case DataSelect::PerfCounter:
                NoteCpWrite(release_mem->Address<VAddr>(), release_mem->Address<const void*>(),
                            sizeof(u64));
                break;
            default:
                break;
            }
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    if (rasterizer) {
        rasterizer->SetAsyncCompute(false);
        rasterizer->FlushComputeQueue();
    }
    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::RecordCpLabel(VAddr address, const void* data, u32 num_bytes) {
    // Labels are one or two dwords. Longer writes are data: only their first dwords are kept.
    const u32 num_dwords = std::min<u32>(num_bytes / sizeof(u32), 8);
    for (u32 i = 0; i < num_dwords; ++i) {
        u32 value;
        std::memcpy(&value, static_cast<const u8*>(data) + i * sizeof(u32), sizeof(value));
        const VAddr dword_address = address + i * sizeof(u32);
        cp_labels[(dword_address / sizeof(u32)) % NumCpLabels] = {dword_address, value};
    }
}

void Liverpool::WriteEmulatedOcclusionCounters(VAddr address) {
    static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
    static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
    u64* results = reinterpret_cast<u64*>(address);
    for (u32 i = 0; i < num_counter_pairs; ++i, results += 2) {
        *results = pixel_counter | OcclusionCounterValidMask;
    }
    pixel_counter += OcclusionCounterStep;
    if (rasterizer) {
        rasterizer->OnGpuThreadWrite(address, num_counter_pairs * 2 * sizeof(u64));
    }
}

void Liverpool::NoteCpWrite(VAddr address, const void* data, u32 num_bytes) {
    RecordCpLabel(address, data, num_bytes);
    VideoCore::NoteHostWrite();
}

bool Liverpool::IsWrittenByCp(VAddr address) const {
    const auto& label = cp_labels[(address / sizeof(u32)) % NumCpLabels];
    return label.address == address && label.value == *reinterpret_cast<const u32*>(address);
}

bool Liverpool::SkipOcclusionWait(const PM4CmdWaitRegMem& wait) {
    if (wait.mem_space.Value() != PM4CmdWaitRegMem::MemSpace::Memory) {
        return false;
    }
    const VAddr address = wait.Address<VAddr>();
    if (wait.IsAlwaysSatisfied()) {
        // Nothing to read: reading memory the GPU wrote (the fill of occlusion counters this
        // waits for) would wait for the GPU. The commands that follow run after it anyway.
        Common::Perf::Add(Common::Perf::WaitsTrivial);
        if (rasterizer && rasterizer->IsDumping()) [[unlikely]] {
            rasterizer->DumpNote("wait", fmt::format("{:x} func {} mask {:x} ref {:x} always "
                                                     "satisfied, skipped",
                                                     address,
                                                     static_cast<u32>(wait.function.Value()),
                                                     wait.mask, wait.ref));
        }
        return true;
    }
    if (!rasterizer) {
        return false;
    }
    std::optional<u32> known;
    const bool counted = rasterizer->IsCountedOcclusionResult(address, known);
    const bool skip = known && wait.TestValue(*known);
    if (rasterizer->IsDumping()) [[unlikely]] {
        rasterizer->DumpNote("wait", fmt::format("{:x} func {} mask {:x} ref {:x} {}", address,
                                                 static_cast<u32>(wait.function.Value()), wait.mask,
                                                 wait.ref,
                                                 !counted ? "polled"
                                                 : skip   ? "occlusion result known, skipped"
                                                 : known  ? "occlusion result known, polled"
                                                          : "occlusion samples, polled"));
    }
    if (counted) {
        Common::Perf::Add(skip ? Common::Perf::OcclusionWaitsSkipped
                               : Common::Perf::OcclusionWaitsRead);
    }
    return skip;
}

void Liverpool::OnWaitRegMemSatisfied(const PM4CmdWaitRegMem& wait) {
    if (wait.mem_space.Value() != PM4CmdWaitRegMem::MemSpace::Memory) {
        // Registers are only written by commands.
        return;
    }
    const VAddr address = wait.Address<VAddr>();
    if (vo_port && vo_port->IsVoLabel(reinterpret_cast<const u64*>(address))) {
        // Written by the presentation: the commands that follow were submitted before.
        Common::Perf::Add(Common::Perf::EpochVoWaitsSkipped);
        return;
    }
    if (IsWrittenByCp(address)) {
        // Released by the GPU itself (end of pipe fence of another queue or of earlier work):
        // the guest wrote the data of the commands that follow before submitting them.
        Common::Perf::Add(Common::Perf::EpochCpWaitsSkipped);
        return;
    }
    // The guest CPU writes the data before the value waited for.
    Common::Perf::Add(Common::Perf::EpochCpuWaits);
    VideoCore::NextUploadEpoch();
}

void Liverpool::SubmitDone() noexcept {
    Common::Perf::GuestFrameCounter().fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lk{submit_mutex};
    mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
    mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
    submit_done = true;

    // Guest frames whose submissions the command processor has not finished processing yet.
    frame_marks[frame_index % NumFrameMarks] = submits_issued.load(std::memory_order_relaxed);
    ++frame_index;
    const u64 completed = submits_completed.load(std::memory_order_acquire);
    u64 in_flight = 0;
    for (u64 i = 0; i < std::min<u64>(frame_index, NumFrameMarks); ++i) {
        if (frame_marks[(frame_index - 1 - i) % NumFrameMarks] > completed) {
            ++in_flight;
        }
    }
    Common::Perf::Add(Common::Perf::GpuLagFrames, in_flight);
    Common::Perf::Add(Common::Perf::GpuLagSamples);
    submit_cv.notify_one();

    // Optionally, the guest does not start a new frame before the command processor has
    // processed all the frames but the last max_lag ones (bounded wait).
    const u32 max_lag =
        Common::Perf::GetTuning().max_gpu_lag_frames.load(std::memory_order_relaxed);
    if (max_lag != 0 && frame_index > max_lag && max_lag < NumFrameMarks) {
        const u64 target = frame_marks[(frame_index - 1 - max_lag) % NumFrameMarks];
        if (submits_completed.load(std::memory_order_acquire) < target) {
            const u64 wait_start = Common::Perf::NowNs();
            submit_cv.wait_for(lk, std::chrono::milliseconds{50}, [&] {
                return submits_completed.load(std::memory_order_acquire) >= target;
            });
            Common::Perf::Add(Common::Perf::GpuLagWaitNs, Common::Perf::NowNs() - wait_start);
        }
    }

    // Optionally, the guest waits until the command processor has processed the work submitted
    // so far: compute queues (1), or all queues (2). On the PS4 the GPU keeps up with the frame,
    // so the results of this work exist when the guest reads them back during its next frame;
    // here the command processor may still be a frame behind and the guest would read the
    // results of an older frame (bounded wait).
    const u32 sync = Common::Perf::GetTuning().gpu_sync_frame_end.load(std::memory_order_relaxed);
    if (sync != 0) {
        const u64 target = submits_issued.load(std::memory_order_relaxed);
        const auto done = [&] {
            return sync >= 2 ? submits_completed.load(std::memory_order_acquire) >= target
                             : asc_submits.load(std::memory_order_acquire) == 0;
        };
        if (!done()) {
            const u64 wait_start = Common::Perf::NowNs();
            submit_cv.wait_for(lk, std::chrono::milliseconds{50}, done);
            Common::Perf::Add(Common::Perf::GpuLagWaitNs, Common::Perf::NowNs() - wait_start);
        }
    }
}

void Liverpool::StartScout(ScoutClient& client) {
    scout = std::make_unique<Scout>(client);
}

void Liverpool::BeginScoutDraw(const void* packet) {
    if (scout_submission) {
        current_scout_draw = scout_submission->Next(packet);
    }
}

void Liverpool::EndScoutDraw() {
    current_scout_draw = nullptr;
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    // The guest wrote the memory this submission reads before submitting it: hot pages are
    // uploaded again from now on (submission order is the guest thread's program order).
    Common::Perf::Add(Common::Perf::EpochGfxSubmits);
    VideoCore::NextUploadEpoch();

    u64 scout_seq = 0;
    {
        // Submission order is the order of the sequence numbers the scout and the command
        // processor use to meet.
        std::scoped_lock lock{queue.m_access};
        if (scout && ccb.empty()) {
            scout_seq = ++gfx_submit_seq;
            if (Common::Perf::GetTuning().scout.load(std::memory_order_relaxed)) {
                scout->Submit(scout_seq, dcb);
            } else {
                scout->Skip(scout_seq);
            }
        }
        auto task = ProcessGraphics(dcb, ccb, scout_seq);
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    submits_issued.fetch_add(1, std::memory_order_relaxed);
    ++num_submits;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    Common::Perf::Add(Common::Perf::EpochAscSubmits);
    VideoCore::NextUploadEpoch();
    const auto& task = ProcessCompute(acb, vqid, Common::Perf::NowNs());
    // Counted before the command processor can see (and complete) the submission.
    asc_submits.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    submits_issued.fetch_add(1, std::memory_order_relaxed);
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    submit_cv.notify_one();
}

} // namespace AmdGpu
