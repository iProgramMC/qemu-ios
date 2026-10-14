#include "hw/arm/ipod_touch_nand.h"

// iProgramInCpp's Config:
//#define NAND_ALLOW_RW_ACCESS

#define NAND_PAGES_PER_BANK 524288

static void itnand_mmap_read(ITNandState* s, size_t bank, size_t page, void* buf_page, void* buf_spare);
static void itnand_mmap_write(ITNandState* s, size_t bank, size_t page, const void* buf_page, const void* buf_spare);

static int get_bank(ITNandState *s) {
    uint32_t bank_bitmap = (s->fmctrl0 >> 1) & 0xFF;
    for(int bank = 0; bank < NAND_NUM_BANKS; bank++) {
        if((bank_bitmap & (1 << bank)) != 0) {
            return bank;
        }
    }
    return -1;
}

static void set_bank(ITNandState *s, uint32_t activate_bank) {
    for(int bank = 0; bank < 8; bank++) {
        // clear bit, toggle if it is active
        s->fmctrl0 &= ~(1 << (bank + 1));
        if(bank == activate_bank) {
            s->fmctrl0 ^= 1 << (bank + 1);
        }
    }
}

void nand_set_buffered_page(ITNandState *s, uint32_t page) {
    uint32_t bank = get_bank(s);
    if(bank == -1) {
        hw_error("Active bank not set while nand_read with page %d is called (reading multiple pages: %d)!", page, s->reading_multiple_pages);
    }

    if(bank != s->buffered_bank || page != s->buffered_page) {
        // refresh the buffered page
        uint32_t vpn = page * 8 + bank;
        (void) vpn;
        
        itnand_mmap_read(s, bank,page, s->page_buffer, s->page_spare_buffer);
        
        s->buffered_page = page;
        s->buffered_bank = bank;
        // printf("Buffered bank: %d, page: %d\n", s->buffered_bank, s->buffered_page);
    }
}

static uint64_t itnand_read(void *opaque, hwaddr addr, unsigned size)
{
    ITNandState *s = (ITNandState *) opaque;
    if(s->reading_multiple_pages) {
        //fprintf(stderr, "%s: reading from 0x"TARGET_PLX_FMT"\n", __func__, addr);
    }

    switch (addr) {
        case NAND_FMCTRL0:
            return s->fmctrl0;
        case NAND_FMFIFO:
            if(s->cmd == NAND_CMD_ID) {
                return NAND_CHIP_ID;
            }
            else if(s->cmd == NAND_CMD_READSTATUS) {
                return (1 << 6);
            }
            else {
                uint32_t read_val = 0;
                if(s->reading_multiple_pages) {
                    // which bank are we at?
                    if(s->fmdnum % 0x800 == 0) {
                        s->cur_bank_reading += 1;
                        //printf("WILL TURN TO BANK %d (cnt: %d)\n", s->cur_bank_reading, s->fmdnum);
                        set_bank(s, s->banks_to_read[s->cur_bank_reading]);
                    }

                    // compute the offset in the page
                    uint32_t page_offset = s->fmdnum % 0x800;
                    if(page_offset == 0) { page_offset = 0x800; }
                    nand_set_buffered_page(s, s->pages_to_read[s->cur_bank_reading]);
                    //printf("Reading page %d\n", s->pages_to_read[s->cur_bank_reading]);
                    read_val = ((uint32_t *)s->page_buffer)[(NAND_BYTES_PER_PAGE - page_offset) / 4];
                    //printf("FMDNUM: %d, offset: %d\n", s->fmdnum, (NAND_BYTES_PER_PAGE - page_offset) / 4);
                    //printf("Page offset: %d, bytes: 0x%08x\n", page_offset, read_val);
                }
                else {
                    uint32_t page = (s->fmaddr1 << 16) | (s->fmaddr0 >> 16);
                    nand_set_buffered_page(s, page);
                    //printf("Reading page %d\n", page);

                    if(s->reading_spare) {
                        read_val = ((uint32_t *)s->page_spare_buffer)[(NAND_BYTES_PER_SPARE - s->fmdnum - 1) / 4];
                    } else {
                        read_val = ((uint32_t *)s->page_buffer)[(NAND_BYTES_PER_PAGE - s->fmdnum - 1) / 4];
                    }
                }
                s->fmdnum -= 4;
                return read_val;
            }

        case NAND_FMCSTAT: {
            int flags = (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12);
            
            flags |= 1 << 0; // OpeniBoot wants this
            flags |= 1 << 1; // ?
            flags |= 1 << 2; // FMCSTAT_ADDRESS_DONE
            flags |= 1 << 3; // FMCSTAT_TRANSFER_DONE
            
            return flags; // this indicates that everything is ready, including our eight banks
        }
        case NAND_RSCTRL:
            return s->rsctrl;
        default:
            break;
    }
    return 0;
}

