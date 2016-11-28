/*
  Copyright (C) 2003-2013 Paul Brossier <piem@aubio.org>

  This file is part of aubio.

  aubio is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  aubio is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with aubio.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <assert.h>

#include "aubio_priv.h"
#include "fvec.h"
#include "fmat.h"
#include "io/source.h"
#include "utils/ringbuffer.h"
#include "synth/sampler.h"

#define HAVE_THREADS 1
#define READER_THREAD_ON 1
#if 0
#undef HAVE_THREADS
#endif

#ifdef HAVE_THREADS
#include <pthread.h>
#endif

typedef enum {
  aubio_sampler_reading_from_source,
  aubio_sampler_reading_from_table,
  aubio_sampler_n_reading_methods
} aubio_sampler_reading_method;


typedef enum {
  aubio_sampler_interp_pitchtime,
  aubio_sampler_interp_quad,
  aubio_sampler_interp_lin,
  aubio_sampler_n_interp_methods
} aubio_sampler_interp_method;

struct _aubio_sampler_t {
  uint_t samplerate;
  uint_t blocksize;
  // current reading mode (can be a file or an array)
  uint_t reading_from;
  // current interpolation mode (can be quadratic, timestretch, ...)
  uint_t interp;
  aubio_ringbuffer_t *ring;
  uint_t perfectloop;
  uint_t eof_remaining;
  // reading from a table
  fvec_t *table;
  uint_t table_index;
  // reading from a source
  aubio_source_t *source;
  const char_t *uri;
  uint_t playing;
  uint_t opened;
  uint_t loop;
  uint_t finished;              // end of file was reached
  uint_t eof;                   // end of file is now
#ifdef HAVE_THREADS
  // file reading thread
  pthread_t read_thread;
  uint_t threaded_read;         // use reading thread?
  pthread_mutex_t read_mutex;
  pthread_cond_t read_avail;
  pthread_cond_t read_request;
  uint_t source_blocksize;
  fvec_t *source_output;
  fmat_t *source_moutput;
  uint_t channels;
  // file opening thread
  pthread_t open_thread;
  pthread_mutex_t open_mutex;
  uint_t waited;                // number of frames skipped while opening
  const char_t *next_uri;
  uint_t open_thread_running;
  sint_t available;             // number of samples currently available
  uint_t started;               // source warmed up
  uint_t read_thread_finish;    // flag to tell reading thread to exit
#endif
};

static sint_t aubio_sampler_pull_from_source(aubio_sampler_t *s);

static void aubio_sampler_do_eof(aubio_sampler_t *s);

static void aubio_sampler_read(aubio_sampler_t *s, fvec_t *output, uint_t *read);
static void aubio_sampler_read_from_source(aubio_sampler_t *s, fvec_t *output, uint_t *read);
static void aubio_sampler_read_from_table(aubio_sampler_t *s, fvec_t *output, uint_t *read);

#ifdef HAVE_THREADS
static void *aubio_sampler_openfn(void *p);
static void *aubio_sampler_readfn(void *p);
static void aubio_sampler_open_opening_thread(aubio_sampler_t *o);
static void aubio_sampler_open_reading_thread(aubio_sampler_t *o);
static void aubio_sampler_close_opening_thread(aubio_sampler_t *o);
static void aubio_sampler_close_reading_thread(aubio_sampler_t *o);
#endif

aubio_sampler_t *new_aubio_sampler(uint_t blocksize, uint_t samplerate)
{
  aubio_sampler_t *s = AUBIO_NEW(aubio_sampler_t);
  if ((sint_t)blocksize < 1) {
    AUBIO_ERR("sampler: got blocksize %d, but can not be < 1\n", blocksize);
    goto beach;
  }
  s->samplerate = samplerate;
  s->blocksize = blocksize;
  s->source = NULL;
  s->playing = 0;
  s->loop = 0;
  s->uri = NULL;
  s->finished = 1;
  s->eof = 0;
  s->opened = 0;
  s->available = 0;

  s->threaded_read = 0;
  s->perfectloop = 0;
#if 0 // naive mode
  s->source_blocksize = s->blocksize;
#elif 0 // threaded mode, no ringbuffer
  s->source_blocksize = s->blocksize;
  s->threaded_read = 1;
#elif 0 // unthreaded, with ringbuffer
  s->source_blocksize = 2048; //32 * s->blocksize;
  s->perfectloop = 1;
#elif 1 // threaded with ringhbuffer
  s->source_blocksize = 2048; //32 * s->blocksize;
  s->perfectloop = 1;
  s->threaded_read = 1;
#endif

  if (s->source_blocksize < s->blocksize) {
    s->source_blocksize = s->blocksize;
  }
  // FIXME: perfectloop fails if source_blocksize > 2048 with source_avcodec
  //s->source_blocksize = 8192;

  if (s->perfectloop || s->source_blocksize != s->blocksize) {
    s->ring = new_aubio_ringbuffer(s->source_blocksize * 2, s->blocksize);
  }
  if (s->threaded_read || s->perfectloop || s->ring)
    s->source_output = new_fvec(s->source_blocksize);
  //s->channels = 1;
  //s->source_moutput = new_fmat(s->source_blocksize, s->channels);

#ifdef HAVE_THREADS
  aubio_sampler_open_opening_thread(s);

  if (s->threaded_read) {
    //AUBIO_WRN("sampler: starting reading thread\n");
    aubio_sampler_open_reading_thread(s);
  }
#endif

#if 0
  s->reading_from = aubio_sampler_reading_from_table;
  s->perfectloop = 1;
  s->threaded_read = 0;
  s->opened = 1;
  s->finished = 1;
  s->table_index = 0;
#endif

  return s;
beach:
  AUBIO_FREE(s);
  return NULL;
}

#ifdef HAVE_THREADS
void aubio_sampler_open_opening_thread(aubio_sampler_t *s) {
  pthread_mutex_init(&s->open_mutex, 0);
  s->waited = 0;
  s->open_thread = 0;
  s->open_thread_running = 0;
}

void aubio_sampler_open_reading_thread(aubio_sampler_t *s) {
  s->read_thread_finish = 0;
  pthread_mutex_init(&s->read_mutex, 0);
  pthread_cond_init (&s->read_avail, 0);
  pthread_cond_init (&s->read_request, 0);
  pthread_create(&s->read_thread, 0, aubio_sampler_readfn, s);
}

void aubio_sampler_close_opening_thread(aubio_sampler_t *o) {
  // clean up opening thread
  void *threadret;
  if (!o->open_thread) return;
  pthread_mutex_destroy(&o->open_mutex);
  if (o->open_thread_running) {
    if (pthread_cancel(o->open_thread)) {
      AUBIO_WRN("sampler: cancelling file opening thread failed\n");
    }
  }
  if (o->open_thread && pthread_join(o->open_thread, &threadret)) {
    AUBIO_WRN("sampler: joining file opening thread failed\n");
  }
  pthread_mutex_destroy(&o->open_mutex);
  o->open_thread = 0;
}

void aubio_sampler_close_reading_thread(aubio_sampler_t *o) {
  // clean up reading thread
  void *threadret;
  if (!o->read_thread) return;
  o->read_thread_finish = 1;
  pthread_cond_signal(&o->read_request);
  if (pthread_cancel(o->read_thread)) {
    AUBIO_WRN("sampler: cancelling file reading thread failed\n");
  }
  if (pthread_join(o->read_thread, &threadret)) {
    AUBIO_WRN("sampler: joining file reading thread failed\n");
  }
  pthread_mutex_destroy(&o->read_mutex);
  pthread_cond_destroy(&o->read_avail);
  pthread_cond_destroy(&o->read_request);
  o->read_thread = 0;
}
#endif

uint_t aubio_sampler_load( aubio_sampler_t * o, const char_t * uri )
{
  uint_t ret = AUBIO_FAIL;
  aubio_source_t *oldsource = o->source, *newsource = NULL;
  newsource = new_aubio_source(uri, o->samplerate, o->source_blocksize);
  if (newsource) {
    uint_t duration = aubio_source_get_duration(newsource);
    if (duration < o->blocksize) {
      AUBIO_WRN("sampler: %s is %d frames long, but blocksize is %d\n",
          uri, duration, o->blocksize);
    }
    o->source = newsource;
    if (oldsource) del_aubio_source(oldsource);
    if (o->samplerate == 0) {
      o->samplerate = aubio_source_get_samplerate(o->source);
    }
    o->uri = uri;
    o->finished = 0;
    o->eof = 0;
    o->eof_remaining = 0;
    o->opened = 1;
    ret = AUBIO_OK;
    AUBIO_MSG("sampler: loaded %s\n", uri);
    if (o->waited) {
      AUBIO_WRN("sampler: loading took %.2fms (%d samples)\n", 1000. *
          o->waited / (smpl_t)o->samplerate, o->waited);
    }
  } else {
    o->source = NULL;
    if (oldsource) del_aubio_source(oldsource);
    o->playing = 0;
    o->uri = NULL;
    o->finished = 1;
    o->eof = 0;
    o->eof_remaining = 0;
    o->opened = 0;
    AUBIO_WRN("sampler: failed loading %s\n", uri);
  }
  if (o->ring) {
    //AUBIO_WRN("sampler: resetting ringbuffer\n");
    aubio_ringbuffer_reset(o->ring);
  }
  return ret;
}

#ifdef HAVE_THREADS
static void *aubio_sampler_openfn(void *z) {
  aubio_sampler_t *p = z;
  uint_t err;
  int oldtype;
  void *ret;
  pthread_setcancelstate(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);
  pthread_mutex_lock(&p->open_mutex);
  p->open_thread_running = 1;
  err = aubio_sampler_load(p, p->next_uri);
  p->open_thread_running = 0;
  pthread_mutex_unlock(&p->open_mutex);
  ret = &err;
  pthread_exit(ret);
}
#endif

uint_t
aubio_sampler_queue(aubio_sampler_t *o, const char_t *uri)
{
#ifdef HAVE_THREADS
  uint_t ret = AUBIO_OK;

  if (o->reading_from == aubio_sampler_reading_from_table) {
    o->reading_from = aubio_sampler_reading_from_source;
    o->opened = 0;
    o->finished = 1;
  }
  /* open uri in open_thread */
  if (o->open_thread_running) {
    // cancel previous open_thread
    if (pthread_cancel(o->open_thread)) {
      AUBIO_WRN("sampler: failed queuing %s (cancelling existing open thread failed)\n", uri);
      return AUBIO_FAIL;
    } else {
      AUBIO_WRN("sampler: cancelled queuing %s (queuing %s now)\n",
          o->next_uri, uri);
    }
    o->open_thread_running = 0;
  }
  void *threadret;
  if (o->open_thread && pthread_join(o->open_thread, &threadret)) {
    AUBIO_WRN("sampler: joining thread failed\n");
  }
  if (pthread_mutex_trylock(&o->open_mutex)) {
    AUBIO_WRN("sampler: failed queuing %s (locking failed)\n", uri);
    ret = AUBIO_FAIL;
    goto lock_failed;
  }
  o->opened = 0; // while opening
  o->started = 0;
  o->available = 0;
  o->next_uri = uri;
  o->waited = 0;
  if (pthread_create(&o->open_thread, 0, aubio_sampler_openfn, o) != 0) {
    AUBIO_ERR("sampler: failed creating opening thread\n");
    ret = AUBIO_FAIL;
    goto thread_create_failed;
  }

