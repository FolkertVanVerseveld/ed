#define _BSD_SOURCE
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NPEEK 0x10
#define BUFSZ 4096
#define CMDSZ 80
#define READONLY 1

mode_t mode;
int fd = -1;
int flags;
const char *name;
unsigned char buf[BUFSZ];
ssize_t bufl;
off_t off;
char cmd[CMDSZ];
struct stat st;

/* safely close buffer */
static void bufclose(void)
{
	if (fd != -1) {
		close(fd);
		fd = -1;
	}
}

static void cleanup(void)
{
	bufclose();
}

int bufopen(const char *fname)
{
	bufclose();
	name = fname;
	/* touch file and chmod 664 */
	mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;
	fd = open(name, O_CREAT | O_RDWR, mode);
	if (fd != -1)
		goto stat;
	if (errno != EACCES)
		goto fail;
	/* try touch and chmod 644 */
	mode &= S_IWGRP;
	fd = open(name, O_CREAT | O_RDWR, mode);
	if (fd != -1)
		goto stat;
	if (errno != EACCES)
		goto fail;
	/* readonly (444) is the only option left */
	mode = S_IRUSR | S_IRGRP | S_IROTH;
	flags |= READONLY;
	fd = open(name, O_RDONLY);
	if (fd == -1)
		goto fail;
stat:
	if (fstat(fd, &st))
		goto fail;
	return 0;
fail:
	perror(name);
	return 1;
}

static const char hex[] = "0123456789ABCDEF";

unsigned char nybtonr(int ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return 0;
}

/* XXX don't use pread as this may not be available */
/* seek to offset. if this fails, buf contents is undefined */
int bufseek(off_t pos)
{
	ssize_t zd;
	off_t op;
	if (!(pos < off || pos >= off + bufl))
		return 0; /* no need to seek */
#ifdef DEBUG
	printf("seek %lX\n", (long unsigned)pos);
#endif
	op = lseek(fd, pos, SEEK_SET);
	if (op == (off_t)-1)
		goto fail;
	zd = read(fd, buf, sizeof buf);
	op = lseek(fd, pos, SEEK_SET);
	if (op == (off_t)-1)
		goto fail;
	/* apply read buffer */
	off = op;
	bufl = zd;
	return 0;
fail:
	perror("seek");
	return 1;
}

int parse(void)
{
	off_t o, op;
	char *ptr, *q;
	unsigned i;
	ssize_t zd;
	ptr = cmd;
	switch (*ptr) {
	case 'g':
		printf("%lX\n", (long unsigned)off);
		return 0;
	case 's':
		printf("%lX\n", (long unsigned)st.st_size);
		return 0;
	}
	for (o = 0; *ptr && isxdigit(*ptr); ++ptr) {
		o <<= 4;
		o += nybtonr(*ptr);
	}
	switch (*ptr) {
	case '\n':
		/* peek */
		/* dump dummy bytes if peek is out of range */
		if (o >= st.st_size) {
			printf("%lX", (long unsigned)o);
			i = 0;
			goto overflow;
		}
		/* seek to address if not buffered */
		if (bufseek(o))
			return 1;
		op = o - off;
		printf("%lX", (long unsigned)op);
		for (i = 0; op < bufl && i < NPEEK; ++i, ++op, ++o) {
			unsigned char ch;
			ch = buf[op];
			printf(" %c%c", hex[(ch >> 4) & 0xf], hex[ch & 0xf]);
		}
		for (; i < NPEEK && o < st.st_size; ++i, ++o)
			fputs(" ~~", stdout);
overflow:
		for (; i < NPEEK; ++i)
			fputs(" ??", stdout);
		putchar('\n');
		return 0;
	case ' ':
		/* poke */
		if (flags & READONLY)
			goto readonly;
		/* count number of elements */
		op = o;
		for (q = ptr; *q; ++q) {
			while (*q && isspace(*q))
				++q;
			if (isxdigit(*q)) {
				++op;
				for (++q; isxdigit(*q); ++q);
			}
		}
		/* resize if necessary */
		if (op >= st.st_size) {
			/* resize */
			if (ftruncate(fd, op) || fstat(fd, &st)) {
				perror("resize");
				return 1;
			}
		}
		if (bufseek(o))
			return 1;
		for (op = o - off; op < bufl && *ptr; ++op, ++ptr) {
			unsigned val;
			while (*ptr && isspace(*ptr))
				++ptr;
			if (isxdigit(*ptr)) {
				val = nybtonr(*ptr);
				++ptr;
				if (isxdigit(*ptr))
					val = (val << 4) + nybtonr(*ptr);
				buf[op] = val;
			}
		}
		/* XXX always write pokes */
		zd = write(fd, buf, (size_t)bufl);
		if (zd != bufl) {
			perror("poke");
			return 1;
		}
		return 0;
	case 's':
		/* resize */
		if (flags & READONLY)
			goto readonly;
		if (ftruncate(fd, o) || bufseek(off)) {
			perror("resize");
			return 1;
		}
		return 0;
	}
	return -1;
readonly:
	fputs("readonly\n", stderr);
	return 0;
}

int main(int argc, char **argv)
{
	int ret = 1;
	ssize_t zd;
	errno = 0;
	if (argc != 2) {
		fprintf(stderr, "usage: %s file\n", argc > 0 ? argv[0] : "ed");
		goto fail;
	}
	atexit(cleanup);
	if (bufopen(argv[1]))
		goto fail;
	memset(buf, 0, sizeof buf);
	zd = read(fd, buf, sizeof buf);
	off = lseek(fd, 0, SEEK_SET);
	if (zd < 0) {
		perror("read");
		goto fail;
	}
	bufl = zd;
	while (fgets(cmd, CMDSZ, stdin) && *cmd != 'q') {
		cmd[CMDSZ - 1] = '\0';
		if (parse()) printf("? %s", cmd);
	}
	ret = 0;
fail:
	return ret;
}
