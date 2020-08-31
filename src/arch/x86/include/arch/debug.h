#ifndef _X86_DEBUG_H
#define _X86_DEBUG_H

// void (*debug_putchar) (char ch);
// 
// Note: this mean you define "debug_putchar" variable in the head file;
// When other file include this file like "#include<arch/debug.h>", it will
// define many times,I think if your compile let it go through because the 
// version is old, GCC-10.0.1 get an error: Multiple definitions.
// so I put the define "debug_putchar" to "src/arch/x86/mach-i386/debug.c".
//
//
// And put a "extern" in "src/kernel/debug.c" because just one func "printk" reference
// the "debug_putchar" (i just find)

// more butter put in this place :-)
extern void (*debug_putchar)(char ch);

void init_kernel_debug();
void init_serial_debug();
void serial_putchar(char ch);
void init_console_debug();
void console_putchar(char ch);

#endif  /* _X86_DEBUG_H */