thread_create_failed:
  pthread_mutex_unlock(&o->open_mutex);
lock_failed:
  if (ret == AUBIO_OK) {
    //AUBIO_WRN("sampler: queued %s\n", uri);
  } else {
    AUBIO_ERR("sampler: queueing %s failed\n", uri);
  }
  return ret;
#else
  AUBIO_WRN("sampler: opening %s, not queueing (not compiled with threading)\n", uri);
  return aubio_sampler_load(o, uri);
#endif
}

#ifdef HAVE_THREADS

uint_t aubio_sampler_reading_from_source_ring_fetch(aubio_sampler_t*s);

void *aubio_sampler_readfn(void *z) {
  aubio_sampler_t *p = z;
  while(1) {
    pthread_mutex_lock(&p->read_mutex);
    if (p->open_thread_running) {
      //AUBIO_WRN("sampler: readfn(): file is being opened\n");
      pthread_cond_signal(&p->read_avail);
      //pthread_cond_wait(&p->read_request, &p->read_mutex);
    } else if (p->opened && !p->started && !p->finished) {
      //AUBIO_WRN("sampler: readfn(): file started\n");
      if (p->ring) {
        p->available = aubio_sampler_reading_from_source_ring_fetch(p);
      } else {
        p->available = aubio_sampler_pull_from_source(p);
        if (p->available < (sint_t)p->source_blocksize)
          aubio_sampler_do_eof(p);
      }
      pthread_cond_signal(&p->read_avail);
      if (!p->finished) {
        pthread_cond_wait(&p->read_request, &p->read_mutex);
      }
    } else {
      //AUBIO_WRN("sampler: readfn(): idle?\n");
      //pthread_cond_signal(&p->read_avail);
      pthread_cond_wait(&p->read_request, &p->read_mutex);
      if (p->read_thread_finish) {
        goto done;
      }
    }
    pthread_mutex_unlock(&p->read_mutex);
  }
done:
  //AUBIO_WRN("sampler: exiting reading thread\n");
  pthread_mutex_unlock(&p->read_mutex);
  pthread_exit(NULL);
}
#endif

