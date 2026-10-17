/*
 * AUDIONT - kernel definitions for the Windows 2000 / XP (x86 and x64)
 * Plug and Play build: the MinGW-w64 DDK headers plus the few helpers the
 * NT 3.x / 4.0 build defines in ntk.h itself.
 */
#ifndef AUDIONT_NTKWDM_H
#define AUDIONT_NTKWDM_H

#include <ntddk.h>

#define IRP_LE_FIELD Tail.Overlay.ListEntry
#define IRP_DC_FIELD Tail.Overlay.DriverContext

#ifndef FASTCALL
#define FASTCALL __attribute__((fastcall))
#endif

/* port I/O */
static __inline__ UCHAR  inb(USHORT p) { UCHAR v;  __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static __inline__ USHORT inw(USHORT p) { USHORT v; __asm__ __volatile__("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static __inline__ ULONG  inl(USHORT p) { ULONG v;  __asm__ __volatile__("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static __inline__ void outb(USHORT p, UCHAR v)  { __asm__ __volatile__("outb %0,%1" :: "a"(v), "Nd"(p)); }
static __inline__ void outw(USHORT p, USHORT v) { __asm__ __volatile__("outw %0,%1" :: "a"(v), "Nd"(p)); }
static __inline__ void outl(USHORT p, ULONG v)  { __asm__ __volatile__("outl %0,%1" :: "a"(v), "Nd"(p)); }
#define MMIO8(b,o)  (*(volatile UCHAR  *)((PUCHAR)(b) + (o)))
#define MMIO16(b,o) (*(volatile USHORT *)((PUCHAR)(b) + (o)))
#define MMIO32(b,o) (*(volatile ULONG  *)((PUCHAR)(b) + (o)))

/* interrupts off on this CPU (the flags are given back by IntRestore) */
#ifdef __x86_64__
static __inline__ ULONG IntSave(void)
{ unsigned long long f; __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return (ULONG)f; }
static __inline__ void IntRestore(ULONG f)
{ if (f & 0x200) __asm__ __volatile__("sti" ::: "memory"); }
#else
static __inline__ ULONG IntSave(void)  { ULONG f; __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f; }
static __inline__ void  IntRestore(ULONG f) { __asm__ __volatile__("pushl %0; popfl" :: "r"(f) : "memory", "cc"); }
#endif
#ifndef MemoryBarrier
#define MemoryBarrier() __sync_synchronize()
#endif

#endif
