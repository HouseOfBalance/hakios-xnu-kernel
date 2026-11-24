/*
 * Qualcomm UFSHCI (UFS 2.2 Host Controller, 0x04804000 on SM6225) read-only block device:
 * the system image's partition ("xnusys"), read when the kernel asks for it rather than held
 * in DRAM. Boot-arg ufs_root=1 enables it and makes it the root device; /dev/ufs0 and
 * /dev/rufs0 are its device nodes once devfs is up.
 *
 * XBL / ABL have already powered the UFS rail, trained the M-PHY / UniPro link, enabled the
 * host controller (HCE = 1) and read the boot partitions by the time XNU starts. This driver
 * maps the UFSHCI MMIO registers, allocates a single-slot UTP Transfer Request Descriptor
 * (UTRD) list, Command UPIU, Response UPIU, PRDT and bounce buffer in physically contiguous
 * DMA memory, and issues synchronous SCSI READ (10) commands on LUN 0 in polled mode.
 *
 * UFS uses 4096-byte logical blocks on LUN 0 while the BSD block layer and GPT reader work in
 * 512-byte sectors; this driver translates 512-byte sector offsets and counts into 4096-byte
 * UFS LBAs transparently.
 *
 * Read only, by construction: opening for writing and write strategy requests return EROFS.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/qcom_ufs.h>
#include "hakios_board.h"
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern pmap_t kernel_pmap;

#define QCOM_UFS_DEFAULT_BASE   0x04804000ULL       /* SM6225 UFSHCI controller (verified on sapphire) */
#define QCOM_UFS_MMIO_SIZE      0x4000ULL

/* Standard UFSHCI registers (JEDEC JESD223) */
#define REG_UFS_CAP             0x00
#define REG_UFS_VER             0x08
#define REG_INTERRUPT_STATUS    0x20
#define REG_INTERRUPT_ENABLE    0x24
#define REG_CONTROLLER_STATUS   0x30
#define REG_CONTROLLER_ENABLE   0x34
#define REG_UTRLBA              0x50
#define REG_UTRLBAU             0x54
#define REG_UTRLDBR             0x58
#define REG_UTRLCLR             0x5c
#define REG_UTRLRSR             0x60

/* REG_CONTROLLER_STATUS bits */
#define HCS_DP                  (1u << 0)           /* Device Present */
#define HCS_UTRLRDY             (1u << 1)           /* UTRL Ready */
#define HCS_UCRDY               (1u << 3)           /* UIC Command Ready */

/* REG_INTERRUPT_STATUS bits */
#define UFSHCI_IS_UTRCS         (1u << 0)           /* UTP Transfer Request Completion */
#define UFSHCI_IS_UE            (1u << 2)           /* UIC Error */
#define UFSHCI_IS_UTMRCS        (1u << 3)           /* UTP Task Management Request Completion */
#define UFSHCI_IS_UPMS          (1u << 4)           /* UIC Power Mode Status */
#define UFSHCI_IS_DFES          (1u << 11)          /* Device Fatal Error */
#define UFSHCI_IS_UTPES         (1u << 12)          /* UTP Error */
#define UFSHCI_IS_HCFES         (1u << 16)          /* Host Controller Fatal Error */
#define UFSHCI_IS_SBFES         (1u << 17)          /* System Bus Fatal Error */
#define UFSHCI_IS_FATAL         (UFSHCI_IS_DFES | UFSHCI_IS_UTPES | UFSHCI_IS_HCFES | UFSHCI_IS_SBFES)

/* UTRD DW0 command type & data direction */
#define UTRD_CMD_TYPE_UFS_STORAGE   (1u << 28)
#define UTRD_DD_READ                (2u << 25)      /* Target to Initiator (Read) */
#define UTRD_INTR_CMD               (1u << 24)
#define UTRD_OCS_SUCCESS            0x00u
#define UTRD_OCS_INVALID            0x0fu

