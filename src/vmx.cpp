/*
 * Virtual Machine Extensions (VMX)
 *
 * Copyright (C) 2006-2009, Udo Steinberg <udo@hypervisor.org>
 *
 * This file is part of the NOVA microhypervisor.
 *
 * NOVA is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOVA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 */

#include "cmdline.h"
#include "counter.h"
#include "cpu.h"
#include "extern.h"
#include "gdt.h"
#include "hip.h"
#include "idt.h"
#include "msr.h"
#include "stdio.h"
#include "tss.h"
#include "vmx.h"

Vmcs *              Vmcs::current;
unsigned            Vmcs::vpid_ctr;
Vmcs::vmx_basic     Vmcs::basic;
Vmcs::vmx_ept_vpid  Vmcs::ept_vpid;
Vmcs::vmx_ctrl_pin  Vmcs::ctrl_pin;
Vmcs::vmx_ctrl_cpu  Vmcs::ctrl_cpu[2];
Vmcs::vmx_ctrl_exi  Vmcs::ctrl_exi;
Vmcs::vmx_ctrl_ent  Vmcs::ctrl_ent;
Vmcs::vmx_fix_cr0   Vmcs::fix_cr0;
Vmcs::vmx_fix_cr4   Vmcs::fix_cr4;

Vmcs::Vmcs (mword esp, mword cr3, mword eptp) : rev (basic.revision)
{
    clear();

    make_current();

    uint32 pin = PIN_EXTINT | PIN_NMI;
    pin |= ctrl_pin.set;
    pin &= ctrl_pin.clr;
    write (PIN_EXEC_CTRL, pin);

    uint32 exi = EXI_INTA;
    exi |= ctrl_exi.set;
    exi &= ctrl_exi.clr;
    write (EXI_CONTROLS, exi);

    uint32 ent = 0;
    ent |= ctrl_ent.set;
    ent &= ctrl_ent.clr;
    write (ENT_CONTROLS, ent);

    write (PF_ERROR_MASK, 0);
    write (PF_ERROR_MATCH, 0);
    write (CR3_TARGET_COUNT, 0);

    write (VMCS_LINK_PTR,    ~0ul);
    write (VMCS_LINK_PTR_HI, ~0ul);

    write (VPID, ++vpid_ctr);
    write (EPTP, eptp | 0x1e);
    write (EPTP_HI, 0);

    write (HOST_SEL_CS, SEL_KERN_CODE);
    write (HOST_SEL_SS, SEL_KERN_DATA);
    write (HOST_SEL_DS, SEL_KERN_DATA);
    write (HOST_SEL_ES, SEL_KERN_DATA);
    write (HOST_SEL_TR, SEL_TSS_RUN);

    write (HOST_CR3, cr3);
    write (HOST_CR0, Cpu::get_cr0());
    write (HOST_CR4, Cpu::get_cr4());

    assert (Cpu::get_cr0() & Cpu::CR0_TS);

    write (HOST_BASE_TR,   reinterpret_cast<mword>(&Tss::run));
    write (HOST_BASE_GDTR, reinterpret_cast<mword>(Gdt::gdt));
    write (HOST_BASE_IDTR, reinterpret_cast<mword>(Idt::idt));

    write (HOST_SYSENTER_CS,  SEL_KERN_CODE);
    write (HOST_SYSENTER_ESP, reinterpret_cast<mword>(&Tss::run.sp0));
    write (HOST_SYSENTER_EIP, reinterpret_cast<mword>(&entry_sysenter));

    write (HOST_RSP, esp);
    write (HOST_RIP, reinterpret_cast<mword>(&entry_vmx));
}

void Vmcs::init()
{
    if (!Cpu::feature (Cpu::FEAT_VMX)) {
        Hip::disfeature (Hip::FEAT_VMX);
        return;
    }

    unsigned bits = Cpu::secure ? 0x3 : 0x5;
    if ((Msr::read<uint32>(Msr::IA32_FEATURE_CONTROL) & bits) != bits)
        return;

    fix_cr0.set =  Msr::read<mword>(Msr::IA32_VMX_CR0_FIXED0);
    fix_cr0.clr = ~Msr::read<mword>(Msr::IA32_VMX_CR0_FIXED1);
    fix_cr4.set =  Msr::read<mword>(Msr::IA32_VMX_CR4_FIXED0);
    fix_cr4.clr = ~Msr::read<mword>(Msr::IA32_VMX_CR4_FIXED1);

    basic.val       = Msr::read<uint64>(Msr::IA32_VMX_BASIC);
    ctrl_exi.val    = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_EXIT  : Msr::IA32_VMX_CTRL_EXIT);
    ctrl_ent.val    = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_ENTRY : Msr::IA32_VMX_CTRL_ENTRY);
    ctrl_pin.val    = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_PIN   : Msr::IA32_VMX_CTRL_PIN);
    ctrl_cpu[0].val = Msr::read<uint64>(basic.ctrl ? Msr::IA32_VMX_TRUE_CPU0  : Msr::IA32_VMX_CTRL_CPU0);

    if (has_secondary())
        ctrl_cpu[1].val = Msr::read<uint64>(Msr::IA32_VMX_CTRL_CPU1);
    if (has_ept() || has_vpid())
        ept_vpid.val = Msr::read<uint64>(Msr::IA32_VMX_EPT_VPID);

    ctrl_cpu[0].set |= CPU_HLT | CPU_IO | CPU_SECONDARY;
    ctrl_cpu[1].set |= CPU_VPID;

    if (Cmdline::noept || !ept_vpid.invept)
        ctrl_cpu[1].clr &= ~CPU_EPT;
    if (Cmdline::novpid || !ept_vpid.invvpid)
        ctrl_cpu[1].clr &= ~CPU_VPID;

    Cpu::set_cr0 ((Cpu::get_cr0() & ~fix_cr0.clr) | fix_cr0.set);
    Cpu::set_cr4 ((Cpu::get_cr4() & ~fix_cr4.clr) | fix_cr4.set);

    Vmcs *root = new Vmcs;

    trace (0, "VMCS:%#010lx REV:%#x CPU:%#x/%#x VPID:%u EPT:%u",
           Buddy::ptr_to_phys (root),
           basic.revision,
           ctrl_cpu[0].clr, ctrl_cpu[1].clr,
           has_vpid(), has_ept());
}