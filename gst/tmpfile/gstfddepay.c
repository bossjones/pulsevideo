/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstfddepay
 *
 * The fddepay element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! fddepay ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstfddepay.h"
#include "wire-protocol.h"
#include "../gstnetcontrolmessagemeta.h"

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/allocators/gstdmabuf.h>
#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


GST_DEBUG_CATEGORY_STATIC (gst_fddepay_debug_category);
#define GST_CAT_DEFAULT gst_fddepay_debug_category

/* prototypes */


static GstCaps *gst_fddepay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static void gst_fddepay_dispose (GObject * object);

static GstFlowReturn gst_fddepay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

/* pad templates */

static GstStaticCaps fd_caps = GST_STATIC_CAPS ("application/x-fd");

static GstStaticPadTemplate gst_fddepay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_fddepay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-fd"));


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstFddepay, gst_fddepay, GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_fddepay_debug_category, "fddepay", 0,
        "debug category for fddepay element"));

static void
gst_fddepay_class_init (GstFddepayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fddepay_src_template));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fddepay_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Simple FD Deplayloder", "Generic",
      "Simple File-descriptor Depayloader for zero-copy video IPC",
      "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_fddepay_dispose;
  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_fddepay_transform_caps);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_fddepay_transform_ip);

}

static void
gst_fddepay_init (GstFddepay * fddepay)
{
  fddepay->dmabuf_allocator = gst_dmabuf_allocator_new ();
}

void
gst_fddepay_dispose (GObject * object)
{
  GstFddepay *fddepay = GST_FDDEPAY (object);

  GST_DEBUG_OBJECT (fddepay, "dispose");

  /* clean up as possible.  may be called multiple times */
  if (fddepay->dmabuf_allocator != NULL) {
    g_object_unref (G_OBJECT (fddepay->dmabuf_allocator));
    fddepay->dmabuf_allocator = NULL;
  }

  G_OBJECT_CLASS (gst_fddepay_parent_class)->dispose (object);
}

static GstCaps *
gst_fddepay_transform_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);
  GstCaps *othercaps;

  GST_DEBUG_OBJECT (fddepay, "transform_caps");


  if (direction == GST_PAD_SRC) {
    /* transform caps going upstream */
    othercaps = gst_static_caps_get (&fd_caps);
  } else {
    /* transform caps going downstream */
    othercaps = gst_caps_new_any ();
  }

  if (filter) {
    GstCaps *intersect;

    intersect = gst_caps_intersect (othercaps, filter);
    gst_caps_unref (othercaps);

    return intersect;
  } else {
    return othercaps;
  }
}

static GstFlowReturn
gst_fddepay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);
  FDMessage msg;
  GstMemory *dmabufmem = NULL;
  GstNetControlMessageMeta * meta;
  GUnixFDList *fds = NULL;
  int fd = -1;
  struct stat statbuf;

  GST_DEBUG_OBJECT (fddepay, "transform_ip");

  if (gst_buffer_get_size (buf) != sizeof (msg)) {
    /* We're guaranteed that we can't `read` from a socket across an attached
     * file descriptor so we should get the data in chunks no bigger than
     * sizeof(FDMessage) */
    GST_WARNING_OBJECT (fddepay, "fddepay: Received wrong amount of data "
        "between fds.");
    goto error;
  }

  gst_buffer_extract (buf, 0, &msg, sizeof (msg));

  meta = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE));

  if (meta &&
      g_socket_control_message_get_msg_type (meta->message) == SCM_RIGHTS) {
    fds = g_unix_fd_message_get_fd_list ((GUnixFDMessage*) meta->message);
    meta = NULL;
  }

  if (g_unix_fd_list_get_length (fds) != 1) {
    GST_WARNING_OBJECT (fddepay, "fddepay: Expect to receive 1 FD for each "
        "buffer, received %i", g_unix_fd_list_get_length (fds));
    goto error;
  }

  fd = g_unix_fd_list_get (fds, 0, NULL);
  fds = NULL;

  if (fd == -1) {
    GST_WARNING_OBJECT (fddepay, "fddepay: Could not get FD from buffer's "
        "GUnixFDList");
    goto error;
  }

  if (G_UNLIKELY (fstat (fd, &statbuf) != 0)) {
    GST_WARNING_OBJECT (fddepay, "fddepay: Could not stat received fd %i: %s",
        fd, strerror(errno));
    goto error;
  }
  if (G_UNLIKELY (statbuf.st_size < msg.offset + msg.size)) {
    /* Note: This is for sanity and debugging rather than security.  To be
       secure we'd first need to check that it was a sealed memfd. */
    GST_WARNING_OBJECT (fddepay, "fddepay: Received fd %i is too small to "
        "contain data (%zu < %zu + %zu)", fd, statbuf.st_size, msg.offset,
        msg.size);
    goto error;
  }
  dmabufmem = gst_dmabuf_allocator_alloc (fddepay->dmabuf_allocator, fd,
      msg.offset + msg.size);
  fd = -1;
  gst_memory_resize (dmabufmem, msg.offset, msg.size);

  gst_buffer_remove_all_memory (buf);
  gst_buffer_remove_meta (buf,
      gst_buffer_get_meta (buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE));
  gst_buffer_append_memory (buf, dmabufmem);
  dmabufmem = NULL;

  return GST_FLOW_OK;
error:
  if (fd >= 0)
    close (fd);
  return GST_FLOW_ERROR;
}
