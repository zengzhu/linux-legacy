/* Copyright (c) 2002,2007-2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/delay.h>
#include <linux/sched.h>

#include "gsl.h"
#include "gsl_hal.h"
#include "gsl_cmdstream.h"

#ifdef CONFIG_ARCH_MX35
#define V3_SYNC
#endif

#ifdef GSL_BLD_G12
#define GSL_IRQ_TIMEOUT         200


//----------------------------------------------------------------------------

#define GSL_HAL_NUMCMDBUFFERS           5
#define GSL_HAL_CMDBUFFERSIZE           (1024 + 13) * sizeof(unsigned int)

#define     ALIGN_IN_BYTES( dim, alignment ) ( ( (dim) + (alignment-1) ) & ~(alignment-1) )


#ifdef _Z180
#define     NUMTEXUNITS                         4
#define     TEXUNITREGCOUNT                     25
#define     VG_REGCOUNT                         0x39
#define     GSL_HAL_EDGE0BUFSIZE                0x3E8+64
#define     GSL_HAL_EDGE1BUFSIZE                0x8000+64
#define     GSL_HAL_EDGE2BUFSIZE                0x80020+64
#define     GSL_HAL_EDGE0REG                    ADDR_VGV1_CBUF
#define     GSL_HAL_EDGE1REG                    ADDR_VGV1_BBUF
#define     GSL_HAL_EDGE2REG                    ADDR_VGV1_EBUF
#else
#define     NUMTEXUNITS                          2
#define     TEXUNITREGCOUNT                      24
#define     VG_REGCOUNT                          0x3A
#define     L1TILESIZE                           64
#define     GSL_HAL_EDGE0BUFSIZE                 L1TILESIZE*L1TILESIZE*4+64
#define     GSL_HAL_EDGE1BUFSIZE                 L1TILESIZE*L1TILESIZE*16+64
#define     GSL_HAL_EDGE0REG                     ADDR_VGV1_CBASE1
#define     GSL_HAL_EDGE1REG                     ADDR_VGV1_UBASE2
#endif

#define     PACKETSIZE_BEGIN        3
#define     PACKETSIZE_G2DCOLOR     2
#define     PACKETSIZE_TEXUNIT      (TEXUNITREGCOUNT*2)
#define     PACKETSIZE_REG          (VG_REGCOUNT*2)
#define     PACKETSIZE_STATE        (PACKETSIZE_TEXUNIT*NUMTEXUNITS + PACKETSIZE_REG + PACKETSIZE_BEGIN + PACKETSIZE_G2DCOLOR)
#define     PACKETSIZE_STATESTREAM   ALIGN_IN_BYTES((PACKETSIZE_STATE*sizeof(unsigned int)), 32) / sizeof(unsigned int) 

//----------------------------------------------------------------------------

typedef struct
{
    unsigned int id;
   // unsigned int regs[];
}gsl_hal_z1xxdrawctx_t;

typedef struct
{
  unsigned int      offs;
  unsigned int      curr;
  unsigned int      prevctx;

  gsl_memdesc_t     e0;
  gsl_memdesc_t     e1;
  gsl_memdesc_t     e2;
  unsigned int*     cmdbuf[GSL_HAL_NUMCMDBUFFERS];
  gsl_memdesc_t     cmdbufdesc[GSL_HAL_NUMCMDBUFFERS];
  gsl_timestamp_t   timestamp[GSL_HAL_NUMCMDBUFFERS];

  unsigned int      numcontext;
  unsigned int      nextUniqueContextID;
}gsl_z1xx_t;

static gsl_z1xx_t   g_z1xx      = {0}; 

extern int z160_version;

//----------------------------------------------------------------------------


//////////////////////////////////////////////////////////////////////////////
// functions
//////////////////////////////////////////////////////////////////////////////

static int kgsl_g12_issueibcmds(gsl_device_t* device, int drawctxt_index, gpuaddr_t ibaddr, int sizedwords, gsl_timestamp_t *timestamp, unsigned int flags);
static int kgsl_g12_context_create(gsl_device_t* device, gsl_context_type_t type, unsigned int *drawctxt_id, gsl_flags_t flags);
static int kgsl_g12_context_destroy(gsl_device_t* device, unsigned int drawctxt_id);
static unsigned int drawctx_id  = 0;
static int kgsl_g12_idle(gsl_device_t *device, unsigned int timeout);

//----------------------------------------------------------------------------

void
kgsl_g12_intrcallback(gsl_intrid_t id, void *cookie)
{
    gsl_device_t  *device = (gsl_device_t *) cookie;

    switch(id)
    {
        // non-error condition interrupt
        case GSL_INTR_G12_G2D:
			queue_work(device->irq_workq, &(device->irq_work));
			break;
#ifndef _Z180
        case GSL_INTR_G12_FBC:
            // signal intr completion event
            complete_all(&device->intr.evnt[id]);
            break;
#endif //_Z180

        // error condition interrupt
        case GSL_INTR_G12_FIFO:
		printk(KERN_ERR "GPU: Z160 FIFO Error\n");
		schedule_work(&device->irq_err_work);
		break;

        case GSL_INTR_G12_MH:
            // don't do anything. this is handled by the MMU manager
            break;

        default:
            break;
    }
}

//----------------------------------------------------------------------------

int
kgsl_g12_isr(gsl_device_t *device)
{
    unsigned int           status;
#ifdef _DEBUG
    REG_MH_MMU_PAGE_FAULT  page_fault = {0};
    REG_MH_AXI_ERROR       axi_error  = {0};
#endif // DEBUG

    // determine if G12 is interrupting
    device->ftbl.device_regread(device, (ADDR_VGC_IRQSTATUS >> 2), &status);

    if (status)
    {
        // if G12 MH is interrupting, clear MH block interrupt first, then master G12 MH interrupt
        if (status & (1 << VGC_IRQSTATUS_MH_FSHIFT))
        {
#ifdef _DEBUG
            // obtain mh error information
            device->ftbl.device_regread(device, ADDR_MH_MMU_PAGE_FAULT, (unsigned int *)&page_fault);
            device->ftbl.device_regread(device, ADDR_MH_AXI_ERROR, (unsigned int *)&axi_error);
#endif // DEBUG

            kgsl_intr_decode(device, GSL_INTR_BLOCK_G12_MH);
        }

        kgsl_intr_decode(device, GSL_INTR_BLOCK_G12);
    }

    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

int
kgsl_g12_tlbinvalidate(gsl_device_t *device, unsigned int reg_invalidate, unsigned int pid)
{
#ifndef GSL_NO_MMU
    REG_MH_MMU_INVALIDATE  mh_mmu_invalidate = {0};

    // unreferenced formal parameter
	(void) pid;

    mh_mmu_invalidate.INVALIDATE_ALL = 1;
    mh_mmu_invalidate.INVALIDATE_TC  = 1;

    device->ftbl.device_regwrite(device, reg_invalidate, *(unsigned int *) &mh_mmu_invalidate);
#else
    (void)device;
    (void)reg_invalidate;
#endif
    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

int
kgsl_g12_setpagetable(gsl_device_t *device, unsigned int reg_ptbase, gpuaddr_t ptbase, unsigned int pid)
{
	// unreferenced formal parameter
	(void) pid;
#ifndef GSL_NO_MMU
    device->ftbl.device_idle(device, GSL_TIMEOUT_DEFAULT);
	device->ftbl.device_regwrite(device, reg_ptbase, ptbase);
#else
    (void)device;
    (void)reg_ptbase;
    (void)reg_varange;
#endif
    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

static void kgsl_g12_updatetimestamp(gsl_device_t *device)
{
	unsigned int count = 0;
	device->ftbl.device_regread(device, (ADDR_VGC_IRQ_ACTIVE_CNT >> 2), &count);
	count >>= 8;
	count &= 255;
	device->timestamp += count;	
#ifdef V3_SYNC
	if (device->current_timestamp > device->timestamp)
	{
	    kgsl_cmdwindow_write0(2, GSL_CMDWINDOW_2D, ADDR_VGV3_CONTROL, 2);
	    kgsl_cmdwindow_write0(2, GSL_CMDWINDOW_2D, ADDR_VGV3_CONTROL, 0);
	}
#endif
	kgsl_sharedmem_write0(&device->memstore, GSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp), &device->timestamp, 4, 0);
}

//----------------------------------------------------------------------------

static void kgsl_g12_irqtask(struct work_struct *work)
{
	gsl_device_t *device = &gsl_driver.device[GSL_DEVICE_G12-1];
	kgsl_g12_updatetimestamp(device);
	wake_up_interruptible_all(&device->timestamp_waitq);
}

static void kgsl_g12_irqerr(struct work_struct *work)
{
	gsl_device_t *device = &gsl_driver.device[GSL_DEVICE_G12-1];
	device->ftbl.device_destroy(device);
}


//----------------------------------------------------------------------------

int
kgsl_g12_init(gsl_device_t *device)
{
    int  status = GSL_FAILURE; 

    device->flags |= GSL_FLAGS_INITIALIZED;

    kgsl_hal_setpowerstate(device->id, GSL_PWRFLAGS_POWER_ON, 100);

    // setup MH arbiter - MH offsets are considered to be dword based, therefore no down shift
    device->ftbl.device_regwrite(device, ADDR_MH_ARBITER_CONFIG, *(unsigned int *) &gsl_cfg_g12_mharb);

    // init interrupt
    status = kgsl_intr_init(device);
    if (status != GSL_SUCCESS)
    {
        device->ftbl.device_stop(device);
        return (status);
    }

    // enable irq
    device->ftbl.device_regwrite(device, (ADDR_VGC_IRQENABLE >> 2), 0x3);

#ifndef GSL_NO_MMU
    // enable master interrupt for G12 MH
    kgsl_intr_attach(&device->intr, GSL_INTR_G12_MH, kgsl_g12_intrcallback, (void *) device);
    kgsl_intr_enable(&device->intr, GSL_INTR_G12_MH);

    // init mmu
    status = kgsl_mmu_init(device);
    if (status != GSL_SUCCESS)
    {
        device->ftbl.device_stop(device);
        return (status);
    }
#endif

#ifdef IRQTHREAD_POLL
    // Create event to trigger IRQ polling thread
    init_completion(&device->irqthread_event);
#endif

    // enable interrupts
    kgsl_intr_attach(&device->intr, GSL_INTR_G12_G2D,  kgsl_g12_intrcallback, (void *) device);
    kgsl_intr_attach(&device->intr, GSL_INTR_G12_FIFO, kgsl_g12_intrcallback, (void *) device);
    kgsl_intr_enable(&device->intr, GSL_INTR_G12_G2D);
    kgsl_intr_enable(&device->intr, GSL_INTR_G12_FIFO);

#ifndef _Z180
    kgsl_intr_attach(&device->intr, GSL_INTR_G12_FBC,  kgsl_g12_intrcallback, (void *) device);
  //kgsl_intr_enable(&device->intr, GSL_INTR_G12_FBC);
#endif //_Z180

    // create thread for IRQ handling
    device->irq_workq = create_singlethread_workqueue("z160_workqueue");
    INIT_WORK(&device->irq_work, kgsl_g12_irqtask);
    INIT_WORK(&device->irq_err_work, kgsl_g12_irqerr);

    return (status);
}

//----------------------------------------------------------------------------

int
kgsl_g12_close(gsl_device_t *device)
{
    int status = GSL_FAILURE; 

    if (device->refcnt == 0)
    {
        // wait pending interrupts before shutting down G12 intr thread to
        // empty irq counters. Otherwise there's a possibility to have them in
        // registers next time systems starts up and this results in a hang.
        status = device->ftbl.device_idle(device, 1000);
        DEBUG_ASSERT(status == GSL_SUCCESS);

	destroy_workqueue(device->irq_workq);

        // shutdown command window
        kgsl_cmdwindow_close(device);

#ifndef GSL_NO_MMU
        // shutdown mmu
        kgsl_mmu_close(device);
#endif
        // disable interrupts
        kgsl_intr_detach(&device->intr, GSL_INTR_G12_MH);
        kgsl_intr_detach(&device->intr, GSL_INTR_G12_G2D);
        kgsl_intr_detach(&device->intr, GSL_INTR_G12_FIFO);
#ifndef _Z180
        kgsl_intr_detach(&device->intr, GSL_INTR_G12_FBC);
#endif //_Z180

        // shutdown interrupt
        kgsl_intr_close(device);

        kgsl_hal_setpowerstate(device->id, GSL_PWRFLAGS_POWER_OFF, 0);

        device->flags &= ~GSL_FLAGS_INITIALIZED;

        drawctx_id = 0;

        DEBUG_ASSERT(g_z1xx.numcontext == 0);

	memset(&g_z1xx, 0, sizeof(gsl_z1xx_t));
    }

    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

int
kgsl_g12_destroy(gsl_device_t *device)
{
    int           i;
    unsigned int  pid;

#ifdef _DEBUG
    // for now, signal catastrophic failure in a brute force way
    DEBUG_ASSERT(0);
#endif // _DEBUG

    //todo: hard reset core?

    for (i = 0; i < GSL_CALLER_PROCESS_MAX; i++)
    {
        pid = device->callerprocess[i];
        if (pid)
        {
            device->ftbl.device_stop(device);
            kgsl_driver_destroy(pid);

            // todo: terminate client process?
        }
    }

    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

int
kgsl_g12_start(gsl_device_t *device, gsl_flags_t flags)
{
    int  status = GSL_SUCCESS;

    (void) flags;       // unreferenced formal parameter

    kgsl_hal_setpowerstate(device->id, GSL_PWRFLAGS_CLK_ON, 100);

    // init command window
    status = kgsl_cmdwindow_init(device);
    if (status != GSL_SUCCESS)
    {
        device->ftbl.device_stop(device);
        return (status);
    }

    DEBUG_ASSERT(g_z1xx.numcontext == 0);

    device->flags |= GSL_FLAGS_STARTED;

    return (status);
}

//----------------------------------------------------------------------------

int
kgsl_g12_stop(gsl_device_t *device)
{
    int status;

    DEBUG_ASSERT(device->refcnt == 0);

    /* wait for device to idle before setting it's clock off */
    status = device->ftbl.device_idle(device, 1000);
    DEBUG_ASSERT(status == GSL_SUCCESS);

    status = kgsl_hal_setpowerstate(device->id, GSL_PWRFLAGS_CLK_OFF, 0);
    device->flags &= ~GSL_FLAGS_STARTED;

    return (status);
}

