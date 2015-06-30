/*****************************************************************************
* This file is part of Kvazaar HEVC encoder.
*
* Copyright (C) 2013-2015 Tampere University of Technology and others (see
* COPYING file).
*
* Kvazaar is free software: you can redistribute it and/or modify it under
* the terms of the GNU Lesser General Public License as published by the
* Free Software Foundation; either version 2.1 of the License, or (at your
* option) any later version.
*
* Kvazaar is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
* FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with Kvazaar.  If not, see <http://www.gnu.org/licenses/>.
****************************************************************************/

#include "kvazaar.h"

#include <stdlib.h>

#include "config.h"
#include "encoder.h"
#include "strategyselector.h"
#include "encoderstate.h"
#include "checkpoint.h"
#include "bitstream.h"


static void kvazaar_close(kvz_encoder *encoder)
{
  if (encoder) {
    if (encoder->states) {
      for (unsigned i = 0; i < encoder->num_encoder_states; ++i) {
        encoder_state_finalize(&encoder->states[i]);
      }
    }
    FREE_POINTER(encoder->states);

    encoder_control_free(encoder->control);
    encoder->control = NULL;
  }
  FREE_POINTER(encoder);
}


static kvz_encoder * kvazaar_open(const kvz_config *cfg)
{
  kvz_encoder *encoder = NULL;

  //Initialize strategies
  // TODO: Make strategies non-global
  if (!strategyselector_init(cfg->cpuid)) {
    fprintf(stderr, "Failed to initialize strategies.\n");
    goto kvazaar_open_failure;
  }

  init_exp_golomb();

  encoder = calloc(1, sizeof(kvz_encoder));
  if (!encoder) {
    goto kvazaar_open_failure;
  }

  encoder->control = encoder_control_init(cfg);
  if (!encoder->control) {
    goto kvazaar_open_failure;
  }

  encoder->num_encoder_states = encoder->control->owf + 1;
  encoder->cur_state_num = 0;
  encoder->frames_started = 0;
  encoder->frames_done = 0;
  encoder->states = calloc(encoder->num_encoder_states, sizeof(encoder_state_t));
  if (!encoder->states) {
    goto kvazaar_open_failure;
  }

  for (unsigned i = 0; i < encoder->num_encoder_states; ++i) {
    encoder->states[i].encoder_control = encoder->control;

    if (!encoder_state_init(&encoder->states[i], NULL)) {
      goto kvazaar_open_failure;
    }

    encoder->states[i].global->QP = (int8_t)cfg->qp;
  }

  for (int i = 0; i < encoder->num_encoder_states; ++i) {
    if (i == 0) {
      encoder->states[i].previous_encoder_state = &encoder->states[encoder->num_encoder_states - 1];
    } else {
      encoder->states[i].previous_encoder_state = &encoder->states[(i - 1) % encoder->num_encoder_states];
    }
    encoder_state_match_children_of_previous_frame(&encoder->states[i]);
  }

  encoder->states[encoder->cur_state_num].global->frame = -1;

  return encoder;

kvazaar_open_failure:
  kvazaar_close(encoder);
  return NULL;
}


static int kvazaar_encode(kvz_encoder *enc,
                          kvz_picture *pic_in,
                          kvz_picture **pic_out,
                          kvz_data_chunk **data_out)
{
  if (pic_out) *pic_out = NULL;
  if (data_out) *data_out = NULL;

  encoder_state_t *state = &enc->states[enc->cur_state_num];

  if (!state->prepared) {
    encoder_next_frame(state);
  }

  if (pic_in != NULL) {
    // FIXME: The frame number printed here is wrong when GOP is enabled.
    CHECKPOINT_MARK("read source frame: %d", state->global->frame + enc->control->cfg->seek);
  }

  if (encoder_feed_frame(state, pic_in)) {
    assert(state->global->frame == enc->frames_started);
    // Start encoding.
    encode_one_frame(state);
    enc->frames_started += 1;
  }

  // If we have finished encoding as many frames as we have started, we are done.
  if (enc->frames_done == enc->frames_started) {
    return 1;
  }

  // Move to the next encoder state.
  enc->cur_state_num = (enc->cur_state_num + 1) % (enc->num_encoder_states);
  state = &enc->states[enc->cur_state_num];

  if (!state->frame_done) {
    threadqueue_waitfor(enc->control->threadqueue, state->tqj_bitstream_written);

    if (pic_out) *pic_out = image_copy_ref(state->tile->frame->rec);
    if (data_out) *data_out = bitstream_take_chunks(&state->stream);

    state->frame_done = 1;
    state->prepared = 0;
    enc->frames_done += 1;
  }

  return 1;
}

kvz_api kvz_8bit_api = {
  .config_alloc = config_alloc,
  .config_init = config_init,
  .config_destroy = config_destroy,
  .config_parse = config_parse,

  .picture_alloc = image_alloc,
  .picture_free = image_free,

  .chunk_free = bitstream_free_chunks,

  .encoder_open = kvazaar_open,
  .encoder_close = kvazaar_close,
  .encoder_encode = kvazaar_encode,
};


const kvz_api * kvz_api_get(int bit_depth)
{
  return &kvz_8bit_api;
}