void
aubio_sampler_read(aubio_sampler_t *s, fvec_t *output, uint_t *read) {
  if (s->reading_from == aubio_sampler_reading_from_source) {
    aubio_sampler_read_from_source(s, output, read);
  } else if (s->reading_from == aubio_sampler_reading_from_table) {
    aubio_sampler_read_from_table(s, output, read);
  }
}

static void
aubio_sampler_reading_from_source_naive(aubio_sampler_t *s, fvec_t * output,
    uint_t *read)
{
  // directly read from disk
  //aubio_source_do(s->source, output, read);
  s->source_output = output;
  *read = aubio_sampler_pull_from_source(s);
  if (*read < s->source_blocksize) {
    //AUBIO_WRN("sampler: calling go_eof in _read_from_source()\n");
    aubio_sampler_do_eof(s);
  }
}

uint_t
aubio_sampler_reading_from_source_ring_fetch(aubio_sampler_t*s) {
  // read source_blocksize (> blocksize) at once
  int ring_avail = aubio_ringbuffer_get_available(s->ring);
  //if (ring_avail < s->blocksize) {
  uint_t available = 0;
  if (ring_avail < (sint_t)s->blocksize) {
    available = aubio_sampler_pull_from_source(s);
    if (available > 0) {
      aubio_ringbuffer_push(s->ring, s->source_output, available);
    }
    if (available < s->source_blocksize) {
      if (ring_avail + available <= s->blocksize) {
        s->eof_remaining = available + ring_avail;
        if (s->eof_remaining == 0) s->eof_remaining = s->blocksize;
        //AUBIO_ERR("sampler: marking special eof got: %d, in ring: %d, %d, eof remaining %d\n",
        //    available, ring_avail, s->blocksize, s->eof_remaining);
        if (s->loop) {
          aubio_sampler_seek(s,0);
          // read some frames from beginning of source for perfect looping
          if (s->perfectloop) {
            available = aubio_sampler_pull_from_source(s);
            if (available <= 0) {
              AUBIO_ERR("sampler: perfectloop but s->available = 0 !\n");
            } else {
              aubio_ringbuffer_push(s->ring, s->source_output, available);
            }
          }
        }
      }
    }
  }
  return available;
}