//----------------------------------------------------------------------------

int
kgsl_g12_getproperty(gsl_device_t *device, gsl_property_type_t type, void *value, unsigned int sizebytes)
{
    int  status = GSL_FAILURE;
    // unreferenced formal parameter
    (void) sizebytes;

    if (type == GSL_PROP_DEVICE_INFO)
    {
        gsl_devinfo_t  *devinfo = (gsl_devinfo_t *) value;

        DEBUG_ASSERT(sizebytes == sizeof(gsl_devinfo_t));

        devinfo->device_id   = device->id;
        devinfo->chip_id     = (gsl_chipid_t)device->chip_id;
#ifndef GSL_NO_MMU
        devinfo->mmu_enabled = kgsl_mmu_isenabled(&device->mmu);
#endif
	if (z160_version == 1)
	    devinfo->high_precision = 1;
	else
	    devinfo->high_precision = 0;

        status = GSL_SUCCESS;
    }

    return (status);
}

//----------------------------------------------------------------------------

int
kgsl_g12_setproperty(gsl_device_t *device, gsl_property_type_t type, void *value, unsigned int sizebytes)
{
    int  status = GSL_FAILURE;

    // unreferenced formal parameters
    (void) device;

    if (type == GSL_PROP_DEVICE_POWER)
    {
        gsl_powerprop_t  *power = (gsl_powerprop_t *) value;

        DEBUG_ASSERT(sizebytes == sizeof(gsl_powerprop_t));

        if (!(device->flags & GSL_FLAGS_SAFEMODE))
        {
            kgsl_hal_setpowerstate(device->id, power->flags, power->value);
        }

        status = GSL_SUCCESS;
    }
    return (status);
}

