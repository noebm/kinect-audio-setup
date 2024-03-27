/*
 * Copyright 2011 Drew Fisher <drew.m.fisher@gmail.com>. All rights reserved.
 * Copyright (C) 2016  Antonio Ospite <ao2@ao2.it>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS  ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL DREW FISHER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of Drew Fisher.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libusb.h>

#include "endian.h"

#define KINECT_AUDIO_VID           0x045e
#define KINECT_AUDIO_PID           0x02be
#define KINECT_AUDIO_CONFIGURATION 1
#define KINECT_AUDIO_INTERFACE     0
#define KINECT_AUDIO_IN_EP         0x81
#define KINECT_AUDIO_OUT_EP        0x01

static libusb_device_handle *dev;
static unsigned int seq;

typedef struct {
	uint32_t magic;
	uint32_t seq;
	uint32_t bytes;
	uint32_t cmd;
	uint32_t write_addr;
	uint32_t unk;
} bootloader_command;

typedef struct {
	uint32_t magic;
	uint32_t seq;
	uint32_t status;
} status_code;

#define LOG(...) printf(__VA_ARGS__)

#if __BYTE_ORDER == __BIG_ENDIAN
static inline uint32_t fn_le32(uint32_t d)
{
	return (d<<24) | ((d<<8)&0xFF0000) | ((d>>8)&0xFF00) | (d>>24);
}
#else
#define fn_le32(x) (x)
#endif

static void dump_bl_cmd(bootloader_command cmd) {
	int i;
	for (i = 0; i < 24; i++)
		LOG("%02X ", ((unsigned char*)(&cmd))[i]);
	LOG("\n");
}

static int get_first_reply(void) {
	unsigned char buffer[512];
	int res;

	int transferred = 0;
	res = libusb_bulk_transfer(dev, KINECT_AUDIO_IN_EP, buffer, 512, &transferred, 0);
	if (res != 0 ) {
		LOG("Error reading first reply: %d\ttransferred: %d (expected %d)\n", res, transferred, 0x60);
		return res;
	}
	LOG("Reading first reply: ");
	int i;
	for (i = 0; i < transferred; ++i) {
		LOG("%02X ", buffer[i]);
	}
	LOG("\n");
	return res;
}

static int get_reply(void) {
	union {
		status_code buffer;
		/* The following is needed because libusb_bulk_transfer might
		 * fail when working on a buffer smaller than 512 bytes.
		 */
		unsigned char dump[512];
	} reply;
	int res;
	int transferred = 0;

	res = libusb_bulk_transfer(dev, KINECT_AUDIO_IN_EP, reply.dump, 512, &transferred, 0);
	if (res != 0 || transferred != sizeof(status_code)) {
		LOG("Error reading reply: %d\ttransferred: %d (expected %zu)\n", res, transferred, sizeof(status_code));
		return res;
	}
	if (fn_le32(reply.buffer.magic) != 0x0a6fe000) {
		LOG("Error reading reply: invalid magic %08X\n", reply.buffer.magic);
		return -1;
	}
	if (fn_le32(reply.buffer.seq) != seq) {
		LOG("Error reading reply: non-matching sequence number %08X (expected %08X)\n", reply.buffer.seq, seq);
		return -1;
	}
	if (fn_le32(reply.buffer.status) != 0) {
		LOG("Notice reading reply: last uint32_t was nonzero: %d\n", reply.buffer.status);
	}

	LOG("Reading reply: ");
	int i;
	for (i = 0; i < transferred; ++i) {
		LOG("%02X ", reply.dump[i]);
	}
	LOG("\n");

	return res;
}

