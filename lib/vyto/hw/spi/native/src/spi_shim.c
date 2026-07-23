/* vyto/hw/spi native backing — SPI master via /dev/spidev* (SPI_IOC_MESSAGE).

   The Vyto-way handle / byte-transfer half (docs/HARDWARE.md), the sibling of hw/i2c.
   SPI is full-duplex: every transfer clocks out `tx` while clocking in the same number
   of bytes to `rx` simultaneously. Mode (CPOL/CPHA 0..3), bits-per-word, and max clock
   are set once via the SPI_IOC_WR_* ioctls. The bus fd is owned by the Vyto side and
   closed in deinit.

   Zero #link, kernel char device. Bus access needs the `spi` group or root. No easy
   kernel mock (needs a real bus or a MISO->MOSI loopback jumper). Non-Linux → -1. */

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

/* Open an SPI device (e.g. "/dev/spidev0.0"). Returns the fd, or -1. */
int spi_open(const char *path) {
    return open(path, O_RDWR | O_CLOEXEC);
}

/* Set mode (0..3), bits-per-word (usually 8), and max clock in Hz. 0 ok, -1 error. */
int spi_configure(int fd, int mode, int bits, int speed_hz) {
    unsigned char m = (unsigned char)mode;
    unsigned char b = (unsigned char)bits;
    unsigned int  s = (unsigned int)speed_hz;
    if (ioctl(fd, SPI_IOC_WR_MODE, &m) < 0) return -1;
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &b) < 0) return -1;
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &s) < 0) return -1;
    return 0;
}

/* Full-duplex transfer: clock out `len` bytes from tx while clocking `len` bytes into
   rx. `len` on success, -1 on error. tx and rx may be distinct buffers of length len. */
long spi_transfer(int fd, const char *tx, char *rx, long len, int speed_hz, int bits) {
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof tr);
    tr.tx_buf = (unsigned long)(uintptr_t)tx;
    tr.rx_buf = (unsigned long)(uintptr_t)rx;
    tr.len = (unsigned int)len;
    tr.speed_hz = (unsigned int)speed_hz;
    tr.bits_per_word = (unsigned char)bits;
    int r = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    return (r < 0) ? -1 : len;
}

void spi_close(int fd) { if (fd >= 0) close(fd); }

#else /* non-Linux: no spidev */

int  spi_open(const char *path) { (void)path; return -1; }
int  spi_configure(int fd, int mode, int bits, int speed_hz) { (void)fd; (void)mode; (void)bits; (void)speed_hz; return -1; }
long spi_transfer(int fd, const char *tx, char *rx, long len, int speed_hz, int bits) {
    (void)fd; (void)tx; (void)rx; (void)len; (void)speed_hz; (void)bits; return -1;
}
void spi_close(int fd) { (void)fd; }

#endif
