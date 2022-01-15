/*
	raw2imd: read a disk image file and create IMD

	Cloned from dumpfloppy.c Mar 29, 2021 Douglas Miller <durgadas311@gmail.com>

	Origins credits:
	Copyright (C) 2013 Adam Sampson <ats@offog.org>

	Permission to use, copy, modify, and/or distribute this software for
	any purpose with or without fee is hereby granted, provided that the
	above copyright notice and this permission notice appear in all
	copies.

	THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
	WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
	WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
	AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
	DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
	PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
	TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
	PERFORMANCE OF THIS SOFTWARE.
*/

/*
	The techniques used here are based on the "How to identify an
	unknown disk" document from the fdutils project:
	  http://www.fdutils.linux.lu/disk-id.html
*/

/*
 * simulator "logdisk" format e.g.
 * "5m512z9p2s80t1d0i1l0h\n":
 *	5" drive
 *	512-byte sectors
 *	9 spt
 *	2 sides
 *	80 tracks (cylinders)
 *	1 density (DD)
 *	0 interlace (side 1 placement)
 *	1 logical skew (not relevant)
 *	0 hard sectors (i.e. soft sectored)
 */

#include "disk.h"
#include "imd.h"
#include "util.h"
#include "show.h"

/* derived from disk.c */
#define MFM_250K	0	// 5.25" DD
#define FM_250K		1	// 5.25" SD
#define MFM_300K	2	// (DD media in 5.25" HD drives)
#define FM_300K		3	// ('')
#define MFM_500K	4	// 8" DD (5.25" HD, 3.5" HD)
#define FM_500K		5	// 8" SD (5.25" HD, 3.5" HD)
#define MFM_1000K	6	// 3.5" ED

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "IMD"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.18"
#endif

static struct args {
	int cylinders;
	int heads;
	int sectors;
	int length;	// sector length
	int length_code;
	int size;
	int mfm;
	int dmode;	// data mode, for DATA_MODES[]
	int policy;	// 2-side policy
	int *sectbl;	// physical skew table
	int *sectbl2;	// side 2
	int offset1;	// first sector number/offset
	int offset2;	// side 2
	int force;
	int ignore;
	bool read_comment;
	const char *title;
	const char *imd_filename;
	const char *image_filename;
	int logdisk;
	int verbose;
} args;

static int dev_fd;

static int snoop_media(const char *file) {
	int e;
	char buf[128];
	int fd = open(file, O_RDONLY);
	if (fd < 0) return -1;
	e = lseek(fd, -128L, SEEK_END);
	if (e < 0) goto out;
	e = read(fd, buf, sizeof(buf));
	if (e < 0) goto out;
	e = 0;
	while (buf[e] != '\n' && buf[e] != '\0' && e < sizeof(buf)) {
		int p = 0;
		char *end;
		p = strtoul(&buf[e], &end, 10);
		switch (*end) {
		case 'm':
			args.size = p;
			break;
		case 'z':
			args.length = p;
			break;
		case 'p':
			args.sectors = p;
			break;
		case 's':
			args.heads = p;
			break;
		case 't':
			args.cylinders = p;
			break;
		case 'd':
			args.mfm = p;
			break;
		case 'i':
// Policy for second side:
// 0: wrap        0.0 1.0 2.0 ...39.0 39.1 38.1 37.1...
// 1: interlace   0.0 0.1 1.0 1.1 ...
// 2: kaypro, interlace but side 1 sectors start at 10
//    (also, first sector is "0")
			args.policy = p;
			break;
		case 'l':
			// args.skew = p; // logical skew - not used
			break;
		case 'h':
			// ignore hard-sectoring?
			break;
		default:
			errno = EINVAL;
			e = -1;
			goto out;
		}
		e = (end - buf) + 1;
	}
	if (args.policy == 2) { // special-case for Kaypro
		if (args.offset1 < 0) args.offset1 = 0;
		if (args.offset2 < 0) args.offset2 = args.sectors;
	} else {
		if (args.offset1 < 0) args.offset1 = 1;
		// args.offset2 corrected later, if needed...
	}
	e = 0;
out:
	close(fd);
	return e;
}

