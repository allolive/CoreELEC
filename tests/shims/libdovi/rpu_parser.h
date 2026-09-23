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
