/*
 * Copyright (c) 2019 Luc Verhaegen <libv@skynet.be>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Small tool to capture the output of hdmi_output and qiuckly verify
 * some pixels for signal integrity and frame sequentiality.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdbool.h>

#include <linux/videodev2.h>

#include <drm_fourcc.h>

#include "capture.h"
#include "kms.h"
#include "status.h"
#include "projector.h"
#include "juggler.h"

int capture_fd = -1;

char *v4l2_device_card_name = "sun4i_csi1";

enum v4l2_buf_type capture_type = -1;
int capture_width;
int capture_height;
size_t capture_pitch;
size_t capture_plane_size;
uint32_t capture_fourcc;

int capture_frame_offset = -1;

int capture_buffer_count;
struct capture_buffer *capture_buffers;

pthread_t capture_thread[1];

static int
v4l2_device_find(void)
{
	struct v4l2_capability capability[1];
	char filename[128];
	int fd, ret, i;

	for (i = 0; i < 16; i++) {
		ret = snprintf(filename, sizeof(filename), "/dev/video%d", i);
		if (ret <= 10) {
			fprintf(stderr,
				"failed to create v4l2 device filename: %d",
				ret);
			return ret;
		}

		fd = open(filename, O_RDWR);
		if (fd < 0) {
			if ((errno == ENODEV) || (errno == ENOENT)) {
				continue;
			} else {
				fprintf(stderr, "Error: failed to open %s: "
					"%s\n", filename, strerror(errno));
				return fd;
			}
		}

		memset(capability, 0, sizeof(struct v4l2_capability));

		ret = ioctl(fd, VIDIOC_QUERYCAP, capability);
		if (ret < 0) {
			fprintf(stderr, "Error: ioctl(VIDIOC_QUERYCAP) on %s"
				" failed: %s\n", filename, strerror(errno));
			return ret;
		}

		if (!strcmp("sun4i_csi1", (const char *) capability->driver) &&
		    (capability->device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
			printf("Found sun4i_csi1 driver as %s.\n",
			       filename);
			return fd;
		}

		close(fd);
	}

	fprintf(stderr, "Error: unable to find /dev/videoX node for "
		"\"sun4i_csi1\"\n");
	return -ENODEV;
}

static int
v4l2_format_get(void)
{
	struct v4l2_format format[1] = {{
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		}};
	struct v4l2_pix_format_mplane *pixel;
	int ret;

	ret = ioctl(capture_fd, VIDIOC_G_FMT, format);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_G_FMT) failed: %s\n",
			strerror(errno));
		return ret;
	}

	pixel = &format->fmt.pix_mp;

	capture_width = pixel->width;
	capture_height = pixel->height;
	capture_pitch = pixel->plane_fmt[0].bytesperline;
	capture_plane_size = pixel->plane_fmt[0].sizeimage;
	capture_fourcc = pixel->pixelformat;

	printf("Format is %dx%d (3x%dbytes, %dkB) %C%C%C%C\n",
	       capture_width, capture_height,
	       (int) capture_pitch,
	       (int) (capture_plane_size >> 10),
	       (capture_fourcc >> 0) & 0xFF, (capture_fourcc >> 8) & 0xFF,
	       (capture_fourcc >> 16) & 0xFF, (capture_fourcc >> 24) & 0xFF);

	return 0;
}

#define SUN4I_CSI1_HDISPLAY_START (V4L2_CID_USER_BASE + 0xC000 + 1)
#define SUN4I_CSI1_VDISPLAY_START (V4L2_CID_USER_BASE + 0xC000 + 2)

static int
v4l2_controls_get(void)
{
	struct v4l2_queryctrl hquery[1] = {{
			.id = SUN4I_CSI1_HDISPLAY_START,
		}};
	struct v4l2_control hctrl[1] = {{
			.id = SUN4I_CSI1_HDISPLAY_START,
		}};
	struct v4l2_queryctrl vquery[1] = {{
			.id = SUN4I_CSI1_VDISPLAY_START,
		}};
	struct v4l2_control vctrl[1] = {{
			.id = SUN4I_CSI1_VDISPLAY_START,
		}};
	int ret;

	ret = ioctl(capture_fd, VIDIOC_QUERYCTRL, hquery);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_QUERYCTRL) failed: %s\n",
			strerror(errno));
		return ret;
	}

	ret = ioctl(capture_fd, VIDIOC_G_CTRL, hctrl);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_G_CTRL) failed: %s\n",
			strerror(errno));
		return ret;
	}

	printf("Control \"%s\":  %d vs %d [%d-%d]\n", hquery->name,
	       hctrl->value, hquery->default_value, hquery->minimum,
	       hquery->maximum);

	ret = ioctl(capture_fd, VIDIOC_QUERYCTRL, vquery);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_QUERYCTRL) failed: %s\n",
			strerror(errno));
		return ret;
	}

	ret = ioctl(capture_fd, VIDIOC_G_CTRL, vctrl);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_G_CTRL) failed: %s\n",
			strerror(errno));
		return ret;
	}

	printf("Control \"%s\":  %d vs %d [%d-%d]\n", vquery->name,
	       vctrl->value, vquery->default_value, vquery->minimum,
	       vquery->maximum);

	return 0;
}

/*
 * Again, assuming that all planes have the same size.
 */