static void itnand_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ITNandState *s = (ITNandState *) opaque;
    //if(s->reading_multiple_pages) {
        //fprintf(stderr, "%s: writing 0x%08llx to 0x%08llx\n", __func__, val, addr);
    //}
    

    switch(addr) {
        case NAND_FMCTRL0:
            s->fmctrl0 = val;
            break;
        case NAND_FMCTRL1:
            s->fmctrl1 = val;
            break;
        case NAND_FMADDR0:
            s->fmaddr0 = val;
            break;
        case NAND_FMADDR1:
            s->fmaddr1 = val;
            break;
        case NAND_FMANUM:
            s->fmanum = val;
            break;
        case NAND_CMD:
            s->cmd = val;
            break;
        case NAND_FMDNUM:
            if(val == NAND_BYTES_PER_SPARE - 1) {
                s->reading_spare = 1;
            } else {
                s->reading_spare = 0;
            }
            s->fmdnum = val;
            break;
        case NAND_FMFIFO:
            if(!s->is_writing) {
                fprintf(stderr, "%s: NAND_FMFIFO writing while not in writing mode, writing %08x!\n", __func__, (uint32_t) val);
                return;
            }
            
            if (s->ignore_write_count) {
                s->ignore_write_count--;
                //fprintf(stderr, "%s: Delaying write for this value %08x, because end of page reached. %u left, fmdnum = %u.\n", __func__, (uint32_t) val, s->ignore_write_count, s->fmdnum);
                break;
            }
            
            if (s->reading_multiple_pages)
            {
                // which bank are we at?
                if (s->fmdnum % 0x800 == 0)
                {
                    s->cur_bank_reading += 1;
                    set_bank(s, s->banks_to_read[s->cur_bank_reading]);
                }

                // compute the offset in the page
                uint32_t page_offset = s->fmdnum % 0x800;
                if (page_offset == 0) { page_offset = 0x800; }
                
                nand_set_buffered_page(s, s->pages_to_read[s->cur_bank_reading]);
                
                ((uint32_t *)s->page_buffer)[(NAND_BYTES_PER_PAGE - page_offset) / 4] = val;    
                
                s->fmdnum -= 4;
                if (s->fmdnum % 0x800 == 0)
                {
                    s->ignore_write_count = 4;
                    
                    //uint32_t vpn = s->buffered_page * 8 + s->buffered_bank;
                    //fprintf(stderr, "Flushing page %d, bank %d, vpn %d (multi-page write, %u bytes left)\n", s->buffered_page, s->buffered_bank, vpn, s->fmdnum);
                    
                    itnand_mmap_write(s, s->buffered_bank, s->buffered_page, s->page_buffer, s->page_spare_buffer);
                }
                
                if (s->fmdnum == 0)
                {
                    // we're done!
                    s->is_writing = false;
                }
            }
            else
            {
                ((uint32_t *)s->page_buffer)[(NAND_BYTES_PER_PAGE - s->fmdnum) / 4] = val;
                s->fmdnum -= 4;

                if (s->fmdnum == 0)
                {
                    // we're done!
                    s->is_writing = false;
                    s->ignore_write_count = 4;

                    //uint32_t vpn = s->buffered_page * 8 + s->buffered_bank;
                    //fprintf(stderr, "Flushing page %d, bank %d, vpn %d\n", s->buffered_page, s->buffered_bank, vpn);
                    
                    itnand_mmap_write(s, s->buffered_bank, s->buffered_page, s->page_buffer, s->page_spare_buffer);
                }
            }
            break;
        case NAND_RSCTRL:
            s->rsctrl = val;
            break;
        default:
            break;
    }
}
/*
static uint64_t itnand_read2(void *opaque, hwaddr addr, unsigned size) {
    ITNandState *s = (ITNandState *) opaque;
    qemu_mutex_lock(&s->lock);
    uint64_t rv = itnand_read(opaque, addr, size);
    qemu_mutex_unlock(&s->lock);
    return rv;
}

static void itnand_write2(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    ITNandState *s = (ITNandState *) opaque;
    qemu_mutex_lock(&s->lock);
    itnand_write(opaque, addr, val, size);
    qemu_mutex_unlock(&s->lock);
}
*/
static const MemoryRegionOps nand_ops = {
    .read = itnand_read,
    .write = itnand_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void itnand_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    ITNandState *s = ITNAND(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &nand_ops, s, "nand", 0x1000);
    sysbus_init_irq(sbd, &s->irq);

    s->page_buffer = (uint8_t *)malloc(NAND_BYTES_PER_PAGE);
    s->page_spare_buffer = (uint8_t *)malloc(NAND_BYTES_PER_SPARE);
    s->buffered_page = -1;
    s->buffered_bank = -1;

    qemu_mutex_init(&s->lock);
}