static int upload_firmware(FILE *fw) {
	int res;

	seq = 1;

	bootloader_command cmd;
	cmd.magic = fn_le32(0x06022009);
	cmd.seq = fn_le32(seq);
	cmd.bytes = fn_le32(0x60);
	cmd.cmd = fn_le32(0);
	cmd.write_addr = fn_le32(0x15);
	cmd.unk = fn_le32(0);

	LOG("About to send: ");
	dump_bl_cmd(cmd);

	int transferred = 0;

	res = libusb_bulk_transfer(dev, KINECT_AUDIO_OUT_EP, (unsigned char *)&cmd, sizeof(cmd), &transferred, 0);
	if (res != 0 || transferred != sizeof(cmd)) {
		LOG("Error: res: %d\ttransferred: %d (expected %zu)\n", res, transferred, sizeof(cmd));
		goto out;
	}

	// This first one doesn't have the usual magic bytes at the beginning,
	// and is 96 bytes long - much longer than the usual 12-byte replies.
	res = get_first_reply();
	if (res < 0) {
		LOG("get_first_reply() failed");
		goto out;
	}

	// I'm not sure why we do this twice here, but maybe it'll make sense
	// later.
	res = get_reply();
	if (res < 0) {
		LOG("First get_reply() failed");
		goto out;
	}
	seq++;

	// Split addr declaration and assignment in order to compile as C++,
	// otherwise this would give "jump to label '...' crosses initialization"
	// errors.
	uint32_t addr;
	addr = 0x00080000;
	unsigned char page[0x4000];
	int read;
	do {
		read = (int)fread(page, 1, 0x4000, fw);
		if (read <= 0) {
			break;
		}
		//LOG("");
		cmd.seq = fn_le32(seq);
		cmd.bytes = fn_le32((unsigned int)read);
		cmd.cmd = fn_le32(0x03);
		cmd.write_addr = fn_le32(addr);
		LOG("About to send: ");
		dump_bl_cmd(cmd);
		// Send it off!
		transferred = 0;
		res = libusb_bulk_transfer(dev, KINECT_AUDIO_OUT_EP, (unsigned char *)&cmd, sizeof(cmd), &transferred, 0);
		if (res != 0 || transferred != sizeof(cmd)) {
			LOG("Error: res: %d\ttransferred: %d (expected %zu)\n", res, transferred, sizeof(cmd));
			goto out;
		}
		int bytes_sent = 0;
		while (bytes_sent < read) {
			int to_send = (read - bytes_sent > 512 ? 512 : read - bytes_sent);
			transferred = 0;
			res = libusb_bulk_transfer(dev, KINECT_AUDIO_OUT_EP, &page[bytes_sent], to_send, &transferred, 0);
			if (res != 0 || transferred != to_send) {
				LOG("Error: res: %d\ttransferred: %d (expected %d)\n", res, transferred, to_send);
				goto out;
			}
			bytes_sent += to_send;
		}
		res = get_reply();
		if (res < 0) {
			LOG("get_reply failed");
			goto out;
		}

		addr += (uint32_t)read;
		seq++;
	} while (read > 0);

	cmd.seq = fn_le32(seq);
	cmd.bytes = fn_le32(0);
	cmd.cmd = fn_le32(0x04);
	cmd.write_addr = fn_le32(0x00080030);
	dump_bl_cmd(cmd);
	transferred = 0;
	res = libusb_bulk_transfer(dev, KINECT_AUDIO_OUT_EP, (unsigned char *)&cmd, sizeof(cmd), &transferred, 0);
	if (res != 0 || transferred != sizeof(cmd)) {
		LOG("Error: res: %d\ttransferred: %d (expected %zu)\n", res, transferred, sizeof(cmd));
		goto out;
	}
	res = get_reply();
	seq++;

out:
	return res;
}

int main(int argc, char *argv[]) {
	char default_filename[] = "firmware.bin";
	char *filename = default_filename;
	int ret = 0;

	if (argc == 2) {
		filename = argv[1];
	}

	FILE *fw = fopen(filename, "rb");
	if (fw == NULL) {
		fprintf(stderr, "Failed to open %s: %s\n", filename, strerror(errno));
		return -errno;
	}

	ret = libusb_init(NULL);
	if (ret < 0) {
		fprintf(stderr, "libusb_init failed: %s\n",
			libusb_error_name(ret));
		goto out;
	}

	libusb_set_debug(NULL, 3);

	dev = libusb_open_device_with_vid_pid(NULL, KINECT_AUDIO_VID, KINECT_AUDIO_PID);
	if (dev == NULL) {
		fprintf(stderr, "libusb_open failed: %s\n", strerror(errno));
		ret = -errno;
		goto out_libusb_exit;
	}

	int current_configuration = -1;
	ret = libusb_get_configuration(dev, &current_configuration);
	if (ret < 0) {
		fprintf(stderr, "libusb_get_configuration failed: %s\n",
			libusb_error_name(ret));
		goto out_libusb_close;
	}

	if (current_configuration != KINECT_AUDIO_CONFIGURATION) {
		ret = libusb_set_configuration(dev, KINECT_AUDIO_CONFIGURATION);
		if (ret < 0) {
			fprintf(stderr, "libusb_set_configuration failed: %s\n",
				libusb_error_name(ret));
			fprintf(stderr, "Cannot set configuration %d\n",
				KINECT_AUDIO_CONFIGURATION);
			goto out_libusb_close;
		}
	}

	libusb_set_auto_detach_kernel_driver(dev, 1);

	ret = libusb_claim_interface(dev, KINECT_AUDIO_INTERFACE);
	if (ret < 0) {
		fprintf(stderr, "libusb_claim_interface failed: %s\n",
			libusb_error_name(ret));
		fprintf(stderr, "Cannot claim interface %d\n",
			KINECT_AUDIO_INTERFACE);
		goto out_libusb_close;
	}

	/*
	 * Checking that the configuration has not changed, as suggested in
	 * http://libusb.sourceforge.net/api-1.0/caveats.html
	 */
	current_configuration = -1;
	ret = libusb_get_configuration(dev, &current_configuration);
	if (ret < 0) {
		fprintf(stderr, "libusb_get_configuration after claim failed: %s\n",
			libusb_error_name(ret));
		goto out_libusb_release_interface;
	}

	if (current_configuration != KINECT_AUDIO_CONFIGURATION) {
		fprintf(stderr, "libusb configuration changed (expected: %d, current: %d)\n",
			KINECT_AUDIO_CONFIGURATION, current_configuration);
		ret = -EINVAL;
		goto out_libusb_release_interface;
	}

	ret = upload_firmware(fw);
	// Now the device reenumerates.

out_libusb_release_interface:
	libusb_release_interface(dev, KINECT_AUDIO_INTERFACE);
out_libusb_close:
	libusb_close(dev);
out_libusb_exit:
	libusb_exit(NULL);
out:
	fclose(fw);
	return ret;
}
