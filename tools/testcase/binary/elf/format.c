/*
 * ELF format
 *
 * (C) 2018.03 BiscuitOS <buddy.zhang@aliyun.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>

#include <test/debug.h>
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <linux/binfmts.h>
#include <linux/string.h>
#include <linux/stat.h>

#include <asm/segment.h>

#define __LIBRARY__
#include <linux/unistd.h>
/*
 * Parse ELF header
 *
 * The ELF header defines whether to use 32-bit addresses. The header 
 * contains three fields that are affected by this setting and offset other 
 * fields that follow them. The ELF header is 52 bytes long for 32-bit 
 * binaries respectively.
 *
 * 32 Bit ELF header
 * -----------------------------------------------------------------------
 * | Offset | Size |       Field       |            Purpose              |
 * -----------------------------------------------------------------------
 * | 0x00   |  4   | e_ident[EI_MAG0]  | 0x7F followed by ELF(45 4c 46)  |
 * |        |      | e_ident[EI_MAG3]  | in ASCII; these four bytes      |
 * |        |      |                   | constitute the magic number.    |
 * -----------------------------------------------------------------------
 * | 0x04   |  1   | e_ident[EI_CLASS] | This byte is set to 1 or 2 to   |
 * |        |      |                   |signif 32-bit format             |
 * -----------------------------------------------------------------------
 * | 0x05   |  1   | e_ident[EI_DATA]  | This byte is set to either 1 or |
 * |        |      |                   | 2 to signify little or big      |
 * |        |      |                   | endianness, respectively. This  |
 * |        |      |                   | affects interpretation of       |
 * |        |      |                   | multi-byte fields starting with |
 * |        |      |                   | offset 0x10.                    |
 * -----------------------------------------------------------------------
 * | 0x06   |  1   | e_ident[EI_VE..N] | Set to 1 for the original       |
 * |        |      | EI_VERSION        | version of ELF.                 |
 * -----------------------------------------------------------------------
 * | 0x07   |  1   | e_ident[EI_OSABI] | Identifies the target operating |
 * |        |      |                   | system ABI. It is often set to  |
 * |        |      |                   | 0 regardless of the target      |
 * |        |      |                   | platform.                       |
 * -----------------------------------------------------------------------
 * | 0x08   |  1   | e_ident[EI_AB..N] | Further specifies the ABI       |
 * |        |      | EI_ABIVERSION     | version. Its interpretation     |
 * |        |      |                   | depends on the target ABI.      |
 * |        |      |                   | Linux kernel (after at least    |
 * |        |      |                   | 2.6) has no definition of it.   |
 * |        |      |                   | In that case, offset and size   |
 * |        |      |                   | of EI_PAD are 8.                |
 * -----------------------------------------------------------------------
 * | 0x09   |  7   | e_ident[EI_PAD]   | currently unused                |
 * -----------------------------------------------------------------------
 * | 0x10   |  2   | e_type            | 1, 2, 3, 4 specify whether the  |
 * |        |      |                   | object is relocatable,          |
 * |        |      |                   | executable, shared, or core,    |
 * |        |      |                   | respectively.                   |
 * -----------------------------------------------------------------------
 * | 0x12   |  2   | e_machine         | Specifies target instruction    |
 * |        |      |                   | set architecture.               |
 * ----------------------------------------------------------------------- 
 * | 0x14   |  4   | e_version         | Set to 1 for the original       |
 * |        |      |                   | version of ELF.                 |
 * -----------------------------------------------------------------------
 * | 0x18   |  4   | e_entry           | This is the memory address of   | 
 * |        |      |                   | the entry point from where the  |
 * |        |      |                   | process starts executing. This  |
 * |        |      |                   | field is either 32 or 64 bits   |
 * |        |      |                   | long depending on the format    |
 * |        |      |                   | defined earlier.                |
 * -----------------------------------------------------------------------
 * | 0x1C   |  4   | e_phoff           | Points to the start of the      |
 * |        |      |                   | program header table. It        |
 * |        |      |                   | usually follows the file header |
 * |        |      |                   | immediately, making the offset  |
 * |        |      |                   | 0x34 for 32-bit ELF executables |
 * -----------------------------------------------------------------------
 * | 0x20   |  4   | e_shoff           | Points to the start of the      |
 * |        |      |                   | section header table.           |
 * -----------------------------------------------------------------------
 * | 0x24   |  4   | e_flags           | Interpretation of this field    |
 * |        |      |                   | depends on the target           |
 * |        |      |                   | architecture.                   |
 * -----------------------------------------------------------------------
 * | 0x28   |  2   | e_ehsize          | Contains the size of this       |
 * |        |      |                   | header normally 52 Bytes        |
 * ----------------------------------------------------------------------- 
 * | 0x2A   |  2   | e_phentsize       | Contains the size of a program  |
 * |        |      |                   | header table entry.             |
 * -----------------------------------------------------------------------
 * | 0x2C   |  2   | e_phnum           | Contains the number of entries  |
 * |        |      |                   | in the program header table.    |
 * -----------------------------------------------------------------------
 * | 0x2E   |  2   | e_shentsize       | Contains the size of a section  |
 * |        |      |                   | header table entry.             |
 * -----------------------------------------------------------------------
 * | 0x30   |  2   | e_shnum           | Contains the number of entries  |
 * |        |      |                   | in the section header table     |
 * -----------------------------------------------------------------------
 * | 0x32   |  2   | e_shstrndx        | Contains index of the section   |
 * |        |      |                   | header table entry that         |
 * |        |      |                   | contains the section names.     |
 * -----------------------------------------------------------------------
 *
 */
