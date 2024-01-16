/* Copyright 2019 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CRAS_SRC_PLC_CRAS_PLC_H_
#define CRAS_SRC_PLC_CRAS_PLC_H_

#include <stdint.h>
#include <sbc/sbc.h>

#include "cras_audio_codec.h"

#define MSBC_SAMPLE_SIZE 2  // 2 bytes
#define MSBC_PKT_LEN 57     // Packet length without the header
#define MSBC_FS 120         // Frame Size
#define MSBC_CODE_SIZE 240  // MSBC_SAMPLE_SIZE * MSBC_FS

#ifdef __cplusplus
extern "C" {
#endif

/* PLC library provides helper functions to mask the effects of lost or
 * disrupted packets. It currentyl only supports the mSBC codec.
 *
 * This struct contains informations needed for applying the PLC algorithm.
 */
struct cras_msbc_plc;

/* Creates a plc component for mSBC codec, which is used for wideband speech
 * mode of HFP
 */
struct cras_msbc_plc* cras_msbc_plc_create(void);

/* Destroys a mSBC PLC.
 * Args:
 *    plc - The PLC to destroy.
 */
void cras_msbc_plc_destroy(struct cras_msbc_plc* plc);

/* Conceals the packet loss by writing the substitution samples to the ouput
 * buffer provided by the caller. The samples will be generated based on the
 * informations recorded in the PLC struct passed in.
 * Args:
 *    plc - The PLC you use.
 *    codec - The mSBC codec.
 *    output - Pointer to the output buffer.
 * Returns:
 *    The number of bytes written to the output buffer.
 */
int cras_msbc_plc_handle_bad_frames(struct cras_msbc_plc* state, sbc_t *msbc,
                                    uint8_t* output);
/* Updates informations needed and potentially processes the input samples to
 * help it to reconverge after a frame loss.
 *
 * The memory space input and output pointers point to can be overlapping.
 * Args:
 *   plc - The PLC you use.
 *   input - Pointer to the true input.
 *   output - Pointer to the output buffer.
 * Returns:
 *    The number of bytes written to the output buffer.
 */
int cras_msbc_plc_handle_good_frames(struct cras_msbc_plc* plc,
                                     const uint8_t* input,
                                     uint8_t* output);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
