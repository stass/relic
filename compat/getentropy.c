/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "compat.h"

/*
 * The most getentropy(2) hands out in one call.  The interface was designed
 * for seeding, not for streaming, and every implementation refuses a larger
 * request rather than satisfying part of it.
 */
#define	GETENTROPY_MAX	256

/*
 * Fill buf with buflen bytes from the kernel's random device, or fail having
 * written nothing a caller may use.
 *
 * Returns 0 only when every byte was read.  A short read is retried rather
 * than reported, because the interface promises all of the bytes or an error
 * and never fewer; a read interrupted by a signal is retried for the same
 * reason.  A request larger than GETENTROPY_MAX fails with EIO, as the native
 * interface does, and so does a device that reports end of file.  So does a
 * /dev/urandom that is not a character device: a regular file there, as a
 * hand-built chroot may have, would hand out the same bytes to every caller.
 * Any other failure, including a system with no /dev/urandom, keeps the
 * errno of the call that failed.
 */
int
getentropy(void *buf, size_t buflen)
{
	struct stat device;
	unsigned char *bytes;
	size_t filled;
	ssize_t got;
	int saved_errno;
	int fd;

	if (buflen > GETENTROPY_MAX) {
		errno = EIO;
		return (-1);
	}

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (-1);

	if (fstat(fd, &device) == -1)
		saved_errno = errno;
	else if (!S_ISCHR(device.st_mode))
		saved_errno = EIO;
	else
		saved_errno = 0;

	if (saved_errno != 0) {
		(void)close(fd);
		errno = saved_errno;
		return (-1);
	}

	bytes = buf;
	filled = 0;
	while (filled < buflen) {
		got = read(fd, bytes + filled, buflen - filled);
		if (got == -1 && errno == EINTR)
			continue;

		if (got <= 0) {
			saved_errno = got == 0 ? EIO : errno;
			(void)close(fd);
			errno = saved_errno;
			return (-1);
		}

		filled += (size_t)got;
	}

	(void)close(fd);

	return (0);
}