static void read_track(track_t *track, int cyl, int hd, int fd) {
	int s;
	// for now, assume image is:
	// cyl 0 hd 0 sec 0-n
	// cyl 0 hd 1 sec 0-n
	// cyl 1 hd 0 sec 0-n
	// ...
#if 0
	off_t o = lseek(fd, 0L, SEEK_CUR);
	o = o / args.length;
	o = o / args.sectors;
	int cyl = o / args.heads;
	int hd = o % args.heads;
#endif
	if (args.policy == 0) { // "continuation" side 0 first, then side 1
		off_t o;
		//o = (cyl * args.heads + hd) * args.sectors * args.length;
		o = (hd * args.cylinders + cyl) * args.sectors * args.length;
		o = lseek(fd, o, SEEK_SET);
		if (o < 0) {
			perror(args.image_filename);
			exit(1);
		}
	}
	track->data_mode = &DATA_MODES[args.dmode];
	track->phys_cyl = cyl;
	track->phys_head = hd;
	track->num_sectors = args.sectors;
	track->sector_size_code = args.length_code;
	track->status = TRACK_PROBED;
	for (s = 0; s < args.sectors; ++s) {
		int sn = s;	// assume no skew (1:1)
		if (hd > 0 && args.sectbl2 != NULL) {
			sn = args.sectbl2[s];
		} else if (args.sectbl != NULL) {
			sn = args.sectbl[s];
		}
		sector_t *sec = &track->sectors[sn];
		sec->log_cyl = track->phys_cyl;
		if (args.policy == 2) {
			sec->log_head = 0;	// Kaypro special
		} else {
			sec->log_head = track->phys_head;
		}
		// TODO: more complex translations?
		// raw images have sectors in numerical order...
		sec->log_sector = s + (hd ? args.offset2 : args.offset1);
		sec->deleted = false;
		sec->status = SECTOR_GOOD;
		sec->data = malloc(args.length);
		if (sec->data == NULL) {
			perror("malloc");
			exit(1);
		}
		int n = read(fd, sec->data, args.length);
		if (n < 0) {
			perror(args.image_filename);
			exit(1);
		}
	}
}

static void process_raw(void) {
	struct stat stb;

	dev_fd = open(args.image_filename, O_RDONLY);
	if (dev_fd < 0) {
		die_errno("cannot open %s", args.image_filename);
	}
	fstat(dev_fd, &stb);
	int cap = args.cylinders * args.heads * args.sectors * args.length;
	if (args.logdisk) {
		stb.st_size -= 128;
	}
	if (!args.ignore && stb.st_size > cap) {
		die("image file too large: %s", args.image_filename);
	}
	if (!args.force && stb.st_size < cap) {
		die("image file too small: %s", args.image_filename);
	}

	disk_t disk;
	init_disk(&disk);
	make_disk_comment(PACKAGE_NAME, PACKAGE_VERSION, &disk);

	if (args.title != NULL) {
		alloc_append(args.title, strlen(args.title),
				&disk.comment, &disk.comment_len);
	}
	if (args.read_comment) {
		if (isatty(0)) {
			fprintf(stderr, "Enter comment, terminated by EOF\n");
		}

		while (true) {
			char buf[4096];
			ssize_t count = read(0, buf, sizeof buf);
			if (count == 0) break;
			if (count < 0) {
				die("read from stdin failed");
			}

			alloc_append(buf, count, &disk.comment, &disk.comment_len);
		}
	}

	disk.num_phys_cyls = args.cylinders;
	disk.num_phys_heads = args.heads;

	FILE *image = NULL;
	if (args.imd_filename != NULL) {
		// FIXME: if the image exists already, load it
		// (so the comment is preserved)

		image = fopen(args.imd_filename, "wb");
		if (image == NULL) {
			die_errno("cannot open %s", args.imd_filename);
		}

		write_imd_header(&disk, image);
	}

	// FIXME: retry disk if not complete -- option for number of retries
	// FIXME: if retrying, ensure we've moved the head across the disk
	// FIXME: if retrying, turn the motor off and on (delay? close?)
	// FIXME: pull this out to a read_disk function
	for (int cyl = 0; cyl < disk.num_phys_cyls; cyl++) {
		for (int head = 0; head < disk.num_phys_heads; head++) {
			track_t *track = &(disk.tracks[cyl][head]);

			read_track(track, cyl, head, dev_fd);

			if (image != NULL) {
				write_imd_track(track, image);
				fflush(image);
			}
		}
	}

	if (image != NULL) {
		fclose(image);
	}
	close(dev_fd);
	if (args.verbose) {
		show_disk(&disk, args.verbose > 1, stdout);
	}
	free_disk(&disk);
}

