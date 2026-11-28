/*
 * Raw UART setup shared by bblog and bbsend (header only, no SDK).
 * isatty() false (a pty or FIFO in the host tests): used as a byte stream.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

static inline speed_t serial_baud(int b)
{
	switch (b) {
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 500000: return B500000;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	default: return 0;
	}
}

/* flags: O_RDONLY / O_WRONLY (+ O_NONBLOCK). Only the input queue is
 * flushed, and only when reading: in the loopback test the sender opens the
 * same tty and must not throw away what the logger has not read yet. */
static inline int serial_open(const char *dev, int baud, int flags)
{
	speed_t sp = serial_baud(baud);
	if (!sp) {
		fprintf(stderr, "unsupported baud rate %d\n", baud);
		return -1;
	}
	int fd = open(dev, flags | O_NOCTTY);
	if (fd < 0) {
		perror(dev);
		return -1;
	}
	struct termios tio;
	if (tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= CLOCAL | CREAD;
		tio.c_cflag &= ~(CSTOPB | CRTSCTS);
		tio.c_cc[VMIN] = 0;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, sp);
		cfsetospeed(&tio, sp);
		if (tcsetattr(fd, TCSANOW, &tio))
			perror("tcsetattr");
		if ((flags & O_ACCMODE) == O_RDONLY)
			tcflush(fd, TCIFLUSH);
	}
	return fd;
}

#endif
