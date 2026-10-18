/*
 * First boot: grow the GBA partition to the end of the SD card.
 *
 * The image's p3 is a 64 MiB FAT; written to a card it leaves the rest
 * of the card unallocated. Growing a FAT in place is not worth the
 * complexity, so the file system is made anew: its files are copied into
 * a tmpfs (64 MiB at most, against 1 GiB of RAM), the p3 entry of the MBR
 * is grown to the end of the card (4 MiB aligned, at most 2 TiB), p3 is
 * formatted FAT32 (fat32_format, there is no mkfs in the root fs) and the
 * files are copied back and compared.
 *
 * Power loss: a header in the last 4 KiB before the new end of p3 (free
 * space until the grow) records the attempt: magic, geometry, the old p3
 * entry, a phase, an attempt count, the CRC of a raw backup of the old p3
 * and its own CRC. While it is valid an attempt is in progress. Before the
 * MBR changes, the old p3 is copied raw into the free space just below
 * the header. The new file system is laid out from the start of p3 and the
 * copied files take its first clusters (the FSInfo next free hint is 3,
 * vfat allocates upwards from it), so the backup at the far end is never
 * touched; the room for that is checked before starting. The steps and
 * what an interruption after each leaves:
 *
 *   0. header "started" with the attempt number, fsync
 *   1. mount p3 read-only, copy into the tmpfs, unmount
 *   2. raw backup of p3 into the free space, fsync
 *   3. header "backed up", fsync
 *                         0 to 3: the MBR is unchanged and the old p3 was
 *                         only read, so the next boot just starts over,
 *                         counting the attempt (a torn header fails its
 *                         CRC: it starts over too)
 *   4. p3 entry in the MBR grown (one sector), fsync, BLKPG resize
 *   5. FAT32 format, fsync
 *   6. mount, copy back, unmount, flush
 *   7. mount again (cold cache), compare with the tmpfs, unmount
 *                         4 to 7: valid header and the grown p3 size, so
 *                         the next boot restores the old p3 from the backup
 *                         (its CRC checked first, only the chunks that
 *                         differ written), then the old p3 entry, and starts
 *                         over. An interrupted restore is simply done again
 *                         (data first, the MBR entry after it).
 *   8. header cleared, fsync
 *                         done; a torn clear fails the CRC: done too
 *
 * A failure in the same boot (I/O error, full, compare mismatch) restores
 * the old p3 the same way and marks the header "gave up", which later
 * boots respect, as they do after MAX_ATTEMPTS interrupted attempts (a
 * crash in the middle would otherwise loop). Reasons not to grow that are
 * found before step 2 (p3 does not mount and is not blank, too little RAM
 * or room) clear the header, and the next boot checks again. A p3 that
 * does not mount is only formatted if its first 64 KiB are blank.
 *
 * The torn MBR write in step 4 is the one window without a fallback; it
 * is a single sector, which SD cards write atomically in practice.
 *
 * Cost on later boots: the MBR, sysfs sizes and, once grown, the 4 KiB
 * header block are read; nothing else.
 *
 * Raw I/O goes through the whole card device with O_DIRECT, file system
 * I/O through mounts of p3; p3's page cache is flushed (BLKFLSBUF) around
 * every mount, so neither side sees stale data from the other.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/blkpg.h>
#include <linux/fs.h>

#include "fe.h"
#include "sdgrow.h"

#define ALIGN_SECTORS 8192u          /* 4 MiB: p3 end, FAT32 data area */
#define MIN_FREE_SECTORS (64u << 11) /* grow only for more than 64 MiB */
#define MARGIN_SECTORS (16u << 11)   /* between the copied files and backup */
#define HDR_SECTORS 8u
#define CHUNK (1u << 20)
#define CHUNK_SECTORS (CHUNK / 512)
#define BLANK_SECTORS 128u
#define MAX_ATTEMPTS 3u
#define MAX_DEPTH 64
#define MBR_P3 (446 + 2 * 16)
#define MIB(sectors) ((unsigned)((sectors) >> 11))

#define HDR_MAGIC "RG251GRW"
#define HDR_VERSION 1
enum {
	PHASE_STARTED = 1,    /* attempt counted, no backup yet */
	PHASE_BACKED_UP = 2,  /* backup complete: the MBR may be changed */
	PHASE_GAVE_UP = 3,    /* restored after a failure, never again */
};

/* The progress header; little endian like both the device and a PC. */
struct hdr {
	char magic[8];
	uint32_t version, phase, attempt;
	uint32_t start, old_size, new_size;
	uint32_t hdr_lba, backup_lba, backup_sectors, backup_crc;
	uint64_t card, nonce;
	uint8_t old_entry[16];
	char reason[32];
	uint32_t crc;      /* of everything before */
};
_Static_assert(sizeof(struct hdr) <= 512, "header fits a sector");

struct grow {
	int test;
	int fd;                         /* the whole card or the image */
	char disk[32], part[32];        /* mmcblk0, mmcblk0p3 */
	char disk_dev[48], part_dev[48];
	const char *img, *hook, *stop, *fail;
	char old_dir[300], new_dir[300], copy_dir[300], check_dir[300];
	int tmpfs;
	uint8_t *buf, *buf2;            /* CHUNK each, aligned for O_DIRECT */
	uint8_t mbr[512];
	struct sdgrow_plan p;
	sdgrow_show_fn show;
};

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static int64_t ms_since(int64_t t0)
{
	return (now_ns() - t0) / 1000000;
}