/*
 * Creates the physical sector skew table needed to
 * determine track->sectors[x] when reading sequential
 * sectors from the raw image file.
 * Raw image files contain physical sectors in order:
 * 1,2,3,4,5,6,7,8,9 (spt=9)
 * This table places the sectors into the actual order
 * they should appear on the disk, e.g. with skew=4:
 * 1,8,6,4,2,9,7,5,3 (spt=9)
 * when using &track->sectors[tbl[s]] (s: 0 <= s < spt).
 */
static int *mkskew(int skew, int secs) {
	int *tbl = malloc(secs * sizeof(int));
	int *tbl_out = malloc(secs * sizeof(int));
	memset(tbl, 0xff, secs * sizeof(int)); // -1
	int nudge = 1;
	int sk = skew;
	if (sk < 0) {
		sk = -sk;
		nudge = -1;
	}
	int s;
	for (s = 0; s < secs; ++s) {
		int sn = s;
		sn = (s * sk) % secs;
		// handle skew that is factor of secs
		while (tbl[sn] >= 0) {
			sn += nudge;
			if (sn < 0) sn = secs - 1;
			if (sn >= secs) sn = 0;
		}
		tbl[sn] = s;
		tbl_out[s] = sn;
	}
	free(tbl);
	if (0) {
		printf("skew(%d) = {", skew);
		for (s = 0; s < secs; ++s) {
			printf(" %d", tbl_out[s]);
		}
		printf(" }\n");
	}
	return tbl_out;
}

static void usage(void) {
	fprintf(stderr, "usage: raw2imd [OPTION]... RAW-FILE [IMAGE-FILE]\n");
	fprintf(stderr, "  -5		 RAW-FILE is 5.25\" diskette (default)\n");
	fprintf(stderr, "  -8		 RAW-FILE is 8\" diskette\n");
	fprintf(stderr, "  -c NUM	 number of cylinders\n");
	fprintf(stderr, "  -h NUM	 number of heads (sides)\n");
	fprintf(stderr, "  -s NUM	 number of sectors/track\n");
	fprintf(stderr, "  -l NUM	 sector length\n");
	fprintf(stderr, "  -m		 RAW-FILE is MFM (i.e. double density)\n");
	fprintf(stderr, "  -r NUM	 override data rate [250,300,500]\n");
	fprintf(stderr, "  -L		 RAW-FILE is logdisk format (has geom)\n");
	fprintf(stderr, "  -o		 sector number offset (1)\n");
	fprintf(stderr, "  -O		 side 1 sector number offset (-o)\n");
	fprintf(stderr, "  -k NUM	 physical sector skew (1)\n");
	fprintf(stderr, "  -K NUM	 side 1 physical skew (-k)\n");
	fprintf(stderr, "  -i		 ignore excess data in RAW-FILE\n");
	fprintf(stderr, "  -f		 force using smaller RAW-FILE\n");
	fprintf(stderr, "  -C		 read comment from stdin\n");
	fprintf(stderr, "  -T STR	 use STR as comment\n");
	fprintf(stderr, "  -v		 verbose output (multiple)\n");
}