/* UPIU transaction codes and flags */
#define UPIU_TRANSACTION_COMMAND    0x01u
#define UPIU_TRANSACTION_RESPONSE   0x21u
#define UPIU_CMD_FLAGS_READ         0x40u

/* SCSI opcodes */
#define SCSI_READ_10                0x28u

/* Block and transfer sizes */
#define UFS_BLOCK_SIZE              4096u           /* UFS native logical block size */
#define BSD_SECTOR_SIZE             512u            /* 512-byte sectors exposed to BSD */
#define SECTORS_PER_UFS_BLK         (UFS_BLOCK_SIZE / BSD_SECTOR_SIZE)
#define UFS_MAX_BLOCKS              32u             /* 32 * 4 KB = 128 KB per transfer */
#define UFS_MAX_SECTORS             (UFS_MAX_BLOCKS * SECTORS_PER_UFS_BLK)

/* UTP Transfer Request Descriptor (32 bytes, 1 KB aligned list) */
struct ufshci_utrd {
	uint32_t dw0;               /* CT[31:28], DD[26:25], I[24] */
	uint32_t dw1;               /* reserved */
	uint32_t dw2;               /* Overall Command Status (OCS) in bits [7:0] */
	uint32_t dw3;               /* reserved */
	uint32_t ucdba;             /* UTP Command Descriptor Base Address (low 32, 128-byte aligned) */
	uint32_t ucdbau;            /* UTP Command Descriptor Base Address (high 32) */
	uint16_t resp_upiu_len;     /* Response UPIU Length in DWords */
	uint16_t resp_upiu_off;     /* Response UPIU Offset in DWords */
	uint16_t prdt_len;          /* PRDT Length in entries */
	uint16_t prdt_off;          /* PRDT Offset in DWords */
} __attribute__((packed));

/* Physical Region Description Table entry (16 bytes) */
struct ufshci_prdt {
	uint32_t base_addr;         /* Data base physical address (low 32, 4-byte aligned) */
	uint32_t upper_addr;        /* Data base physical address (high 32) */
	uint32_t reserved;
	uint32_t size;              /* Data byte count minus 1 (bits [17:0]) */
} __attribute__((packed));

/* Command UPIU (32 bytes, padded to 64 bytes inside UCD) */
struct ufs_cmd_upiu {
	uint8_t trans_type;         /* 0x01 = Command UPIU */
	uint8_t flags;              /* 0x40 = Read */
	uint8_t lun;                /* Logical Unit Number (0 = main user storage) */
	uint8_t task_tag;
	uint8_t cmd_set_type;       /* 0 = SCSI */
	uint8_t reserved[3];
	uint8_t tot_ehs_len;        /* 0 */
	uint8_t reserved2;
	uint16_t data_seg_len;      /* 0 */
	uint32_t exp_data_xfer_len; /* Big-endian byte count */
	uint8_t cdb[16];            /* 16-byte SCSI CDB */
} __attribute__((packed));

/* Response UPIU (32 bytes header + sense data, padded to 64 bytes inside UCD) */
struct ufs_resp_upiu {
	uint8_t trans_type;         /* 0x21 = Response UPIU */
	uint8_t flags;
	uint8_t lun;
	uint8_t task_tag;
	uint8_t cmd_set_type;
	uint8_t response;           /* 0x00 = Target Success */
	uint8_t status;             /* SCSI status: 0x00 = GOOD */
	uint8_t tot_ehs_len;
	uint8_t device_info;
	uint8_t reserved;
	uint16_t data_seg_len;
	uint32_t residual_Count;
	uint8_t reserved2[16];
	uint8_t sense_data[32];
} __attribute__((packed));

/* UTP Command Descriptor in DMA memory (128-byte aligned) */
struct ufs_ucd {
	struct ufs_cmd_upiu cmd;
	uint8_t cmd_pad[32];
	struct ufs_resp_upiu resp;
	struct ufshci_prdt prdt[UFS_MAX_BLOCKS];
} __attribute__((packed, aligned(128)));

