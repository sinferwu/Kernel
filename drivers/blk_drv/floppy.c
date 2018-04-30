/*
 *  linux/kernel/floppy.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 02.12.91 - Changed to static variables to indicate need for reset
 * and recalibrate. This makes some things easier (output_byte reset
 * checking etc), and means less interrupt jumping in case of errors,
 * so the code is hopefully easier to understand.
 */

/*
 * This file is certainly a mess. I've tried my best to get it working,
 * but I don't like programming floppies, and I have only one anyway.
 * Urgel. I should check for more errors, and do more graceful error
 * recovery. Seems there are problems with several drives. I've tried to
 * correct them. No promises. 
 */

/*
 * As with hd.c, all routines within this file can (and will) be called
 * by interrupts, so extreme caution is needed. A hardware interrupt
 * handler may not sleep, or a kernel panic will happen. Thus I cannot
 * call "floppy-on" directly, but have to set a special timer interrupt
 * etc.
 */

/*
 * 28.02.92 - made track-buffering routines, based on the routines written
 * by entropy@wintermute.wpi.edu (Lawrence Foard). Linus.
 */

#include <linux/fs.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <asm/io.h>
#include <asm/system.h>
#include <linux/fdreg.h>
#include <linux/blk.h>
#include <linux/sched.h>

unsigned int changed_floppies = 0;

static int recalibrate = 0;
static int reset = 0;
static int seek = 0;

#define TYPE(x)    ((x)>>2)
#define DRIVE(x)   ((x)&0x03)

#define MAJOR_NR              2
#define DEVICE_NAME           "floppy"
#define DEVICE_NR(device)     ((device) & 3)
#define CURRENT               (blk_dev[MAJOR_NR].current_request)
#define CURRENT_DEV           DEVICE_NR(CURRENT->dev)
#define DEVICE_ON(device)     floppy_on(DEVICE_NR(device))
#define DEVICE_OFF(device)    floppy_off(DEVICE_NR(device))

#define INIT_REQUEST \
repeat:              \
    if (!CURRENT)    \
        return;      \
    if (MAJOR(CURRENT->dev) != MAJOR_NR)  \
        panic(": request list destroyed"); \
    if (CURRENT->bh) {     \
        panic(": block not locked");       \
    }

static void (do_fd_request) (void);

/*
 * This struct defines the different floppy types. Unlike minix
 * linux does't have a "search for right type"-type. as the code
 * for that is convoluted and weird. I'vd got enough problem with
 * this driver as it is.
 * The 'stretch' tells if the tracks need to be boubled for some
 * types (ie 360kb diskette in 1.2Mb drive etc). Others should
 * be self-explanatory.
 */
struct floppy_struct {
	unsigned int size, sect, head, track, stretch;
	unsigned char gap, rate, spec1;
};

static struct floppy_struct floppy_type[] = {
	{
	0, 0, 0, 0, 0, 0x00, 0x00, 0x00},	/* no thing */
	{
	720, 9, 2, 40, 0, 0x2A, 0x02, 0xDF},	/* 360kb PC diskettes */
	{
	2400, 15, 2, 80, 0, 0x1B, 0x00, 0xDF},	/* 1.2 MB AT-diskettes */
	{
	720, 9, 2, 40, 1, 0x2A, 0x02, 0xDF},	/* 360kB in 720kB drive */
	{
	1440, 9, 2, 80, 0, 0x2A, 0x02, 0xDF},	/* 3.5" 720KB diskette */
	{
	720, 9, 2, 40, 1, 0x23, 0x01, 0xDF},	/* 360kB in 1.2MB drive */
	{
	1440, 9, 2, 80, 0, 0x23, 0x01, 0xDF},	/* 720kB in 1.2MB drive */
	{
	2880, 18, 2, 80, 0, 0x1B, 0x00, 0xCF},	/* 1.44MB diskette */
};