int main(int argc, char **argv) {
	int x;
	int skew = -1;
	int skew2 = -1;
	int data_rate = -1;

	dev_fd = -1;
	args.cylinders = -1;
	args.heads = -1;
	args.sectors = -1;
	args.length = -1;
	args.size = -1;
	args.offset1 = -1;
	args.offset2 = -1;
	args.mfm = 0;	// need numeric values 0/1
	args.dmode = -1; // index into DATA_MODES[]
	args.policy = 1; // default to "interlaced"
	args.sectbl = NULL;
	args.sectbl2 = NULL;
	args.force = false;
	args.ignore = false;
	args.read_comment = false;
	args.title = NULL;
	args.imd_filename = NULL;
	args.image_filename = NULL;
	args.logdisk = false;
	args.verbose = 0;

	while (true) {
		int opt = getopt(argc, argv, "58p:c:h:s:l:o:O:mr:ifCT:Lk:K:v");
		if (opt == -1) break;

		switch (opt) {
		case '5':
			args.size = 5;
			break;
		case '8':
			args.size = 8;
			break;
		case 'p':
			args.policy = atoi(optarg);
			break;
		case 'c':
			args.cylinders = atoi(optarg);
			break;
		case 'h':
			args.heads = atoi(optarg);
			break;
		case 's':
			args.sectors = atoi(optarg);
			break;
		case 'l':
			args.length = atoi(optarg);
			break;
		case 'o':
			args.offset1 = atoi(optarg);
			break;
		case 'O':
			args.offset2 = atoi(optarg);
			break;
		case 'm':
			args.mfm = 1;
			break;
		case 'r':	// data rate (250/300/500kbps)
			data_rate = atoi(optarg);
			if (data_rate != 250 && data_rate != 300 &&
					data_rate != 500 && data_rate != 1000) {
				goto error;
			}
			break;
		case 'i':
			args.ignore = true;
			break;
		case 'f':
			args.force = true;
			break;
		case 'C':
			args.read_comment = true;
			break;
		case 'T':
			args.title = optarg;
			break;
		case 'L':
			args.logdisk = true;
			break;
		case 'k':
			skew = atoi(optarg);
			break;
		case 'K':
			skew2 = atoi(optarg);
			break;
		case 'v':
			++args.verbose;
			break;
		default:
error:
			usage();
			return 1;
		}
	}

	x = optind;
	if (x == argc) {
		// raw file missing - or no arguments
		usage();
		return 0;
	}
	args.image_filename = argv[x++];
	if (x == argc) {
		// No image file.
	} else if (x + 1 == argc) {
		args.imd_filename = argv[x++];
	} else {
		usage();
		return 1;
	}

	if (args.logdisk) {
		x = snoop_media(args.image_filename);
		if (x < 0) {
			perror(args.image_filename);
			exit(1);
		}
	}
	if (args.cylinders < 0 || args.heads < 0 ||
			args.sectors < 0 || args.length < 0) {
		usage();
		return 1;
	}
	switch (args.length) {
	case 128: args.length_code = 0; break;
	case 256: args.length_code = 1; break;
	case 512: args.length_code = 2; break;
	case 1024: args.length_code = 3; break;
	default:
		usage();
		return 1;
	}
	if (args.size < 0) {
		args.size = 5;
	}
	if (data_rate < 0) {
		if (args.size == 8) {
			args.dmode = args.mfm ? MFM_500K : FM_500K;
		} else if (args.size == 5) {
			args.dmode = args.mfm ? MFM_250K : FM_250K;
		} else {
			args.dmode = MFM_250K; // punt
		}
	} else switch (data_rate) {
		case 250:
			args.dmode = args.mfm ? MFM_250K : FM_250K;
			break;
		case 300:
			args.dmode = args.mfm ? MFM_300K : FM_300K;
			break;
		case 500:
			args.dmode = args.mfm ? MFM_500K : FM_500K;
			break;
		case 1000:
			args.dmode = MFM_1000K;
			args.mfm = 1;
			break;
	}
	if (args.offset1 < 0) {
		args.offset1 = 1; // default to industry-standard
	}
	if (args.offset2 < 0) {
		args.offset2 = args.offset1;
	}
	if (abs(skew) > 1) { // physical skew - if specified
		args.sectbl = mkskew(skew, args.sectors);
	}
	if (abs(skew2) > 1) {
		args.sectbl2 = mkskew(skew2, args.sectors);
	}

	process_raw();

	return 0;
}