static void
aubio_sampler_reading_from_source_ring_pull(aubio_sampler_t *s, fvec_t *output,
    uint_t *read)
{
  // write into output
  int ring_avail = aubio_ringbuffer_get_available(s->ring);
  if (ring_avail >= (sint_t)s->blocksize) {
    //AUBIO_MSG("sampler: pulling %d / %d from ringbuffer\n", s->blocksize, ring_avail);
    aubio_ringbuffer_pull(s->ring, output, s->blocksize);
    *read = s->blocksize;
    if (s->eof_remaining > 0) {
      if (s->eof_remaining <= s->blocksize) {
        s->eof = 1;
        s->eof_remaining = 0;
      } else if (s->eof_remaining <= s->source_blocksize) {
        s->eof_remaining -= s->blocksize;
      }
    }
  } else {
    //AUBIO_MSG("sampler: last frame, pulling remaining %d left\n", ring_avail);
    *read = 0;
    if (ring_avail > 0) {
      // pull remaining frames in ring buffer
      aubio_ringbuffer_pull(s->ring, output, ring_avail);
      *read += ring_avail;
    }
    // signal eof
    aubio_sampler_do_eof(s);
    // finished playing, reset ringbuffer for next read
    if (!s->playing)
      aubio_ringbuffer_reset(s->ring);
  }
}