static int floppy_sizes[] ={
	   0,   0,   0,   0,
	 360, 360 ,360, 360,
	1200,1200,1200,1200,
	 360, 360, 360, 360,
	 720, 720, 720, 720,
	 360, 360, 360, 360,
	 720, 720, 720, 720,
	1440,1440,1440,1440
};


/*
 * globals used by 'result()'
 */
#define MAX_REPLIES   7
static unsigned char reply_buffer[MAX_REPLIES];
#define ST0 (reply_buffer[0])
#define ST1 (reply_buffer[1])
#define ST2 (reply_buffer[2])
#define ST3 (reply_buffer[3])

/*
 * These are global variables, as that's the easiest way to give
 * information to interrupts. They are the data used for the current
 * request.
 */
#define NO_TRACK 255

static int read_track = 0; /* flag to indicate if we want to read all track */
static int buffer_track = -1;
static int buffer_drive = -1;
static int cur_spec1 = -1;
static int cur_rate = -1;
static struct floppy_struct *floppy = floppy_type;
static unsigned char current_drive = 255;
static unsigned char sector = 0;
static unsigned char head = 0;
static unsigned char track = 0;
static unsigned char seek_track = 0;
static unsigned char current_track = NO_TRACK;
static unsigned char command = 0;
unsigned char selected = 0;
struct task_struct *wait_on_floppy_select = NULL;

/*
 * Rate is 0 for 500kb/s, 2 for 300kbps, 1 for 250kbps
 * Spec1 is 0xSH, where S is stepping rate (F=1ms, E=2ms, D=3ms etc),
 * H is head unload time (1 = 16ms, 2 = 32ms, etc)
 *
 * Spec2 is (HLD<<1 | ND), where HLD is head load time (1 = 2ms,
 * 2=4 ms etc) and ND is set means no DMA. Hardcoded to 6 (HLD=6ms,
 * use DMA).
 */
extern void floppy_interrupt(void);
extern char tmp_floppy_area[1024];
extern char floppy_track_buffer[512*2*18];

extern unsigned char current_DOR;

void do_fd_request(void);

void (*do_floppy) (void) = NULL;

#define immoutb_p(val, port)   \
	__asm__ ("outb %0, %1\n\tjmp 1f\n1:\tjmp 1f\n1:" \
			 :: "a" ((char) (val)), "i" (port))

/*
 * Note that MAX_ERRORS=X doesn't imply that we retry every bad read
 * max X times - some types of errors increase the errorcount by 2 or
 * even 3, so we might actually retry only X/2 times before giving up.
 */
#define MAX_ERRORS 12

static void (do_fd_request) (void);

