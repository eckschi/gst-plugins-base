/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2005 Wim Taymans <wim@fluendo.com>
 *
 * gstbaseaudiosink.c: 
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:gstbaseaudiosink
 * @short_description: Base class for audio sinks
 * @see_also: #GstAudioSink, #GstRingBuffer.
 *
 * This is the base class for audio sinks. Subclasses need to implement the
 * ::create_ringbuffer vmethod. This base class will then take care of
 * writing samples to the ringbuffer, synchronisation, clipping and flushing.
 *
 * Last reviewed on 2006-09-27 (0.10.12)
 */

#include <string.h>

#include "gstbaseaudiosink.h"

GST_DEBUG_CATEGORY_STATIC (gst_base_audio_sink_debug);
#define GST_CAT_DEFAULT gst_base_audio_sink_debug

#define GST_BASE_AUDIO_SINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_BASE_AUDIO_SINK, GstBaseAudioSinkPrivate))

struct _GstBaseAudioSinkPrivate
{
  /* upstream latency */
  GstClockTime us_latency;
  /* the clock slaving algorithm in use */
  GstBaseAudioSinkSlaveMethod slave_method;
  /* running average of clock skew */
  GstClockTimeDiff avg_skew;
  /* the number of samples we aligned last time */
  gint64 last_align;
};

/* BaseAudioSink signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

/* we tollerate half a second diff before we start resyncing. This
 * should be enough to compensate for various rounding errors in the timestamp
 * and sample offset position. 
 * This is an emergency resync fallback since buffers marked as DISCONT will
 * always lock to the correct timestamp immediatly and buffers not marked as
 * DISCONT are contiguous by definition.
 */
#define DIFF_TOLERANCE  2

/* FIXME: 0.11, store the buffer_time and latency_time in nanoseconds */
#define DEFAULT_BUFFER_TIME     ((200 * GST_MSECOND) / GST_USECOND)
#define DEFAULT_LATENCY_TIME    ((10 * GST_MSECOND) / GST_USECOND)
#define DEFAULT_PROVIDE_CLOCK   TRUE
#define DEFAULT_SLAVE_METHOD    GST_BASE_AUDIO_SINK_SLAVE_SKEW

enum
{
  PROP_0,
  PROP_BUFFER_TIME,
  PROP_LATENCY_TIME,
  PROP_PROVIDE_CLOCK,
  PROP_SLAVE_METHOD
};

#define GST_TYPE_SLAVE_METHOD (slave_method_get_type ())

static GType
slave_method_get_type (void)
{
  static GType slave_method_type = 0;
  static const GEnumValue slave_method[] = {
    {GST_BASE_AUDIO_SINK_SLAVE_RESAMPLE, "Resampling slaving", "resample"},
    {GST_BASE_AUDIO_SINK_SLAVE_SKEW, "Skew slaving", "skew"},
    {GST_BASE_AUDIO_SINK_SLAVE_NONE, "No slaving", "none"},
    {0, NULL, NULL},
  };

  if (!slave_method_type) {
    slave_method_type =
        g_enum_register_static ("GstBaseAudioSinkSlaveMethod", slave_method);
  }
  return slave_method_type;
}


#define _do_init(bla) \
    GST_DEBUG_CATEGORY_INIT (gst_base_audio_sink_debug, "baseaudiosink", 0, "baseaudiosink element");

GST_BOILERPLATE_FULL (GstBaseAudioSink, gst_base_audio_sink, GstBaseSink,
    GST_TYPE_BASE_SINK, _do_init);

static void gst_base_audio_sink_dispose (GObject * object);

static void gst_base_audio_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_base_audio_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_base_audio_sink_async_play (GstBaseSink *
    basesink);
static GstStateChangeReturn gst_base_audio_sink_change_state (GstElement *
    element, GstStateChange transition);
static gboolean gst_base_audio_sink_activate_pull (GstBaseSink * basesink,
    gboolean active);
static gboolean gst_base_audio_sink_query (GstElement * element, GstQuery *
    query);

static GstClock *gst_base_audio_sink_provide_clock (GstElement * elem);
static GstClockTime gst_base_audio_sink_get_time (GstClock * clock,
    GstBaseAudioSink * sink);
static void gst_base_audio_sink_callback (GstRingBuffer * rbuf, guint8 * data,
    guint len, gpointer user_data);

static GstFlowReturn gst_base_audio_sink_preroll (GstBaseSink * bsink,
    GstBuffer * buffer);
static GstFlowReturn gst_base_audio_sink_render (GstBaseSink * bsink,
    GstBuffer * buffer);
static gboolean gst_base_audio_sink_event (GstBaseSink * bsink,
    GstEvent * event);
static void gst_base_audio_sink_get_times (GstBaseSink * bsink,
    GstBuffer * buffer, GstClockTime * start, GstClockTime * end);
static gboolean gst_base_audio_sink_setcaps (GstBaseSink * bsink,
    GstCaps * caps);
static void gst_base_audio_sink_fixate (GstBaseSink * bsink, GstCaps * caps);

/* static guint gst_base_audio_sink_signals[LAST_SIGNAL] = { 0 }; */

static void
gst_base_audio_sink_base_init (gpointer g_class)
{
}

static void
gst_base_audio_sink_class_init (GstBaseAudioSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  g_type_class_add_private (klass, sizeof (GstBaseAudioSinkPrivate));

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_get_property);
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_base_audio_sink_dispose);

  g_object_class_install_property (gobject_class, PROP_BUFFER_TIME,
      g_param_spec_int64 ("buffer-time", "Buffer Time",
          "Size of audio buffer in microseconds", 1,
          G_MAXINT64, DEFAULT_BUFFER_TIME,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LATENCY_TIME,
      g_param_spec_int64 ("latency-time", "Latency Time",
          "Audio latency in microseconds", 1,
          G_MAXINT64, DEFAULT_LATENCY_TIME,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PROVIDE_CLOCK,
      g_param_spec_boolean ("provide-clock", "Provide Clock",
          "Provide a clock to be used as the global pipeline clock",
          DEFAULT_PROVIDE_CLOCK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SLAVE_METHOD,
      g_param_spec_enum ("slave-method", "Slave Method",
          "Algorithm to use to match the rate of the masterclock",
          GST_TYPE_SLAVE_METHOD, DEFAULT_SLAVE_METHOD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_change_state);
  gstelement_class->provide_clock =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_provide_clock);
  gstelement_class->query = GST_DEBUG_FUNCPTR (gst_base_audio_sink_query);

  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_base_audio_sink_event);
  gstbasesink_class->preroll = GST_DEBUG_FUNCPTR (gst_base_audio_sink_preroll);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_base_audio_sink_render);
  gstbasesink_class->get_times =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_get_times);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_base_audio_sink_setcaps);
  gstbasesink_class->fixate = GST_DEBUG_FUNCPTR (gst_base_audio_sink_fixate);
  gstbasesink_class->async_play =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_async_play);
  gstbasesink_class->activate_pull =
      GST_DEBUG_FUNCPTR (gst_base_audio_sink_activate_pull);

  /* ref class from a thread-safe context to work around missing bit of
   * thread-safety in GObject */
  g_type_class_ref (GST_TYPE_AUDIO_CLOCK);
  g_type_class_ref (GST_TYPE_RING_BUFFER);
}