static int
v4l2_buffers_alloc(int width, int height, int pitch, int plane_size,
		   uint32_t fourcc)
{
	struct v4l2_requestbuffers request[1] = {{
			.count = 16,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
		}};
	uint32_t drm_format;
	int ret, i;

	switch (fourcc) {
	case V4L2_PIX_FMT_YUV444M:
		drm_format = DRM_FORMAT_R8_G8_B8;
		break;
	default:
		fprintf(stderr, "%s(): unsupported format: %C%C%C%C\n",
			__func__, (fourcc >> 0) & 0xFF, (fourcc >> 8) & 0xFF,
			(fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF);
		return -1;
	}

	ret = ioctl(capture_fd, VIDIOC_REQBUFS, request);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_REQBUFS) failed: %s\n",
			strerror(errno));
		return ret;
	}

	capture_buffer_count = request->count;
	printf("Requested %d buffers.\n", request->count);

	capture_buffers = calloc(capture_buffer_count,
				 sizeof(struct capture_buffer));
	if (!capture_buffers) {
		fprintf(stderr, "Failed to allocate buffers structure.\n");
		return ENOMEM;
	}

	for (i = 0; i < capture_buffer_count; i++) {
		capture_buffers[i].index = i;

		capture_buffers[i].width = width;
		capture_buffers[i].height = height;

		capture_buffers[i].pitch = pitch;
		capture_buffers[i].plane_size = plane_size;

		capture_buffers[i].v4l2_fourcc = fourcc;
		capture_buffers[i].drm_format = drm_format;

		pthread_mutex_init(capture_buffers[i].reference_count_mutex,
				   NULL);
	}

	return 0;
}

static int
v4l2_buffer_mmap(int index, struct capture_buffer *buffer)
{
	struct v4l2_plane planes[3] = {{ 0 }};
	struct v4l2_buffer query[1] = {{
			.index = buffer->index,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 3,
			.m.planes = planes,
		}};
	int ret, i;

	ret = ioctl(capture_fd, VIDIOC_QUERYBUF, query);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_QUERYBUF) failed: %s\n",
			strerror(errno));
		return ret;
	}

	for (i = 0; i < 3; i++) {
		off_t offset = query->m.planes[i].m.mem_offset;
		void *map;

		map = mmap(NULL, capture_plane_size, PROT_READ, MAP_SHARED,
			   capture_fd, offset);
		if (map == MAP_FAILED) {
			fprintf(stderr, "Error: failed to mmap buffer %d[%d]:"
				" %s\n", buffer->index, i, strerror(errno));
			return errno;
		}

		printf("Mapped buffer %02d[%d] @ 0x%08lX to %p.\n",
		       buffer->index, i, offset, map);

		buffer->planes[i].offset = offset;
		buffer->planes[i].map = map;
	}

	return 0;
}