static inline void unlock_buffer(struct buffer_head *bh)
{
	if (!bh->b_lock)
		printk(": free buffer being unlocked\n");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

static inline void end_request(int uptodate)
{
	floppy_off(CURRENT->dev);
	if (CURRENT->bh) {
		CURRENT->bh->b_uptodate = uptodate;
		unlock_buffer(CURRENT->bh);
	}
	if (!uptodate) {
		printk(" I/O error\n\r");
		printk("dev %04x, block %d\n\r", CURRENT->dev,
		       CURRENT->bh->b_blocknr);
	}
	wake_up(&CURRENT->waiting);
	wake_up(&wait_for_request);
	CURRENT->dev = -1;
	CURRENT = CURRENT->next;
}

void floppy_deselect(unsigned int nr)
{
	if (nr != (current_DOR & 3))
		printk("floppy deselect: drive not selected\n\r");
	selected = 0;
	wake_up(&wait_on_floppy_select);
}

static void output_byte(char byte)
{
	int counter;
	unsigned char status;

	if (reset)
		return;
	for (counter = 0; counter < 10000; counter++) {
		status = inb_p(FD_STATUS) & (STATUS_READY | STATUS_DIR);
		if (status == STATUS_READY) {
			outb(byte, FD_DATA);
			return;
		}
	}
	current_track = NO_TRACK;
	reset = 1;
	printk("Unable to send byte to FDC\n\r");
}

static int result(void)
{
	int i = 0, counter, status;

	if (reset)
		return -1;
	for (counter = 0; counter < 10000; counter++) {
		status = inb_p(FD_STATUS) & (STATUS_DIR | STATUS_READY |
					     STATUS_BUSY);
		if (status == STATUS_READY)
			return i;
		if (status == (STATUS_DIR | STATUS_READY | STATUS_BUSY)) {
			if (i >= MAX_REPLIES)
				break;
			reply_buffer[i++] = inb_p(FD_DATA);
		}
	}
	reset = 1;
	current_track = NO_TRACK;
	printk("Getstatus time out\n\r");
	return -1;
}

static void reset_interrupt(void)
{
	output_byte(FD_SENSEI);
	(void)result();
	output_byte(FD_SPECIFY);
	output_byte(cur_spec1);	/* hut etc */
	output_byte(6);		/* Head load time = 6ms, DMA */
	do_fd_request();
}

/*
 * Specical case - used after a unexpected interrupt (or reset)
 */
static void recal_interrupt(void)
{
	output_byte(FD_SENSEI);
	current_track = NO_TRACK;
	if (result() != 2 || (ST0 & 0xE0) == 0x60)
		reset = 1;
	do_fd_request();
}

/*
 * reset is done by pulling bit 2 of DOR low for a while.
 */
static void reset_floppy(void)
{
	int i;

	do_floppy = reset_interrupt;
	reset = 0;
	current_track = NO_TRACK;
	cur_spec1 = -1;
	cur_rate = -1;
	recalibrate = 1;
	printk("Reset-floppy called\n\r");
	cli();
	outb_p(current_DOR & ~0x04,FD_DOR);
	for (i=0 ; i<1000 ; i++)
		__asm__("nop");
	outb(current_DOR,FD_DOR);
	sti();
}

static void recalibrate_floppy(void)
{
	recalibrate = 0;
	current_track = 0;
	do_floppy = recal_interrupt;
	output_byte(FD_RECALIBRATE);
	output_byte(head << 2 | current_drive);
	if (reset)
		do_fd_request();
}

static void bad_flp_intr(void)
{
	current_track = NO_TRACK;
	CURRENT->errors++;
	if (CURRENT->errors > MAX_ERRORS) {
		floppy_deselect(current_drive);
		end_request(0);
	}
	if (CURRENT->errors > MAX_ERRORS / 2)
		reset = 1;
	else
		recalibrate = 1;
}

#define copy_buffer(from, to) \
	__asm__ ("cld ; rep ; movsl" \
			 :: "c" (BLOCK_SIZE / 4), "S" ((long)(from)), \
			    "D" ((long)(to) \
                         :  "cx", "di", "si")   \
)

static void setup_DMA(void)
{
	unsigned long addr = (long) CURRENT->buffer;
	unsigned long count = 1024;

	cli();
	if (read_track) {
/* mark buffer-track bad, in case all this fails.. */
		buffer_drive = buffer_track = -1;
		count = floppy->sect*2*512;
		addr = (long) floppy_track_buffer;
	} else if (addr >= 0x100000) {
		addr = (long) tmp_floppy_area;
		if (command == FD_WRITE)
			copy_buffer(CURRENT->buffer,tmp_floppy_area);
	}
	/* mask DMA 2 */
	immoutb_p(4 | 2, 10);
	/*
	 * output command byte. I don't know why, but everything(minix,
	 * sanches & canton) output this twice, first to 12 thenn to 11
	 */
	__asm__("outb %%al, $12\n\tjmp 1f\n1:\tjmp 1f\n1:\t"
		"outb %%al, $11\n\tjmp 1f\n1:\tjmp 1f\n1:"::"a"((char)
	    ((command == FD_READ) ?  DMA_READ : DMA_WRITE)));
	/* 8 low bits of addr */
	immoutb_p(addr, 4);
	addr >>= 8;
	/* bits 8-15 addr */
	immoutb_p(addr, 4);
	addr >>= 8;
	/* bits 16-19 of addr */
	immoutb_p(addr, 0x81);
	/* low 8 bits of count-1 */
	count--;
        immoutb_p(count, 5);
        count >>= 8;
	/* high 8 bits of count-1 */
	immoutb_p(count, 5);
	/* activate DMA 2 */
	immoutb_p(0 | 2, 10);
	sti();
}

/*
 * Ok, this interrupt is called after a DMA read/write thas succeeded,
 * so we check the results, and copy any buffers.
 */
static void rw_interrupt(void)
{
	char *buffer_area;

	if (result() != 7 || (ST0 & 0xf8) || (ST1 & 0xbf) || (ST2 & 0x73)) {
		if (ST1 & 0x02) {
			printk("Drive %d is write protected\n\r",
			       current_drive);
			floppy_deselect(current_drive);
			end_request(0);
		} else
			bad_flp_intr();
		do_fd_request();
		return;
	}
	if (read_track) {
		buffer_track = seek_track;
		buffer_drive = current_drive;
		buffer_area = floppy_track_buffer +
			((sector-1 + head*floppy->sect)<<9);
		copy_buffer(buffer_area,CURRENT->buffer);
	} else if (command == FD_READ &&
		(unsigned long)(CURRENT->buffer) >= 0x100000)
		copy_buffer(tmp_floppy_area,CURRENT->buffer);
	floppy_deselect(current_drive);
	end_request(1);
	do_fd_request();
}

/*
 * We try to read tracks, but if we get too many errors, we
 * go back to reading just one sector at a time.
 *
 * This means we should be able to read a sector even if there
 * are other bad sectors on this track.
 */
inline void setup_rw_floppy(void)
{
	setup_DMA();
	do_floppy = rw_interrupt;
	output_byte(command);
	if (read_track) {
		output_byte(current_drive);
		output_byte(track);
		output_byte(0);
		output_byte(1);
	} else {
		output_byte(head<<2 | current_drive);
		output_byte(track);
		output_byte(head);
		output_byte(sector);
	}
	output_byte(2);		/* sector size = 512 */
	output_byte(floppy->sect);
	output_byte(floppy->gap);
	output_byte(0xFF);	/* sector size (0xff when n!=0 ?) */
	if (reset)
		do_fd_request();
}

/*
 * This is the routine called after every seek (or recalibrate) interrupt
 * from the floppy controller. Note that the "unexpected interrupt" routine
 * also does a recalibrate, but does't come here.
 */
static void seek_interrupt(void)
{
	/* sense drive status */
	output_byte(FD_SENSEI);
	if (result() != 2 || (ST0 & 0xF8) != 0x20 || ST1 != seek_track) {
                recalibrate = 1;
		bad_flp_intr();
		do_fd_request();
		return;
	}
	current_track = ST1;
	setup_rw_floppy();
}

/*
 * This routine is called when everything should be correctly
 * set up for the transfer (ie floppy motor is on and the correct
 * floppy is selected).
 */
static void transfer(void)
{
	read_track = (command == FD_READ) && (CURRENT->errors < 4);
	if (cur_spec1 != floppy->spec1) {
		cur_spec1 = floppy->spec1;
		output_byte(FD_SPECIFY);
		output_byte(cur_spec1);		/* hut etc */
		output_byte(6);			/* Head load time =6ms, DMA */
	}
	if (cur_rate != floppy->rate)
		outb_p(cur_rate = floppy->rate,FD_DCR);
	if (reset) {
		do_fd_request();
		return;
	}
	if (!seek) {
		setup_rw_floppy();
		return;
	}
	do_floppy = seek_interrupt;
	output_byte(FD_SEEK);
	if (read_track)
		output_byte(current_drive);
	else
		output_byte((head<<2) | current_drive);
	output_byte(seek_track);
	if (reset)
		do_fd_request();
}

static void floppy_on_interrupt(void)
{
	if (inb(FD_DIR) & 0x80) {
		changed_floppies |= 1<<current_drive;
		buffer_track = -1;
	}
	if (reset) {
		reset_floppy();
		return;
	}
	if (recalibrate) {
		recalibrate_floppy();
		return;
	}
/* We cannot do a floppy-select, as that might sleep. We just force it */
	selected = 1;
	if (current_drive != (current_DOR & 3)) {
		current_DOR &= 0xFC;
		current_DOR |= current_drive;
		outb(current_DOR,FD_DOR);
		add_timer(2,&transfer);
	} else
		transfer();
}

void do_fd_request(void)
{
	unsigned int block;
	char * buffer_area;

	INIT_REQUEST;
	seek = 0;
	floppy = (MINOR(CURRENT->dev)>>2) + floppy_type;
	if (current_drive != CURRENT_DEV)
		current_track = NO_TRACK;
	current_drive = CURRENT_DEV;
	block = CURRENT->sector;
	if (block+2 > floppy->size) {
		end_request(0);
		goto repeat;
	}
	sector = block % floppy->sect;
	block /= floppy->sect;
	head = block % floppy->head;
	track = block / floppy->head;
	seek_track = track << floppy->stretch;
	if (CURRENT->cmd == READ)
		command = FD_READ;
	else if (CURRENT->cmd == WRITE)
		command = FD_WRITE;
	else {
		printk("do_fd_request: unknown command\n");
		end_request(0);
		goto repeat;
	}
	if ((seek_track == buffer_track) &&
	 (current_drive == buffer_drive)) {
		buffer_area = floppy_track_buffer +
			((sector + head*floppy->sect)<<9);
		if (command == FD_READ) {
			copy_buffer(buffer_area,CURRENT->buffer);
			end_request(1);
			goto repeat;
		} else
			copy_buffer(CURRENT->buffer,buffer_area);
	}
	if (seek_track != current_track)
		seek = 1;
	sector++;
	add_timer(ticks_to_floppy_on(current_drive),&floppy_on_interrupt);
}

void floppy_init(void)
{
	outb(current_DOR,FD_DOR);
	blk_size[MAJOR_NR] = floppy_sizes;
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	set_intr_gate(0x26,&floppy_interrupt);
	outb(inb_p(0x21)&~0x40,0x21);
}

/*
 * floppy-change is never called from an interrupt, so we can relax a bit
 * here, sleep etc. Note that floppy-on tries to set current_DOR to point
 * to the desired drive, but it will probably not survive the sleep if
 * several floppies are used at the same time: thus the loop.
 */
int floppy_change(unsigned int nr)
{
	unsigned int mask = 1 << (bh->b_dev & 0x03);

	if (MAJOR(bh->b_dev) != 2) {
		printk("floppy_changed: not a floppy\n");
		return 0;
	}
	if (changed_floppies & mask) {
		changed_floppies &= ~mask;
		recalibrate = 1;
		return 1;
	}
	if (!bh)
		return 0;
	if (bh->b_dirt)
		ll_rw_block(WRITE,bh);
	else {
		buffer_track = -1;
		bh->b_uptodate = 0;
		ll_rw_block(READ,bh);
	}
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
	if (changed_floppies & mask) {
		changed_floppies &= ~mask;
		recalibrate = 1;
		return 1;
	}	
	return 0;
}

void unexpected_floppy_interrupt(void)
{
    current_track = NO_TRACK;
    output_byte(FD_SENSEI);
    if (result() != 2 || (ST0 & 0xE0) == 0x60)
        reset = 1;
    else
        recalibrate = 1;
}
