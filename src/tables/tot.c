/*****************************************************************************
 * tot.c: TDT/TOT decoder/generator
 *----------------------------------------------------------------------------
 * Copyright (C) 2001-2011 VideoLAN
 * $Id$
 *
 * Authors: Johann Hanne
 *          heavily based on pmt.c which was written by
 *          Arnaud de Bossoreille de Ribou <bozo@via.ecp.fr>
 *          Jean-Paul Saman <jpsaman@videolan.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *----------------------------------------------------------------------------
 *
 *****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#include <stdint.h>
#endif

#include <assert.h>

#include "../dvbpsi.h"
#include "../dvbpsi_private.h"
#include "../psi.h"
#include "../descriptor.h"
#include "../demux.h"
#include "tot.h"
#include "tot_private.h"

/*****************************************************************************
 * dvbpsi_AttachTOT
 *****************************************************************************
 * Initialize a TDT/TOT subtable decoder.
 *****************************************************************************/
bool dvbpsi_AttachTOT(dvbpsi_t* p_dvbpsi, uint8_t i_table_id, uint16_t i_extension,
                     dvbpsi_tot_callback pf_callback, void* p_cb_data)
{
    assert(p_dvbpsi);
    assert(p_dvbpsi->p_private);

    dvbpsi_demux_t* p_demux = (dvbpsi_demux_t*)p_dvbpsi->p_private;
    dvbpsi_demux_subdec_t* p_subdec;
    dvbpsi_tot_decoder_t*  p_tot_decoder;

    i_extension = 0; /* NOTE: force to 0 when handling TDT/TOT */
    if (dvbpsi_demuxGetSubDec(p_demux, i_table_id, i_extension))
    {
        dvbpsi_error(p_dvbpsi, "TDT/TOT decoder",
                     "Already a decoder for (table_id == 0x%02x,"
                     "extension == 0x%02x)",
                     i_table_id, 0);
        return false;
    }

    p_subdec = (dvbpsi_demux_subdec_t*)calloc(1, sizeof(dvbpsi_demux_subdec_t));
    if(p_subdec == NULL)
        return false;

    p_tot_decoder = (dvbpsi_tot_decoder_t*)calloc(1, sizeof(dvbpsi_tot_decoder_t));
    if (p_tot_decoder == NULL)
    {
        free(p_subdec);
        return false;
    }

    /* subtable decoder configuration */
    p_subdec->pf_callback = &dvbpsi_GatherTOTSections;
    p_subdec->p_cb_data = p_tot_decoder;
    p_subdec->i_id = (uint32_t)i_table_id << 16 | (uint32_t)0;
    p_subdec->pf_detach = dvbpsi_DetachTOT;

    /* Attach the subtable decoder to the demux */
    p_subdec->p_next = p_demux->p_first_subdec;
    p_demux->p_first_subdec = p_subdec;

    /* TDT/TOT decoder information */
    p_tot_decoder->pf_tot_callback = pf_callback;
    p_tot_decoder->p_cb_data = p_cb_data;

    return true;
}

/*****************************************************************************
 * dvbpsi_DetachTOT
 *****************************************************************************
 * Close a TDT/TOT decoder.
 *****************************************************************************/
void dvbpsi_DetachTOT(dvbpsi_t* p_dvbpsi, uint8_t i_table_id,
                      uint16_t i_extension)
{
    assert(p_dvbpsi);
    assert(p_dvbpsi->p_private);

    dvbpsi_demux_t *p_demux = (dvbpsi_demux_t *)p_dvbpsi->p_private;
    dvbpsi_demux_subdec_t* p_subdec;
    dvbpsi_demux_subdec_t** pp_prev_subdec;

    i_extension = 0; /* NOTE: force to 0 when handling TDT/TOT */
    p_subdec = dvbpsi_demuxGetSubDec(p_demux, i_table_id, i_extension);
    if (p_demux == NULL)
    {
        dvbpsi_error(p_dvbpsi, "TDT/TOT Decoder",
                     "No such TDT/TOT decoder (table_id == 0x%02x,"
                     "extension == 0x%02x)",
                     i_table_id, 0);
        return;
    }

    free(p_subdec->p_cb_data);

    pp_prev_subdec = &p_demux->p_first_subdec;
    while(*pp_prev_subdec != p_subdec)
        pp_prev_subdec = &(*pp_prev_subdec)->p_next;

    *pp_prev_subdec = p_subdec->p_next;
    free(p_subdec);
    p_subdec = NULL;
}