static void
gst_base_audio_sink_init (GstBaseAudioSink * baseaudiosink,
    GstBaseAudioSinkClass * g_class)
{
  baseaudiosink->priv = GST_BASE_AUDIO_SINK_GET_PRIVATE (baseaudiosink);

  baseaudiosink->buffer_time = DEFAULT_BUFFER_TIME;
  baseaudiosink->latency_time = DEFAULT_LATENCY_TIME;
  baseaudiosink->provide_clock = DEFAULT_PROVIDE_CLOCK;
  baseaudiosink->priv->slave_method = DEFAULT_SLAVE_METHOD;

  baseaudiosink->provided_clock = gst_audio_clock_new ("GstAudioSinkClock",
      (GstAudioClockGetTimeFunc) gst_base_audio_sink_get_time, baseaudiosink);

  GST_BASE_SINK (baseaudiosink)->can_activate_push = TRUE;
  /* FIXME, enable pull mode when segments, latency, state changes, negotiation
   * and clock slaving are figured out */
  GST_BASE_SINK (baseaudiosink)->can_activate_pull = FALSE;
}

static void
gst_base_audio_sink_dispose (GObject * object)
{
  GstBaseAudioSink *sink;

  sink = GST_BASE_AUDIO_SINK (object);

  if (sink->provided_clock)
    gst_object_unref (sink->provided_clock);
  sink->provided_clock = NULL;

  if (sink->ringbuffer) {
    gst_object_unparent (GST_OBJECT_CAST (sink->ringbuffer));
    sink->ringbuffer = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static GstClock *
gst_base_audio_sink_provide_clock (GstElement * elem)
{
  GstBaseAudioSink *sink;
  GstClock *clock;

  sink = GST_BASE_AUDIO_SINK (elem);

  /* we have no ringbuffer (must be NULL state) */
  if (sink->ringbuffer == NULL)
    goto wrong_state;

  if (!gst_ring_buffer_is_acquired (sink->ringbuffer))
    goto wrong_state;

  GST_OBJECT_LOCK (sink);
  if (!sink->provide_clock)
    goto clock_disabled;

  clock = GST_CLOCK_CAST (gst_object_ref (sink->provided_clock));
  GST_OBJECT_UNLOCK (sink);

  return clock;

  /* ERRORS */
wrong_state:
  {
    GST_DEBUG_OBJECT (sink, "ringbuffer not acquired");
    return NULL;
  }
clock_disabled:
  {
    GST_DEBUG_OBJECT (sink, "clock provide disabled");
    GST_OBJECT_UNLOCK (sink);
    return NULL;
  }
}

static gboolean
gst_base_audio_sink_query (GstElement * element, GstQuery * query)
{
  gboolean res = FALSE;

  GstBaseAudioSink *basesink = GST_BASE_AUDIO_SINK (element);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      gboolean live, us_live;
      GstClockTime min_l, max_l;

      GST_DEBUG_OBJECT (basesink, "latency query");

      if (!basesink->ringbuffer || !basesink->ringbuffer->spec.rate) {
        GST_DEBUG_OBJECT (basesink,
            "we are not yet negotiated, can't report latency yet");
        res = FALSE;
        goto done;
      }

      /* ask parent first, it will do an upstream query for us. */
      if ((res =
              gst_base_sink_query_latency (GST_BASE_SINK_CAST (basesink), &live,
                  &us_live, &min_l, &max_l))) {
        GstClockTime min_latency, max_latency;

        /* we and upstream are both live, adjust the min_latency */
        if (live && us_live) {
          GstRingBufferSpec *spec;

          spec = &basesink->ringbuffer->spec;

          basesink->priv->us_latency = min_l;

          min_latency =
              gst_util_uint64_scale_int (spec->seglatency * spec->segsize,
              GST_SECOND, spec->rate * spec->bytes_per_sample);

          /* we cannot go lower than the buffer size and the min peer latency */
          min_latency = min_latency + min_l;
          /* the max latency is the max of the peer, we can delay an infinite
           * amount of time. */
          max_latency = min_latency + (max_l == -1 ? 0 : max_l);

          GST_DEBUG_OBJECT (basesink,
              "peer min %" GST_TIME_FORMAT ", our min latency: %"
              GST_TIME_FORMAT, GST_TIME_ARGS (min_l),
              GST_TIME_ARGS (min_latency));
        } else {
          GST_DEBUG_OBJECT (basesink,
              "peer or we are not live, don't care about latency");
          min_latency = 0;
          max_latency = -1;
        }
        gst_query_set_latency (query, live, min_latency, max_latency);
      }
      break;
    }
    default:
      res = GST_ELEMENT_CLASS (parent_class)->query (element, query);
      break;
  }

done:
  return res;
}


static GstClockTime
gst_base_audio_sink_get_time (GstClock * clock, GstBaseAudioSink * sink)
{
  guint64 raw, samples;
  guint delay;
  GstClockTime result, us_latency;

  if (sink->ringbuffer == NULL || sink->ringbuffer->spec.rate == 0)
    return GST_CLOCK_TIME_NONE;

  /* our processed samples are always increasing */
  raw = samples = gst_ring_buffer_samples_done (sink->ringbuffer);

  /* the number of samples not yet processed, this is still queued in the
   * device (not played for playback). */
  delay = gst_ring_buffer_delay (sink->ringbuffer);

  if (G_LIKELY (samples >= delay))
    samples -= delay;
  else
    samples = 0;

  result = gst_util_uint64_scale_int (samples, GST_SECOND,
      sink->ringbuffer->spec.rate);

  /* latency before starting the clock */
  us_latency = sink->priv->us_latency;

  result += us_latency;

  GST_DEBUG_OBJECT (sink,
      "processed samples: raw %llu, delay %u, real %llu, time %"
      GST_TIME_FORMAT ", upstream latency %" GST_TIME_FORMAT, raw, delay,
      samples, GST_TIME_ARGS (result), GST_TIME_ARGS (us_latency));

  return result;
}