/* Single page of DMA descriptors: UTRD at offset 0 (1 KB aligned), UCD at offset 1024 */
struct ufs_dma_page {
	struct ufshci_utrd utrd[32];
	struct ufs_ucd ucd;
} __attribute__((aligned(4096)));

static vm_offset_t ufs_mmio;
static uint64_t ufs_phys_base = QCOM_UFS_DEFAULT_BASE;
static uint32_t ufs_ver, ufs_cap;
static struct ufs_dma_page ufs_dma __attribute__((aligned(4096)));
static uint8_t ufs_bounce[UFS_MAX_BLOCKS * UFS_BLOCK_SIZE] __attribute__((aligned(4096)));
static uint64_t ufs_part_lba512, ufs_part_sectors;
static int ufs_bmajor = -1, ufs_cmajor = -1;
static bool ufs_ready;
static const char *ufs_failed;
static uint32_t ufs_failed_status;
static uint64_t ufs_reads, ufs_read_sectors, ufs_errors;
static uint8_t ufs_task_seq;
static LCK_GRP_DECLARE(ufs_lck_grp, "hakios-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);

static inline uint32_t
ufs_rd(uint32_t reg)
{
	return *(volatile uint32_t *)(ufs_mmio + reg);
}

static inline void
ufs_wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(ufs_mmio + reg) = value;
}

static inline uint32_t
cpu_to_be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static inline uint16_t
cpu_to_be16(uint16_t v)
{
	return __builtin_bswap16(v);
}

static uint64_t
kva_to_phys(const void *va)
{
	vm_offset_t addr = (vm_offset_t)va;
	ppnum_t ppn = pmap_find_phys(kernel_pmap, (addr64_t)addr);

	if (ppn == 0) {
		return 0;
	}
	return ((uint64_t)ppn << PAGE_SHIFT) | (addr & PAGE_MASK);
}