static void itnand_reset(DeviceState *d)
{
    ITNandState *s = (ITNandState *) d;

    s->fmctrl0 = 0;
    s->fmctrl1 = 0;
    s->fmaddr0 = 0;
    s->fmaddr1 = 0;
    s->fmanum = 0;
    s->fmdnum = 0;
    s->rsctrl = 0;
    s->cmd = 0;
    s->reading_spare = 0;
    s->buffered_page = -1;
}

static void itnand_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->reset = itnand_reset;
}

static const TypeInfo itnand_info = {
    .name          = TYPE_ITNAND,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ITNandState),
    .instance_init = itnand_init,
    .class_init    = itnand_class_init,
};

static void itnand_register_types(void)
{
    type_register_static(&itnand_info);
}

type_init(itnand_register_types)


// iProgramInCpp added this.  This basically maps the entire nand in memory to be fast to access.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const char* convert_windows_error(DWORD err)
{
    static char buf[512]; // static buffer like strerror
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        0,
        buf,
        sizeof(buf),
        NULL
    );
    if (size == 0) {
        snprintf(buf, sizeof(buf), "Unknown error %lu", err);
    }
    return buf;
}

static void* iprogs_mmap_file_into_memory(const char* file_name, size_t size)
{
    HANDLE file = CreateFileA(file_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "could not open nand file %s: %s", file_name, convert_windows_error(GetLastError()));
        exit(1);
    }
    
#ifdef NAND_ALLOW_RW_ACCESS
    int permissions = PAGE_READWRITE, access = FILE_MAP_ALL_ACCESS;
#else
    int permissions = PAGE_WRITECOPY, access = FILE_MAP_COPY;
#endif
    
    HANDLE mapping = CreateFileMappingA(file, NULL, permissions, (DWORD)(size >> 32), (DWORD) size, NULL);
    if (!mapping) {
        CloseHandle(file);
        fprintf(stderr, "could not create file mapping for nand file %s: %s", file_name, convert_windows_error(GetLastError()));
        exit(1);
    }
    
    void* map = MapViewOfFile(mapping, access, 0, 0, size);
    CloseHandle(mapping);
    CloseHandle(file);
    
    if (!map) {
        fprintf(stderr, "could not map nand file %s into memory: %s", file_name, convert_windows_error(GetLastError()));
        exit(1);
    }
    
    return map;
}