uint32_t sdgrow_crc32(uint32_t crc, const void *buf, size_t len)
{
	static uint32_t table[256];
	const uint8_t *p = buf;

	if (!table[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;

			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}
	crc = ~crc;
	while (len--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* ------------------------------------------------------------- layout */

int sdgrow_plan(const uint8_t *mbr, uint64_t card, struct sdgrow_plan *p,
		char *why, size_t n)
{
	uint32_t s[4], sz[4];
	uint8_t type[4];
	uint64_t lim;

	memset(p, 0, sizeof(*p));
	p->card = card;
	if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
		snprintf(why, n, "no MBR signature");
		return -1;
	}
	for (int i = 0; i < 4; i++) {
		const uint8_t *e = mbr + 446 + 16 * i;

		type[i] = e[4];
		s[i] = get32(e + 8);
		sz[i] = get32(e + 12);
	}
	if (!sz[1] || !sz[2]) {
		snprintf(why, n, "no p2 or p3");
		return -1;
	}
	if (type[2] != 0x0c && type[2] != 0x0b) {
		snprintf(why, n, "p3 type 0x%02x is not FAT32", type[2]);
		return -1;
	}
	if (type[3] || sz[3]) {
		snprintf(why, n, "there is a p4");
		return -1;
	}
	if ((uint64_t)s[2] < (uint64_t)s[1] + sz[1]) {
		snprintf(why, n, "p3 does not start after p2");
		return -1;
	}
	for (int i = 0; i < 2; i++) {
		if (sz[i] && s[i] >= s[2]) {
			snprintf(why, n, "p%d lies after p3", i + 1);
			return -1;
		}
	}
	if ((uint64_t)s[2] + sz[2] > card) {
		snprintf(why, n, "p3 ends beyond the card");
		return -1;
	}
	/* MBR: start and size are 32 bit, the end stays below 2 TiB. */
	lim = card < 0xffffffffu ? card : 0xffffffffu;
	p->start = s[2];
	p->size = sz[2];
	p->end = (uint32_t)(lim & ~(uint64_t)(ALIGN_SECTORS - 1));
	p->hdr_lba = p->end - HDR_SECTORS;
	return 0;
}

/* ------------------------------------------------------------- FAT32 */

int fat32_geometry(uint32_t part_lba, uint32_t sectors, struct fat32_geom *g)
{
	uint64_t bytes = (uint64_t)sectors * 512;
	uint32_t spc;

	/* Windows' FAT32 defaults: 4 KiB up to 8 GiB, 8 up to 16, 16 up to
	 * 32, 32 KiB above (Windows does not format those as FAT32 but
	 * reads them, as do macOS and Linux). */
	spc = bytes <= (8ull << 30) ? 8 : bytes <= (16ull << 30) ? 16 :
	      bytes <= (32ull << 30) ? 32 : 64;
	for (; spc; spc >>= 1) {
		uint32_t res = 32, fat = 1, pad;
		uint64_t clusters;

		for (;;) {
			uint64_t need;

			if ((uint64_t)res + 2ull * fat + spc > sectors)
				return -1;
			clusters = (sectors - res - 2ull * fat) / spc;
			need = ((clusters + 2) * 4 + 511) / 512;
			if (need <= fat)
				break;
			fat = (uint32_t)need;
		}
		/* Data area (and so every cluster) aligned on the card. */
		pad = (ALIGN_SECTORS - (part_lba + res + 2 * fat) % ALIGN_SECTORS) %
		      ALIGN_SECTORS;
		res += pad;
		if ((uint64_t)res + 2ull * fat + spc > sectors)
			continue;
		clusters = (sectors - res - 2ull * fat) / spc;
		/* The FAT type follows from the cluster count alone; keep clear
		 * of the FAT16 limit and the reserved values. */
		if (clusters < 65525 + 16 || clusters > 0x0ffffff5 - 16)
			continue;
		g->sectors = sectors;
		g->reserved = res;
		g->fat_sectors = fat;
		g->spc = spc;
		g->clusters = (uint32_t)clusters;
		g->data = res + 2 * fat;
		return 0;
	}
	return -1;
}

static void fat_now(uint16_t *date, uint16_t *time)
{
	struct tm tm;

	realtime_utc(&tm);
	if (tm.tm_year < 80) {
		*date = 1 << 5 | 1;  /* 1980-01-01 */
		*time = 0;
		return;
	}
	*date = (tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday;
	*time = tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2;
}

static int pwrite_all(int fd, const void *buf, size_t len, uint64_t off)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pwrite(fd, (const char *)buf + done, len - done,
				   (off_t)(off + done));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0 ? errno : EIO;
		done += n;
	}
	return 0;
}

static int pread_all(int fd, void *buf, size_t len, uint64_t off)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, (char *)buf + done, len - done,
				  (off_t)(off + done));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0 ? errno : EIO;
		done += n;
	}
	return 0;
}

static void boot_sector(uint8_t *s, uint32_t part_lba,
			const struct fat32_geom *g, uint32_t volume_id,
			const char *label)
{
	memset(s, 0, 512);
	s[0] = 0xeb;  /* jmp short 0x5a; nop */
	s[1] = 0x58;
	s[2] = 0x90;
	memcpy(s + 3, "MSWIN4.1", 8);
	put16(s + 11, 512);
	s[13] = g->spc;
	put16(s + 14, g->reserved);
	s[16] = 2;               /* FATs */
	s[21] = 0xf8;            /* media: fixed disk */
	put16(s + 24, 63);       /* sectors per track */
	put16(s + 26, 255);      /* heads */
	put32(s + 28, part_lba); /* hidden sectors */
	put32(s + 32, g->sectors);
	put32(s + 36, g->fat_sectors);
	put32(s + 44, 2);        /* root directory cluster */
	put16(s + 48, 1);        /* FSInfo sector */
	put16(s + 50, 6);        /* backup boot sector */
	s[64] = 0x80;            /* drive number */
	s[66] = 0x29;            /* extended boot signature */
	put32(s + 67, volume_id);
	memset(s + 71, ' ', 11);
	memcpy(s + 71, label, strnlen(label, 11));
	memcpy(s + 82, "FAT32   ", 8);
	/* Boot code: int 18h (no bootable disk), then halt. */
	s[90] = 0xcd;
	s[91] = 0x18;
	s[92] = 0xf4;
	s[93] = 0xeb;
	s[94] = 0xfd;
	s[510] = 0x55;
	s[511] = 0xaa;
}

