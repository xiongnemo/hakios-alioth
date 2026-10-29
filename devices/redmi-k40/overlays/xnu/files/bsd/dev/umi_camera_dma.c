/* SM8250 IFE-lite48x diagnostic capture. Board-local, never a default boot path.
 * A 320x240 CSID480 test pattern validates bus-v3 and the UMI IFE SIDs.
 * Sensor streaming and high-rate clocks remain outside this diagnostic.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern uint64_t loader_recording[3];
extern lck_mtx_t hakios_smmu_lock;
extern void hakios_tl(const char *, ...) __printflike(1, 2);
int umi_camera_tpg(vm_offset_t, vm_offset_t);
void umi_camera_dma_quiesce(bool);

#define UD_IOVA 0x40000000ull
#define UD_SID 0x800u
#define UD_MASK 0x440u /* DT data writers 800/840, aliases c00/c40; no odd streams */
#define UD_FSR_FAULT 0xc00001feu
#define UD_WIDTH 320u
#define UD_HEIGHT 240u
#define UD_STRIDE 400u
#define UD_BYTES (UD_STRIDE*UD_HEIGHT)
static struct {
	vm_offset_t ife, csid, smmu, gr1, cb, memory, tables;
	vm_size_t size;
	unsigned smr, context, installed;
	unsigned slots[2];
	uint32_t old_smrs[2], old_s2crs[2];
	uint32_t old_smr, old_s2cr, old_cbar, old_cba2r, old_cb[9];
	bool mapped, running, context_changed;
} ud;
static const unsigned ud_cbregs[9] = {0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c};
struct ud_header {
	char magic[8];
	uint32_t width,height,stride,format,bayer,frames,frame_bytes,source;
	char sensor[16];
	uint32_t last;
};
static uint32_t
dr(vm_offset_t p, unsigned o)
{
	uint32_t v=*(volatile uint32_t *)(p+o);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}