#ifdef CONFIG_PARSE_ELF_HEADER
static int parse_elf_header(struct elfhdr *eh)
{
    printk("FIRST %#x\n", eh->e_ident[0]);
    return 0;
}
#endif // CONFIG_PARSE_ELF_HEADER

/* Read elf file from VFS  */
static int elf_read(struct inode *inode, unsigned long offset,
                    char *addr, unsigned long count)
{
    struct file file;
    int result = -ENOEXEC;

    if (!inode->i_op || !inode->i_op->default_file_ops)
        goto end_readexec;

    file.f_mode  = 1;
    file.f_flags = 0;
    file.f_count = 1;
    file.f_inode = inode;
    file.f_pos   = 0;
    file.f_reada = 0;
    file.f_op    = inode->i_op->default_file_ops;
    if (file.f_op->open)
        if (file.f_op->open(inode, &file))
            goto end_readexec;
    if (!file.f_op || !file.f_op->read)
        goto close_readexec;
    if (file.f_op->lseek) {
        if (file.f_op->lseek(inode, &file, offset, 0) != offset)
            goto close_readexec;
    } else
        file.f_pos = offset;
    if (get_fs() == USER_DS) {
        result = verify_area(VERIFY_WRITE, addr, count);
        if (result)
            goto close_readexec;
    }
    result = file.f_op->read(inode, &file, addr, count);
    
close_readexec:
    if (file.f_op->release)
        file.f_op->release(inode, &file);
end_readexec:
    return result;
}

/*
 * Load specical EXEC file.
 */
static int do_elf(char *filename, char **argv, char **envp, 
                  struct pt_regs *regs)
{
    struct linux_binprm bprm;
    struct linux_binfmt *fmt;
    unsigned long old_fs;
    int i;
    int retval = 0;

    retval = open_namei(filename, 0, 0, &bprm.inode, NULL);
    if (retval)
        return retval;
    if (!S_ISREG(bprm.inode->i_mode)) {
        retval = -EACCES;
        goto elf_error2; 
    }
    if (!bprm.inode->i_sb) {
        retval = -EACCES;
        goto elf_error2;
    }
#ifdef CONFIG_PARSE_ELF_HEADER
    /* Read Execute-file */
    memset(bprm.buf, 0, sizeof(bprm.buf));
    old_fs = get_fs();
    set_fs(get_ds());
    retval = elf_read(bprm.inode, 0, bprm.buf, 128);
    set_fs(old_fs);
    if (retval < 0)
        goto elf_error2;
    parse_elf_header((struct elfhdr *)bprm.buf);
#endif

elf_error2:
    iput(bprm.inode);

    return retval;
}

/* Common system call entry */
int sys_d_parse_elf(struct pt_regs regs)
{
    int error;
    char *filename;

    error = getname((char *)regs.ebx, &filename);
    if (error)
        return error;
    error = do_elf(filename, (char **)regs.ecx, (char **)regs.edx, &regs);
    putname(filename);
    return error;
}

/* Invoke by system call: int $0x80 */
int debug_binary_elf_format(void)
{
#ifdef CONFIG_ELF_RELOCATABLE_FILE
    d_parse_elf("/bin/demo.o", NULL, NULL);
#endif
    return 0;
}
