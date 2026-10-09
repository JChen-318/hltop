#include "nvtop/hl_smi_parser.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
  char query[] =
      "0, HL-225, 0000:02:00.0, 34, 0, 98304, 97536, 768, 51, 1600, 1800, 4, 4, 16, 16, 0\n"
      "1, HL-225, 0000:03:00.0, N/A, 107, 103079215104, 100663296000, 2415919104, 53, N/A, 1800, 4, 4, 16, 8, 2\n";
  struct hl_smi_device_sample devices[2];
  memset(devices, 0, sizeof(devices));
  size_t count = hl_smi_parse_query(query, devices, 2);
  assert(count == 2);
  assert(devices[0].index == 0);
  assert(strcmp(devices[0].name, "HL-225") == 0);
  assert(strcmp(devices[0].bus_id, "0000:02:00.0") == 0);
  assert(devices[0].memory_total_bytes == UINT64_C(98304) * 1024 * 1024);
  assert(devices[0].power_draw_mw == 51000);
  assert(devices[1].utilization_percent == 100);
  assert(devices[1].memory_total_bytes == UINT64_C(98304) * 1024 * 1024);
  assert(!devices[1].has_temperature);
  assert(!devices[1].has_soc_clock);
  assert(devices[1].pcie_width_current == 8);
  assert(devices[1].ecc_uncorrected == 2);

  char table[] =
      "+-----------------------------------------------------------------------------+\n"
      "|   0  HL-225              N/A  | 0000:02:00.0     N/A |                   0  |\n"
      "| N/A   34C   P0   58W /  600W  |   768MiB /  98304MiB |     0%            0% |\n"
      "|-------------------------------+----------------------+----------------------|\n"
      "|   1  HL-225              N/A  | 0000:03:00.0     N/A |                   0  |\n"
      "| N/A   35C   P0   61W /  600W  |  2304MiB /  98304MiB |    10%            0% |\n"
      "| Compute Processes:                                               AIP Memory |\n"
      "|  AIP       PID   Type   Process name                             Usage      |\n"
      "|   0      12345   C      python train.py                           512MiB     |\n"
      "|   1      23456   C      /usr/bin/python3                         1GiB       |\n"
      "|   1        N/A   N/A    N/A                                      N/A        |\n";
  struct hl_smi_process_sample processes[4];
  memset(processes, 0, sizeof(processes));
  count = hl_smi_parse_table(table, devices, 2, processes, 4);
  assert(count == 2);
  assert(devices[0].has_power_draw && devices[0].power_draw_mw == 58000);
  assert(devices[1].has_power_draw && devices[1].power_draw_mw == 61000);
  assert(devices[0].has_power_limit && devices[0].power_limit_mw == 600000);
  assert(devices[1].has_power_limit && devices[1].power_limit_mw == 600000);
  assert(processes[0].device_index == 0 && processes[0].pid == 12345);
  assert(processes[0].has_memory && processes[0].memory_bytes == UINT64_C(512) * 1024 * 1024);
  assert(processes[1].device_index == 1 && processes[1].pid == 23456);
  assert(processes[1].has_memory && processes[1].memory_bytes == UINT64_C(1024) * 1024 * 1024);
  return 0;
}