int fat32_format(int fd, uint32_t part_lba, const struct fat32_geom *g,
		 uint32_t volume_id, const char *label)
{
	uint64_t base = (uint64_t)part_lba * 512;
	uint64_t zero_end = ((uint64_t)g->data + g->spc) * 512;
	uint8_t *buf, *s;
	uint16_t date, time;
	int err;

	if (posix_memalign((void **)&buf, 4096, CHUNK))
		return ENOMEM;
	/* Reserved area (the old boot sector goes first), both FATs and
	 * the root directory cluster. The boot sector is written last. */
	memset(buf, 0, CHUNK);
	for (uint64_t off = 0; off < zero_end; off += CHUNK) {
		size_t n = zero_end - off < CHUNK ? zero_end - off : CHUNK;

		if ((err = pwrite_all(fd, buf, n, base + off)))
			goto out;
	}
	s = buf;
	/* FAT[0] media, FAT[1] clean and no errors, FAT[2] root: end */
	put32(s, 0x0ffffff8);
	put32(s + 4, 0x0fffffff);
	put32(s + 8, 0x0fffffff);
	for (int i = 0; i < 2; i++) {
		uint64_t at = base + ((uint64_t)g->reserved +
				      (uint64_t)i * g->fat_sectors) * 512;

		if ((err = pwrite_all(fd, s, 512, at)))
			goto out;
	}
	/* Root directory: the volume label entry (what Windows shows). */
	memset(s, 0, 512);
	memset(s, ' ', 11);
	memcpy(s, label, strnlen(label, 11));
	s[11] = 0x08;
	fat_now(&date, &time);
	put16(s + 14, time);
	put16(s + 16, date);
	put16(s + 18, date);
	put16(s + 22, time);
	put16(s + 24, date);
	if ((err = pwrite_all(fd, s, 512, base + (uint64_t)g->data * 512)))
		goto out;
	/* FSInfo: free count and next free cluster. */
	memset(s, 0, 512);
	put32(s, 0x41615252);
	put32(s + 484, 0x61417272);
	put32(s + 488, g->clusters - 1);
	put32(s + 492, 3);
	put32(s + 508, 0xaa550000);
	if ((err = pwrite_all(fd, s, 512, base + 1 * 512)) ||
	    (err = pwrite_all(fd, s, 512, base + 7 * 512)))
		goto out;
	boot_sector(s, part_lba, g, volume_id, label);
	if ((err = pwrite_all(fd, s, 512, base + 6 * 512)))
		goto out;
	if (fsync(fd)) {
		err = errno;
		goto out;
	}
	if ((err = pwrite_all(fd, s, 512, base)))
		goto out;
	if (fsync(fd))
		err = errno;
out:
	free(buf);
	return err;
}

/* -------------------------------------------------------- tree copies */

struct walk {
	struct sdgrow_tree *t;
	uint8_t *buf, *buf2;
};

static int read_full(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = read(fd, buf + got, len - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		got += n;
	}
	return (int)got;
}

static int copy_file(struct walk *w, int sfd, int dfd, const char *name,
		     const struct stat *st)
{
	struct timespec ts[2] = { st->st_atim, st->st_mtim };
	int in, out, n, err = 0;

	in = openat(sfd, name, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		log_msg("sdgrow: open %s: %s", name, strerror(errno));
		return -1;
	}
	out = openat(dfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		log_msg("sdgrow: create %s: %s", name, strerror(errno));
		close(in);
		return -1;
	}
	while ((n = read_full(in, w->buf, CHUNK)) > 0) {
		for (int done = 0; done < n;) {
			ssize_t k = write(out, w->buf + done, n - done);

			if (k < 0 && errno == EINTR)
				continue;
			if (k <= 0) {
				err = k < 0 ? errno : ENOSPC;
				break;
			}
			done += k;
		}
		if (err)
			break;
		w->t->bytes += n;
	}
	if (n < 0)
		err = errno;
	if (!err && futimens(out, ts))
		err = errno;
	/* vfat maps a mode without write bits to the read-only attribute. */
	if (!err && !(st->st_mode & 0222))
		fchmod(out, 0444);
	if (close(out) && !err)
		err = errno;
	close(in);
	if (err) {
		log_msg("sdgrow: copy %s: %s", name, strerror(err));
		return -1;
	}
	w->t->files++;
	return 0;
}

