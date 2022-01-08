/*
  usbcat copies stdin/stdout to/from usb-out/in bulk endpoints on specified USB device

  this should build on Linux and MacOS as long as you have libusb-x.y somewhere

    cc -Werror -Wfatal-errors -Wall -Wextra -Wundef \
        -O -I/opt/local/include/libusb-1.0 -L/opt/local/lib -lusb-1.0 usbcat.c -o usbcat

*/

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "libusb.h"

static const char*    argv0;
static int            verbosity = 0;
libusb_device_handle* device    = NULL;

static void usage(void) {
    fprintf(stderr, "usage: %s [-v] [-n]  {-d vid:pid [-s n] | -p bus,dev} [-f iface] < infile > outfile\n", argv0);
    fprintf(stderr,
            "  -v           increase verbosity\n"
            "  -n           don't read stdin\n"
            "  -d vid:pid   hex Vendor and Product ID\n"
            "  -s n         n'th device that matches vid:pid\n"
            "  -p bus,dev   decimal device address\n"
            "  -f iface     interface number\n");
    exit(2);
}

static void crash(const char* fmt, ...) {
    va_list ap;
    char    buf[1000];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    fprintf(stderr, "%s%s%s\n", buf, (errno) ? ": " : "", (errno) ? strerror(errno) : "");
    exit(1);
    va_end(ap);
    return;
}

static void fault(int i) {
    (void)i;
    crash("fault");
}
static void exit_libusb(void) { libusb_exit(NULL); }
static void exit_device(void) { libusb_close(device); }

static libusb_device_handle* open_device(uint32_t vid, uint32_t pid, uint32_t busnum, uint32_t devaddr, int skip) {
    (void)busnum;
    (void)devaddr;
    (void)skip;

#if 0
	// discover devices
	libusb_device **list;
	libusb_device *found = NULL;
	ssize_t cnt = libusb_get_device_list(NULL, &list);
	ssize_t i = 0;
	int err = 0;
	if (cnt < 0)
	    error();
	 
	for (i = 0; i < cnt; i++) {
	    libusb_device *device = list[i];
	    if (is_interesting(device)) {
	        found = device;
	        break;
	    }
	}
	 
	if (found) {
	    libusb_device_handle *handle;
	 
	    err = libusb_open(found, &handle);
	    if (err)
	        error();
	    // etc
	}
	 
	libusb_free_device_list(list, 1);
#else
    return libusb_open_device_with_vid_pid(NULL, vid, pid);
#endif
}

static const struct libusb_endpoint_descriptor* findbulkep(const struct libusb_interface_descriptor* altsetting, int in) {
    for (int i = 0; i < altsetting->bNumEndpoints; ++i) {
        // fprintf(stderr, " checking endpoint %x %x\n", altsetting->endpoint[i].bEndpointAddress, altsetting->endpoint[i].bmAttributes);
        if ((altsetting->endpoint[i].bEndpointAddress & 0x80) != (in ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT))
            continue;
        if (altsetting->endpoint[i].bmAttributes != LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK)
            continue;
        return &altsetting->endpoint[i];
    }
    return NULL;
}

void xfer_in_done(struct libusb_transfer* xfr) {
    switch (xfr->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        if (write(fileno(stdout), xfr->buffer, xfr->actual_length) < xfr->actual_length)
            crash("Short write on stdout");

    case LIBUSB_TRANSFER_TIMED_OUT:
        break;

    case LIBUSB_TRANSFER_NO_DEVICE:
        crash("Device disconnected.");
    case LIBUSB_TRANSFER_OVERFLOW:
        crash("Device sent more data than requested.");
    default:
        crash("Unexpected status in xfer done: %d", xfr->status);
    }

    int status = libusb_submit_transfer(xfr);
    if (status < 0)
        crash("libusb_submit_transfer (IN): %s", libusb_error_name(status));
}

static volatile int out_xfr_pending = 0;

