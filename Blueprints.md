
## Phase 2: Core Kernel Multitasking & The VFS Ring-0 Integration

_To support running both a user terminal and a background network driver simultaneously, the kernel needs to stop running a single sequential loop and handle real thread slicing._

### Preemptive Round-Robin Thread Scheduler

> Let's upgrade our execution layer from a single process context to a preemptive round-robin task scheduler. Map out a Process Control Block (PCB) structure in Rust/C that holds saved register states (`rsp`, `rip`, `cr3`, general-purpose registers). Wire our 100Hz Programmable Interval Timer (PIT) interrupt directly into an assembly `irq_scheduler_yield` vector that saves the current execution context, indexes to the next runnable PID bitmask, swaps `CR3` mapping hierarchies, and executes an assembly `iretq` to restore user space context smoothly.

### Real Virtual File System (VFS) Infrastructure

> Let's bridge our disk storage with our task manager. Implement a read/write Virtual File System layer. Combine our `virtio-blk` raw sector read pathways with our Content-Addressable Storage indexing array. Expose standard, capability-gated file operations to ring-3 via system calls: `sys_open`, `sys_read`, `sys_write`, and an upgraded `sys_exec` that reads the ELF executable segments straight from a storage hash rather than an in-memory GRUB module block.

---

## Phase 3: Ring-0 Interrupt Routing & Net Stack Realization

_Currently, our user space network driver polls hardware registers, burning 100% CPU. We need to implement true interrupt signaling and data routing._

### Ring-0 to Ring-3 Asynchronous Interrupt Routing

> Right now, our userspace Virtio-Net driver relies on constant register polling. Let's fix this architecture. Implement an interrupt propagation engine. When the network card triggers an IDT hardware interrupt vector in Ring 0, the kernel must catch it, map it to the owning PID with the `CAP_NETWORK` privilege flag, and transition that process out of a blocked state into the active scheduling ring. Implement a blocking system call `sys_wait_event(uint64_t event_type)` that safely parks a thread until hardware notifications arrive.

### Freestanding IP/UDP Zero-Copy Packet Router

> We have asynchronous packet receipt running. Now, implement a minimal zero-copy network routing framework. Build an ring-3 library that parses raw network packets straight out of the shared Virtio descriptor rings. Write efficient byte-manipulation routines to parse Ethernet frames, extract IPv4 payloads, and handle UDP packet structures without ever invoking memory copies (`memcpy`). If an incoming packet matches our port filter array, route the pointer data block directly to the targeted execution slot.

---

## Phase 4: UI Graphics Server & The Chronological Canvas Compositor

_This phase builds the distinctive visual identity of Outrun OS, bringing hardware-accelerated presentation layer structures straight to bare metal._

### VBE/GOP Direct Framebuffer Graphics Architecture

> Let's build our visual display engine. Collect the linear frame buffer physical address, resolution boundary arrays, and color depth metrics passed down via our UEFI/BIOS bootloader structures. Implement a core graphics layout engine in our kernel space. Provide an absolute zero-copy user memory-mapping system call `sys_map_framebuffer` that allows a privileged user graphics manager to write pixels directly to the display, including a double-buffering page flip routine to prevent visual rendering lag.

### The Metropolis-Terminal Spatial Canvas Compositor

> You are a Principal UI/UX Architect and Graphics Shader Developer. Let's construct our native user desktop server. Using our direct mapped framebuffer layout, write a software renderer or WebGPU-compatible compositor that utilizes our Metropolis-Terminal visual styling. Implement asymmetric panel windows with sharp 45-degree angle layouts. Build the "Infinite Canvas" physics loop: windows must be treated as physical objects with mass, spring elasticity, and inertia (${\zeta = 0.82}$ damping ratio), pushing each other aside on collision rather than stacking. Write the pixel filters to enforce horizontal scanline rendering and real-time glass diffusion math with accessible contrast properties over dynamic background states.

---

## Phase 5: The Timeline Context Workspace & Production Hardening