//----------------------------------------------------------------------------

int
kgsl_g12_idle(gsl_device_t *device, unsigned int timeout)
{
	if ( device->flags & GSL_FLAGS_STARTED )
	{
		for ( ; ; )
		{
			gsl_timestamp_t retired = kgsl_cmdstream_readtimestamp0( device->id, GSL_TIMESTAMP_RETIRED );
			gsl_timestamp_t ts_diff = retired - device->current_timestamp;
			if ( ts_diff >= 0 || ts_diff < -GSL_TIMESTAMP_EPSILON )
				break;
			msleep(10);
		}
	}

    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

int
kgsl_g12_regread(gsl_device_t *device, unsigned int offsetwords, unsigned int *value)
{
    // G12 MH register values can only be retrieved via dedicated read registers
    if ((offsetwords >= ADDR_MH_ARBITER_CONFIG && offsetwords <= ADDR_MH_AXI_HALT_CONTROL) ||
        (offsetwords >= ADDR_MH_MMU_CONFIG     && offsetwords <= ADDR_MH_MMU_MPU_END))
    {
#ifdef _Z180
        device->ftbl.device_regwrite(device, (ADDR_VGC_MH_READ_ADDR >> 2), offsetwords);
        GSL_HAL_REG_READ(device->id, (unsigned int) device->regspace.mmio_virt_base, (ADDR_VGC_MH_READ_ADDR >> 2), value);
#else
        device->ftbl.device_regwrite(device, (ADDR_MMU_READ_ADDR >> 2), offsetwords);
        GSL_HAL_REG_READ(device->id, (unsigned int) device->regspace.mmio_virt_base, (ADDR_MMU_READ_DATA >> 2), value);
#endif
    }
    else
    {
        GSL_HAL_REG_READ(device->id, (unsigned int) device->regspace.mmio_virt_base, offsetwords, value);
    }

    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

int
kgsl_g12_regwrite(gsl_device_t *device, unsigned int offsetwords, unsigned int value)
{
    // G12 MH registers can only be written via the command window
    if ((offsetwords >= ADDR_MH_ARBITER_CONFIG && offsetwords <= ADDR_MH_AXI_HALT_CONTROL) ||
        (offsetwords >= ADDR_MH_MMU_CONFIG     && offsetwords <= ADDR_MH_MMU_MPU_END))
    {
        kgsl_cmdwindow_write0(device->id, GSL_CMDWINDOW_MMU, offsetwords, value);
    }
    else
    {
        GSL_HAL_REG_WRITE(device->id, (unsigned int) device->regspace.mmio_virt_base, offsetwords, value);
    }

    // idle device when running in safe mode
    if (device->flags & GSL_FLAGS_SAFEMODE)
    {
        device->ftbl.device_idle(device, GSL_TIMEOUT_DEFAULT);
    }

    return (GSL_SUCCESS);
}

int
kgsl_g12_waittimestamp(gsl_device_t *device, gsl_timestamp_t timestamp, unsigned int timeout)
{
	int status = wait_event_interruptible_timeout(device->timestamp_waitq,
	                                              kgsl_cmdstream_check_timestamp(device->id, timestamp),
												  msecs_to_jiffies(timeout));
	if (status > 0)
		return GSL_SUCCESS;
	else
		return GSL_FAILURE;
}

int
kgsl_g12_getfunctable(gsl_functable_t *ftbl)
{
    ftbl->device_init           = kgsl_g12_init;
    ftbl->device_close          = kgsl_g12_close;
    ftbl->device_destroy        = kgsl_g12_destroy;
    ftbl->device_start          = kgsl_g12_start;
    ftbl->device_stop           = kgsl_g12_stop;
    ftbl->device_getproperty    = kgsl_g12_getproperty;
    ftbl->device_setproperty    = kgsl_g12_setproperty;
    ftbl->device_idle           = kgsl_g12_idle;
    ftbl->device_regread        = kgsl_g12_regread;
    ftbl->device_regwrite       = kgsl_g12_regwrite;
	ftbl->device_waittimestamp  = kgsl_g12_waittimestamp;
    ftbl->device_runpending     = NULL;
    ftbl->intr_isr              = kgsl_g12_isr;
    ftbl->mmu_tlbinvalidate     = kgsl_g12_tlbinvalidate;
    ftbl->mmu_setpagetable      = kgsl_g12_setpagetable;
    ftbl->cmdstream_issueibcmds = kgsl_g12_issueibcmds;
    ftbl->context_create        = kgsl_g12_context_create;
    ftbl->context_destroy       = kgsl_g12_context_destroy;

    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

static void addmarker(gsl_z1xx_t* z1xx)
{
    DEBUG_ASSERT(z1xx);
    {
        unsigned int *p = z1xx->cmdbuf[z1xx->curr];
        /* todo: use symbolic values */
        p[z1xx->offs++] = 0x7C000176;
        p[z1xx->offs++] = (0x8000|5);
        p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
        p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
        p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
        p[z1xx->offs++] = 0x7C000176;
        p[z1xx->offs++] = 5;
        p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
        p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
        p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
    }
}

//----------------------------------------------------------------------------
static void beginpacket(gsl_z1xx_t* z1xx, gpuaddr_t cmd, unsigned int nextcnt)
{
    unsigned int *p = z1xx->cmdbuf[z1xx->curr];

    p[z1xx->offs++] = 0x7C000176;
    p[z1xx->offs++] = 5;
    p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
    p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
    p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
    p[z1xx->offs++] = 0x7C000275;
    p[z1xx->offs++] = cmd;
    p[z1xx->offs++] = 0x1000|nextcnt;  // nextcount
    p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
    p[z1xx->offs++] = ADDR_VGV3_LAST<<24;
}

//----------------------------------------------------------------------------

static int
kgsl_g12_issueibcmds(gsl_device_t* device, int drawctxt_index, gpuaddr_t ibaddr, int sizedwords, gsl_timestamp_t *timestamp, unsigned int flags)
{
    unsigned int ofs      = PACKETSIZE_STATESTREAM*sizeof(unsigned int);
    unsigned int cnt      = 5;
    unsigned int cmd      = ibaddr;
    unsigned int nextbuf  = (g_z1xx.curr+1)%GSL_HAL_NUMCMDBUFFERS;
    unsigned int nextaddr = g_z1xx.cmdbufdesc[nextbuf].gpuaddr;
    unsigned int nextcnt  = 0x9000|5;
    gsl_memdesc_t tmp     = {0};
    gsl_timestamp_t processed_timestamp;

    (void) flags;

    // read what is the latest timestamp device have processed
    GSL_CMDSTREAM_GET_EOP_TIMESTAMP(device, (int *)&processed_timestamp);

	/* wait for the next buffer's timestamp to occur */
    while(processed_timestamp < g_z1xx.timestamp[nextbuf])
    {
		kgsl_cmdstream_waittimestamp(device->id, g_z1xx.timestamp[nextbuf], 1000);
		GSL_CMDSTREAM_GET_EOP_TIMESTAMP(device, (int *)&processed_timestamp);
    }

    *timestamp = g_z1xx.timestamp[nextbuf] = device->current_timestamp + 1;

    /* context switch */
    if (drawctxt_index != (int)g_z1xx.prevctx)
    {
        cnt = PACKETSIZE_STATESTREAM;
        ofs = 0;
    }
    g_z1xx.prevctx = drawctxt_index;

    g_z1xx.offs = 10;
    beginpacket(&g_z1xx, cmd+ofs, cnt);

    tmp.gpuaddr=ibaddr+(sizedwords*sizeof(unsigned int));
    kgsl_sharedmem_write0(&tmp, 4, &nextaddr, 4, false);
    kgsl_sharedmem_write0(&tmp, 8, &nextcnt,  4, false);

    /* sync mem */
    kgsl_sharedmem_write0((const gsl_memdesc_t *)&g_z1xx.cmdbufdesc[g_z1xx.curr], 0, g_z1xx.cmdbuf[g_z1xx.curr], (512 + 13) * sizeof(unsigned int), false);
    mb();

    g_z1xx.offs = 0;
    g_z1xx.curr = nextbuf;

    /* increment mark counter */
#ifdef V3_SYNC
    if (device->timestamp == device->current_timestamp)
    {
		kgsl_cmdwindow_write0(2, GSL_CMDWINDOW_2D, ADDR_VGV3_CONTROL, flags);
		kgsl_cmdwindow_write0(2, GSL_CMDWINDOW_2D, ADDR_VGV3_CONTROL, 0);
    }
#else
	kgsl_cmdwindow_write0(2, GSL_CMDWINDOW_2D, ADDR_VGV3_CONTROL, flags);
	kgsl_cmdwindow_write0(2, GSL_CMDWINDOW_2D, ADDR_VGV3_CONTROL, 0);
#endif

    /* increment consumed timestamp */
    device->current_timestamp++;
    kgsl_sharedmem_write0(&device->memstore, GSL_DEVICE_MEMSTORE_OFFSET(soptimestamp), &device->current_timestamp, 4, 0);
    return (GSL_SUCCESS);
}

//----------------------------------------------------------------------------

static int
kgsl_g12_context_create(gsl_device_t* device, gsl_context_type_t type, unsigned int *drawctxt_id, gsl_flags_t flags)
{
    int status = 0;
    int i;
    int cmd;
	gsl_flags_t gslflags = (GSL_MEMFLAGS_CONPHYS | GSL_MEMFLAGS_ALIGNPAGE);

    // unreferenced formal parameters
    (void) device;
    (void) type;
    //(void) drawctxt_id;
    (void) flags;

    kgsl_device_active(device);

    if (g_z1xx.numcontext==0)
    {
	g_z1xx.nextUniqueContextID = 0;
         /* todo: move this to device create or start. Error checking!! */
        for (i=0;i<GSL_HAL_NUMCMDBUFFERS;i++)
        {
            status = kgsl_sharedmem_alloc0(GSL_DEVICE_ANY, gslflags, GSL_HAL_CMDBUFFERSIZE, &g_z1xx.cmdbufdesc[i]);
            DEBUG_ASSERT(status == GSL_SUCCESS);
            g_z1xx.cmdbuf[i]=kmalloc(GSL_HAL_CMDBUFFERSIZE, GFP_KERNEL);
            DEBUG_ASSERT(g_z1xx.cmdbuf[i]);
            memset((void*)g_z1xx.cmdbuf[i], 0, GSL_HAL_CMDBUFFERSIZE);

            g_z1xx.curr = i;
            g_z1xx.offs = 0;
            addmarker(&g_z1xx);
            status = kgsl_sharedmem_write0(&g_z1xx.cmdbufdesc[i],0, g_z1xx.cmdbuf[i],  (512 + 13) * sizeof(unsigned int), false);
            DEBUG_ASSERT(status == GSL_SUCCESS);
        }
        g_z1xx.curr = 0;
        cmd = (int)(((VGV3_NEXTCMD_JUMP) & VGV3_NEXTCMD_NEXTCMD_FMASK)<< VGV3_NEXTCMD_NEXTCMD_FSHIFT);

        /* set cmd stream buffer to hw */
        status |= kgsl_cmdwindow_write0(GSL_DEVICE_G12, GSL_CMDWINDOW_2D, ADDR_VGV3_MODE, 4);
        status |= kgsl_cmdwindow_write0(GSL_DEVICE_G12, GSL_CMDWINDOW_2D, ADDR_VGV3_NEXTADDR, g_z1xx.cmdbufdesc[0].gpuaddr );
        status |= kgsl_cmdwindow_write0(GSL_DEVICE_G12, GSL_CMDWINDOW_2D, ADDR_VGV3_NEXTCMD,  cmd | 5);

        DEBUG_ASSERT(status == GSL_SUCCESS);

        /* Edge buffer setup todo: move register setup to own function.
           This function can be then called, if power managemnet is used and clocks are turned off and then on.
        */
        status |= kgsl_sharedmem_alloc0(GSL_DEVICE_ANY, gslflags, GSL_HAL_EDGE0BUFSIZE, &g_z1xx.e0);
        status |= kgsl_sharedmem_alloc0(GSL_DEVICE_ANY, gslflags, GSL_HAL_EDGE1BUFSIZE, &g_z1xx.e1);
        status |= kgsl_sharedmem_set0(&g_z1xx.e0, 0, 0, GSL_HAL_EDGE0BUFSIZE);
        status |= kgsl_sharedmem_set0(&g_z1xx.e1, 0, 0, GSL_HAL_EDGE1BUFSIZE);

        status |= kgsl_cmdwindow_write0(GSL_DEVICE_G12, GSL_CMDWINDOW_2D, GSL_HAL_EDGE0REG, g_z1xx.e0.gpuaddr);
        status |= kgsl_cmdwindow_write0(GSL_DEVICE_G12, GSL_CMDWINDOW_2D, GSL_HAL_EDGE1REG, g_z1xx.e1.gpuaddr);
#ifdef _Z180
        kgsl_sharedmem_alloc0(GSL_DEVICE_ANY, gslflags, GSL_HAL_EDGE2BUFSIZE, &g_z1xx.e2);
        kgsl_sharedmem_set0(&g_z1xx.e2, 0, 0, GSL_HAL_EDGE2BUFSIZE);
        kgsl_cmdwindow_write0(GSL_DEVICE_G12, GSL_CMDWINDOW_2D, GSL_HAL_EDGE2REG, g_z1xx.e2.gpuaddr);
#endif
        DEBUG_ASSERT(status == GSL_SUCCESS);
    }

    if(g_z1xx.numcontext < GSL_CONTEXT_MAX)
    {
        g_z1xx.numcontext++;
	g_z1xx.nextUniqueContextID++;
	*drawctxt_id=g_z1xx.nextUniqueContextID;
        status = GSL_SUCCESS;
    }
    else
    {
        status = GSL_FAILURE;
    }

    return status;
}

//----------------------------------------------------------------------------

static int
kgsl_g12_context_destroy(gsl_device_t* device, unsigned int drawctxt_id)
{

    // unreferenced formal parameters
    (void) device;
    (void) drawctxt_id;

    g_z1xx.numcontext--;
    if (g_z1xx.numcontext<0)
    {
        g_z1xx.numcontext=0;
        return (GSL_FAILURE);
    }

    if (g_z1xx.numcontext==0)
    {
        int i;
        for (i=0;i<GSL_HAL_NUMCMDBUFFERS;i++)
        {
            kgsl_sharedmem_free0(&g_z1xx.cmdbufdesc[i], current->tgid);
            kfree(g_z1xx.cmdbuf[i]);
        }
        kgsl_sharedmem_free0(&g_z1xx.e0, current->tgid);
        kgsl_sharedmem_free0(&g_z1xx.e1, current->tgid);
#ifdef _Z180
        kgsl_sharedmem_free0(&g_z1xx.e2, current->tgid);
#endif
    }
    return (GSL_SUCCESS);
}

#endif /* GSL_BLD_G12 */
