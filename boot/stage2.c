#include <basic_runtime.h>
#include <x86_64.h>
#include <booto.h>
#include <storage.h>

// fix headers
void *load_elf(void *base, u64 offset, heap pages, heap bss);

static void print_block(void *addr, int length)
{
    for (int i = 0; i< length; i+=8){
        print_u64(*(u64 *)(addr+i));
        console ("\n");
    }
}

extern void run64(u32 entry);

// there are a few of these little allocators
u64 working = 0x1000;

static u64 stage2_allocator(heap h, bytes b)
{
    u64 result = working;
    working += b;
    return working;
}

// xxx - instead of pulling the thread on generic storage support,
// we hardwire in the kernel resolution. revisit - it may be
// better to start populating the node tree here.
boolean lookup_kernel(void *fs, u64 *offset, u64 *length)
{
    snode sn = {fs, 0};
    u32 off, loff, coff;
    if (!snode_lookup(sn, "children", &off)) return false;
    sn.offset = off;
    if (!snode_lookup(sn, "kernel", &off)) return false;
    sn.offset = off;    
    if (!snode_lookup(sn, "contents", &coff)) return false;
    if (!snode_lookup(sn, "length", &loff)) return false;    
    *offset = coff;
    *length = loff; // this shouldn't be an immediate
    return true;
}

// pass the memory parameters (end of load, end of mem)
void centry()
{
    console("stage2\n");
    struct heap workings;
    workings.alloc = stage2_allocator;
    heap working = &workings;
    
    // move this to the end of memory or the beginning of the pci gap
    // (under the begining of the kernel)
    u64 identity_start = 0x100000;
    u64 identity_length = 0x300000;

    for (region e = regions; region_type(e); e -= 1) {
        if (identity_start == region_base(e)) 
            region_base(e) = identity_start + identity_length;
    }

    create_region(identity_start, identity_length, REGION_IDENTITY);
        
    heap pages = region_allocator(working, PAGESIZE, REGION_IDENTITY);
    heap physical = region_allocator(working, PAGESIZE, REGION_PHYSICAL);
    void *vmbase = allocate_zero(pages, PAGESIZE);
    mov_to_cr("cr3", vmbase);
    map(identity_start, identity_start, identity_length, pages);

    // leak a page, and assume ph is in the first page
    void *header = allocate(physical, PAGESIZE);

    unsigned int fs_start = STAGE1SIZE + STAGE2SIZE;
    struct snode sn = {header, 0};
    read_sectors(header, fs_start, PAGESIZE); // read in the head of the filesystem

    u64 kernel_length, kernel_offset;
    if (!lookup_kernel(header, &kernel_offset, &kernel_length)) {
        console("unable to find kernel\n");
        QEMU_HALT();
    }
    
    void *kernel = allocate(physical, pad(kernel_length, PAGESIZE));
    read_sectors(kernel, fs_start + kernel_offset, kernel_length);

    // should drop this in stage3? ... i think we just need
    // service32 and the stack.. this doesn't show up in the e820 regions
    // stack is currently in the first page, so lets leave it mapped
    // and take it out later...ideally move the stack here
    map(0, 0, 0xa000, pages);
    // tell stage3 that this is off limits..could actually move there
    create_region(0, 0xa0000, REGION_VIRTUAL);
    void *k = load_elf(kernel, 0, pages, physical);
    run64(k);
}