static void
dw(vm_offset_t p,unsigned o,uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(p+o)=v;
	__asm__ volatile("dsb osh" ::: "memory");
}
static void
dcache(vm_offset_t p,vm_size_t size)
{
	for(vm_offset_t end=p+size;p<end;p+=64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}
static bool
dsync(void)
{
	dw(ud.cb,0x610,ud.context); dw(ud.cb,0x7f0,1);
	for(unsigned us=0;us<1000;us++) {
		if(!(dr(ud.cb,0x7f4)&1))return true;
		delay(1);
	}
	return false;
}

static int
dmap(void)
{
	/* Only a free match and an unused, disabled, fault-free context qualify.
	 * The same mutex serializes the board's other SMMU clients.
	 */
	if(!ud.smmu)ud.smmu=ml_io_map(0x15000000,0x1000);
	if(!ud.smmu)return ENOMEM;
	uint32_t scr0=dr(ud.smmu,0),id0=dr(ud.smmu,0x20),id1=dr(ud.smmu,0x24);
	unsigned page=(id1>>31)?0x10000:0x1000, pages=2u<<((id1>>28)&7);
	unsigned matches=id0&255, contexts=id1&255;
	hakios_tl("UMI camera DMA: SMMU IDR0=%#x IDR1=%#x matches=%u contexts=%u\n",id0,id1,matches,contexts);
	if(!(id0&(1u<<27))||(scr0&(1u<<3))||!matches||matches>128||!contexts||contexts>128||contexts>pages||2ull*page*pages!=0x100000)return ENODEV;
	if(!ud.gr1)ud.gr1=ml_io_map(0x15000000+page,0x1000);
	if(!ud.gr1)return ENOMEM;
	bool used[128]={false};
	unsigned free_match=~0u, available=0;
	for(unsigned i=0;i<matches;i++) {
		uint32_t smr=dr(ud.smmu,0x800+4*i),s2cr=dr(ud.smmu,0xc00+4*i);
		if(smr&(1u<<31)) {
			if((s2cr&255)<128)used[s2cr&255]=true;
			if(!(((smr&0x7fff)^UD_SID)&~(((smr>>16)&0x7fff)|UD_MASK)))return EBUSY;
		} else if(available<2) {ud.slots[available++]=i;if(free_match==~0u)free_match=i;}
	}
	if(available<2)return EBUSY;
	unsigned cb;
	for(cb=0;cb<contexts;cb++) {
		if(used[cb])continue;
		vm_offset_t mapped=ml_io_map(0x15000000+(uint64_t)(pages+cb)*page,0x1000);
		if(!mapped)return ENOMEM;
		if(!(dr(mapped,0)&1)&&!(dr(mapped,0x58)&UD_FSR_FAULT)) {ud.cb=mapped;break;}
	}
	if(cb==contexts)return EBUSY;
	ud.context=cb;ud.smr=free_match;
	ud.old_smr=dr(ud.smmu,0x800+4*free_match);
	ud.old_s2cr=dr(ud.smmu,0xc00+4*free_match);
	for(unsigned i=0;i<2;i++) {
		ud.old_smrs[i]=dr(ud.smmu,0x800+4*ud.slots[i]);
		ud.old_s2crs[i]=dr(ud.smmu,0xc00+4*ud.slots[i]);
	}
	ud.old_cbar=dr(ud.gr1,4*cb);ud.old_cba2r=dr(ud.gr1,0x800+4*cb);
	for(unsigned i=0;i<9;i++)ud.old_cb[i]=dr(ud.cb,ud_cbregs[i]);
	hakios_tl("UMI camera DMA: selected CB%u at %#lx SMR%u old CBAR=%#x CBA2R=%#x SCTLR=%#x FSR=%#x\n",cb,(unsigned long)(0x15000000+(uint64_t)(pages+cb)*page),free_match,ud.old_cbar,ud.old_cba2r,ud.old_cb[0],dr(ud.cb,0x58));
	uint64_t *l1=(uint64_t *)ud.tables,*l2=(uint64_t *)(ud.tables+4096),*l3=(uint64_t *)(ud.tables+8192);
	l1[(UD_IOVA>>30)&511]=kvtophys((vm_offset_t)l2)|3;
	l2[(UD_IOVA>>21)&511]=kvtophys((vm_offset_t)l3)|3;
	for(unsigned p=0;p<ud.size/4096;p++)
		l3[p]=kvtophys(ud.memory+4096ull*p)|3ull|(1ull<<6)|(2ull<<8)|(1ull<<10)|(1ull<<11);
	dcache(ud.tables,16384);
	ud.context_changed=true;
	dw(ud.cb,0,0);dw(ud.gr1,0x800+4*cb,1);dw(ud.gr1,4*cb,1u<<16|3u<<8|15u<<12);
	dw(ud.cb,0x10,1u|1u<<4|7u<<15);dw(ud.cb,0x30,25u|1u<<8|1u<<10|3u<<12|1u<<23);
	uint64_t ttbr=kvtophys(ud.tables)|(uint64_t)cb<<48;
	dw(ud.cb,0x20,(uint32_t)ttbr);dw(ud.cb,0x24,(uint32_t)(ttbr>>32));
	dw(ud.cb,0x28,0);dw(ud.cb,0x2c,0);dw(ud.cb,0x38,0xf404ff44);dw(ud.cb,0x3c,0);
	/* Prove the translation configuration before enabling the context or
	 * exposing a DMA stream. A stale TTBR must never become our mapping.
	 */
	if((dr(ud.gr1,4*cb)&0x30000)!=(1u<<16)||!(dr(ud.gr1,0x800+4*cb)&1)||
	    (dr(ud.cb,0x10)&0x38017)!=(1u|1u<<4|7u<<15)||
	    dr(ud.cb,0x30)!=(25u|1u<<8|1u<<10|3u<<12|1u<<23)||
	    dr(ud.cb,0x20)!=(uint32_t)ttbr||dr(ud.cb,0x24)!=(uint32_t)(ttbr>>32)||
	    dr(ud.cb,0x28)||dr(ud.cb,0x2c)||dr(ud.cb,0x38)!=0xf404ff44||dr(ud.cb,0x3c)) {
		hakios_tl("UMI camera DMA: context translation registers did not read back; refusing DMA\n");
		return EIO;
	}
	if(!dsync())return ETIMEDOUT;
	dw(ud.cb,0,1u|1u<<1|1u<<2|1u<<5|1u<<8|1u<<12);
	hakios_tl("UMI camera DMA: CB%u enable readback SCTLR=%#x TCR=%#x TCR2=%#x TTBR=%#x:%08x FSR=%#x\n",cb,dr(ud.cb,0),dr(ud.cb,0x30),dr(ud.cb,0x10),dr(ud.cb,0x24),dr(ud.cb,0x20),dr(ud.cb,0x58));
	if(!(dr(ud.cb,0)&1))return EIO;
	/* Use only DT data writers 800/840 (and their bit10 aliases).
	 * The CPU configures this capture directly; it does not use the odd
	 * command streams. UMI accepted 800 but rejected the 801 SMR write.
	 */
	static const unsigned sids[2]={0x800,0x840};
	ud.mapped=true;
	for(unsigned i=0;i<2;i++) {
		unsigned slot=ud.slots[i];
		uint32_t wanted=1u<<31|0x400u<<16|sids[i], route=cb|(3u<<8);
		ud.installed=i+1; /* rollback includes a partially written slot */
		dw(ud.smmu,0xc00+4*slot,route); /* v500 non-shareable output */
		dw(ud.smmu,0x800+4*slot,wanted);
		uint32_t smr_back=dr(ud.smmu,0x800+4*slot),s2cr_back=dr(ud.smmu,0xc00+4*slot);
		hakios_tl("UMI camera DMA: route SMR%u requested %#x read %#x; S2CR requested %#x read %#x\n",slot,wanted,smr_back,route,s2cr_back);
		if(smr_back!=wanted||(s2cr_back&0x300ff)!=cb)return EIO;
	}
	hakios_tl("UMI camera DMA: context %u match %u, SID %#x mask %#x, %lu bytes\n",cb,free_match,UD_SID,UD_MASK,(unsigned long)ud.size);
	return 0;
}

void
umi_camera_dma_quiesce(bool panic)
{
	if(ud.running) {
		dw(ud.csid,0x600,0); /* TPG first, then the receiver and writer. */
		dw(ud.csid,0x208,2);dw(ud.csid,0x200,dr(ud.csid,0x200)&~(1u<<31));
		dw(ud.ife,0x1c00,0);dw(ud.ife,0x20,1);delay(1000);
		dw(ud.ife,0x1a18,0);dw(ud.ife,0x1a20,1u<<4);dw(ud.ife,0x1a30,1);
		dw(ud.csid,0x100,0);ud.running=false;
	}
	if(!ud.context_changed)return;
	/* Panic has stopped the other CPUs; never block on their mutex. Only
	 * this client's match/context are changed, not global SMMU controls.
	 */
	if(!panic)lck_mtx_lock(&hakios_smmu_lock);
	uint32_t fault=dr(ud.cb,0x58)&UD_FSR_FAULT;
	if(fault)hakios_tl("UMI camera DMA: context fault %#x FAR=%#x:%08x syndrome=%#x\n",fault,dr(ud.cb,0x64),dr(ud.cb,0x60),dr(ud.cb,0x68));
	if(ud.mapped) {
		for(unsigned n=ud.installed;n;n--) {
			unsigned i=n-1;
			dw(ud.smmu,0x800+4*ud.slots[i],ud.old_smrs[i]);
			dw(ud.smmu,0xc00+4*ud.slots[i],ud.old_s2crs[i]);
		}
		ud.installed=0;ud.mapped=false;
	}
	dw(ud.cb,0,0);
	if(!dsync())hakios_tl("UMI camera DMA: cleanup TLB synchronization timed out\n");
	if(fault)dw(ud.cb,0x58,fault);
	for(unsigned i=1;i<9;i++)dw(ud.cb,ud_cbregs[i],ud.old_cb[i]);
	dw(ud.gr1,4*ud.context,ud.old_cbar);dw(ud.gr1,0x800+4*ud.context,ud.old_cba2r);
	dw(ud.cb,0,ud.old_cb[0]);ud.context_changed=false;
	if(!panic)lck_mtx_unlock(&hakios_smmu_lock);
}

int
umi_camera_tpg(vm_offset_t ife,vm_offset_t csid)
{
	if(ud.memory)return EBUSY; /* one retained capture per diagnostic boot */
	ud.ife=ife;ud.csid=csid;ud.size=round_page(4096+UD_BYTES);
	if(kmem_alloc_contig(kernel_map,&ud.memory,ud.size,PAGE_MASK,0,0,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
	if(kmem_alloc(kernel_map,&ud.tables,16384,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
	struct ud_header *h=(struct ud_header *)ud.memory;
	memcpy(h->magic,"RWCAM001",8);h->width=UD_WIDTH;h->height=UD_HEIGHT;h->stride=UD_STRIDE;
	h->format=1;h->bayer=0;h->frame_bytes=UD_BYTES;h->source=0;strlcpy(h->sensor,"UMI CSID TPG",sizeof(h->sensor));
	memset((void *)(ud.memory+4096),0xa5,UD_BYTES);dcache(ud.memory,ud.size);
	lck_mtx_lock(&hakios_smmu_lock);int error=dmap();lck_mtx_unlock(&hakios_smmu_lock);
	if(error){umi_camera_dma_quiesce(false);return error;}
	/* CSID480 RAW10 RDI0, internal test generator; no PHY is selected. */
	ud.running=true;
	dw(csid,0x100,3u|0x3210u<<4);dw(csid,0x104,1u<<6|1u);
	dw(csid,0x200,0x2bu<<16|15u<<12|1u<<2|3u);dw(csid,0x204,2);
	static const unsigned rdi_regs[]={0x20c,0x210,0x214,0x218,0x224,0x228,0x22c,0x230};
	for(unsigned i=0;i<8;i++)dw(csid,rdi_regs[i],i&1?1:0);
	dw(csid,0x200,1u<<31|0x2bu<<16|15u<<12|1u<<2|3u);dw(csid,0x208,1);
	/* VFE bus-v3 frame mode, RDI0 at 1c00, completion group bit4. */
	dw(ife,0x1a08,0x3ffffff);dw(ife,0x1a34,0);dw(ife,0x1adc,0);
	dw(ife,0x1c64,15);dw(ife,0x1c0c,0xffff);dw(ife,0x1c10,0);dw(ife,0x1c14,UD_STRIDE);dw(ife,0x1c18,0);
	dw(ife,0x1c08,0);dw(ife,0x1c30,0);dw(ife,0x1c34,1);dw(ife,0x1c38,0);dw(ife,0x1c3c,1);
	dw(ife,0x1a20,1u<<4);dw(ife,0x1a30,1);dw(ife,0x1a18,1u<<4);
	dw(ife,0x1c04,(uint32_t)(UD_IOVA+4096));dw(ife,0x1c00,0x10001);dw(ife,0x20,1);
	dw(csid,0x604,1u<<10);dw(csid,0x608,0x3ffu<<12|0x740);
	dw(csid,0x60c,0x12345678);dw(csid,0x610,UD_WIDTH<<16|UD_HEIGHT);
	dw(csid,0x614,0x2b);dw(csid,0x618,2u<<16|8);dw(csid,0x640,1u<<5);dw(csid,0x648,0);
	dw(csid,0x600,4u<<20|0x80u<<8|3u<<4|7u);
	bool done=false;
	for(unsigned ms=0;ms<2000;ms+=10){
		delay_for_interval(10,NSEC_PER_MSEC);
		if(dr(ife,0x1a28)&(1u<<4)){done=true;break;}
		if(dr(ud.cb,0x58)&UD_FSR_FAULT)break;
	}
	hakios_tl("UMI camera DMA: TPG done=%u IFE IRQ=%#x/%#x bus=%#x/%#x CSID RDI=%#x status=%#x size=%#x/%#x\n",done,dr(ife,0x40),dr(ife,0x44),dr(ife,0x1a28),dr(ife,0x1a2c),dr(csid,0x30),dr(csid,0x250),dr(ife,0x1a68),dr(ife,0x1a70));
	bool faulted=(dr(ud.cb,0x58)&UD_FSR_FAULT)!=0;
	hakios_tl("UMI camera DMA: SMMU global fault=%#x stream=%#x\n",dr(ud.smmu,0x48),dr(ud.smmu,0x54));
	umi_camera_dma_quiesce(false);dcache(ud.memory,ud.size);
	const uint8_t *frame=(const uint8_t *)(ud.memory+4096);
	bool changed=false,varied=false;
	for(unsigned i=0;i<UD_BYTES;i++){changed|=frame[i]!=0xa5;varied|=frame[i]!=frame[0];}
	bool guards=!memcmp(h->magic,"RWCAM001",8)&&h->width==UD_WIDTH&&h->height==UD_HEIGHT&&h->stride==UD_STRIDE;
	for(unsigned i=4096+UD_BYTES;i<ud.size;i++)guards&=((const uint8_t *)ud.memory)[i]==0;
	h->frames=done&&changed&&varied&&guards&&!faulted?1:0;
	hakios_tl("UMI camera DMA: buffer guards=%u context fault=%u\n",guards,faulted);
	hakios_tl("UMI camera DMA: frame changed=%u varied=%u frames=%u first=%02x %02x %02x %02x\n",changed,varied,h->frames,frame[0],frame[1],frame[2],frame[3]);
	dcache(ud.memory,4096);
	loader_recording[0]=kvtophys(ud.memory);loader_recording[1]=ud.size;loader_recording[2]=ud.memory;
	return h->frames?0:EIO;
}
