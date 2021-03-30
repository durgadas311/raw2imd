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
	int policy;	// 2-side policy
	int skew;	// physical skew
	int *sectbl;
	int offset1;
	int offset2;
	int force;
	int ignore;
	bool read_comment;
	const char *title;
	const char *imd_filename;
	const char *image_filename;
	int logdisk;
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
	// TODO: what are the right values here?
	if (args.size == 8) {
		track->data_mode = &DATA_MODES[5-args.mfm];	// "FM-500k" - "MFM-500k"
	} else {
		track->data_mode = &DATA_MODES[3-args.mfm];	// "FM-300k" - "MFM-300k"
	}
	track->phys_cyl = cyl;
	track->phys_head = hd;
	track->num_sectors = args.sectors;
	track->sector_size_code = args.length_code;
	track->status = TRACK_PROBED;
	for (s = 0; s < args.sectors; ++s) {
		int sn = s;
		if (args.skew > 1) {
			sn = args.sectbl[s];
		}
		sector_t *sec = &track->sectors[sn];
		sec->log_cyl = track->phys_cyl;
		sec->log_head = track->phys_head;
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
	free_disk(&disk);
	close(dev_fd);
}

static void usage(void) {
	fprintf(stderr, "usage: raw2imd [OPTION]... RAW-FILE [IMAGE-FILE]\n");
	fprintf(stderr, "  -5		 raw represents 5.25\" diskette (default)\n");
	fprintf(stderr, "  -8		 raw represents 8\" diskette\n");
	fprintf(stderr, "  -c NUM	 number of cylinders\n");
	fprintf(stderr, "  -h NUM	 number of heads (sides)\n");
	fprintf(stderr, "  -s NUM	 number of sectors/track\n");
	fprintf(stderr, "  -l NUM	 sectors length\n");
	fprintf(stderr, "  -k NUM	 physical sector skew\n");
	fprintf(stderr, "  -K NUM	 side 1 physical skew\n");
	fprintf(stderr, "  -o		 sector number offset (1)\n");
	fprintf(stderr, "  -O		 side 1 sector number offset (-o)\n");
	fprintf(stderr, "  -m		 raw represents MFM (double density)\n");
	fprintf(stderr, "  -i		 ignore extra data in raw file\n");
	fprintf(stderr, "  -f		 force using smaller raw file\n");
	fprintf(stderr, "  -C		 read comment from stdin\n");
	fprintf(stderr, "  -T STR	 use STR as comment\n");
	fprintf(stderr, "  -L		 raw is logdisk format\n");
	fprintf(stderr, "  -v		 verbose output\n");
}

int main(int argc, char **argv) {
	int x;

	dev_fd = -1;
	args.cylinders = -1;
	args.heads = -1;
	args.sectors = -1;
	args.length = -1;
	args.size = -1;
	args.offset1 = -1;
	args.offset2 = -1;
	args.mfm = 0;	// need numeric values 0/1
	args.policy = 1; // default to "interlaced"
	args.skew = 0;	// physical skew
	args.sectbl = NULL;
	args.force = false;
	args.ignore = false;
	args.read_comment = false;
	args.title = NULL;
	args.imd_filename = NULL;
	args.image_filename = NULL;
	args.logdisk = false;

	while (true) {
		int opt = getopt(argc, argv, "58c:h:s:l:o:O:mifCT:Lk:K:");
		if (opt == -1) break;

		switch (opt) {
		case '5':
			args.size = 5;
			break;
		case '8':
			args.size = 8;
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
			args.skew = atoi(optarg);
			break;
		case 'K':
			//args.skew2 = atoi(optarg);
			break;
		default:
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
	if (args.offset1 < 0) {
		args.offset1 = 1; // default to industry-standard
	}
	if (args.offset2 < 0) {
		args.offset2 = args.offset1;
	}
	if (args.skew > 1) { // physical skew - if specified
		args.sectbl = malloc(args.sectors * sizeof(*args.sectbl));
		int s;
		for (s = 0; s < args.sectors; ++s) {
			int sn = s;
			sn = (s * args.skew) % args.sectors;
			args.sectbl[sn] = s;
		}
	}

	process_raw();

	return 0;
}
