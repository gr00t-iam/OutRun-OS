# OutRun Web integration (work in progress)

Kernel owner: add `outrun_web.elf` launcher with PCAP_NET plus existing desktop and file capabilities. Browser worker owns Makefile/GRUB.

CORRECTION to prior handoff: case 104 calls clock_read_ns(0), which DOES add g_realtime_off_ns when g_rtc_boot_epoch is set. The old comment above case 104 is stale. No new realtime syscall is needed. HTTPS will fail closed if RTC is unavailable (epoch < 2024).

Existing network ABI: 35(AF_INET=2, STREAM=1 or DGRAM=2 OR 0x800 nonblocking), 37(fd, host-order IPv4, host-order port), 38/39(fd, buffer, length), close=8. TCP connect returns -115; send before established returns -107, not EAGAIN. Browser will retry -107 only while connecting, with a monotonic deadline. DNS uses connected UDP to slirp resolver 10.0.2.3:53. Syscall 104 clockid=1 timespec is two 64-bit words.

Entropy: CPUID-gated RDRAND, fails closed when unavailable (never synthetic seeds). Desktop QEMU needs `-cpu max` (TCG remains mandatory), or a future genuine entropy syscall. No kernel edits by browser worker.