static void
aubio_sampler_reading_from_source_ring(aubio_sampler_t *s, fvec_t *output,
    uint_t *read)
{
  aubio_sampler_reading_from_source_ring_fetch(s);
  aubio_sampler_reading_from_source_ring_pull(s, output, read);
}

#ifdef HAVE_THREADS
static void
aubio_sampler_read_from_source_threaded(aubio_sampler_t *s, fvec_t *output,
    uint_t *read) {
  // request at least output->length
  // make sure we have enough samples read from source
  int available;
  pthread_mutex_lock(&s->read_mutex);
  if (!s->opened || s->open_thread_running) {
    //AUBIO_ERR("sampler: _read_from_source: not opened, signaling read_request\n");
    pthread_cond_signal(&s->read_request);
    available = 0;
  } else if (!s->finished) {
    pthread_cond_signal(&s->read_request);
    pthread_cond_wait(&s->read_avail, &s->read_mutex);
    //AUBIO_ERR("sampler: _read_from_source: %d\n", s->available);
    available = s->available;
  } else {
    //AUBIO_WRN("sampler: _read_from_source: eof\n");
    pthread_cond_signal(&s->read_request);
    available = 0;
  }
  pthread_mutex_unlock(&s->read_mutex);
  //AUBIO_WRN("sampler: got %d available in _read_from_source\n", available);
  // read -> number of samples read
  if (!s->perfectloop && s->source_blocksize == s->blocksize) {
    if (available >= (sint_t)s->blocksize) {
      fvec_copy(s->source_output, output);
      *read = s->blocksize;
    } else if (available > 0) {
      fvec_copy(s->source_output, output);
      *read = available;
    } else {
      fvec_zeros(output);
      *read = 0;
    }
  } else {
    aubio_sampler_reading_from_source_ring_pull(s, output, read);
  }
}
#endif

void
aubio_sampler_read_from_source(aubio_sampler_t *s, fvec_t *output, uint_t *read) {
#ifdef HAVE_THREADS
  if (s->threaded_read) { // if threaded
    aubio_sampler_read_from_source_threaded(s, output, read);
  } else
#endif
  {
    if (s->finished) {
      *read = 0;
    }
    else if (s->source_blocksize == s->blocksize && !s->perfectloop) {
      aubio_sampler_reading_from_source_naive(s, output, read);
    } else {
      aubio_sampler_reading_from_source_ring(s, output, read);
    }
#if 1
    if (s->loop && s->perfectloop && *read != s->blocksize) { // && s->started && !s->finished) {
      AUBIO_ERR("sampler: perfectloop but read only %d\n", *read);
    }
#endif
  }
}

