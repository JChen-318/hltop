/*
 * HL-SMI text/CSV parser used by the Intel Gaudi backend.
 *
 * This file is part of hltop, based on nvtop. It is distributed under the
 * terms of the GNU General Public License version 3 or later.
 */

#ifndef HL_SMI_PARSER_H_
#define HL_SMI_PARSER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HL_SMI_NAME_LEN 128
#define HL_SMI_BUS_ID_LEN 16
#define HL_SMI_PROCESS_TYPE_LEN 8

struct hl_smi_device_sample {
  unsigned index;
  char name[HL_SMI_NAME_LEN];
  char bus_id[HL_SMI_BUS_ID_LEN];

  unsigned temperature_c;
  unsigned utilization_percent;
  uint64_t memory_total_bytes;
  uint64_t memory_free_bytes;
  uint64_t memory_used_bytes;
  unsigned power_draw_mw;
  unsigned power_limit_mw;
  unsigned soc_clock_mhz;
  unsigned soc_clock_max_mhz;
  unsigned pcie_gen_max;
  unsigned pcie_gen_current;
  unsigned pcie_width_max;
  unsigned pcie_width_current;
  uint64_t ecc_uncorrected;

  bool has_temperature;
  bool has_utilization;
  bool has_memory_total;
  bool has_memory_free;
  bool has_memory_used;
  bool has_power_draw;
  bool has_power_limit;
  bool has_soc_clock;
  bool has_soc_clock_max;
  bool has_pcie_gen_max;
  bool has_pcie_gen_current;
  bool has_pcie_width_max;
  bool has_pcie_width_current;
  bool has_ecc_uncorrected;
};

struct hl_smi_process_sample {
  unsigned device_index;
  int pid;
  char type[HL_SMI_PROCESS_TYPE_LEN];
  uint64_t memory_bytes;
  bool has_memory;
};

/* The input buffers are modified in-place. */
size_t hl_smi_parse_query(char *text, struct hl_smi_device_sample *samples, size_t capacity);
size_t hl_smi_parse_table(char *text, struct hl_smi_device_sample *devices, size_t device_count,
                          struct hl_smi_process_sample *processes, size_t process_capacity);

#endif