/**
 * gst_base_audio_sink_set_provide_clock:
 * @sink: a #GstBaseAudioSink
 * @provide: new state
 *
 * Controls whether @sink will provide a clock or not. If @provide is %TRUE, 
 * gst_element_provide_clock() will return a clock that reflects the datarate
 * of @sink. If @provide is %FALSE, gst_element_provide_clock() will return NULL.
 *
 * Since: 0.10.16
 */
void
gst_base_audio_sink_set_provide_clock (GstBaseAudioSink * sink,
    gboolean provide)
{
  g_return_if_fail (GST_IS_BASE_AUDIO_SINK (sink));

  GST_OBJECT_LOCK (sink);
  sink->provide_clock = provide;
  GST_OBJECT_UNLOCK (sink);
}

/**
 * gst_base_audio_sink_get_provide_clock:
 * @sink: a #GstBaseAudioSink
 *
 * Queries whether @sink will provide a clock or not. See also
 * gst_base_audio_sink_set_provide_clock.
 *
 * Returns: %TRUE if @sink will provide a clock.
 *
 * Since: 0.10.16
 */
gboolean
gst_base_audio_sink_get_provide_clock (GstBaseAudioSink * sink)
{
  gboolean result;

  g_return_val_if_fail (GST_IS_BASE_AUDIO_SINK (sink), FALSE);

  GST_OBJECT_LOCK (sink);
  result = sink->provide_clock;
  GST_OBJECT_UNLOCK (sink);

  return result;
}

/**
 * gst_base_audio_sink_set_slave_method:
 * @sink: a #GstBaseAudioSink
 * @method: the new slave method
 *
 * Controls how clock slaving will be performed in @sink. 
 *
 * Since: 0.10.16
 */
void
gst_base_audio_sink_set_slave_method (GstBaseAudioSink * sink,
    GstBaseAudioSinkSlaveMethod method)
{
  g_return_if_fail (GST_IS_BASE_AUDIO_SINK (sink));

  GST_OBJECT_LOCK (sink);
  sink->priv->slave_method = method;
  GST_OBJECT_UNLOCK (sink);
}

/**
 * gst_base_audio_sink_get_slave_method:
 * @sink: a #GstBaseAudioSink
 *
 * Get the current slave method used by @sink.
 *
 * Returns: The current slave method used by @sink.
 *
 * Since: 0.10.16
 */
GstBaseAudioSinkSlaveMethod
gst_base_audio_sink_get_slave_method (GstBaseAudioSink * sink)
{
  GstBaseAudioSinkSlaveMethod result;

  g_return_val_if_fail (GST_IS_BASE_AUDIO_SINK (sink), -1);

  GST_OBJECT_LOCK (sink);
  result = sink->priv->slave_method;
  GST_OBJECT_UNLOCK (sink);

  return result;
}