void
aubio_sampler_read_from_table(aubio_sampler_t *s, fvec_t *output, uint_t *read) {
  *read = 0;
  if (s->table == NULL) {
    AUBIO_WRN("sampler: _pull_from_table but table not set %d, %d\n",
        output->length, *read);
  } else if (s->playing) {
    uint_t available = s->table->length - s->table_index;
    fvec_t tmp;
    tmp.data = s->table->data + s->table_index;
    if (available < s->blocksize) {
      //AUBIO_WRN("sampler: _pull_from_table: table length %d, index: %d, read %d\n",
      //    s->table->length, s->table_index, *read);
      tmp.length = available;
      fvec_t tmpout; tmpout.data = output->data; tmpout.length = available;
      fvec_copy(&tmp, &tmpout);
      if (s->loop && s->perfectloop) {
        uint_t remaining = s->blocksize - available;
        tmpout.data = output->data + available; tmpout.length = remaining;
        tmp.data = s->table->data; tmp.length = remaining;
        fvec_copy(&tmp, &tmpout);
        s->table_index = remaining;
        *read = s->blocksize;
      } else {
        s->table_index = 0;
        *read = available;
      }
      aubio_sampler_do_eof(s);
    } else {
      tmp.length = s->blocksize;
      fvec_copy(&tmp, output);
      s->table_index += output->length;
      *read = s->blocksize;
    }
  }
}

uint_t
aubio_sampler_set_table(aubio_sampler_t *s, fvec_t *samples) {
  if (!samples || !s) return AUBIO_FAIL;
  if (s->reading_from == aubio_sampler_reading_from_source) {
    //aubio_sampler_close_reading_thread(s);
  }
  s->table = samples;
  //AUBIO_INF("sampler: setting table (%d long)\n", s->table->length);
  s->table_index = 0;
  s->reading_from = aubio_sampler_reading_from_table;
  //s->threaded_read = 0;
  s->opened = 1;
  s->finished = 1;
  return AUBIO_OK;
}

sint_t
aubio_sampler_pull_from_source(aubio_sampler_t *s)
{
  // pull source_blocksize samples from source, return available frames
  uint_t source_read = s->source_blocksize;
  if (s->source == NULL) {
    AUBIO_ERR("sampler: trying to fetch on NULL source\n");
    return -1;
  }
  aubio_source_do(s->source, s->source_output, &source_read);
  return source_read;
}


uint_t
aubio_sampler_get_samplerate (aubio_sampler_t *o)
{
  return o->samplerate;
}

uint_t
aubio_sampler_get_opened (aubio_sampler_t *o)
{
  return o->opened; //== 1 ? AUBIO_OK : AUBIO_FAIL;
}

uint_t
aubio_sampler_get_finished(aubio_sampler_t *o)
{
  return o->finished;
}

uint_t
aubio_sampler_get_eof (aubio_sampler_t *o)
{
  return o->eof;
}

uint_t
aubio_sampler_get_waited_opening (aubio_sampler_t *o, uint_t waited) {
#ifdef HAVE_THREADS
  if (o->playing) {
    if (!o->opened) {
      o->waited += waited;
    } else if (o->waited) {
      //AUBIO_WRN("sampler: waited %d frames (%.2fms) while opening %s\n",
      //    o->waited, 1000.*o->waited/(smpl_t)o->samplerate, o->uri);
      uint_t waited = o->waited;
      o->waited = 0;
      return waited;
    }
  }
#endif
  return 0;
}

uint_t
aubio_sampler_seek(aubio_sampler_t * o, uint_t pos)
{
  //AUBIO_WRN("sampler: seeking to 0\n");
  uint_t ret = AUBIO_FAIL;
  o->finished = 0;
  if (!o->opened) return AUBIO_OK;
  if (o->source) {
    ret = aubio_source_seek(o->source, pos);
  } else {
    o->table_index = pos;
  }
  return ret;
}