static void
cache_clean_inval(const void *va, size_t len)
{
	uintptr_t start = (uintptr_t)va & ~63ULL;
	uintptr_t end = ((uintptr_t)va + len + 63ULL) & ~63ULL;

	__asm__ volatile("dsb sy" ::: "memory");
	for (uintptr_t p = start; p < end; p += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static bool
ufs_fail(const char *what, uint32_t status)
{
	ufs_failed = what;
	ufs_failed_status = status;
	return false;
}

static const char *
ufs_partition_name(void)
{
	if (hakios_board() != NULL && hakios_board()->system_partition != NULL &&
	    hakios_board()->system_partition[0] != '\0') {
		return hakios_board()->system_partition;
	}
	return "xnusys";
}

/*
 * Issue a synchronous SCSI READ (10) on LUN 0 for `ufs_blks` 4096-byte UFS blocks starting
 * at 4096-byte LBA `ufs_lba4k`, copying the result into `ufs_bounce`.
 */
static bool
ufs_read_4k_blocks(uint64_t ufs_lba4k, uint32_t ufs_blks)
{
	struct ufshci_utrd *utrd = &ufs_dma.utrd[0];
	struct ufs_ucd *ucd = &ufs_dma.ucd;
	uint64_t utrd_pa = kva_to_phys(utrd);
	uint64_t ucd_pa = kva_to_phys(ucd);
	uint32_t xfer_bytes = ufs_blks * UFS_BLOCK_SIZE;

	if (ufs_blks == 0 || ufs_blks > UFS_MAX_BLOCKS) {
		return ufs_fail("invalid block count", ufs_blks);
	}
	if (utrd_pa == 0 || ucd_pa == 0) {
		return ufs_fail("DMA physical translation failed", 0);
	}
	if (!(ufs_rd(REG_CONTROLLER_ENABLE) & 1u)) {
		return ufs_fail("controller not enabled", ufs_rd(REG_CONTROLLER_ENABLE));
	}
	if ((ufs_rd(REG_CONTROLLER_STATUS) & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
		return ufs_fail("controller not ready", ufs_rd(REG_CONTROLLER_STATUS));
	}

	/* Ensure slot 0 is idle before programming descriptors */
	if (ufs_rd(REG_UTRLDBR) & 1u) {
		ufs_wr(REG_UTRLCLR, ~1u);
	}

	memset(utrd, 0, sizeof(*utrd));
	memset(ucd, 0, sizeof(*ucd));

	/* Build Command UPIU for SCSI READ (10) on LUN 0 */
	ucd->cmd.trans_type = UPIU_TRANSACTION_COMMAND;
	ucd->cmd.flags = UPIU_CMD_FLAGS_READ;
	ucd->cmd.lun = 0;
	ucd->cmd.task_tag = ++ufs_task_seq ? ufs_task_seq : ++ufs_task_seq;
	ucd->cmd.cmd_set_type = 0;
	ucd->cmd.exp_data_xfer_len = cpu_to_be32(xfer_bytes);

	ucd->cmd.cdb[0] = SCSI_READ_10;
	ucd->cmd.cdb[1] = 0;
	ucd->cmd.cdb[2] = (uint8_t)((ufs_lba4k >> 24) & 0xffu);
	ucd->cmd.cdb[3] = (uint8_t)((ufs_lba4k >> 16) & 0xffu);
	ucd->cmd.cdb[4] = (uint8_t)((ufs_lba4k >> 8) & 0xffu);
	ucd->cmd.cdb[5] = (uint8_t)(ufs_lba4k & 0xffu);
	ucd->cmd.cdb[6] = 0;
	ucd->cmd.cdb[7] = (uint8_t)((ufs_blks >> 8) & 0xffu);
	ucd->cmd.cdb[8] = (uint8_t)(ufs_blks & 0xffu);
	ucd->cmd.cdb[9] = 0;

	/* Build PRDT entries (one 4 KB page per entry) */
	for (uint32_t i = 0; i < ufs_blks; i++) {
		uint64_t page_pa = kva_to_phys(ufs_bounce + i * UFS_BLOCK_SIZE);

		if (page_pa == 0) {
			return ufs_fail("bounce page physical translation failed", i);
		}
		ucd->prdt[i].base_addr = (uint32_t)(page_pa & 0xffffffffULL);
		ucd->prdt[i].upper_addr = (uint32_t)(page_pa >> 32);
		ucd->prdt[i].reserved = 0;
		ucd->prdt[i].size = UFS_BLOCK_SIZE - 1;
	}

	/* Build UTRD in slot 0 */
	utrd->dw0 = UTRD_CMD_TYPE_UFS_STORAGE | UTRD_DD_READ | UTRD_INTR_CMD;
	utrd->dw2 = UTRD_OCS_INVALID;
	utrd->ucdba = (uint32_t)(ucd_pa & 0xffffffffULL);
	utrd->ucdbau = (uint32_t)(ucd_pa >> 32);
	utrd->resp_upiu_off = (uint16_t)(__builtin_offsetof(struct ufs_ucd, resp) / sizeof(uint32_t));
	utrd->resp_upiu_len = (uint16_t)(sizeof(struct ufs_resp_upiu) / sizeof(uint32_t));
	utrd->prdt_off = (uint16_t)(__builtin_offsetof(struct ufs_ucd, prdt) / sizeof(uint32_t));
	utrd->prdt_len = (uint16_t)ufs_blks;

	cache_clean_inval(&ufs_dma, sizeof(ufs_dma));
	cache_clean_inval(ufs_bounce, xfer_bytes);

	/*
	 * JEDEC UFSHCI: UTRLBA/UTRLBAU are latched only while UTRLRSR is 0.
	 * ABL leaves UTRLRSR=1 pointing at ABL's own UTRD buffer, so stop UTRLRSR
	 * whenever UTRLBA/UTRLBAU do not yet match our utrd_pa.
	 */
	uint32_t want_lo = (uint32_t)(utrd_pa & 0xffffffffULL);
	uint32_t want_hi = (uint32_t)(utrd_pa >> 32);
	if (ufs_rd(REG_UTRLBA) != want_lo || ufs_rd(REG_UTRLBAU) != want_hi || !(ufs_rd(REG_UTRLRSR) & 1u)) {
		if (ufs_rd(REG_UTRLRSR) & 1u) {
			ufs_wr(REG_UTRLRSR, 0u);
			for (uint32_t s = 0; s < 1000000u && (ufs_rd(REG_UTRLRSR) & 1u); s++) {
			}
		}
		ufs_wr(REG_UTRLBA, want_lo);
		ufs_wr(REG_UTRLBAU, want_hi);
		ufs_wr(REG_UTRLRSR, 1u);
		for (uint32_t s = 0; s < 1000000u && !(ufs_rd(REG_UTRLRSR) & 1u); s++) {
		}
	}

	/* Clear any stale interrupt status bits and ring doorbell slot 0 */
	ufs_wr(REG_INTERRUPT_STATUS, ufs_rd(REG_INTERRUPT_STATUS));
	ufs_wr(REG_UTRLDBR, 1u);

	for (uint32_t spin = 0; spin < 20000000; spin++) {
		uint32_t is = ufs_rd(REG_INTERRUPT_STATUS);
		uint32_t dbr = ufs_rd(REG_UTRLDBR);

		if (is & UFSHCI_IS_FATAL) {
			ufs_wr(REG_INTERRUPT_STATUS, is);
			ufs_wr(REG_UTRLCLR, ~1u);
			return ufs_fail("fatal UFSHCI error", is);
		}
		if (!(dbr & 1u)) {
			if (is) {
				ufs_wr(REG_INTERRUPT_STATUS, is);
			}
			cache_clean_inval(&ufs_dma, sizeof(ufs_dma));
			cache_clean_inval(ufs_bounce, xfer_bytes);
			if ((utrd->dw2 & 0xffu) != UTRD_OCS_SUCCESS) {
				return ufs_fail("UTRD OCS error", utrd->dw2 & 0xffu);
			}
			if ((ucd->resp.trans_type & 0x3fu) != UPIU_TRANSACTION_RESPONSE ||
			    ucd->resp.response != 0 || ucd->resp.status != 0) {
				return ufs_fail("SCSI response error",
				    ((uint32_t)ucd->resp.response << 8) | ucd->resp.status);
			}
			return true;
		}
	}

	ufs_wr(REG_UTRLCLR, ~1u);
	return ufs_fail("transfer timeout", ufs_rd(REG_INTERRUPT_STATUS));
}

/*
 * Read `sectors` 512-byte blocks starting at 512-byte LBA `lba512` into `dst`.
 */
static bool
ufs_read_sectors_once(uint64_t lba512, uint32_t sectors, uint8_t *dst)
{
	while (sectors > 0) {
		uint64_t lba4k = lba512 / SECTORS_PER_UFS_BLK;
		uint32_t sub_off = (uint32_t)(lba512 % SECTORS_PER_UFS_BLK);
		uint32_t max_sec = UFS_MAX_SECTORS - sub_off;
		uint32_t chunk_sec = sectors < max_sec ? sectors : max_sec;
		uint32_t blks4k = (sub_off + chunk_sec + SECTORS_PER_UFS_BLK - 1) / SECTORS_PER_UFS_BLK;

		if (!ufs_read_4k_blocks(lba4k, blks4k)) {
			return false;
		}
		memcpy(dst, ufs_bounce + sub_off * BSD_SECTOR_SIZE, chunk_sec * BSD_SECTOR_SIZE);
		dst += chunk_sec * BSD_SECTOR_SIZE;
		lba512 += chunk_sec;
		sectors -= chunk_sec;
	}
	return true;
}

static bool
ufs_read_retrying(uint64_t lba512, uint32_t sectors, void *dst)
{
	if (ufs_read_sectors_once(lba512, sectors, (uint8_t *)dst)) {
		return true;
	}
	ufs_errors++;
	printf("ufs: reading %u sectors at %llu: %s (%#x); trying again\n",
	    sectors, lba512, ufs_failed ? ufs_failed : "error", ufs_failed_status);
	return ufs_read_sectors_once(lba512, sectors, (uint8_t *)dst);
}

/* ---- GPT scanning (supports both 4096-byte native UFS GPT and 512-byte GPT) ---- */

static uint32_t gpt_sector[UFS_BLOCK_SIZE / 4] __attribute__((aligned(16)));
static uint32_t gpt_entries[128 * 128 / 4] __attribute__((aligned(16)));

static bool
ufs_find_partition(void)
{
	const char *part_name = ufs_partition_name();
	const uint8_t *s = (const uint8_t *)gpt_sector;
	uint32_t lba_shift = 3;                     /* UFS LUN 0 uses 4096-byte LBAs (1 LBA = 8 * 512B) */
	uint64_t entries_lba, at;
	uint32_t count, size;

	/* Try GPT header at 4 KB LBA 1 (512B sector 8) first, then fall back to 512B sector 1 */
	if (!ufs_read_retrying(8, 1, gpt_sector) || memcmp(s, "EFI PART", 8) != 0) {
		lba_shift = 0;
		if (!ufs_read_retrying(1, 1, gpt_sector) || memcmp(s, "EFI PART", 8) != 0) {
			printf("ufs: no GPT on LUN 0 (%s)\n", ufs_failed ? ufs_failed : "no signature");
			return false;
		}
	}

	memcpy(&entries_lba, s + 72, 8);
	memcpy(&count, s + 80, 4);
	memcpy(&size, s + 84, 4);
	if (count > 128 || size != 128 ||
	    !ufs_read_retrying(entries_lba << lba_shift, (count * size) / BSD_SECTOR_SIZE, gpt_entries)) {
		printf("ufs: invalid GPT entries (%u of %u bytes)\n", count, size);
		return false;
	}

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)gpt_entries + i * size;
		size_t k;
		uint64_t first, last;

		for (k = 0; part_name[k] && e[56 + 2 * k] == (uint8_t)part_name[k] && e[57 + 2 * k] == 0; k++) {
		}
		if (part_name[k] != 0 || e[56 + 2 * k] != 0 || e[57 + 2 * k] != 0) {
			continue;
		}
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (first == 0 || last < first) {
			return false;
		}
		ufs_part_lba512 = first << lba_shift;
		ufs_part_sectors = (last - first + 1) << lba_shift;
		break;
	}

	if (ufs_part_lba512 == 0) {
		printf("ufs: no %s partition\n", part_name);
		return false;
	}

	/* Measure contiguous APFS containers inside the partition */
	for (at = 0; at < ufs_part_sectors * BSD_SECTOR_SIZE;) {
		uint32_t bs;
		uint64_t n;

		if (!ufs_read_retrying(ufs_part_lba512 + at / BSD_SECTOR_SIZE, 1, gpt_sector) ||
		    gpt_sector[8] != 0x4253584eu) { /* "NXSB" */
			break;
		}
		bs = gpt_sector[9];
		memcpy(&n, s + 40, 8);
		if (bs < 512 || bs > 65536 || n == 0 || n * bs > ufs_part_sectors * BSD_SECTOR_SIZE - at) {
			break;
		}
		at += n * bs;
	}
	if (at == 0) {
		/*
		 * Fallback for >2 GiB iOS 27 images on sapphire: check the unused tail of
		 * Android's `super` partition (offset +6488064 512B-sectors = 3,321,888,768 B,
		 * leaving 4,194,304,000 B = 4.00 GiB untouched by active slot _b).
		 */
		const char *sup = "super";
		uint64_t sup_lba = 0, sup_secs = 0;
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *e = (const uint8_t *)gpt_entries + i * size;
			size_t k;
			uint64_t first, last;
			for (k = 0; sup[k] && e[56 + 2 * k] == (uint8_t)sup[k] && e[57 + 2 * k] == 0; k++) {
			}
			if (sup[k] != 0 || e[56 + 2 * k] != 0 || e[57 + 2 * k] != 0) {
				continue;
			}
			memcpy(&first, e + 32, 8);
			memcpy(&last, e + 40, 8);
			if (first != 0 && last > first) {
				sup_lba = (first << lba_shift) + 6480000ULL;
				sup_secs = ((last - first + 1) << lba_shift);
				sup_secs = sup_secs > 6480000ULL ? (sup_secs - 6480000ULL) : 0;
			}
			break;
		}
		if (sup_lba != 0 && sup_secs != 0) {
			ufs_part_lba512 = sup_lba;
			ufs_part_sectors = sup_secs;
			part_name = "super-tail";
			for (at = 0; at < ufs_part_sectors * BSD_SECTOR_SIZE;) {
				uint32_t bs;
				uint64_t n;
				if (!ufs_read_retrying(ufs_part_lba512 + at / BSD_SECTOR_SIZE, 1, gpt_sector) ||
				    gpt_sector[8] != 0x4253584eu) {
					break;
				}
				bs = gpt_sector[9];
				memcpy(&n, s + 40, 8);
				if (bs < 512 || bs > 65536 || n == 0 || n * bs > ufs_part_sectors * BSD_SECTOR_SIZE - at) {
					break;
				}
				at += n * bs;
			}
		}
	}
	if (at == 0) {
		printf("ufs: no APFS container in %s\n", part_name);
		return false;
	}
	printf("ufs: %s at 512B-sector %llu, %llu MB of %llu MB in APFS containers\n",
	    part_name, ufs_part_lba512, at >> 20, (ufs_part_sectors * BSD_SECTOR_SIZE) >> 20);
	ufs_part_sectors = at / BSD_SECTOR_SIZE;
	return true;
}