/*****************************************************************************
 * dvbpsi_InitTOT
 *****************************************************************************
 * Initialize a pre-allocated dvbpsi_tot_t structure.
 *****************************************************************************/
void dvbpsi_InitTOT(dvbpsi_tot_t* p_tot, uint64_t i_utc_time)
{
    p_tot->i_utc_time = i_utc_time;
    p_tot->p_first_descriptor = NULL;
}

/*****************************************************************************
 * dvbpsi_EmptyTOT
 *****************************************************************************
 * Clean a dvbpsi_tot_t structure.
 *****************************************************************************/
void dvbpsi_EmptyTOT(dvbpsi_tot_t* p_tot)
{
    dvbpsi_DeleteDescriptors(p_tot->p_first_descriptor);
    p_tot->p_first_descriptor = NULL;
}

/*****************************************************************************
 * dvbpsi_TOTAddDescriptor
 *****************************************************************************
 * Add a descriptor in the TOT.
 *****************************************************************************/
dvbpsi_descriptor_t* dvbpsi_TOTAddDescriptor(dvbpsi_tot_t* p_tot,
                                             uint8_t i_tag, uint8_t i_length,
                                             uint8_t* p_data)
{
    dvbpsi_descriptor_t* p_descriptor
                        = dvbpsi_NewDescriptor(i_tag, i_length, p_data);
    if (p_descriptor == NULL)
        return NULL;

    if (p_tot->p_first_descriptor == NULL)
        p_tot->p_first_descriptor = p_descriptor;
    else
    {
        dvbpsi_descriptor_t* p_last_descriptor = p_tot->p_first_descriptor;
        while(p_last_descriptor->p_next != NULL)
            p_last_descriptor = p_last_descriptor->p_next;
        p_last_descriptor->p_next = p_descriptor;
    }
    return p_descriptor;
}

/*****************************************************************************
 * dvbpsi_GatherTOTSections
 *****************************************************************************
 * Callback for the PSI decoder.
 *****************************************************************************/
void dvbpsi_GatherTOTSections(dvbpsi_t* p_dvbpsi,
                              void * p_private_decoder,
                              dvbpsi_psi_section_t* p_section)
{
    assert(p_dvbpsi);
    assert(p_dvbpsi->p_private);

    dvbpsi_tot_decoder_t* p_tot_decoder
                        = (dvbpsi_tot_decoder_t*)p_private_decoder;

    dvbpsi_debug(p_dvbpsi, "TDT/TOT decoder", "got a section");

    if (p_section->i_table_id != 0x70 && p_section->i_table_id != 0x73)
    {
        /* Invalid table_id value */
        dvbpsi_error(p_dvbpsi, "TDT/TOT decoder",
                     "invalid section (table_id == 0x%02x)",
                     p_section->i_table_id);
        dvbpsi_DeletePSISections(p_section);
        return;
    }

    if (p_section->b_syntax_indicator != false)
    {
        /* Invalid section_syntax_indicator */
        dvbpsi_error(p_dvbpsi, "TDT/TOT decoder",
                    "invalid section (section_syntax_indicator != 0)");
        dvbpsi_DeletePSISections(p_section);
        return;
    }

    /* TS discontinuity check */
    if (p_tot_decoder->b_discontinuity)
    {
        /* We don't care about discontinuities with the TDT/TOT as it
           only consists of one section anyway */
        p_tot_decoder->b_discontinuity = false;
    }

    dvbpsi_tot_t* p_building_tot;
    p_building_tot = (dvbpsi_tot_t*)malloc(sizeof(dvbpsi_tot_t));
    if (p_building_tot)
        dvbpsi_InitTOT(p_building_tot,   ((uint64_t)p_section->p_payload_start[0] << 32)
                                       | ((uint64_t)p_section->p_payload_start[1] << 24)
                                       | ((uint64_t)p_section->p_payload_start[2] << 16)
                                       | ((uint64_t)p_section->p_payload_start[3] <<  8)
                                       |  (uint64_t)p_section->p_payload_start[4]);
    else
        dvbpsi_error(p_dvbpsi, "TOT decoder", "failed decoding section");

    /* Decode the section */
    dvbpsi_DecodeTOTSections(p_dvbpsi, p_building_tot, p_section);
    /* Delete the section */
    dvbpsi_DeletePSISections(p_section);
    /* signal the new TDT/TOT */
    p_tot_decoder->pf_tot_callback(p_tot_decoder->p_cb_data, p_building_tot);
}