void
aubio_sampler_do_eof (aubio_sampler_t * o)
{
  //AUBIO_MSG("sampler: calling  _do_eof()\n");
  o->finished = 1;
  o->eof = 1;
  if (!o->loop) {
    o->playing = 0;
  } else {
    if (o->reading_from == aubio_sampler_reading_from_source)
      aubio_sampler_seek(o, 0);
    //o->finished = 0;
  }
}

void aubio_sampler_do ( aubio_sampler_t * o, fvec_t * output, uint_t *read)
{
  o->eof = 0;
  if (o->opened == 1 && o->playing) {
    aubio_sampler_read(o, output, read);
    //AUBIO_WRN("sampler: _read got %d\n", *read);
    if (*read < o->blocksize) {
      if (*read > 0 && o->loop) {
        // TODO pull (hopsize - read) frames
      }
    }
  } else {
    fvec_zeros(output);
    *read = 0;
  }
}

void aubio_sampler_do_multi ( aubio_sampler_t * o, fmat_t * output, uint_t *read)
{
  o->eof = 0;
  if (o->opened == 1 && o->playing) {
    //aubio_sampler_read_multi(o, output, read);
    if (*read < o->blocksize) {
      if (*read > 0) {
        // TODO pull (hopsize - read) frames
        //memset(...  tail , 0)
      }
    }
  } else {
    fmat_zeros(output);
    *read = 0;
  }
}

uint_t aubio_sampler_get_playing ( const aubio_sampler_t * o )
{
  return o->playing;
}

uint_t aubio_sampler_set_playing ( aubio_sampler_t * o, uint_t playing )
{
  o->playing = (playing == 1) ? 1 : 0;
  return 0;
}

uint_t aubio_sampler_get_loop ( aubio_sampler_t * o )
{
  return o->loop;
}

uint_t aubio_sampler_set_loop ( aubio_sampler_t * o, uint_t loop )
{
  o->loop = (loop == 1) ? 1 : 0;
  return 0;
}

uint_t aubio_sampler_play ( aubio_sampler_t * o )
{
  //aubio_source_seek (o->source, 0);
  return aubio_sampler_set_playing (o, 1);
}

uint_t aubio_sampler_stop ( aubio_sampler_t * o )
{
  return aubio_sampler_set_playing (o, 0);
}

uint_t aubio_sampler_loop ( aubio_sampler_t * o )
{
  aubio_sampler_set_loop(o, 1);
  aubio_sampler_seek(o, 0);
  return aubio_sampler_set_playing (o, 1);
}

uint_t aubio_sampler_trigger ( aubio_sampler_t * o )
{
  aubio_sampler_set_loop(o, 0);
  aubio_sampler_seek(o, 0);
  return aubio_sampler_set_playing (o, 1);
}

uint_t aubio_sampler_set_perfectloop (aubio_sampler_t *s, uint_t perfectloop) {
  if (!s) return AUBIO_FAIL;
  s->perfectloop = perfectloop;
  return AUBIO_OK;
}

uint_t aubio_sampler_get_perfectloop (aubio_sampler_t *s) {
  if (!s) return AUBIO_FAIL;
  return s->perfectloop;
}

void del_aubio_sampler( aubio_sampler_t * o )
{
#ifdef HAVE_THREADS
  // close opening thread
  aubio_sampler_close_opening_thread(o);
  // close reading thread
  aubio_sampler_close_reading_thread(o);
#endif
  //if (o->source_output) {
  if (o->source_output && (o->threaded_read || o->perfectloop)) {
    del_fvec(o->source_output);
  }
  if (o->source_moutput) {
    del_fmat(o->source_moutput);
  }
  if (o->ring) {
    del_aubio_ringbuffer(o->ring);
  }
  if (o->source) {
    del_aubio_source(o->source);
  }
  AUBIO_FREE(o);
}