_The final piece weaves our communications pipeline directly into our unique chronological file architecture, wrapping it all in independent validation tests._

### The Time-Stream Engine & Communication Deck

> Let's build the central workspace workflow. Create the core "Time-Stream Engine"—an asynchronous database substrate that monitors system activity logs, communications incoming via our Comm-Deck (emails, messages), and file write hooks. Instead of placing documents into nested directories, index them sequentially onto an interactive visual timeline. Wire up a background processing routine that parses these events into local vector indices for rapid on-device querying via our terminal interface.

### Production-Ready Validation Matrix & Bare Metal Deployment

> We have our core pillars running (Scheduler, VFS, CAS Storage, Virtio drivers, Compositor, and Time-Stream). We now need to prepare this image for pristine bare-metal deployment. Implement comprehensive validation routines: add strict bounds-checking to all ELF segment loaders, audit our capability checking logic across all system call entry points to prevent privilege escalation, and construct an automated test suite verifying file round-trip integrity over our CAS layer. Update our hybrid `Makefile` and El Torito build parameters to emit a verified production-ready installation ISO that safely installs our long-mode kernel structures directly onto raw physical NVMe drives via `dd`.
## The New Blueprint: The Hybrid Hypervisor OS

1. **The Host Management Layer (The Workstation):** Your core microkernel/host layer boots directly onto the hardware, manages your Content-Addressable Storage (CAS), handles the Chronological Time-Stream, and hosts the high-performance Metropolis terminal workspace.
    
2. **The Zero-Latency Passthrough (The Gaming Engine):** When you want to play a game, the orchestrator launches a highly tuned, minimal gaming runtime environment in a dedicated virtual machine. By implementing absolute **GPU Passthrough**, the gaming environment gets 100% direct, unmediated access to your physical graphics card and NVMe storage.
    

To adapt your roadmap for this exact architecture, here are the revised prompts to feed Claude to make this vision a reality:

---

## Revised Prompts for the Workstation + Gaming Rig Pivot

### IOMMU & PCI Passthrough Initialization (The Hardware Gate)

> We are restructuring Outrun OS to act as a high-performance developer orchestrator capable of bare-metal gaming. Write the kernel initialization code to enable AMD-Vi / Intel VT-d (IOMMU) processing. We need to parse the ACPI tables (`DMAR` or `IVRS`), configure the IOMMU page tables, and create a system call `sys_claim_pci_device(uint16_t domain, uint8_t bus, uint8_t slot, uint8_t func)` that detaches a high-performance GPU and its audio controller from the host kernel, isolating it into a protected DMA domain.

### Zero-Copy Shared Memory Framebuffer for the Compositor

> To keep our Metropolis-Terminal Canvas ultra-responsive while background environments run, implement a zero-copy shared memory interface (`shm`). Create a mechanism where a virtualized workspace can write its display output directly into a raw shared memory region. Our kernel space compositor must read from this shared region and blit the pixels directly to our UEFI GOP linear framebuffer using an optimized SIMD (AVX2) memory copy loop, ensuring zero visual lag when switching between developer tools and a high-performance environment.

### VFIO User-Space Driver Infrastructure

> Implement a freestanding user-space VFIO (Virtual Function I/O) driver module. This module will allow our user-space orchestrator to safely map the MMIO BARs and handle interrupts (MSI-X) of the isolated graphics card directly. Provide compilation-ready structures to map the guest virtual machine physical memory into the IOMMU translation tables so the hardware GPU can perform safe, direct DMA transfers without causing kernel panics on the host.

### The Gaming Runtime Launcher & Streamliner

> Write the user-space orchestration tool for our OS that provisions a streamlined gaming node. This tool must configure a lightweight execution boundary, allocate dedicated physical CPU cores (core pinning), prevent the host scheduler from interrupting those cores, and pass the isolated GPU directly to the node. Include a configuration interface that hooks into our Chronological Time-Stream, logging exactly when a game session starts and snapshots the state of our development workspace before shifting system resources entirely to the game.