/* ---- BSD block and character device ops ---- */

static int
ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	return (flags & FWRITE) ? EROFS : 0;
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
ufs_strategy(buf_t bp)
{
	uint64_t sector = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp), done = 0;
	uint8_t *data = (uint8_t *)buf_dataptr(bp);
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;
		goto out;
	}
	if (sector >= ufs_part_sectors) {
		buf_setresid(bp, count);
		error = sector == ufs_part_sectors ? 0 : EINVAL;
		goto out;
	}
	if (count > (ufs_part_sectors - sector) * BSD_SECTOR_SIZE) {
		count = (uint32_t)((ufs_part_sectors - sector) * BSD_SECTOR_SIZE);
	}

	lck_mtx_lock(&ufs_lock);
	while (done < count && error == 0) {
		uint32_t secs = (count - done) / BSD_SECTOR_SIZE;

		if (secs == 0) {
			uint32_t n = count - done;

			if (!ufs_read_retrying(ufs_part_lba512 + sector + done / BSD_SECTOR_SIZE, 1, gpt_sector)) {
				error = EIO;
				break;
			}
			memcpy(data + done, gpt_sector, n);
			done += n;
			continue;
		}
		if (secs > UFS_MAX_SECTORS) {
			secs = UFS_MAX_SECTORS;
		}
		if (!ufs_read_retrying(ufs_part_lba512 + sector + done / BSD_SECTOR_SIZE, secs, data + done)) {
			error = EIO;
			break;
		}
		ufs_reads++;
		ufs_read_sectors += secs;
		done += secs * BSD_SECTOR_SIZE;
	}
	lck_mtx_unlock(&ufs_lock);
	buf_setresid(bp, buf_count(bp) - done);
	if (error) {
		printf("ufs: sector %llu, %u bytes: %s (%#x)\n",
		    sector, count, ufs_failed ? ufs_failed : "error", ufs_failed_status);
	}
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = BSD_SECTOR_SIZE;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == BSD_SECTOR_SIZE ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_part_sectors;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = UFS_MAX_SECTORS;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_size(__unused dev_t dev)
{
	return BSD_SECTOR_SIZE;
}