/*****************************************************************************
 * dvbpsi_ValidTOTSection
 *****************************************************************************
 * Check the CRC_32 if the section has b_syntax_indicator set.
 *****************************************************************************/
static bool dvbpsi_ValidTOTSection(dvbpsi_t *p_dvbpsi, dvbpsi_psi_section_t* p_section)
{
    if (p_section->i_table_id != 0x73)
    {
        /* A TDT always has a length of 5 bytes (which is only the UTC time) */
        if (p_section->i_length != 5)
        {
            dvbpsi_error(p_dvbpsi, "TDT/TOT decoder",
                         "TDT has an invalid payload size (%d bytes) !!!",
                          p_section->i_length);
            return false;
        }
        return true;
    }

    /* Check the CRC_32 if it's a TOT */
    uint32_t i_crc = 0xffffffff;
    uint8_t* p_byte = p_section->p_data;

    while (p_byte < p_section->p_payload_end)
    {
        i_crc = (i_crc << 8) ^ dvbpsi_crc32_table[(i_crc >> 24) ^ (*p_byte)];
        p_byte++;
    }

    if (i_crc == 0)
        return true;
    else
    {
        dvbpsi_error(p_dvbpsi, "TDT/TOT decoder",
                               "Bad CRC_32 (0x%08x) !!!", i_crc);
        return false;
    }

    return true;
}

/*****************************************************************************
 * dvbpsi_DecodeTOTSections
 *****************************************************************************
 * TDT/TOT decoder.
 *****************************************************************************/
void dvbpsi_DecodeTOTSections(dvbpsi_t* p_dvbpsi, dvbpsi_tot_t* p_tot,
                              dvbpsi_psi_section_t* p_section)
{
    if (p_section)
    {
        uint8_t* p_byte;

        if (!dvbpsi_ValidTOTSection(p_dvbpsi, p_section))
            return;

        p_byte = p_section->p_payload_start;
        if (p_byte + 5 <= p_section->p_payload_end)
        {
            p_tot->i_utc_time = ((uint64_t)p_byte[0] << 32) |
                                ((uint64_t)p_byte[1] << 24) |
                                ((uint64_t)p_byte[2] << 16) |
                                ((uint64_t)p_byte[3] << 8) |
                                 (uint64_t)p_byte[4];
        }

        /* If we have a TOT, extract the descriptors */
        if (p_section->i_table_id == 0x73)
        {
            uint8_t* p_end;

            p_end = p_byte + (   ((uint16_t)(p_section->p_payload_start[5] & 0x0f) << 8)
                                 | p_section->p_payload_start[6]);
            p_byte += 7;

            while (p_byte + 5 <= p_end)
            {
                uint8_t i_tag = p_byte[0];
                uint8_t i_length = p_byte[1];
                if (i_length + 2 <= p_section->p_payload_end - p_byte)
                    dvbpsi_TOTAddDescriptor(p_tot, i_tag, i_length, p_byte + 2);
                p_byte += 2 + i_length;
            }
        }
    }
}

/*****************************************************************************
 * dvbpsi_GenTOTSections
 *****************************************************************************
 * Generate TDT/TOT sections based on the dvbpsi_tot_t structure.
 *****************************************************************************/
