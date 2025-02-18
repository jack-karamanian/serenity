/*
 * Copyright (c) 2020, Liav A. <liavalb@hotmail.co.il>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/StringView.h>
#include <Kernel/ACPI/ACPIParser.h>
#include <Kernel/Interrupts/InterruptManagement.h>
#include <Kernel/Time/HPET.h>
#include <Kernel/Time/HPETComparator.h>
#include <Kernel/Time/TimeManagement.h>
#include <Kernel/VM/MemoryManager.h>

namespace Kernel {

#define ABSOLUTE_MAXIMUM_COUNTER_TICK_PERIOD 0x05F5E100
#define NANOSECOND_PERIOD_TO_HERTZ(x) 1000000000 / x
#define MEGAHERTZ_TO_HERTZ(x) (x / 1000000)

//#define HPET_DEBUG

namespace HPETFlags {
enum class Attributes {
    Counter64BitCapable = 1 << 13,
    LegacyReplacementRouteCapable = 1 << 15
};

enum class Configuration {
    Enable = 0x1,
    LegacyReplacementRoute = 0x2
};

enum class TimerConfiguration : u32 {
    InterruptType = 1 << 1,
    InterruptEnable = 1 << 2,
    TimerType = 1 << 3,
    PeriodicInterruptCapable = 1 << 4,
    Timer64BitsCapable = 1 << 5,
    ValueSet = 1 << 6,
    Force32BitMode = 1 << 7,
    FSBInterruptEnable = 1 << 14,
    FSBInterruptDelivery = 1 << 15
};
};

struct [[gnu::packed]] TimerStructure
{
    u64 configuration_and_capability;
    u64 comparator_value;
    u64 fsb_interrupt_route;
    u64 reserved;
};

struct [[gnu::packed]] HPETCapabilityRegister
{
    u8 revision_id;
    u8 attributes;
    u16 vendor_id;
    u32 main_counter_tick_period;
    u64 reserved;
};

struct [[gnu::packed]] HPETRegister
{
    u64 reg;
    u64 reserved;
};

struct [[gnu::packed]] HPETRegistersBlock
{
    union {
        volatile HPETCapabilityRegister capabilities; // Note: We must do a 32 bit access to offsets 0x0, or 0x4 only, according to HPET spec.
        volatile HPETRegister raw_capabilites;
    };
    HPETRegister configuration;
    HPETRegister interrupt_status;
    u8 reserved[0xF0 - 48];
    HPETRegister main_counter_value;
    TimerStructure timers[3];
    u8 reserved2[0x400 - 0x160];
};

static HPET* s_hpet;
static bool hpet_initialized { false };

bool HPET::initialized()
{
    return hpet_initialized;
}

HPET& HPET::the()
{
    ASSERT(HPET::initialized());
    ASSERT(s_hpet != nullptr);
    return *s_hpet;
}

bool HPET::test_and_initialize()
{
    ASSERT(!HPET::initialized());
    hpet_initialized = true;
    auto hpet = ACPI::Parser::the().find_table("HPET");
    if (hpet.is_null())
        return false;
    klog() << "HPET @ " << hpet;

    auto region = MM.allocate_kernel_region(hpet.page_base(), (PAGE_SIZE * 2), "HPET Initialization", Region::Access::Read);
    auto* sdt = (volatile ACPI::Structures::HPET*)region->vaddr().offset(hpet.offset_in_page()).as_ptr();

    // Note: HPET is only usable from System Memory
    ASSERT(sdt->event_timer_block.address_space == (u8)ACPI::GenericAddressStructure::AddressSpace::SystemMemory);

    if (TimeManagement::is_hpet_periodic_mode_allowed()) {
        if (!check_for_exisiting_periodic_timers()) {
            dbg() << "HPET: No periodic capable timers";
            return false;
        }
    }
    s_hpet = new HPET(PhysicalAddress(hpet));
    return true;
}

bool HPET::check_for_exisiting_periodic_timers()
{
    auto hpet = ACPI::Parser::the().find_table("HPET");
    if (hpet.is_null())
        return false;
    auto region = MM.allocate_kernel_region(hpet.page_base(), (PAGE_SIZE * 2), "HPET Initialization", Region::Access::Read);
    auto* sdt = (volatile ACPI::Structures::HPET*)region->vaddr().offset(hpet.offset_in_page()).as_ptr();

    ASSERT(sdt->event_timer_block.address_space == 0);

    auto p_block = PhysicalAddress(sdt->event_timer_block.address);
    auto block_region = MM.allocate_kernel_region(p_block, (PAGE_SIZE * 2), "HPET Initialization", Region::Access::Read);
    auto* registers = (volatile HPETRegistersBlock*)block_region->vaddr().offset(p_block.offset_in_page()).as_ptr();

    auto* capabilities_register = (volatile HPETCapabilityRegister*)&registers->capabilities;
    size_t timers_count = (capabilities_register->attributes & 0x1f) + 1;
    for (size_t index = 0; index < timers_count; index++) {
        if (registers->timers[index].configuration_and_capability & (u32)HPETFlags::TimerConfiguration::PeriodicInterruptCapable)
            return true;
    }
    return false;
}

const FixedArray<RefPtr<HPETComparator>>& HPET::comparators() const
{
    return m_comparators;
}

void HPET::global_disable()
{
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    registers_block->configuration.reg &= ~(u32)HPETFlags::Configuration::Enable;
}
void HPET::global_enable()
{
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    registers_block->configuration.reg |= (u32)HPETFlags::Configuration::Enable;
}

void HPET::set_periodic_comparator_value(const HPETComparator& comparator, u64 value)
{
    disable(comparator);
    ASSERT(comparator.is_periodic());
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    registers_block->timers[comparator.comparator_number()].configuration_and_capability |= (u32)HPETFlags::TimerConfiguration::ValueSet;
    registers_block->timers[comparator.comparator_number()].comparator_value = value;
    enable(comparator);
}

void HPET::set_non_periodic_comparator_value(const HPETComparator& comparator, u64 value)
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(!comparator.is_periodic());
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    registers_block->timers[comparator.comparator_number()].comparator_value = main_counter_value() + value;
}
void HPET::enable_periodic_interrupt(const HPETComparator& comparator)
{
#ifdef HPET_DEBUG
    klog() << "HPET: Set comparator " << comparator.comparator_number() << " to be periodic.";
#endif
    disable(comparator);
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    ASSERT(registers_block->timers[comparator.comparator_number()].configuration_and_capability & (u32)HPETFlags::TimerConfiguration::PeriodicInterruptCapable);
    registers_block->timers[comparator.comparator_number()].configuration_and_capability |= (u32)HPETFlags::TimerConfiguration::TimerType;
    enable(comparator);
}
void HPET::disable_periodic_interrupt(const HPETComparator& comparator)
{
#ifdef HPET_DEBUG
    klog() << "HPET: Disable periodic interrupt in comparator " << comparator.comparator_number() << ".";
#endif
    disable(comparator);
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    ASSERT(registers_block->timers[comparator.comparator_number()].configuration_and_capability & (u32)HPETFlags::TimerConfiguration::PeriodicInterruptCapable);
    registers_block->timers[comparator.comparator_number()].configuration_and_capability &= ~(u32)HPETFlags::TimerConfiguration::TimerType;
    enable(comparator);
}

void HPET::disable(const HPETComparator& comparator)
{
#ifdef HPET_DEBUG
    klog() << "HPET: Disable comparator " << comparator.comparator_number() << ".";
#endif
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    registers_block->timers[comparator.comparator_number()].configuration_and_capability &= ~(u32)HPETFlags::TimerConfiguration::InterruptEnable;
}
void HPET::enable(const HPETComparator& comparator)
{
#ifdef HPET_DEBUG
    klog() << "HPET: Enable comparator " << comparator.comparator_number() << ".";
#endif
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    registers_block->timers[comparator.comparator_number()].configuration_and_capability |= (u32)HPETFlags::TimerConfiguration::InterruptEnable;
}

u64 HPET::main_counter_value() const
{
    auto* registers_block = (const volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    return registers_block->main_counter_value.reg;
}
u64 HPET::frequency() const
{
    return m_frequency;
}

Vector<unsigned> HPET::capable_interrupt_numbers(const HPETComparator& comparator)
{
    ASSERT(comparator.comparator_number() <= m_comparators.size());
    Vector<unsigned> capable_interrupts;
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    auto* comparator_registers = (const volatile TimerStructure*)&registers_block->timers[comparator.comparator_number()];
    u32 interrupt_bitfield = comparator_registers->configuration_and_capability >> 32;
    for (size_t index = 0; index < 32; index++) {
        if (interrupt_bitfield & 1)
            capable_interrupts.append(index);
        interrupt_bitfield >>= 1;
    }
    return capable_interrupts;
}

Vector<unsigned> HPET::capable_interrupt_numbers(u8 comparator_number)
{
    ASSERT(comparator_number <= m_comparators.size());
    Vector<unsigned> capable_interrupts;
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    auto* comparator_registers = (const volatile TimerStructure*)&registers_block->timers[comparator_number];
    u32 interrupt_bitfield = comparator_registers->configuration_and_capability >> 32;
    for (size_t index = 0; index < 32; index++) {
        if (interrupt_bitfield & 1)
            capable_interrupts.append(index);
        interrupt_bitfield >>= 1;
    }
    return capable_interrupts;
}

void HPET::set_comparator_irq_vector(u8 comparator_number, u8 irq_vector)
{
    ASSERT(comparator_number <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    auto* comparator_registers = (volatile TimerStructure*)&registers_block->timers[comparator_number];
    comparator_registers->configuration_and_capability |= (irq_vector << 9);
}

bool HPET::is_periodic_capable(u8 comparator_number) const
{
    ASSERT(comparator_number <= m_comparators.size());
    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    auto* comparator_registers = (const volatile TimerStructure*)&registers_block->timers[comparator_number];
    return comparator_registers->configuration_and_capability & (u32)HPETFlags::TimerConfiguration::PeriodicInterruptCapable;
}

void HPET::set_comparators_to_optimal_interrupt_state(size_t)
{
    // FIXME: Implement this method for allowing to use HPET timers 2-31...
    ASSERT_NOT_REACHED();
}

PhysicalAddress HPET::find_acpi_hept_registers_block()
{
    auto region = MM.allocate_kernel_region(m_physical_acpi_hpet_table.page_base(), (PAGE_SIZE * 2), "HPET Initialization", Region::Access::Read);
    auto* sdt = (ACPI::Structures::HPET*)region->vaddr().offset(m_physical_acpi_hpet_table.offset_in_page()).as_ptr();
    ASSERT(sdt->event_timer_block.address_space == (u8)ACPI::GenericAddressStructure::AddressSpace::SystemMemory);
    return PhysicalAddress(sdt->event_timer_block.address);
}
u64 HPET::calculate_ticks_in_nanoseconds() const
{
    auto* registers_block = (const volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();
    return ABSOLUTE_MAXIMUM_COUNTER_TICK_PERIOD / registers_block->capabilities.main_counter_tick_period;
}
HPET::HPET(PhysicalAddress acpi_hpet)
    : m_physical_acpi_hpet_table(acpi_hpet)
    , m_physical_acpi_hpet_registers(find_acpi_hept_registers_block())
    , m_hpet_mmio_region(MM.allocate_kernel_region(m_physical_acpi_hpet_registers.page_base(), PAGE_SIZE, "HPET MMIO", Region::Access::Read | Region::Access::Write))
{
    auto region = MM.allocate_kernel_region(m_physical_acpi_hpet_table.page_base(), (PAGE_SIZE * 2), "HPET Initialization", Region::Access::Read);
    auto* sdt = (volatile ACPI::Structures::HPET*)region->vaddr().offset(m_physical_acpi_hpet_table.offset_in_page()).as_ptr();
    m_vendor_id = sdt->pci_vendor_id;
    m_minimum_tick = sdt->mininum_clock_tick;
    klog() << "HPET: Minimum clock tick - " << m_minimum_tick;

    auto* registers_block = (volatile HPETRegistersBlock*)m_hpet_mmio_region->vaddr().offset(m_physical_acpi_hpet_registers.offset_in_page()).as_ptr();

    // Note: We must do a 32 bit access to offsets 0x0, or 0x4 only.
    size_t timers_count = ((registers_block->raw_capabilites.reg >> 8) & 0x1f) + 1;
    klog() << "HPET: Timers count - " << timers_count;
    ASSERT(timers_count >= 2);
    m_comparators.resize(timers_count);
    auto* capabilities_register = (const volatile HPETCapabilityRegister*)&registers_block->raw_capabilites.reg;

    global_disable();

    m_frequency = NANOSECOND_PERIOD_TO_HERTZ(calculate_ticks_in_nanoseconds());
    klog() << "HPET: frequency " << m_frequency << " Hz (" << MEGAHERTZ_TO_HERTZ(m_frequency) << " MHz)";
    ASSERT(capabilities_register->main_counter_tick_period <= ABSOLUTE_MAXIMUM_COUNTER_TICK_PERIOD);

    // Reset the counter, just in case...
    registers_block->main_counter_value.reg = 0;
    if (registers_block->raw_capabilites.reg & (u32)HPETFlags::Attributes::LegacyReplacementRouteCapable)
        registers_block->configuration.reg |= (u32)HPETFlags::Configuration::LegacyReplacementRoute;

    for (size_t index = 0; index < m_comparators.size(); index++) {
        bool periodic = is_periodic_capable(index);
        if (index == 0) {
            m_comparators[index] = HPETComparator::create(index, 0, periodic);
        }
        if (index == 1) {
            m_comparators[index] = HPETComparator::create(index, 8, periodic);
        }
    }

    global_enable();
}
}