static int
ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(ufs_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))ufs_size, uio, BSD_SECTOR_SIZE);
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_size,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_open,
	.d_close      = ufs_close,
	.d_read       = ufs_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

void
qcom_ufs_init(void)
{
	uint32_t ufs_root_arg = 0;
	uint64_t base_arg = 0;

	if (ufs_ready) {
		return;
	}

	/*
	 * ufs_root=1: mount the system image from the UFS partition through the Qualcomm UFSHCI controller.
	 */
	if (!PE_parse_boot_argn("ufs_root", &ufs_root_arg, sizeof(ufs_root_arg)) || ufs_root_arg == 0) {
		return;
	}

	/*
	 * ufs_base=ADDR: physical base address of the UFSHCI controller (default 0x01d84000 on SM6225).
	 */
	if (PE_parse_boot_argn("ufs_base", &base_arg, sizeof(base_arg)) && base_arg != 0) {
		ufs_phys_base = base_arg;
	}

	ufs_mmio = ml_io_map((vm_offset_t)ufs_phys_base, QCOM_UFS_MMIO_SIZE);
	if (ufs_mmio == 0) {
		printf("ufs: ml_io_map(%#llx) failed\n", ufs_phys_base);
		return;
	}

	ufs_cap = ufs_rd(REG_UFS_CAP);
	ufs_ver = ufs_rd(REG_UFS_VER);
	printf("ufs: UFSHCI at %#llx ver %#x cap %#x hce %#x hcs %#x\n",
	    ufs_phys_base, ufs_ver, ufs_cap, ufs_rd(REG_CONTROLLER_ENABLE), ufs_rd(REG_CONTROLLER_STATUS));

	if (!(ufs_rd(REG_CONTROLLER_ENABLE) & 1u)) {
		ufs_wr(REG_CONTROLLER_ENABLE, 1u);
		for (uint32_t i = 0; i < 1000000 && !(ufs_rd(REG_CONTROLLER_ENABLE) & 1u); i++) {
		}
	}
	ufs_ready = true;
}

