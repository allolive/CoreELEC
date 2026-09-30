/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  libdovi's public types, as the converter uses them. libdovi is a built
 *  dependency, not carried in the source, and it is the reason the RPU
 *  generation was considered untestable. It need not be: the converter's whole
 *  derivation is handed to dovi_generate_from_json as text, so a test that
 *  defines these six functions can read every value the derivation produced.
 *
 *  This declares the interface only. The test binary supplies the bodies.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct DoviRpuOpaque DoviRpuOpaque;

typedef struct DoviRpuOpaqueList
{
  const DoviRpuOpaque* const* list;
  size_t len;
  const char* error;
} DoviRpuOpaqueList;

typedef struct DoviData
{
  const uint8_t* data;
  size_t len;
} DoviData;

const DoviRpuOpaqueList* dovi_generate_from_json(const char* json);
const DoviData* dovi_write_unspec62_nalu(const DoviRpuOpaque* rpu);
void dovi_data_free(const DoviData* data);
const char* dovi_rpu_get_error(const DoviRpuOpaque* rpu);
void dovi_rpu_list_free(const DoviRpuOpaqueList* list);

// Fields used by the native bitstream converter. These declarations are a
// controlled link seam, not an ABI for loading the real libdovi library.
typedef struct DoviRpuDataHeader
{
  uint8_t guessed_profile;
  const char* el_type;
} DoviRpuDataHeader;

DoviRpuOpaque* dovi_parse_unspec62_nalu(const uint8_t* data, size_t len);
const DoviRpuDataHeader* dovi_rpu_get_header(const DoviRpuOpaque* rpu);
int dovi_convert_rpu_with_mode(DoviRpuOpaque* rpu, uint8_t mode);
int dovi_rpu_set_active_area_offsets(DoviRpuOpaque* rpu, uint16_t left,
                                   uint16_t right, uint16_t top, uint16_t bottom);
void dovi_rpu_free_header(const DoviRpuDataHeader* header);
void dovi_rpu_free(DoviRpuOpaque* rpu);