static int
v4l2_buffers_mmap(void)
{
	int ret, i;

	for (i = 0; i < capture_buffer_count; i++) {
		ret = v4l2_buffer_mmap(i, &capture_buffers[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int
v4l2_buffer_export(int index, struct capture_buffer *buffer)
{
	struct v4l2_exportbuffer export[1] = {
		{
			.index = buffer->index,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.flags = O_RDONLY,
		},
	};
	int i, ret;

	for (i = 0; i < 3; i++) {
		export->plane = i;

		ret = ioctl(capture_fd, VIDIOC_EXPBUF, export);
		if (ret) {
			fprintf(stderr, "%s: Error: ioctl(VIDIOC_EXPBUF) on"
				" %d.%d failed: %s\n",
				__func__, buffer->index, i, strerror(errno));
			return ret;
		}

		buffer->planes[i].export_fd = export->fd;

		printf("Exported buffer %02d[%d] to %d.\n",
		       buffer->index, i, export->fd);
	}

	return 0;
}

static int
v4l2_buffers_export(void)
{
	int ret, i;

	for (i = 0; i < capture_buffer_count; i++) {
		ret = v4l2_buffer_export(i, &capture_buffers[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int
v4l2_buffers_kms_import(void)
{
	int ret, i;

	for (i = 0; i < capture_buffer_count; i++) {
		ret = kms_buffer_import(&capture_buffers[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int
v4l2_buffer_queue(int index)
{
	struct v4l2_plane planes[3] = {{ 0 }};
	struct v4l2_buffer queue[1] = {{
			.index = index,
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.m.planes = planes,
			.length = 3,
		}};
	int ret;

	ret = ioctl(capture_fd, VIDIOC_QBUF, queue);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_QBUF(%d)) failed: "
			"%s\n", index, strerror(errno));
		return ret;
	}

	return 0;
}

static int
v4l2_buffers_queue(void)
{
	int i, ret;

	for (i = 0; i < capture_buffer_count; i++) {
		ret = v4l2_buffer_queue(i);
		if (ret)
			return ret;
	}

	printf("Queued %d buffers.\n", i);

	return 0;
}

static int
v4l2_streaming_start(void)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	int ret;

	ret = ioctl(capture_fd, VIDIOC_STREAMON, &type);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_STREAMON) failed: %s\n",
			strerror(errno));
		return ret;
	}

	return 0;
}

static int
v4l2_buffer_dequeue(void)
{
	struct v4l2_plane planes[3] = {{ 0 }};
	struct v4l2_buffer dequeue[1] = {{
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.m.planes = planes,
			.length = 3,
		}};
	int ret;

	ret = ioctl(capture_fd, VIDIOC_DQBUF, dequeue);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_DQBUF) failed: %s\n",
			strerror(errno));
		return -ret;
	}

	return dequeue->index;
}

static void
capture_buffer_test_frame(int frame,
			  uint8_t *red, uint8_t *green, uint8_t *blue,
			  int x, int y)
{
	int offset = x + (y * capture_width);

	if (capture_frame_offset == -1) {
		capture_frame_offset = (blue[offset] - frame) & 0xFF;
		printf("frame: 0x%02X, blue: 0x%02X, offset: 0x%02X\n",
		       frame & 0xFF, blue[offset], capture_frame_offset);
	} else {
		int count = (frame + capture_frame_offset) & 0xFF;

		if (count != blue[offset])
			printf("Frame %d: frame mismatch (%4d,%4d):"
			       " 0x%02X should be 0x%02X.\n",
			       frame, x, y, blue[offset], count);
	}

	if (((x & 0xFF) != red[offset]) ||
	    ((y & 0xFF) != green[offset]))
		printf("Frame %d: position mismatch: (%4d,%4d)"
		       "(0x%02X,0x%02X) should be (0x%02X,0x%02X)\n",
		       frame, x, y, red[offset], green[offset],
		       (x & 0xFF), (y & 0xFF));
}

static __maybe_unused void
capture_buffer_test_empty(int frame,
			  uint8_t *red, uint8_t *green, uint8_t *blue,
			  int x, int y)
{
	int offset = x + (y * capture_width);

	if (blue[offset])
		printf("Frame %d: blue channel mismatch (%4d,%4d):"
		       " 0x%02X should be 0.\n", frame, x, y, blue[offset]);

	if (((x & 0xFF) != red[offset]) ||
	    ((y & 0xFF) != green[offset]))
		printf("Frame %d: position mismatch: (%4d,%4d)"
		       "(0x%02X,0x%02X) should be (0x%02X,0x%02X)\n",
		       frame, x, y, red[offset], green[offset],
		       (x & 0xFF), (y & 0xFF));
}

static  __maybe_unused void
capture_buffer_test(struct capture_buffer *buffer, int frame)
{
	uint8_t *red, *green, *blue;
	int center_x = (capture_width >> 1);
	int center_y = (capture_height >> 1);

	/* we have swapped blue and red channels on our system */
	blue = buffer->planes[0].map;
	green = buffer->planes[1].map;
	red = buffer->planes[2].map;

	printf("\rTesting frame %4d (%2d):", frame, buffer->index);

	/*
	 * Test 16x16 pixels in the lower right corner, this will also
	 * initialize the frame counter, so make it the lower corner to
	 * work around the tfp401s limitations.
	 */
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 16, capture_height - 16);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 1, capture_height - 16);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 16, capture_height - 1);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 1, capture_height - 1);

	/* Test 16x16 pixels in the upper left corner */
	capture_buffer_test_frame(frame, red, green, blue, 0, 0);
	capture_buffer_test_frame(frame, red, green, blue, 15, 0);
	capture_buffer_test_frame(frame, red, green, blue, 0, 15);
	capture_buffer_test_frame(frame, red, green, blue, 15, 15);

	/* Test 16x16 pixels in the upper right corner */
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 16, 0);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 1, 0);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 16, 15);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 1, 15);

	/* Test 16x16 pixels in the lower left corner */
	capture_buffer_test_frame(frame, red, green, blue,
				  0, capture_height - 16);
	capture_buffer_test_frame(frame, red, green, blue,
				  15, capture_height - 16);
	capture_buffer_test_frame(frame, red, green, blue,
				  0, capture_height - 1);
	capture_buffer_test_frame(frame, red, green, blue,
				  15, capture_height - 1);

	/* Test 16x16 pixels in the lower right corner */
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 16, capture_height - 16);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 1, capture_height - 16);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 16, capture_height - 1);
	capture_buffer_test_frame(frame, red, green, blue,
				  capture_width - 1, capture_height - 1);

	/* Test 16x16 pixels in the center */
	capture_buffer_test_frame(frame, red, green, blue,
				  center_x - 8, center_y - 8);
	capture_buffer_test_frame(frame, red, green, blue,
				  center_x + 7, center_y - 8);
	capture_buffer_test_frame(frame, red, green, blue,
				  center_x - 8, center_y + 7);
	capture_buffer_test_frame(frame, red, green, blue,
				  center_x + 7, center_y + 7);
}