void xfer_out_done(struct libusb_transfer* xfr) {
    switch (xfr->status) {
    case LIBUSB_TRANSFER_COMPLETED:
    case LIBUSB_TRANSFER_CANCELLED:
        break;
    case LIBUSB_TRANSFER_TIMED_OUT:
        fprintf(stderr, "transfer OUT timeout\n");
        break;

    case LIBUSB_TRANSFER_NO_DEVICE:
        crash("Device disconnected.");
    default:
        crash("Unexpected status in xfer done: %d", xfr->status);
    }

    out_xfr_pending = 0;
}

int main(int argc, char* argv[]) {

    argv0 = strrchr(argv[0], '/');
    if (argv0)
        ++argv0;
    else
        argv0 = argv[0];

    signal(SIGBUS, fault);
    signal(SIGSEGV, fault);

    uint32_t vid     = 0x0483; // STMicroelectronics
    uint32_t pid     = 0x5722; // Bulk Demo
    uint32_t busnum  = 0;
    uint32_t devaddr = 0;
    int      ifno    = -1;
    int      nostdin = 0;
    int      skip    = 0;

    int ch;
    while ((ch = getopt(argc, argv, "d:e:f:hnp:s:v")) != -1) {
        switch (ch) {
        case 'd':
            if (sscanf(optarg, "%x:%x", &vid, &pid) != 2)
                usage();
            break;

        case 'p':
            if (sscanf(optarg, "%u,%u", &busnum, &devaddr) != 2)
                usage();
            break;

        case 's':
            if (sscanf(optarg, "%d", &skip) != 1)
                usage();
            break;

        case 'f':
            if (sscanf(optarg, "%d", &ifno) != 1)
                usage();
            break;

        case 'n':
            ++nostdin;
            break;

        case 'v':
            ++verbosity;
            break;

        case 'h':
        default:
            usage();
        }
    }

    if (verbosity > 0) {
        const struct libusb_version* v = libusb_get_version();
        fprintf(stderr, "%s using LibUSB v%d.%d.%d (%x%s)\n", argv0, v->major, v->minor, v->micro, v->nano, v->rc);
    }

    int status = libusb_init(NULL);
    if (status < 0)
        crash("libusb_init: %s", libusb_error_name(status));
    atexit(exit_libusb);

    libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, verbosity);

    device = open_device(vid, pid, busnum, devaddr, skip);
    atexit(exit_device);

    if (verbosity > 0) {
        libusb_device* dev = libusb_get_device(device);
        fprintf(stderr, "Found device at bus:%d address:%d\n", libusb_get_bus_number(dev), libusb_get_device_address(dev));
    }

    libusb_set_auto_detach_kernel_driver(device, 1);

    const struct libusb_endpoint_descriptor* iep = NULL;
    const struct libusb_endpoint_descriptor* oep = NULL;

    {
        int cfgno = -1;
        status    = libusb_get_configuration(device, &cfgno);
        if (status < 0)
            crash("libusb_get_configuration: %s", libusb_error_name(status));

        if (cfgno == 0) {
            fprintf(stderr, "device unconfigured, requesting default configuration (1).\n");
            status = libusb_set_configuration(device, 1);
            if (status < 0)
                crash("libusb_set_configuration(1): %s", libusb_error_name(status));
        }

        struct libusb_config_descriptor* config;
        status = libusb_get_active_config_descriptor(libusb_get_device(device), &config);
        if (status < 0)
            crash("libusb_get_get_active_config_descriptor: %s", libusb_error_name(status));

        if (verbosity > 2)
            fprintf(stderr, "Active config #%d.\n", config->bConfigurationValue);

        if (ifno == -1) {
            // find first interface that has bulk in/out endpoints on first altsetting
            for (int i = 0; i < config->bNumInterfaces; ++i) {
                iep = findbulkep(&config->interface[i].altsetting[0], 1);
                oep = findbulkep(&config->interface[i].altsetting[0], 0);
                if (iep && (oep || nostdin)) {
                    ifno = config->interface[i].altsetting[0].bInterfaceNumber;
                    break;
                }
            }

        } else {

            for (int i = 0; i < config->bNumInterfaces; ++i) {
                if (config->interface[i].altsetting[0].bInterfaceNumber == ifno) {
                    iep = findbulkep(&config->interface[i].altsetting[0], 1);
                    oep = findbulkep(&config->interface[i].altsetting[0], 0);
                    break;
                }
            }
        }

        if (iep && (oep || nostdin)) {
            status = libusb_claim_interface(device, ifno);
            if (status < 0)
                crash("libusb_claim_interface: %s", libusb_error_name(status));
        } else {
            crash("Could not find interface with proper bulk IN/OUT endpoints.");
        }
    }

    if (verbosity) {
        fprintf(stderr, "Using IN  endpoint 0x%02x with packet size %d\n", iep->bEndpointAddress, iep->wMaxPacketSize);
        if (!nostdin)
            fprintf(stderr, "Using OUT endpoint 0x%02x, with packet size %d\n", oep->bEndpointAddress, oep->wMaxPacketSize);
    }

    // main loop: poll with timeout libusb + stdin
    struct libusb_transfer* xfr = libusb_alloc_transfer(0);
    uint8_t*                buf = malloc(iep->wMaxPacketSize);

    libusb_fill_bulk_transfer(xfr, device, iep->bEndpointAddress, buf, iep->wMaxPacketSize, xfer_in_done, NULL, 0);
    status = libusb_submit_transfer(xfr);
    if (status < 0)
        crash("libusb_submit_transfer (IN): %s", libusb_error_name(status));

    if (nostdin) {

        for (;;)
            libusb_handle_events(NULL);

    } else {

        struct libusb_transfer* oxfr = libusb_alloc_transfer(0);
        uint8_t*                obuf = malloc(oep->wMaxPacketSize);

        fd_set rfds;
        fd_set wfds;

        for (;;) {
            struct timeval tv;
            int            to = libusb_get_next_timeout(NULL, &tv); // 0 no timeout pending, 1 timeout returned (maybe zero)

            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            int maxfd = fileno(stdin);

            if (!out_xfr_pending)
                FD_SET(maxfd, &rfds);

            const struct libusb_pollfd** fds = libusb_get_pollfds(NULL);
            for (const struct libusb_pollfd** i = fds; *i; ++i) {
                if ((*i)->events & POLLIN)
                    FD_SET((*i)->fd, &rfds);
                if ((*i)->events & POLLOUT)
                    FD_SET((*i)->fd, &wfds);
                if (maxfd < (*i)->fd)
                    maxfd = (*i)->fd;
            }
            libusb_free_pollfds(fds);

            int n = select(maxfd + 1, &rfds, &wfds, NULL, to ? &tv : NULL);
            if (n < 0) {
                if (errno != EINTR)
                    crash("select");
            }

            struct timeval zero_tv = {0, 0};
            libusb_handle_events_timeout(NULL, &zero_tv);

            if (!FD_ISSET(fileno(stdin), &rfds))
                continue;

            // TODO maybe buffer bigger
            int l = read(fileno(stdin), obuf, oep->wMaxPacketSize);
            if (l < 0)
                crash("read stdin");
            if (l == 0)
                break; // eof

            libusb_fill_bulk_transfer(oxfr, device, oep->bEndpointAddress, obuf, l, xfer_out_done, NULL, 0);
            out_xfr_pending = 1;
            status          = libusb_submit_transfer(oxfr);
            if (status < 0)
                crash("libusb_submit_transfer (OUT): %s", libusb_error_name(status));
        }

        libusb_cancel_transfer(oxfr);
    }

    libusb_cancel_transfer(xfr);

    return 0;
}