int
qcom_ufs_root(dev_t *root_dev)
{
	if (ufs_bmajor >= 0) {
		*root_dev = makedev(ufs_bmajor, 0);
		return 0;
	}
	qcom_ufs_init();
	if (!ufs_ready) {
		return ENXIO;
	}
	if (!ufs_find_partition()) {
		return ENXIO;
	}
	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	if (ufs_bmajor < 0 || (ufs_cmajor = cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor)) < 0) {
		printf("ufs: no room among the devices (%d, %d)\n", ufs_bmajor, ufs_cmajor);
		return ENXIO;
	}
	*root_dev = makedev(ufs_bmajor, 0);
	return 0;
}

void
qcom_ufs_devfs(void)
{
	if (ufs_bmajor < 0 || ufs_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(ufs_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufs0") == NULL ||
	    devfs_make_node(makedev(ufs_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufs0") == NULL) {
		printf("ufs: no /dev nodes\n");
	}
}

/* sysctl kern.hakios.ufs_*: UFS controller status and read statistics */
static int
ufs_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[160];

	snprintf(line, sizeof(line), "ufs: base %#llx ver %#x, %llu reads, %llu MB, %llu errors%s%s",
	    ufs_phys_base, ufs_ver, ufs_reads, ufs_read_sectors >> 11, ufs_errors,
	    ufs_failed ? "; last: " : "", ufs_failed ? ufs_failed : "");
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, ufs_status, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl, "A", "Qualcomm UFSHCI controller status and reads so far");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_reads,
    "number of UFS block read transfers");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_read_sectors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_read_sectors,
    "number of 512-byte sectors read from UFS");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_errors,
    "number of UFS transfer errors");