#else

static void* iprogs_mmap_file_into_memory(const char* file_name, size_t size)
{
	int fd = open(file_name, O_RDWR, 0);
	if (fd < 0) {
		fprintf(stderr, "iprogs_mmap_file_into_memory: cannot open %s: %s", file_name, strerror(errno));
		exit(1);
	}

	int flags;
#ifdef NAND_ALLOW_RW_ACCESS
	flags = MAP_SHARED;
#else
	flags = MAP_PRIVATE;
#endif
	void* res = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, fd, 0);
	if (res == MAP_FAILED) {
		fprintf(stderr, "iprogs_mmap_file_into_memory: failed to map file %s into memory: %s", file_name, strerror(errno));
		exit(1);
	}

	return res;
}

#endif // _WIN32

void itnand_initialize_nand_files(ITNandState* s)
{
    /*
    char buffer[512];
    for (int i = 0; i < NAND_NUM_BANKS; i++)
    {
        snprintf(buffer, sizeof buffer, "%s/nand_data_%d.img", s->nand_path, i);
        s->nand_mmap_data[i] = iprogs_mmap_file_into_memory(buffer, NAND_PAGES_PER_BANK * NAND_BYTES_PER_PAGE);
    }
    
    for (int i = 0; i < NAND_NUM_BANKS; i++)
    {
        snprintf(buffer, sizeof buffer, "%s/nand_spare_%d.img", s->nand_path, i);
        s->nand_mmap_spare[i] = iprogs_mmap_file_into_memory(buffer, NAND_PAGES_PER_BANK * NAND_BYTES_PER_SPARE);
    }
    */
    
    const uint64_t totalBytesPages = (uint64_t)NAND_PAGES_PER_BANK * NAND_NUM_BANKS * NAND_BYTES_PER_PAGE;
    const uint64_t totalBytesSpares = (uint64_t)NAND_PAGES_PER_BANK * NAND_NUM_BANKS * NAND_BYTES_PER_SPARE;
    
    s->nand_mmap_data = iprogs_mmap_file_into_memory(s->nand_path, totalBytesPages + totalBytesSpares);
    s->nand_mmap_spare = s->nand_mmap_data + totalBytesPages;
}

static void itnand_mmap_read(ITNandState* s, size_t bank, size_t page, void* buf_page, void* buf_spare)
{
    if (bank >= NAND_NUM_BANKS) {
        fprintf(stderr, "ERROR: trying to read from bank %zu!", bank);
        return;
    }
    
    if (page >= NAND_PAGES_PER_BANK) {
        fprintf(stderr, "ERROR: trying to read from page %zu > %zu!", page, (size_t) NAND_PAGES_PER_BANK);
        return;
    }
    
    uint64_t vpn = (uint64_t)page * 8 + bank;
    memcpy(buf_page, s->nand_mmap_data + vpn * NAND_BYTES_PER_PAGE, NAND_BYTES_PER_PAGE);
    memcpy(buf_spare, s->nand_mmap_spare + vpn * NAND_BYTES_PER_SPARE, NAND_BYTES_PER_SPARE);
}

static void itnand_mmap_write(ITNandState* s, size_t bank, size_t page, const void* buf_page, const void* buf_spare)
{
    if (bank >= NAND_NUM_BANKS) {
        fprintf(stderr, "ERROR: trying to write to bank %zu!", bank);
        return;
    }
    
    if (page >= NAND_PAGES_PER_BANK) {
        fprintf(stderr, "ERROR: trying to write to page %zu > %zu!", page, (size_t) NAND_PAGES_PER_BANK);
        return;
    }
    
    uint64_t vpn = (uint64_t)page * 8 + bank;
    memcpy(s->nand_mmap_data + vpn * NAND_BYTES_PER_PAGE, buf_page, NAND_BYTES_PER_PAGE);
    memcpy(s->nand_mmap_spare + vpn * NAND_BYTES_PER_SPARE, buf_spare, NAND_BYTES_PER_SPARE);
}