static int copy_dir(struct walk *w, int sfd, int dfd, int depth)
{
	DIR *d;
	struct dirent *e;
	int ret = 0, fd = dup(sfd);

	if (fd < 0 || !(d = fdopendir(fd))) {
		if (fd >= 0)
			close(fd);
		log_msg("sdgrow: opendir: %s", strerror(errno));
		return -1;
	}
	while (!ret && (e = readdir(d))) {
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fstatat(sfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
			log_msg("sdgrow: stat %s: %s", e->d_name, strerror(errno));
			ret = -1;
		} else if (S_ISDIR(st.st_mode)) {
			struct timespec ts[2] = { st.st_atim, st.st_mtim };
			int s2, d2;

			if (depth >= MAX_DEPTH) {
				log_msg("sdgrow: %s: directories nested too deep",
					e->d_name);
				ret = -1;
				break;
			}
			if (mkdirat(dfd, e->d_name, 0755) && errno != EEXIST) {
				log_msg("sdgrow: mkdir %s: %s", e->d_name,
					strerror(errno));
				ret = -1;
				break;
			}
			s2 = openat(sfd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			d2 = openat(dfd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (s2 < 0 || d2 < 0) {
				log_msg("sdgrow: open dir %s: %s", e->d_name,
					strerror(errno));
				ret = -1;
			} else {
				ret = copy_dir(w, s2, d2, depth + 1);
			}
			if (s2 >= 0)
				close(s2);
			if (d2 >= 0)
				close(d2);
			/* After the content, which changed it. */
			if (!ret)
				utimensat(dfd, e->d_name, ts, 0);
			w->t->dirs++;
		} else if (S_ISREG(st.st_mode)) {
			ret = copy_file(w, sfd, dfd, e->d_name, &st);
		} else {
			log_msg("sdgrow: skipping %s (not a file or directory)",
				e->d_name);
			w->t->skipped++;
		}
	}
	closedir(d);
	return ret;
}

static int count_entries(int fd)
{
	int n = 0, dup_fd = dup(fd);
	struct dirent *e;
	DIR *d;

	if (dup_fd < 0 || !(d = fdopendir(dup_fd))) {
		if (dup_fd >= 0)
			close(dup_fd);
		return -1;
	}
	while ((e = readdir(d)))
		n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
	closedir(d);
	return n;
}

static int compare_file(struct walk *w, int afd, int bfd, const char *name)
{
	int a = openat(afd, name, O_RDONLY | O_CLOEXEC);
	int b = openat(bfd, name, O_RDONLY | O_CLOEXEC);
	int ret = -1, na, nb;

	if (a < 0 || b < 0) {
		log_msg("sdgrow: compare: open %s: %s", name, strerror(errno));
		goto out;
	}
	for (;;) {
		na = read_full(a, w->buf, CHUNK);
		nb = read_full(b, w->buf2, CHUNK);
		if (na < 0 || nb < 0) {
			log_msg("sdgrow: compare: read %s: %s", name, strerror(errno));
			goto out;
		}
		if (na != nb || memcmp(w->buf, w->buf2, na)) {
			log_msg("sdgrow: compare: %s differs", name);
			goto out;
		}
		w->t->bytes += na;
		if (!na)
			break;
	}
	ret = 0;
out:
	if (a >= 0)
		close(a);
	if (b >= 0)
		close(b);
	return ret;
}

static int compare_dir(struct walk *w, int afd, int bfd, int depth)
{
	DIR *d;
	struct dirent *e;
	int ret = 0, n = 0, fd = dup(afd);

	if (fd < 0 || !(d = fdopendir(fd))) {
		if (fd >= 0)
			close(fd);
		log_msg("sdgrow: compare: opendir: %s", strerror(errno));
		return -1;
	}
	while (!ret && (e = readdir(d))) {
		struct stat sa, sb;
		long long dt;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		n++;
		if (fstatat(afd, e->d_name, &sa, AT_SYMLINK_NOFOLLOW) ||
		    fstatat(bfd, e->d_name, &sb, AT_SYMLINK_NOFOLLOW)) {
			log_msg("sdgrow: compare: %s: %s", e->d_name, strerror(errno));
			ret = -1;
		} else if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
			log_msg("sdgrow: compare: %s changed type", e->d_name);
			ret = -1;
		} else if (S_ISDIR(sa.st_mode)) {
			int a2 = openat(afd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			int b2 = openat(bfd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

			if (depth >= MAX_DEPTH || a2 < 0 || b2 < 0)
				ret = -1;
			else
				ret = compare_dir(w, a2, b2, depth + 1);
			if (a2 >= 0)
				close(a2);
			if (b2 >= 0)
				close(b2);
			w->t->dirs++;
		} else if (S_ISREG(sa.st_mode)) {
			dt = (long long)sa.st_mtim.tv_sec - sb.st_mtim.tv_sec;
			if (sa.st_size != sb.st_size) {
				log_msg("sdgrow: compare: %s: %lld bytes, expected %lld",
					e->d_name, (long long)sb.st_size,
					(long long)sa.st_size);
				ret = -1;
			} else if (dt > 2 || dt < -2) {
				log_msg("sdgrow: compare: %s: mtime off by %lld s",
					e->d_name, dt);
				ret = -1;
			} else {
				ret = compare_file(w, afd, bfd, e->d_name);
				w->t->files++;
			}
		}
	}
	closedir(d);
	if (!ret && count_entries(bfd) != n) {
		log_msg("sdgrow: compare: %d entries expected, %d found", n,
			count_entries(bfd));
		ret = -1;
	}
	return ret;
}

static int walk_tree(const char *a, const char *b, struct sdgrow_tree *t,
		     int compare)
{
	struct walk w = { .t = t };
	int afd, bfd, ret = -1;

	memset(t, 0, sizeof(*t));
	afd = open(a, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	bfd = open(b, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	w.buf = malloc(CHUNK);
	w.buf2 = compare ? malloc(CHUNK) : NULL;
	if (afd < 0 || bfd < 0)
		log_msg("sdgrow: open %s or %s: %s", a, b, strerror(errno));
	else if (w.buf && (!compare || w.buf2))
		ret = compare ? compare_dir(&w, afd, bfd, 0) :
				copy_dir(&w, afd, bfd, 0);
	if (afd >= 0)
		close(afd);
	if (bfd >= 0)
		close(bfd);
	free(w.buf);
	free(w.buf2);
	return ret;
}

int sdgrow_copy_tree(const char *src, const char *dst, struct sdgrow_tree *t)
{
	return walk_tree(src, dst, t, 0);
}

int sdgrow_compare_tree(const char *a, const char *b, struct sdgrow_tree *t)
{
	return walk_tree(a, b, t, 1);
}

/* ------------------------------------------------------------ the card */

static int card_io(struct grow *g, int wr, uint64_t lba, void *buf, uint32_t n)
{
	return wr ? pwrite_all(g->fd, buf, (size_t)n * 512, lba * 512) :
		    pread_all(g->fd, buf, (size_t)n * 512, lba * 512);
}

static int card_sync(struct grow *g)
{
	return fsync(g->fd) ? errno : 0;
}

/* Test mode: stop as a power loss would (everything so far is synced). */
static void stop_at(struct grow *g, const char *step)
{
	if (g->test && g->stop && !strcmp(g->stop, step)) {
		log_msg("sdgrow: test: stopping at %s", step);
		_exit(3);
	}
}

/* p3's page cache out and dropped, so a mount reads what is on the card. */
static void flush_part(struct grow *g)
{
	int fd;

	if (g->test)
		return;
	fd = open(g->part_dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	fsync(fd);
	ioctl(fd, BLKFLSBUF, 0);
	close(fd);
}

static int run_hook(struct grow *g, const char *cmd, const char *dir)
{
	char off[24];
	pid_t pid;
	int st;

	snprintf(off, sizeof(off), "%llu", (unsigned long long)g->p.start * 512);
	pid = fork();
	if (pid == 0) {
		execl(g->hook, g->hook, "hook", cmd, g->img, off, dir, (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid)
		return -1;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int rm_entry(const char *path, const struct stat *st, int flag,
		    struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	return flag == FTW_DP ? rmdir(path) : unlink(path);
}

static void rm_tree(const char *dir)
{
	nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* p3's file system at dir; 0 or an errno value. The test mode reads it
 * with the hook into dir (read-only) or starts with an empty dir. */
static int fs_mount(struct grow *g, int rw, const char *dir)
{
	if (g->test) {
		rm_tree(dir);
		if (mkdir(dir, 0755))
			return errno;
		return !rw && run_hook(g, "extract", dir) ? EIO : 0;
	}
	flush_part(g);
	if (mkdir(dir, 0700) && errno != EEXIST)
		return errno;
	if (mount(g->part_dev, dir, "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC |
		  MS_NOATIME | (rw ? 0 : MS_RDONLY), "utf8"))
		return errno;
	return 0;
}

static int fs_umount(struct grow *g, int rw, const char *dir)
{
	int err = 0, fd;

	if (g->test)
		return rw && run_hook(g, "inject", dir) ? EIO : 0;
	if (rw) {
		fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd >= 0) {
			if (syncfs(fd))
				err = errno;
			close(fd);
		}
	}
	if (umount2(dir, 0) && !err)
		err = errno;
	/* fsync on the device also flushes the card's cache. */
	flush_part(g);
	rmdir(dir);
	return err;
}

/* p3 that does not mount but starts with 64 KiB of zeros (or erased
 * 0xff) holds nothing to lose. */
static int part_blank(struct grow *g)
{
	if (card_io(g, 0, g->p.start, g->buf, BLANK_SECTORS))
		return 0;
	for (unsigned i = 1; i < BLANK_SECTORS * 512; i++)
		if (g->buf[i] != g->buf[0])
			return 0;
	return g->buf[0] == 0 || g->buf[0] == 0xff;
}

static long long mem_available(void)
{
	char buf[2048], *p;
	int n = read_file("/proc/meminfo", buf, sizeof(buf) - 1);

	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, "MemAvailable:");
	return p ? strtoll(p + 13, NULL, 10) * 1024 : -1;
}

static int copy_setup(struct grow *g, uint64_t used)
{
	char opts[64];

	if (g->test) {
		rm_tree(g->copy_dir);
		return mkdir(g->copy_dir, 0700) ? errno : 0;
	}
	if (mkdir(g->copy_dir, 0700) && errno != EEXIST)
		return errno;
	/* Small files take a page each in a tmpfs. */
	snprintf(opts, sizeof(opts), "size=%lluk,mode=0700",
		 (unsigned long long)((2 * used + (32 << 20)) >> 10));
	if (mount("tmpfs", g->copy_dir, "tmpfs", MS_NOSUID | MS_NODEV, opts))
		return errno;
	g->tmpfs = 1;
	return 0;
}

static void copy_cleanup(struct grow *g)
{
	if (g->tmpfs && umount2(g->copy_dir, 0) == 0) {
		g->tmpfs = 0;
		rmdir(g->copy_dir);
	}
}

/* Rewrite the p3 entry with size (start and type stay) and make the
 * kernel see it. */
static int set_p3(struct grow *g, const uint8_t *entry, uint32_t size)
{
	uint8_t *e = g->mbr + MBR_P3;
	int err;

	if (entry) {
		memcpy(e, entry, 16);
	} else {
		uint32_t last = g->p.start + size - 1;
		uint32_t cyl = last / (255 * 63), head = last / 63 % 255;
		uint32_t sec = last % 63 + 1;

		if (cyl > 1023) {
			cyl = 1023;
			head = 254;
			sec = 63;
		}
		e[5] = head;
		e[6] = sec | (cyl >> 2 & 0xc0);
		e[7] = cyl;
		put32(e + 12, size);
	}
	memcpy(g->buf, g->mbr, 512);
	if ((err = card_io(g, 1, 0, g->buf, 1)) || (err = card_sync(g)))
		return err;
	g->p.size = size;
	if (!g->test) {
		struct blkpg_partition bp = {
			.start = (long long)g->p.start * 512,
			.length = (long long)size * 512,
			.pno = 3,
		};
		struct blkpg_ioctl_arg a = {
			.op = BLKPG_RESIZE_PARTITION,
			.datalen = sizeof(bp),
			.data = &bp,
		};
		char path[96];
		long now;

		if (ioctl(g->fd, BLKPG, &a))
			return errno;
		snprintf(path, sizeof(path), "/sys/class/block/%s/size", g->part);
		now = read_sysfs_long(path, -1);
		log_msg("sdgrow: kernel sees %s with %ld sectors", g->part, now);
		if (now != (long)size)
			return EIO;
	}
	return 0;
}

static int hdr_write(struct grow *g, struct hdr *h)
{
	int err;

	memset(g->buf, 0, HDR_SECTORS * 512);
	if (h) {
		memcpy(h->magic, HDR_MAGIC, 8);
		h->version = HDR_VERSION;
		h->crc = sdgrow_crc32(0, h, offsetof(struct hdr, crc));
		memcpy(g->buf, h, sizeof(*h));
	}
	if ((err = card_io(g, 1, g->p.hdr_lba, g->buf, HDR_SECTORS)))
		return err;
	return card_sync(g);
}

/* A header of ours for this card and p3: an attempt is in progress. */
static int hdr_read(struct grow *g, struct hdr *h)
{
	const struct sdgrow_plan *p = &g->p;

	if (card_io(g, 0, p->hdr_lba, g->buf, HDR_SECTORS))
		return 0;
	memcpy(h, g->buf, sizeof(*h));
	return !memcmp(h->magic, HDR_MAGIC, 8) && h->version == HDR_VERSION &&
	       h->crc == sdgrow_crc32(0, h, offsetof(struct hdr, crc)) &&
	       h->card == p->card && h->start == p->start &&
	       h->hdr_lba == p->hdr_lba && h->new_size == p->end - p->start &&
	       h->backup_lba + h->backup_sectors == h->hdr_lba &&
	       h->backup_sectors >= h->old_size &&
	       (uint64_t)h->start + h->old_size <= h->backup_lba &&
	       (p->size == h->old_size || p->size == h->new_size);
}

/* The old p3 back from the backup, then its MBR entry; the header is
 * cleared or, with give_up, kept as "gave up". */
static int restore(struct grow *g, struct hdr *h, int give_up)
{
	uint32_t n = h->old_size, written = 0, crc = 0;
	int64_t t0 = now_ns();
	int err;

	log_msg("sdgrow: restore: checking the backup of the old p3 (%u MiB "
		"at %u MiB)", MIB(n), MIB(h->backup_lba));
	for (uint32_t i = 0; i < n; i += CHUNK_SECTORS) {
		uint32_t k = n - i < CHUNK_SECTORS ? n - i : CHUNK_SECTORS;

		if ((err = card_io(g, 0, h->backup_lba + i, g->buf, k))) {
			log_msg("sdgrow: restore: read: %s", strerror(err));
			return -1;
		}
		crc = sdgrow_crc32(crc, g->buf, (size_t)k * 512);
	}
	if (crc != h->backup_crc) {
		log_msg("sdgrow: restore: the backup is damaged (crc %08x, "
			"expected %08x), leaving the card as it is", crc,
			h->backup_crc);
		return -1;
	}
	for (uint32_t i = 0; i < n; i += CHUNK_SECTORS) {
		uint32_t k = n - i < CHUNK_SECTORS ? n - i : CHUNK_SECTORS;

		if ((err = card_io(g, 0, h->backup_lba + i, g->buf, k)) ||
		    (err = card_io(g, 0, h->start + i, g->buf2, k)))
			goto fail;
		if (memcmp(g->buf, g->buf2, (size_t)k * 512)) {
			if ((err = card_io(g, 1, h->start + i, g->buf, k)))
				goto fail;
			written += k;
		}
		if (i >= n / 2)
			stop_at(g, "restore-half");
	}
	if ((err = card_sync(g)))
		goto fail;
	log_msg("sdgrow: restore: %u of %u MiB differed and were written, "
		"%lld ms", MIB(written), MIB(n), (long long)ms_since(t0));
	if (g->p.size != h->old_size &&
	    (err = set_p3(g, h->old_entry, h->old_size)))
		goto fail;
	/* Without give_up the header stays: its attempt count goes on. */
	if (give_up) {
		h->phase = PHASE_GAVE_UP;
		if ((err = hdr_write(g, h)))
			goto fail;
	}
	log_msg("sdgrow: restore: old p3 back (%u MiB at %u MiB)%s, %lld ms",
		MIB(h->old_size), MIB(h->start),
		give_up ? ", not trying again" : "", (long long)ms_since(t0));
	return 0;
fail:
	log_msg("sdgrow: restore failed: %s", strerror(err));
	return -1;
}

/* The old p3 raw into the backup area; returns its CRC through crc. */
static int backup(struct grow *g, uint32_t lba, uint32_t n, uint32_t *crc)
{
	int err;

	*crc = 0;
	for (uint32_t i = 0; i < n; i += CHUNK_SECTORS) {
		uint32_t k = n - i < CHUNK_SECTORS ? n - i : CHUNK_SECTORS;

		if ((err = card_io(g, 0, g->p.start + i, g->buf, k)) ||
		    (err = card_io(g, 1, lba + i, g->buf, k)))
			return err;
		*crc = sdgrow_crc32(*crc, g->buf, (size_t)k * 512);
	}
	return card_sync(g);
}

/* Giving up before the MBR changed: nothing to undo; the header goes so
 * the next boot checks again. */
static int leave(struct grow *g, int ret)
{
	hdr_write(g, NULL);
	copy_cleanup(g);
	return ret;
}

static int grow(struct grow *g, uint32_t attempt)
{
	struct sdgrow_plan *p = &g->p;
	uint32_t old_size = p->size, new_size = p->end - p->start;
	uint32_t backup_sectors = (old_size + 7) & ~7u;
	uint32_t backup_lba = p->hdr_lba - backup_sectors, volume_id;
	struct sdgrow_tree t = { 0 }, t2;
	struct fat32_geom fg;
	struct hdr h;
	uint64_t used = 0, need;
	int64_t t0 = now_ns(), t1;
	const char *step;
	int err, empty = 0;

	log_msg("sdgrow: attempt %u: card %s %llu MiB, p3 at %u MiB with %u MiB,"
		" growing it to %u MiB", attempt, g->disk, (unsigned long long)
		MIB(p->card), MIB(p->start), MIB(old_size), MIB(new_size));
	if (g->show)
		g->show("Growing the GBA partition to fill the SD card. This "
			"runs once. Do not turn off.");

	/* 0. the attempt counted, so a crash in what follows cannot loop */
	memset(&h, 0, sizeof(h));
	h.phase = PHASE_STARTED;
	h.attempt = attempt;
	h.start = p->start;
	h.old_size = old_size;
	h.new_size = new_size;
	h.hdr_lba = p->hdr_lba;
	h.backup_lba = backup_lba;
	h.backup_sectors = backup_sectors;
	h.card = p->card;
	if (getrandom(&h.nonce, sizeof(h.nonce), GRND_NONBLOCK) < 0)
		h.nonce = (uint64_t)now_ns();
	memcpy(h.old_entry, g->mbr + MBR_P3, 16);
	if (p->start + old_size > backup_lba) {
		log_msg("sdgrow: not enough free space for the backup, not growing");
		return 1;
	}
	if ((err = hdr_write(g, &h))) {
		log_msg("sdgrow: header: %s, not growing", strerror(err));
		return 2;
	}

	/* 1. the files into memory */
	t1 = now_ns();
	if ((err = fs_mount(g, 0, g->old_dir))) {
		if (!part_blank(g)) {
			log_msg("sdgrow: p3 does not mount (%s) and is not blank, "
				"leaving it alone", strerror(err));
			return leave(g, 1);
		}
		log_msg("sdgrow: p3 is blank (%s), formatting it empty",
			strerror(err));
		empty = 1;
	} else if (!g->test) {
		struct statvfs sv;
		long long avail = mem_available();

		if (!statvfs(g->old_dir, &sv))
			used = (uint64_t)(sv.f_blocks - sv.f_bfree) * sv.f_frsize;
		log_msg("sdgrow: old p3: %llu KiB used, %lld KiB of RAM available",
			(unsigned long long)(used >> 10), avail >> 10);
		if (avail < 0 || 2 * used + (32 << 20) > (uint64_t)avail / 2) {
			log_msg("sdgrow: not enough RAM for the copy, not growing");
			fs_umount(g, 0, g->old_dir);
			return leave(g, 1);
		}
	}
	if ((err = copy_setup(g, used))) {
		log_msg("sdgrow: tmpfs: %s", strerror(err));
		if (!empty)
			fs_umount(g, 0, g->old_dir);
		return leave(g, 1);
	}
	if (!empty) {
		err = sdgrow_copy_tree(g->old_dir, g->copy_dir, &t);
		fs_umount(g, 0, g->old_dir);
		if (err) {
			log_msg("sdgrow: copying the files out failed, not growing");
			return leave(g, 1);
		}
	}
	log_msg("sdgrow: copied %u files in %u directories, %llu bytes into "
		"memory in %lld ms%s", t.files, t.dirs,
		(unsigned long long)t.bytes, (long long)ms_since(t1),
		t.skipped ? " (some entries skipped)" : "");
	stop_at(g, "copied");

	/* The new file system must leave the backup alone. */
	if (fat32_geometry(p->start, new_size, &fg)) {
		log_msg("sdgrow: no FAT32 layout for %u MiB, not growing",
			MIB(new_size));
		return leave(g, 1);
	}
	need = (t.bytes + 511) / 512 + (uint64_t)(t.files + t.dirs + 1) * fg.spc;
	if ((uint64_t)p->start + fg.data + need + MARGIN_SECTORS > backup_lba) {
		log_msg("sdgrow: not enough free space for the backup, not growing");
		return leave(g, 1);
	}
	log_msg("sdgrow: new p3: FAT32, %u KiB clusters, %u clusters, FATs of "
		"%u sectors, data area at sector %u", fg.spc / 2, fg.clusters,
		fg.fat_sectors, p->start + fg.data);

	/* 2. raw backup, 3. header */
	t1 = now_ns();
	if ((err = backup(g, backup_lba, old_size, &h.backup_crc))) {
		log_msg("sdgrow: backup: %s, not growing", strerror(err));
		return leave(g, 2);
	}
	log_msg("sdgrow: raw backup of the old p3 at %u MiB, crc %08x, %lld ms",
		MIB(backup_lba), h.backup_crc, (long long)ms_since(t1));
	stop_at(g, "backup");
	h.phase = PHASE_BACKED_UP;
	if ((err = hdr_write(g, &h))) {
		log_msg("sdgrow: header: %s, not growing", strerror(err));
		return leave(g, 2);
	}
	stop_at(g, "header");

	/* 4. MBR */
	step = "the partition table change";
	if ((err = set_p3(g, NULL, new_size)))
		goto fail;
	log_msg("sdgrow: MBR: p3 now %u sectors (%u MiB), sectors %u..%u",
		new_size, MIB(new_size), p->start, p->end - 1);
	stop_at(g, "mbr");

	/* 5. format */
	step = "formatting";
	t1 = now_ns();
	if (g->test && g->stop && !strcmp(g->stop, "format-half")) {
		memset(g->buf, 0x5a, CHUNK);
		card_io(g, 1, p->start, g->buf, CHUNK_SECTORS);
		card_sync(g);
		stop_at(g, "format-half");
	}
	if (getrandom(&volume_id, sizeof(volume_id), GRND_NONBLOCK) < 0)
		volume_id = (uint32_t)now_ns();
	if ((err = fat32_format(g->fd, p->start, &fg, volume_id, "GBA")))
		goto fail;
	log_msg("sdgrow: formatted p3 FAT32, label GBA, volume id %04X-%04X, "
		"%lld ms", volume_id >> 16, volume_id & 0xffff,
		(long long)ms_since(t1));
	stop_at(g, "format");

	/* 6. the files back */
	if (g->show)
		g->show("Copying the files back. Do not turn off.");
	step = "copying back";
	t1 = now_ns();
	if ((err = fs_mount(g, 1, g->new_dir)))
		goto fail;
	if (sdgrow_copy_tree(g->copy_dir, g->new_dir, &t2)) {
		err = EIO;
		fs_umount(g, 1, g->new_dir);
		goto fail;
	}
	if ((err = fs_umount(g, 1, g->new_dir)))
		goto fail;
	log_msg("sdgrow: copied %u files, %llu bytes back in %lld ms", t2.files,
		(unsigned long long)t2.bytes, (long long)ms_since(t1));
	stop_at(g, "copy-in");

	/* 7. check, from the card */
	step = "the check";
	t1 = now_ns();
	if ((err = fs_mount(g, 0, g->check_dir)))
		goto fail;
	err = sdgrow_compare_tree(g->copy_dir, g->check_dir, &t2) ? EIO : 0;
	fs_umount(g, 0, g->check_dir);
	if (!err && g->test && g->fail && !strcmp(g->fail, "verify")) {
		log_msg("sdgrow: test: failing the check");
		err = EIO;
	}
	if (err)
		goto fail;
	log_msg("sdgrow: checked %u files, %llu bytes on the new p3 in %lld ms",
		t2.files, (unsigned long long)t2.bytes, (long long)ms_since(t1));
	stop_at(g, "verified");

	/* 8. done */
	if ((err = hdr_write(g, NULL)))
		log_msg("sdgrow: clearing the header: %s", strerror(err));
	copy_cleanup(g);
	log_msg("sdgrow: done: p3 grown from %u MiB to %u MiB, %u files kept, "
		"%lld ms in all", MIB(old_size), MIB(new_size), t.files,
		(long long)ms_since(t0));
	return 0;

fail:
	log_msg("sdgrow: %s failed (%s), restoring the old p3", step,
		strerror(err));
	snprintf(h.reason, sizeof(h.reason), "%.31s", step);
	if (g->show)
		g->show("That did not work. Restoring the old partition. Do not "
			"turn off.");
	restore(g, &h, 1);
	copy_cleanup(g);
	return 2;
}

/* 0: nothing to do or grown, 1: left alone, 2: failed or gave up. */
static int run(struct grow *g)
{
	struct sdgrow_plan *p = &g->p;
	uint32_t attempt = 1, end;
	char why[64];
	struct hdr h;
	int err, grown;

	if ((err = card_io(g, 0, 0, g->buf, 1))) {
		log_msg("sdgrow: reading the MBR: %s", strerror(err));
		return 1;
	}
	memcpy(g->mbr, g->buf, 512);
	if (sdgrow_plan(g->mbr, p->card, p, why, sizeof(why))) {
		log_msg("sdgrow: %s, leaving the card as it is", why);
		return 1;
	}
	end = p->start + p->size;
	grown = end == p->end;
	if (!grown && (end > p->end || p->end - end < MIN_FREE_SECTORS)) {
		log_msg("sdgrow: card %llu MiB, p3 at %u MiB with %u MiB, %u MiB "
			"free after it: nothing to do", (unsigned long long)
			MIB(p->card), MIB(p->start), MIB(p->size),
			end < p->end ? MIB(p->end - end) : 0);
		return 0;
	}
	if (hdr_read(g, &h)) {
		if (p->size == h.new_size && h.phase != PHASE_BACKED_UP) {
			/* The MBR is only changed after the backup. */
			log_msg("sdgrow: p3 grown but the header is in phase %u, "
				"leaving the card as it is", h.phase);
			return 2;
		}
		if (p->size == h.new_size) {
			log_msg("sdgrow: attempt %u was interrupted after the MBR "
				"change", h.attempt);
			if (g->show)
				g->show("The last attempt was interrupted. Restoring "
					"the old partition. Do not turn off.");
			if (h.attempt >= MAX_ATTEMPTS)
				snprintf(h.reason, sizeof(h.reason), "%u interruptions",
					 h.attempt);
			if (restore(g, &h, h.attempt >= MAX_ATTEMPTS))
				return 2;
		} else if (h.phase == PHASE_GAVE_UP) {
			log_msg("sdgrow: gave up in attempt %u (%s), not growing",
				h.attempt, h.reason);
			return 2;
		} else {
			log_msg("sdgrow: attempt %u was interrupted before the MBR "
				"change, the old p3 is untouched", h.attempt);
		}
		if (h.attempt >= MAX_ATTEMPTS) {
			if (h.phase != PHASE_GAVE_UP) {
				h.phase = PHASE_GAVE_UP;
				snprintf(h.reason, sizeof(h.reason), "%u interruptions",
					 h.attempt);
				hdr_write(g, &h);
			}
			log_msg("sdgrow: %u attempts interrupted, not trying again",
				h.attempt);
			return 2;
		}
		attempt = h.attempt + 1;
	} else if (grown) {
		log_msg("sdgrow: p3 (%u MiB at %u MiB) fills the card (%llu MiB): "
			"nothing to do", MIB(p->size), MIB(p->start),
			(unsigned long long)MIB(p->card));
		return 0;
	}
	return grow(g, attempt);
}

static int grow_alloc(struct grow *g)
{
	if (posix_memalign((void **)&g->buf, 4096, CHUNK) ||
	    posix_memalign((void **)&g->buf2, 4096, CHUNK)) {
		log_msg("sdgrow: out of memory");
		return -1;
	}
	return 0;
}

static void grow_free(struct grow *g)
{
	free(g->buf);
	free(g->buf2);
	if (g->fd >= 0)
		close(g->fd);
}

/*
 * The card is the disk of the root partition: /sys/dev/block/<root dev>
 * resolves to .../block/mmcblk0/mmcblk0p2; p3 is the sibling whose
 * "partition" attribute is 3.
 */
static void find_card(struct grow *g)
{
	char link[64], path[600], *real = NULL, *slash;
	struct stat st;
	struct dirent *e;
	DIR *d = NULL;

	snprintf(g->disk, sizeof(g->disk), "mmcblk0");
	snprintf(g->part, sizeof(g->part), "mmcblk0p3");
	if (stat("/", &st))
		goto fallback;
	snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(st.st_dev),
		 minor(st.st_dev));
	real = realpath(link, NULL);
	if (!real || !(slash = strrchr(real, '/')))
		goto fallback;
	*slash = 0;
	slash = strrchr(real, '/');
	if (!slash || strlen(slash + 1) >= sizeof(g->disk) ||
	    strlen(real) > 300 || !(d = opendir(real)))
		goto fallback;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(g->part))
			continue;
		snprintf(path, sizeof(path), "%s/%s", real, e->d_name);
		if (read_attr_long(path, "partition", 0) == 3) {
			snprintf(g->disk, sizeof(g->disk), "%s", slash + 1);
			snprintf(g->part, sizeof(g->part), "%s", e->d_name);
			closedir(d);
			free(real);
			return;
		}
	}
fallback:
	if (d)
		closedir(d);
	free(real);
	log_msg("sdgrow: card not found from the root device, assuming %s",
		g->disk);
}

void sdgrow_boot(sdgrow_show_fn show)
{
	struct grow g = { .fd = -1, .show = show };
	char path[96];
	long lbs;

	if (!fe_is_init)
		return;
	find_card(&g);
	snprintf(g.disk_dev, sizeof(g.disk_dev), "/dev/%s", g.disk);
	snprintf(g.part_dev, sizeof(g.part_dev), "/dev/%s", g.part);
	snprintf(g.old_dir, sizeof(g.old_dir), "/tmp/sdgrow-old");
	snprintf(g.new_dir, sizeof(g.new_dir), "/tmp/sdgrow-new");
	snprintf(g.check_dir, sizeof(g.check_dir), "/tmp/sdgrow-new");
	snprintf(g.copy_dir, sizeof(g.copy_dir), "/tmp/sdgrow-copy");
	snprintf(path, sizeof(path), "/sys/class/block/%s/size", g.disk);
	g.p.card = (uint64_t)read_sysfs_long(path, 0);
	snprintf(path, sizeof(path), "/sys/class/block/%s/queue/logical_block_size",
		 g.disk);
	lbs = read_sysfs_long(path, 512);
	if (!g.p.card || lbs != 512) {
		log_msg("sdgrow: %s: size %llu sectors, logical block %ld bytes, "
			"not growing", g.disk, (unsigned long long)g.p.card, lbs);
		return;
	}
	g.fd = open(g.disk_dev, O_RDWR | O_DIRECT | O_CLOEXEC);
	if (g.fd < 0) {
		log_msg("sdgrow: open %s: %s", g.disk_dev, strerror(errno));
		return;
	}
	if (!grow_alloc(&g))
		run(&g);
	grow_free(&g);
}

int sdgrow_image(const char *img, const char *scratch, const char *hook)
{
	struct grow g = { .fd = -1, .test = 1, .img = img, .hook = hook };
	struct stat st;
	int ret = 1;

	g.stop = getenv("FE_SDGROW_STOP");
	g.fail = getenv("FE_SDGROW_FAIL");
	snprintf(g.disk, sizeof(g.disk), "image");
	snprintf(g.old_dir, sizeof(g.old_dir), "%s/old", scratch);
	snprintf(g.new_dir, sizeof(g.new_dir), "%s/new", scratch);
	snprintf(g.check_dir, sizeof(g.check_dir), "%s/check", scratch);
	snprintf(g.copy_dir, sizeof(g.copy_dir), "%s/copy", scratch);
	mkdir(scratch, 0755);
	g.fd = open(img, O_RDWR | O_CLOEXEC);
	if (g.fd < 0 || fstat(g.fd, &st)) {
		log_msg("sdgrow: open %s: %s", img, strerror(errno));
		grow_free(&g);
		return 1;
	}
	g.p.card = (uint64_t)st.st_size / 512;
	if (!grow_alloc(&g))
		ret = run(&g);
	grow_free(&g);
	return ret;
}