static void
gst_base_audio_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBaseAudioSink *sink;

  sink = GST_BASE_AUDIO_SINK (object);

  switch (prop_id) {
    case PROP_BUFFER_TIME:
      sink->buffer_time = g_value_get_int64 (value);
      break;
    case PROP_LATENCY_TIME:
      sink->latency_time = g_value_get_int64 (value);
      break;
    case PROP_PROVIDE_CLOCK:
      gst_base_audio_sink_set_provide_clock (sink, g_value_get_boolean (value));
      break;
    case PROP_SLAVE_METHOD:
      gst_base_audio_sink_set_slave_method (sink, g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_base_audio_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstBaseAudioSink *sink;

  sink = GST_BASE_AUDIO_SINK (object);

  switch (prop_id) {
    case PROP_BUFFER_TIME:
      g_value_set_int64 (value, sink->buffer_time);
      break;
    case PROP_LATENCY_TIME:
      g_value_set_int64 (value, sink->latency_time);
      break;
    case PROP_PROVIDE_CLOCK:
      g_value_set_boolean (value, gst_base_audio_sink_get_provide_clock (sink));
      break;
    case PROP_SLAVE_METHOD:
      g_value_set_enum (value, gst_base_audio_sink_get_slave_method (sink));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_base_audio_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstBaseAudioSink *sink = GST_BASE_AUDIO_SINK (bsink);
  GstRingBufferSpec *spec;

  if (!sink->ringbuffer)
    return FALSE;

  spec = &sink->ringbuffer->spec;

  GST_DEBUG_OBJECT (sink, "release old ringbuffer");

  /* release old ringbuffer */
  gst_ring_buffer_release (sink->ringbuffer);

  GST_DEBUG_OBJECT (sink, "parse caps");

  spec->buffer_time = sink->buffer_time;
  spec->latency_time = sink->latency_time;

  /* parse new caps */
  if (!gst_ring_buffer_parse_caps (spec, caps))
    goto parse_error;

  gst_ring_buffer_debug_spec_buff (spec);

  GST_DEBUG_OBJECT (sink, "acquire new ringbuffer");

  if (!gst_ring_buffer_acquire (sink->ringbuffer, spec))
    goto acquire_error;

  /* calculate actual latency and buffer times. 
   * FIXME: In 0.11, store the latency_time internally in ns */
  spec->latency_time = gst_util_uint64_scale (spec->segsize,
      (GST_SECOND / GST_USECOND), spec->rate * spec->bytes_per_sample);

  spec->buffer_time = spec->segtotal * spec->latency_time;

  gst_ring_buffer_debug_spec_buff (spec);

  return TRUE;

  /* ERRORS */
parse_error:
  {
    GST_DEBUG_OBJECT (sink, "could not parse caps");
    GST_ELEMENT_ERROR (sink, STREAM, FORMAT,
        (NULL), ("cannot parse audio format."));
    return FALSE;
  }
acquire_error:
  {
    GST_DEBUG_OBJECT (sink, "could not acquire ringbuffer");
    return FALSE;
  }
}

static void
gst_base_audio_sink_fixate (GstBaseSink * bsink, GstCaps * caps)
{
  GstStructure *s;
  gint width, depth;

  s = gst_caps_get_structure (caps, 0);

  /* fields for all formats */
  gst_structure_fixate_field_nearest_int (s, "rate", 44100);
  gst_structure_fixate_field_nearest_int (s, "channels", 2);
  gst_structure_fixate_field_nearest_int (s, "width", 16);

  /* fields for int */
  if (gst_structure_has_field (s, "depth")) {
    gst_structure_get_int (s, "width", &width);
    /* round width to nearest multiple of 8 for the depth */
    depth = GST_ROUND_UP_8 (width);
    gst_structure_fixate_field_nearest_int (s, "depth", depth);
  }
  if (gst_structure_has_field (s, "signed"))
    gst_structure_fixate_field_boolean (s, "signed", TRUE);
  if (gst_structure_has_field (s, "endianness"))
    gst_structure_fixate_field_nearest_int (s, "endianness", G_BYTE_ORDER);
}

static void
gst_base_audio_sink_get_times (GstBaseSink * bsink, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  /* our clock sync is a bit too much for the base class to handle so
   * we implement it ourselves. */
  *start = GST_CLOCK_TIME_NONE;
  *end = GST_CLOCK_TIME_NONE;
}

/* This waits for the drain to happen and can be canceled */
static gboolean
gst_base_audio_sink_drain (GstBaseAudioSink * sink)
{
  GstClockTime base_time;

  if (!sink->ringbuffer)
    return TRUE;
  if (!sink->ringbuffer->spec.rate)
    return TRUE;

  /* need to start playback before we can drain, but only when
   * we have successfully negotiated a format and thus acquired the
   * ringbuffer. */
  if (gst_ring_buffer_is_acquired (sink->ringbuffer))
    gst_ring_buffer_start (sink->ringbuffer);

  if (sink->next_sample != -1) {
    GstClockTime time;

    /* convert next expected sample to time */
    time =
        gst_util_uint64_scale_int (sink->next_sample, GST_SECOND,
        sink->ringbuffer->spec.rate);

    GST_DEBUG_OBJECT (sink,
        "last sample %" G_GUINT64_FORMAT ", time %" GST_TIME_FORMAT,
        sink->next_sample, GST_TIME_ARGS (time));

    /* our time already includes the base_time, _wait_eos() wants a running_time
     * so we have to subtract the base_time again here. FIXME, store an
     * unadjusted EOS time so that we don't have to do this. */
    GST_OBJECT_LOCK (sink);
    base_time = GST_ELEMENT_CAST (sink)->base_time;
    GST_OBJECT_UNLOCK (sink);

    if (time > base_time)
      time -= base_time;
    else
      time = 0;

    /* wait for the EOS time to be reached, this is the time when the last
     * sample is played. */
    gst_base_sink_wait_eos (GST_BASE_SINK (sink), time, NULL);

    sink->next_sample = -1;
  }
  return TRUE;
}

static gboolean
gst_base_audio_sink_event (GstBaseSink * bsink, GstEvent * event)
{
  GstBaseAudioSink *sink = GST_BASE_AUDIO_SINK (bsink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      if (sink->ringbuffer)
        gst_ring_buffer_set_flushing (sink->ringbuffer, TRUE);
      break;
    case GST_EVENT_FLUSH_STOP:
      /* always resync on sample after a flush */
      sink->priv->avg_skew = -1;
      sink->next_sample = -1;
      if (sink->ringbuffer)
        gst_ring_buffer_set_flushing (sink->ringbuffer, FALSE);
      break;
    case GST_EVENT_EOS:
      /* now wait till we played everything */
      gst_base_audio_sink_drain (sink);
      break;
    case GST_EVENT_NEWSEGMENT:
    {
      gdouble rate;

      /* we only need the rate */
      gst_event_parse_new_segment_full (event, NULL, &rate, NULL, NULL,
          NULL, NULL, NULL);

      GST_DEBUG_OBJECT (sink, "new segment rate of %f", rate);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static GstFlowReturn
gst_base_audio_sink_preroll (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstBaseAudioSink *sink = GST_BASE_AUDIO_SINK (bsink);

  if (!gst_ring_buffer_is_acquired (sink->ringbuffer))
    goto wrong_state;

  /* we don't really do anything when prerolling. We could make a
   * property to play this buffer to have some sort of scrubbing
   * support. */
  return GST_FLOW_OK;

wrong_state:
  {
    GST_DEBUG_OBJECT (sink, "ringbuffer in wrong state");
    GST_ELEMENT_ERROR (sink, STREAM, FORMAT, (NULL), ("sink not negotiated."));
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static guint64
gst_base_audio_sink_get_offset (GstBaseAudioSink * sink)
{
  guint64 sample;
  gint writeseg, segdone, sps;
  gint diff;

  /* assume we can append to the previous sample */
  sample = sink->next_sample;
  /* no previous sample, try to insert at position 0 */
  if (sample == -1)
    sample = 0;

  sps = sink->ringbuffer->samples_per_seg;

  /* figure out the segment and the offset inside the segment where
   * the sample should be written. */
  writeseg = sample / sps;

  /* get the currently processed segment */
  segdone = g_atomic_int_get (&sink->ringbuffer->segdone)
      - sink->ringbuffer->segbase;

  /* see how far away it is from the write segment */
  diff = writeseg - segdone;
  if (diff < 0) {
    /* sample would be dropped, position to next playable position */
    sample = (segdone + 1) * sps;
  }

  return sample;
}

static GstClockTime
clock_convert_external (GstClockTime external, GstClockTime cinternal,
    GstClockTime cexternal, GstClockTime crate_num, GstClockTime crate_denom,
    GstClockTime us_latency)
{
  /* adjust for rate and speed */
  if (external >= cexternal) {
    external =
        gst_util_uint64_scale (external - cexternal, crate_denom, crate_num);
    external += cinternal;
  } else {
    external = gst_util_uint64_scale (cexternal - external,
        crate_denom, crate_num);
    if (cinternal > external)
      external = cinternal - external;
    else
      external = 0;
  }
  /* adjust for offset when slaving started */
  if (external > us_latency)
    external -= us_latency;
  else
    external = 0;

  return external;
}

/* algorithm to calculate sample positions that will result in resampling to
 * match the clock rate of the master */
static void
gst_base_audio_sink_resample_slaving (GstBaseAudioSink * sink,
    GstClockTime render_start, GstClockTime render_stop,
    GstClockTime * srender_start, GstClockTime * srender_stop)
{
  GstClockTime cinternal, cexternal;
  GstClockTime crate_num, crate_denom;

  /* get calibration parameters to compensate for speed and offset differences
   * when we are slaved */
  gst_clock_get_calibration (sink->provided_clock, &cinternal, &cexternal,
      &crate_num, &crate_denom);

  GST_DEBUG_OBJECT (sink, "internal %" GST_TIME_FORMAT " external %"
      GST_TIME_FORMAT " %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " = %f",
      GST_TIME_ARGS (cinternal), GST_TIME_ARGS (cexternal), crate_num,
      crate_denom, gst_guint64_to_gdouble (crate_num) /
      gst_guint64_to_gdouble (crate_denom));

  if (crate_num == 0)
    crate_denom = crate_num = 1;

  /* bring external time to internal time */
  render_start = clock_convert_external (render_start, cinternal, cexternal,
      crate_num, crate_denom, sink->priv->us_latency);
  render_stop = clock_convert_external (render_stop, cinternal, cexternal,
      crate_num, crate_denom, sink->priv->us_latency);

  GST_DEBUG_OBJECT (sink,
      "after slaving: start %" GST_TIME_FORMAT " - stop %" GST_TIME_FORMAT,
      GST_TIME_ARGS (render_start), GST_TIME_ARGS (render_stop));

  *srender_start = render_start;
  *srender_stop = render_stop;
}

/* algorithm to calculate sample positions that will result in changing the
 * playout pointer to match the clock rate of the master */
static void
gst_base_audio_sink_skew_slaving (GstBaseAudioSink * sink,
    GstClockTime render_start, GstClockTime render_stop,
    GstClockTime * srender_start, GstClockTime * srender_stop)
{
  GstClockTime cinternal, cexternal, crate_num, crate_denom;
  GstClockTime etime, itime;
  GstClockTimeDiff skew, segtime, segtime2;
  gint segsamples;
  gint64 last_align;

  /* get calibration parameters to compensate for offsets */
  gst_clock_get_calibration (sink->provided_clock, &cinternal, &cexternal,
      &crate_num, &crate_denom);

  /* sample clocks and figure out clock skew */
  etime = gst_clock_get_time (GST_ELEMENT_CLOCK (sink));
  itime = gst_clock_get_internal_time (sink->provided_clock);

  /* make sure we never go below 0 */
  etime = etime > cexternal ? etime - cexternal : 0;
  itime = itime > cinternal ? itime - cinternal : 0;

  skew = GST_CLOCK_DIFF (etime, itime);
  if (sink->priv->avg_skew == -1) {
    /* first observation */
    sink->priv->avg_skew = skew;
  } else {
    /* next observations use a moving average */
    sink->priv->avg_skew = (31 * sink->priv->avg_skew + skew) / 32;
  }

  GST_DEBUG_OBJECT (sink, "internal %" GST_TIME_FORMAT " external %"
      GST_TIME_FORMAT " skew %" G_GINT64_FORMAT " avg %" G_GINT64_FORMAT,
      GST_TIME_ARGS (itime), GST_TIME_ARGS (etime), skew, sink->priv->avg_skew);

  /* the max drift we allow is the length of a segment */
  segtime = sink->ringbuffer->spec.latency_time * 1000;
  segtime2 = segtime / 2;

  /* adjust playout pointer based on skew */
  if (sink->priv->avg_skew > segtime2) {
    /* master is running slower, move internal time forward */
    GST_WARNING_OBJECT (sink,
        "correct clock skew %" G_GINT64_FORMAT " > %" G_GINT64_FORMAT,
        sink->priv->avg_skew, segtime2);
    cexternal = cexternal > segtime ? cexternal - segtime : 0;
    sink->priv->avg_skew -= segtime;

    segsamples =
        sink->ringbuffer->spec.segsize /
        sink->ringbuffer->spec.bytes_per_sample;
    last_align = sink->priv->last_align;

    /* if we were aligning in the wrong direction or we aligned more than what we
     * will correct, resync */
    if (last_align < 0 || last_align > segsamples)
      sink->next_sample = -1;

    GST_DEBUG_OBJECT (sink,
        "last_align %" G_GINT64_FORMAT " segsamples %u, next %"
        G_GUINT64_FORMAT, last_align, segsamples, sink->next_sample);

    gst_clock_set_calibration (sink->provided_clock, cinternal, cexternal,
        crate_num, crate_denom);
  } else if (sink->priv->avg_skew < -segtime2) {
    /* master is running faster, move external time forwards */
    GST_WARNING_OBJECT (sink,
        "correct clock skew %" G_GINT64_FORMAT " < %" G_GINT64_FORMAT,
        sink->priv->avg_skew, -segtime2);
    cexternal += segtime;
    sink->priv->avg_skew += segtime;

    segsamples =
        sink->ringbuffer->spec.segsize /
        sink->ringbuffer->spec.bytes_per_sample;
    last_align = sink->priv->last_align;

    /* if we were aligning in the wrong direction or we aligned more than what we
     * will correct, resync */
    if (last_align > 0 || -last_align > segsamples)
      sink->next_sample = -1;

    GST_DEBUG_OBJECT (sink,
        "last_align %" G_GINT64_FORMAT " segsamples %u, next %"
        G_GUINT64_FORMAT, last_align, segsamples, sink->next_sample);

    gst_clock_set_calibration (sink->provided_clock, cinternal, cexternal,
        crate_num, crate_denom);
  }

  /* convert, ignoring speed */
  render_start = clock_convert_external (render_start, cinternal, cexternal,
      crate_num, crate_denom, sink->priv->us_latency);
  render_stop = clock_convert_external (render_stop, cinternal, cexternal,
      crate_num, crate_denom, sink->priv->us_latency);

  *srender_start = render_start;
  *srender_stop = render_stop;
}

/* apply the clock offset but do no slaving otherwise */
static void
gst_base_audio_sink_none_slaving (GstBaseAudioSink * sink,
    GstClockTime render_start, GstClockTime render_stop,
    GstClockTime * srender_start, GstClockTime * srender_stop)
{
  GstClockTime cinternal, cexternal, crate_num, crate_denom;

  /* get calibration parameters to compensate for offsets */
  gst_clock_get_calibration (sink->provided_clock, &cinternal, &cexternal,
      &crate_num, &crate_denom);

  /* convert, ignoring speed */
  render_start = clock_convert_external (render_start, cinternal, cexternal,
      crate_num, crate_denom, sink->priv->us_latency);
  render_stop = clock_convert_external (render_stop, cinternal, cexternal,
      crate_num, crate_denom, sink->priv->us_latency);

  *srender_start = render_start;
  *srender_stop = render_stop;
}

/* converts render_start and render_stop to their slaved values */
static void
gst_base_audio_sink_handle_slaving (GstBaseAudioSink * sink,
    GstClockTime render_start, GstClockTime render_stop,
    GstClockTime * srender_start, GstClockTime * srender_stop)
{
  switch (sink->priv->slave_method) {
    case GST_BASE_AUDIO_SINK_SLAVE_RESAMPLE:
      gst_base_audio_sink_resample_slaving (sink, render_start, render_stop,
          srender_start, srender_stop);
      break;
    case GST_BASE_AUDIO_SINK_SLAVE_SKEW:
      gst_base_audio_sink_skew_slaving (sink, render_start, render_stop,
          srender_start, srender_stop);
      break;
    case GST_BASE_AUDIO_SINK_SLAVE_NONE:
      gst_base_audio_sink_none_slaving (sink, render_start, render_stop,
          srender_start, srender_stop);
      break;
    default:
      g_warning ("unknown slaving method %d", sink->priv->slave_method);
      break;
  }
}

static GstFlowReturn
gst_base_audio_sink_render (GstBaseSink * bsink, GstBuffer * buf)
{
  guint64 in_offset;
  GstClockTime time, stop, render_start, render_stop, sample_offset;
  GstBaseAudioSink *sink;
  GstRingBuffer *ringbuf;
  gint64 diff, align, ctime, cstop;
  guint8 *data;
  guint size;
  guint samples, written;
  gint bps;
  gint accum;
  gint out_samples;
  GstClockTime base_time = GST_CLOCK_TIME_NONE, latency;
  GstClock *clock;
  gboolean sync, slaved, align_next;

  sink = GST_BASE_AUDIO_SINK (bsink);

  ringbuf = sink->ringbuffer;

  /* can't do anything when we don't have the device */
  if (G_UNLIKELY (!gst_ring_buffer_is_acquired (ringbuf)))
    goto wrong_state;

  bps = ringbuf->spec.bytes_per_sample;

  size = GST_BUFFER_SIZE (buf);
  if (G_UNLIKELY (size % bps) != 0)
    goto wrong_size;

  samples = size / bps;
  out_samples = samples;

  in_offset = GST_BUFFER_OFFSET (buf);
  time = GST_BUFFER_TIMESTAMP (buf);
  stop = time + gst_util_uint64_scale_int (samples, GST_SECOND,
      ringbuf->spec.rate);

  GST_DEBUG_OBJECT (sink,
      "time %" GST_TIME_FORMAT ", offset %llu, start %" GST_TIME_FORMAT
      ", samples %u", GST_TIME_ARGS (time), in_offset,
      GST_TIME_ARGS (bsink->segment.start), samples);

  data = GST_BUFFER_DATA (buf);

  /* if not valid timestamp or we can't clip or sync, try to play
   * sample ASAP */
  if (!GST_CLOCK_TIME_IS_VALID (time)) {
    render_start = gst_base_audio_sink_get_offset (sink);
    render_stop = render_start + samples;
    GST_DEBUG_OBJECT (sink,
        "Buffer of size %u has no time. Using render_start=%" G_GUINT64_FORMAT,
        GST_BUFFER_SIZE (buf), render_start);
    goto no_sync;
  }

  /* samples should be rendered based on their timestamp. All samples
   * arriving before the segment.start or after segment.stop are to be 
   * thrown away. All samples should also be clipped to the segment 
   * boundaries */
  /* let's calc stop based on the number of samples in the buffer instead
   * of trusting the DURATION */
  if (!gst_segment_clip (&bsink->segment, GST_FORMAT_TIME, time, stop, &ctime,
          &cstop))
    goto out_of_segment;

  /* see if some clipping happened */
  diff = ctime - time;
  if (diff > 0) {
    /* bring clipped time to samples */
    diff = gst_util_uint64_scale_int (diff, ringbuf->spec.rate, GST_SECOND);
    GST_DEBUG_OBJECT (sink, "clipping start to %" GST_TIME_FORMAT " %"
        G_GUINT64_FORMAT " samples", GST_TIME_ARGS (ctime), diff);
    samples -= diff;
    data += diff * bps;
    time = ctime;
  }
  diff = stop - cstop;
  if (diff > 0) {
    /* bring clipped time to samples */
    diff = gst_util_uint64_scale_int (diff, ringbuf->spec.rate, GST_SECOND);
    GST_DEBUG_OBJECT (sink, "clipping stop to %" GST_TIME_FORMAT " %"
        G_GUINT64_FORMAT " samples", GST_TIME_ARGS (cstop), diff);
    samples -= diff;
    stop = cstop;
  }

  /* figure out how to sync */
  if ((clock = GST_ELEMENT_CLOCK (bsink)))
    sync = bsink->sync;
  else
    sync = FALSE;

  if (!sync) {
    /* no sync needed, play sample ASAP */
    render_start = gst_base_audio_sink_get_offset (sink);
    render_stop = render_start + samples;
    GST_DEBUG_OBJECT (sink,
        "no sync needed. Using render_start=%" G_GUINT64_FORMAT, render_start);
    goto no_sync;
  }

  /* bring buffer start and stop times to running time */
  render_start =
      gst_segment_to_running_time (&bsink->segment, GST_FORMAT_TIME, time);
  render_stop =
      gst_segment_to_running_time (&bsink->segment, GST_FORMAT_TIME, stop);

  GST_DEBUG_OBJECT (sink,
      "running: start %" GST_TIME_FORMAT " - stop %" GST_TIME_FORMAT,
      GST_TIME_ARGS (render_start), GST_TIME_ARGS (render_stop));

  base_time = gst_element_get_base_time (GST_ELEMENT_CAST (bsink));

  GST_DEBUG_OBJECT (sink, "base_time %" GST_TIME_FORMAT,
      GST_TIME_ARGS (base_time));

  /* add base time to sync against the clock */
  render_start += base_time;
  render_stop += base_time;

  /* compensate for latency */
  latency = gst_base_sink_get_latency (bsink);
  GST_DEBUG_OBJECT (sink,
      "compensating for latency %" GST_TIME_FORMAT, GST_TIME_ARGS (latency));

  /* add latency to get the timestamp to sync against the pipeline clock */
  render_start += latency;
  render_stop += latency;

  GST_DEBUG_OBJECT (sink,
      "after latency: start %" GST_TIME_FORMAT " - stop %" GST_TIME_FORMAT,
      GST_TIME_ARGS (render_start), GST_TIME_ARGS (render_stop));

  if ((slaved = clock != sink->provided_clock)) {
    /* handle clock slaving */
    gst_base_audio_sink_handle_slaving (sink, render_start, render_stop,
        &render_start, &render_stop);
  } else {
    /* no slaving needed but we need to adapt to the clock calibration
     * parameters */
    gst_base_audio_sink_none_slaving (sink, render_start, render_stop,
        &render_start, &render_stop);
  }

  /* and bring the time to the rate corrected offset in the buffer */
  render_start = gst_util_uint64_scale_int (render_start,
      ringbuf->spec.rate, GST_SECOND);
  render_stop = gst_util_uint64_scale_int (render_stop,
      ringbuf->spec.rate, GST_SECOND);

  /* always resync after a discont */
  if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DISCONT))) {
    GST_DEBUG_OBJECT (sink, "resync after discont");
    goto no_align;
  }

  if (G_UNLIKELY (sink->next_sample == -1)) {
    GST_DEBUG_OBJECT (sink,
        "no align possible: no previous sample position known");
    goto no_align;
  }

  /* positive playback rate, first sample is render_start, negative rate, first
   * sample is render_stop */
  if (bsink->segment.rate >= 0.0)
    sample_offset = render_start;
  else
    sample_offset = render_stop;

  /* now try to align the sample to the previous one */
  if (sample_offset >= sink->next_sample)
    diff = sample_offset - sink->next_sample;
  else
    diff = sink->next_sample - sample_offset;

  /* we tollerate half a second diff before we start resyncing. This
   * should be enough to compensate for various rounding errors in the timestamp
   * and sample offset position. We always resync if we got a discont anyway and
   * non-discont should be aligned by definition. */
  if (G_LIKELY (diff < ringbuf->spec.rate / DIFF_TOLERANCE)) {
    /* calc align with previous sample */
    align = sink->next_sample - sample_offset;
    GST_DEBUG_OBJECT (sink,
        "align with prev sample, ABS (%" G_GINT64_FORMAT ") < %d", align,
        ringbuf->spec.rate / DIFF_TOLERANCE);
  } else {
    /* bring sample diff to seconds for error message */
    diff = gst_util_uint64_scale_int (diff, GST_SECOND, ringbuf->spec.rate);
    /* timestamps drifted apart from previous samples too much, we need to
     * resync. We log this as an element warning. */
    GST_ELEMENT_WARNING (sink, CORE, CLOCK,
        ("Compensating for audio synchronisation problems"),
        ("Unexpected discontinuity in audio timestamps of more "
            "than half a second (%" GST_TIME_FORMAT "), resyncing",
            GST_TIME_ARGS (diff)));
    align = 0;
  }
  sink->priv->last_align = align;

  /* apply alignment */
  render_start += align;

  /* only align stop if we are not slaved to resample */
  if (slaved && sink->priv->slave_method == GST_BASE_AUDIO_SINK_SLAVE_RESAMPLE) {
    GST_DEBUG_OBJECT (sink, "no stop time align needed: we are slaved");
    goto no_align;
  }
  render_stop += align;

no_align:
  /* number of target samples is difference between start and stop */
  out_samples = render_stop - render_start;

no_sync:
  /* we render the first or last sample first, depending on the rate */
  if (bsink->segment.rate >= 0.0)
    sample_offset = render_start;
  else
    sample_offset = render_stop;

  GST_DEBUG_OBJECT (sink, "rendering at %" G_GUINT64_FORMAT " %d/%d",
      sample_offset, samples, out_samples);

  /* we need to accumulate over different runs for when we get interrupted */
  accum = 0;
  align_next = TRUE;
  do {
    written =
        gst_ring_buffer_commit_full (ringbuf, &sample_offset, data, samples,
        out_samples, &accum);

    GST_DEBUG_OBJECT (sink, "wrote %u of %u", written, samples);
    /* if we wrote all, we're done */
    if (written == samples)
      break;

    /* else something interrupted us and we wait for preroll. */
    if (gst_base_sink_wait_preroll (bsink) != GST_FLOW_OK)
      goto stopping;

    /* if we got interrupted, we cannot assume that the next sample should
     * be aligned to this one */
    align_next = FALSE;

    samples -= written;
    data += written * bps;
  } while (TRUE);

  if (align_next)
    sink->next_sample = sample_offset;
  else
    sink->next_sample = -1;

  GST_DEBUG_OBJECT (sink, "next sample expected at %" G_GUINT64_FORMAT,
      sink->next_sample);

  if (GST_CLOCK_TIME_IS_VALID (stop) && stop >= bsink->segment.stop) {
    GST_DEBUG_OBJECT (sink,
        "start playback because we are at the end of segment");
    gst_ring_buffer_start (ringbuf);
  }

  return GST_FLOW_OK;

  /* SPECIAL cases */
out_of_segment:
  {
    GST_DEBUG_OBJECT (sink,
        "dropping sample out of segment time %" GST_TIME_FORMAT ", start %"
        GST_TIME_FORMAT, GST_TIME_ARGS (time),
        GST_TIME_ARGS (bsink->segment.start));
    return GST_FLOW_OK;
  }
  /* ERRORS */
wrong_state:
  {
    GST_DEBUG_OBJECT (sink, "ringbuffer not negotiated");
    GST_ELEMENT_ERROR (sink, STREAM, FORMAT, (NULL), ("sink not negotiated."));
    return GST_FLOW_NOT_NEGOTIATED;
  }
wrong_size:
  {
    GST_DEBUG_OBJECT (sink, "wrong size");
    GST_ELEMENT_ERROR (sink, STREAM, WRONG_TYPE,
        (NULL), ("sink received buffer of wrong size."));
    return GST_FLOW_ERROR;
  }
stopping:
  {
    GST_DEBUG_OBJECT (sink, "ringbuffer is stopping");
    return GST_FLOW_WRONG_STATE;
  }
}

/**
 * gst_base_audio_sink_create_ringbuffer:
 * @sink: a #GstBaseAudioSink.
 *
 * Create and return the #GstRingBuffer for @sink. This function will call the
 * ::create_ringbuffer vmethod and will set @sink as the parent of the returned
 * buffer (see gst_object_set_parent()).
 *
 * Returns: The new ringbuffer of @sink.
 */
GstRingBuffer *
gst_base_audio_sink_create_ringbuffer (GstBaseAudioSink * sink)
{
  GstBaseAudioSinkClass *bclass;
  GstRingBuffer *buffer = NULL;

  bclass = GST_BASE_AUDIO_SINK_GET_CLASS (sink);
  if (bclass->create_ringbuffer)
    buffer = bclass->create_ringbuffer (sink);

  if (buffer)
    gst_object_set_parent (GST_OBJECT (buffer), GST_OBJECT (sink));

  return buffer;
}

static gboolean
gst_base_audio_sink_activate_pull (GstBaseSink * basesink, gboolean active)
{
  gboolean ret;
  GstBaseAudioSink *sink = GST_BASE_AUDIO_SINK (basesink);

  if (active) {
    gst_ring_buffer_set_callback (sink->ringbuffer,
        gst_base_audio_sink_callback, sink);
    ret = gst_ring_buffer_start (sink->ringbuffer);
  } else {
    gst_ring_buffer_set_callback (sink->ringbuffer, NULL, NULL);
    /* stop thread */
    ret = gst_ring_buffer_release (sink->ringbuffer);
  }

  return ret;
}

static void
gst_base_audio_sink_callback (GstRingBuffer * rbuf, guint8 * data, guint len,
    gpointer user_data)
{
  GstBaseSink *basesink;
  GstBaseAudioSink *sink;
  GstBuffer *buf;
  GstFlowReturn ret;

  basesink = GST_BASE_SINK (user_data);
  sink = GST_BASE_AUDIO_SINK (user_data);

  /* would be nice to arrange for pad_alloc_buffer to return data -- as it is we
     will copy twice, once into data, once into DMA */
  GST_LOG_OBJECT (basesink, "pulling %d bytes offset %" G_GUINT64_FORMAT
      " to fill audio buffer", len, basesink->offset);
  ret = gst_pad_pull_range (basesink->sinkpad, basesink->offset, len, &buf);

  if (ret != GST_FLOW_OK) {
    if (ret == GST_FLOW_UNEXPECTED)
      goto eos;
    else
      goto error;
  }

  if (len != GST_BUFFER_SIZE (buf)) {
    GST_INFO_OBJECT (basesink, "short read pulling from sink pad: %d<%d",
        len, GST_BUFFER_SIZE (buf));
    len = MIN (GST_BUFFER_SIZE (buf), len);
  }

  basesink->offset += len;

  memcpy (data, GST_BUFFER_DATA (buf), len);

  return;

error:
  {
    GST_WARNING_OBJECT (basesink, "Got flow error but can't return it: %d",
        ret);
    return;
  }
eos:
  {
    /* FIXME: this is not quite correct; we'll be called endlessly until
     * the sink gets shut down; maybe we should set a flag somewhere, or
     * set segment.stop and segment.duration to the last sample or so */
    GST_DEBUG_OBJECT (sink, "EOS");
    gst_element_post_message (GST_ELEMENT_CAST (sink),
        gst_message_new_eos (GST_OBJECT_CAST (sink)));
    gst_base_audio_sink_drain (sink);
  }
}

/* should be called with the LOCK */
static GstStateChangeReturn
gst_base_audio_sink_async_play (GstBaseSink * basesink)
{
  GstClock *clock;
  GstBaseAudioSink *sink;
  GstClockTime itime, etime;
  GstClockTime rate_num, rate_denom;

  sink = GST_BASE_AUDIO_SINK (basesink);

  GST_DEBUG_OBJECT (sink, "ringbuffer may start now");
  gst_ring_buffer_may_start (sink->ringbuffer, TRUE);

  clock = GST_ELEMENT_CLOCK (sink);
  if (clock == NULL)
    goto done;

  /* we provided the global clock, don't need to do anything special */
  if (clock == sink->provided_clock)
    goto done;

  /* if we are slaved to a clock, we need to set the initial
   * calibration */
  /* get external and internal time to set as calibration params */
  etime = gst_clock_get_time (clock);
  itime = gst_clock_get_internal_time (sink->provided_clock);

  sink->priv->avg_skew = -1;
  sink->next_sample = -1;

  GST_DEBUG_OBJECT (sink,
      "internal time: %" GST_TIME_FORMAT " external time: %" GST_TIME_FORMAT,
      GST_TIME_ARGS (itime), GST_TIME_ARGS (etime));

  gst_clock_get_calibration (sink->provided_clock, NULL, NULL, &rate_num,
      &rate_denom);
  gst_clock_set_calibration (sink->provided_clock, itime, etime,
      rate_num, rate_denom);

  switch (sink->priv->slave_method) {
    case GST_BASE_AUDIO_SINK_SLAVE_RESAMPLE:
      /* only set as master if we need to resample */
      GST_DEBUG_OBJECT (sink, "Setting clock as master");
      gst_clock_set_master (sink->provided_clock, clock);
      break;
    default:
      break;
  }

  /* start ringbuffer so we can start slaving right away when we need to */
  gst_ring_buffer_start (sink->ringbuffer);

done:
  return GST_STATE_CHANGE_SUCCESS;
}

static GstStateChangeReturn
gst_base_audio_sink_do_play (GstBaseAudioSink * sink)
{
  GstStateChangeReturn ret;

  GST_OBJECT_LOCK (sink);
  ret = gst_base_audio_sink_async_play (GST_BASE_SINK_CAST (sink));
  GST_OBJECT_UNLOCK (sink);

  return ret;
}

static GstStateChangeReturn
gst_base_audio_sink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstBaseAudioSink *sink = GST_BASE_AUDIO_SINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (sink->ringbuffer == NULL) {
        sink->ringbuffer = gst_base_audio_sink_create_ringbuffer (sink);
      }
      if (!gst_ring_buffer_open_device (sink->ringbuffer))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      sink->next_sample = -1;
      sink->priv->last_align = -1;
      gst_ring_buffer_set_flushing (sink->ringbuffer, FALSE);
      gst_ring_buffer_may_start (sink->ringbuffer, FALSE);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      gst_base_audio_sink_do_play (sink);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* need to take the lock so we don't interfere with an
       * async play */
      GST_OBJECT_LOCK (sink);
      /* ringbuffer cannot start anymore */
      gst_ring_buffer_may_start (sink->ringbuffer, FALSE);
      gst_ring_buffer_pause (sink->ringbuffer);
      GST_OBJECT_UNLOCK (sink);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* make sure we unblock before calling the parent state change
       * so it can grab the STREAM_LOCK */
      gst_ring_buffer_set_flushing (sink->ringbuffer, TRUE);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop slaving ourselves to the master, if any */
      gst_clock_set_master (sink->provided_clock, NULL);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_ring_buffer_release (sink->ringbuffer);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      /* we release again here because the aqcuire happens when setting the
       * caps, which happens before we commit the state to PAUSED and thus the
       * PAUSED->READY state change (see above, where we release the ringbuffer)
       * might not be called when we get here. */
      gst_ring_buffer_release (sink->ringbuffer);
      gst_ring_buffer_close_device (sink->ringbuffer);
      break;
    default:
      break;
  }

  return ret;

  /* ERRORS */
open_failed:
  {
    /* subclass must post a meaningfull error message */
    GST_DEBUG_OBJECT (sink, "open failed");
    return GST_STATE_CHANGE_FAILURE;
  }
}