dvbpsi_psi_section_t* dvbpsi_GenTOTSections(dvbpsi_t *p_dvbpsi, dvbpsi_tot_t* p_tot)
{
    dvbpsi_psi_section_t* p_result;
    dvbpsi_descriptor_t* p_descriptor = p_tot->p_first_descriptor;

    /* If it has descriptors, it must be a TOT, otherwise a TDT */
    p_result = dvbpsi_NewPSISection((p_descriptor != NULL) ? 4096 : 8);

    p_result->i_table_id = (p_descriptor != NULL) ? 0x73 : 0x70;
    p_result->b_syntax_indicator = false;
    p_result->b_private_indicator = false;
    p_result->i_length = 5;
    p_result->p_payload_start = p_result->p_data + 3;
    p_result->p_payload_end = p_result->p_data + 8;

    p_result->p_data[3] = (p_tot->i_utc_time >> 32) & 0xff;
    p_result->p_data[4] = (p_tot->i_utc_time >> 24) & 0xff;
    p_result->p_data[5] = (p_tot->i_utc_time >> 16) & 0xff;
    p_result->p_data[6] = (p_tot->i_utc_time >>  8) & 0xff;
    p_result->p_data[7] =  p_tot->i_utc_time        & 0xff;

    if (p_result->i_table_id == 0x73)
    {
        /* Special handling for TOT only (A TDT doesn't have descriptors!) */
        /* Reserve two bytes for descriptors_loop_length */
        p_result->p_payload_end += 2;
        p_result->i_length += 2;

        /* TOT descriptors */
        while (p_descriptor != NULL)
        {
            /* A TOT cannot have multiple sections! */
            if(   (p_result->p_payload_end - p_result->p_data)
                                        + p_descriptor->i_length > 4090)
            {
                dvbpsi_error(p_dvbpsi, "TDT/TOT generator",
                             "TOT does not fit into one section as it ought to be !!!");
                break;
            }

            /* p_payload_end is where the descriptor begins */
            p_result->p_payload_end[0] = p_descriptor->i_tag;
            p_result->p_payload_end[1] = p_descriptor->i_length;
            memcpy(p_result->p_payload_end + 2,
                   p_descriptor->p_data,
                    p_descriptor->i_length);

            /* Increase length by descriptor_length + 2 */
            p_result->p_payload_end += p_descriptor->i_length + 2;
            p_result->i_length += p_descriptor->i_length + 2;

            p_descriptor = p_descriptor->p_next;
        }

        /* descriptors_loop_length */
        p_result->p_payload_start[5] = ((p_result->i_length - 7) << 8) | 0xf0;
        p_result->p_payload_start[6] =  (p_result->i_length - 7)       & 0xff;
    }

    if (p_result->i_table_id == 0x73)
    {
        /* A TOT has a CRC_32 although it's a private section,
           but the CRC_32 is part of the payload! */
        p_result->p_payload_end += 4;
        p_result->i_length += 4;
    }

    dvbpsi_BuildPSISection(p_dvbpsi, p_result);

    if (p_result->i_table_id == 0x73)
    {
        uint8_t* p_byte = p_result->p_data;
        p_tot->i_crc = 0xffffffff;

        while (p_byte < p_result->p_payload_end - 4)
        {
            p_tot->i_crc =   (p_tot->i_crc << 8)
                           ^ dvbpsi_crc32_table[(p_tot->i_crc >> 24) ^ (*p_byte)];
            p_byte++;
        }

        p_byte[0] = (p_tot->i_crc >> 24) & 0xff;
        p_byte[1] = (p_tot->i_crc >> 16) & 0xff;
        p_byte[2] = (p_tot->i_crc >> 8) & 0xff;
        p_byte[3] = p_tot->i_crc & 0xff;
    }

    if (!dvbpsi_ValidTOTSection(p_dvbpsi, p_result))
    {
        dvbpsi_error(p_dvbpsi, "TDT/TOT generator", "********************************************");
        dvbpsi_error(p_dvbpsi, "TDT/TOT generator", "*  Generated TDT/TOT section is invalid.   *");
        dvbpsi_error(p_dvbpsi, "TDT/TOT generator", "* THIS IS A BUG, PLEASE REPORT TO THE LIST *");
        dvbpsi_error(p_dvbpsi, "TDT/TOT generator", "*  ---  libdvbpsi-devel@videolan.org  ---  *");
        dvbpsi_error(p_dvbpsi, "TDT/TOT generator", "********************************************");
    }

    return p_result;
}
