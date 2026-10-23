/** @file libc_boundary.c @brief Ensure Zig objects retain system libc auxv. */
#include <elf.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>
int main(void) {
    unsigned long headers = getauxval(AT_PHDR);
    unsigned long count = getauxval(AT_PHNUM);
    unsigned long pages = getauxval(AT_PAGESZ);
    long system_pages = sysconf(_SC_PAGESIZE);
    if (!headers || !count || system_pages <= 0 || pages != (unsigned long)system_pages) {
        fputs("Linked Zig objects replaced libc auxiliary-vector lookup\n", stderr);
        return 1;
    }
    puts("C/Zig linked objects preserve libc auxiliary-vector lookup");
    return 0;
}