int
capture_buffer_display_release(struct capture_buffer *buffer)
{
	pthread_mutex_lock(buffer->reference_count_mutex);

	if (buffer->reference_count <= 0) {
		fprintf(stderr, "%s(%d): Error: reference count <= 0\n",
			__func__, buffer->index);
		buffer->reference_count = 0;
	} else
		buffer->reference_count--;

	if (!buffer->reference_count)
		v4l2_buffer_queue(buffer->index);

	pthread_mutex_unlock(buffer->reference_count_mutex);

	return 0;
}

static int
capture_buffer_display(struct capture_buffer *buffer, int frame)
{
	pthread_mutex_lock(buffer->reference_count_mutex);

	if (buffer->reference_count)
		fprintf(stderr, "%s(%d): Error: reference count = %d\n",
			__func__, buffer->index, buffer->reference_count);

	/*
	 * Claim all users at once, and avoid one returning too soon and
	 * prematurely releasing.
	 */
	buffer->reference_count = 3;

	pthread_mutex_unlock(buffer->reference_count_mutex);

	kms_projector_capture_display(buffer);
	kms_status_capture_display(buffer);

	capture_buffer_test(buffer, frame);
	capture_buffer_display_release(buffer);

	return 0;
}

static void *
capture_thread_handler(void *arg)
{
	unsigned long count = (unsigned long) arg;
	int ret, i;

	capture_fd = v4l2_device_find();
	if (capture_fd < 0)
		return NULL;

	ret = v4l2_format_get();
	if (ret)
		return NULL;

	ret = v4l2_controls_get();
	if (ret)
		return NULL;

	ret = v4l2_buffers_alloc(capture_width, capture_height, capture_pitch,
				 capture_plane_size, capture_fourcc);
	if (ret)
		return NULL;

	ret = v4l2_buffers_mmap();
	if (ret)
		return NULL;

	ret = v4l2_buffers_export();
	if (ret)
		return NULL;

	ret = v4l2_buffers_kms_import();
	if (ret)
		return NULL;

	ret = v4l2_buffers_queue();
	if (ret)
		return NULL;

	ret = v4l2_streaming_start();
	if (ret)
		return NULL;

	for (i = 0; i < count; i++) {
		struct capture_buffer *buffer;
		int index = v4l2_buffer_dequeue();

		if (index < 0)
			return NULL;

		/* frame 0 starts at a random line anyway, so skip it */
		if (i) {
			buffer = &capture_buffers[index];
			capture_buffer_display(buffer, i);
		}
	}

	printf("\nCaptured %d buffers.\n", i);

	return NULL;
}

int
capture_init(unsigned long count)
{
	int ret;

	ret = pthread_create(capture_thread, NULL, capture_thread_handler,
			     (void *) count);
	if (ret)
		fprintf(stderr, "%s() failed: %s\n", __func__, strerror(ret));

	return 0;
}
