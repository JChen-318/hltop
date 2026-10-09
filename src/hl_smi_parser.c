/*
 * Parser for Intel Gaudi HL-SMI 1.24 query and summary output.
 *
 * This file is part of hltop, based on nvtop. It is distributed under the
 * terms of the GNU General Public License version 3 or later.
 */

#include "nvtop/hl_smi_parser.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define QUERY_FIELD_COUNT 16
#define MIB (UINT64_C(1024) * UINT64_C(1024))

static char *trim(char *value) {
  while (isspace((unsigned char)*value))
    ++value;
  char *end = value + strlen(value);
  while (end > value && isspace((unsigned char)end[-1]))
    --end;
  *end = '\0';
  return value;
}

static bool parse_u64(const char *value, uint64_t *result) {
  if (!value || !*value || strcmp(value, "N/A") == 0)
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (errno || end == value)
    return false;
  while (isspace((unsigned char)*end))
    ++end;
  if (*end != '\0')
    return false;
  *result = (uint64_t)parsed;
  return true;
}

static bool parse_unsigned(const char *value, unsigned *result) {
  uint64_t parsed;
  if (!parse_u64(value, &parsed) || parsed > UINT32_MAX)
    return false;
  *result = (unsigned)parsed;
  return true;
}

static size_t split_csv(char *line, char **fields, size_t capacity) {
  size_t count = 0;
  char *start = line;
  for (char *cursor = line;; ++cursor) {
    if (*cursor == ',' || *cursor == '\0') {
      if (count < capacity) {
        char delimiter = *cursor;
        *cursor = '\0';
        fields[count++] = trim(start);
        if (delimiter == '\0')
          break;
        start = cursor + 1;
      } else {
        break;
      }
    }
  }
  return count;
}

size_t hl_smi_parse_query(char *text, struct hl_smi_device_sample *samples, size_t capacity) {
  if (!text || !samples || capacity == 0)
    return 0;

  size_t sample_count = 0;
  char *save_line = NULL;
  for (char *line = strtok_r(text, "\r\n", &save_line); line && sample_count < capacity;
       line = strtok_r(NULL, "\r\n", &save_line)) {
    char *fields[QUERY_FIELD_COUNT];
    if (split_csv(line, fields, QUERY_FIELD_COUNT) != QUERY_FIELD_COUNT)
      continue;

    struct hl_smi_device_sample sample;
    memset(&sample, 0, sizeof(sample));
    if (!parse_unsigned(fields[0], &sample.index))
      continue;

    strncpy(sample.name, fields[1], sizeof(sample.name) - 1);
    strncpy(sample.bus_id, fields[2], sizeof(sample.bus_id) - 1);

    sample.has_temperature = parse_unsigned(fields[3], &sample.temperature_c);
    sample.has_utilization = parse_unsigned(fields[4], &sample.utilization_percent);

    /* HL-SMI releases disagree on nounits memory scaling: current 1.24 emits
     * MiB while some older examples emit bytes. Device totals below 1 MiB as
     * a raw number cannot be byte counts for a Gaudi board, so use the total
     * to select the scaling consistently for total/free/used. */
    uint64_t raw_total = 0;
    uint64_t raw_free = 0;
    uint64_t raw_used = 0;
    sample.has_memory_total = parse_u64(fields[5], &raw_total);
    sample.has_memory_free = parse_u64(fields[6], &raw_free);
    sample.has_memory_used = parse_u64(fields[7], &raw_used);
    bool memory_values_are_mib = sample.has_memory_total && raw_total < MIB;
    uint64_t memory_scale = memory_values_are_mib ? MIB : UINT64_C(1);
    if (sample.has_memory_total)
      sample.memory_total_bytes = raw_total * memory_scale;
    if (sample.has_memory_free)
      sample.memory_free_bytes = raw_free * memory_scale;
    if (sample.has_memory_used)
      sample.memory_used_bytes = raw_used * memory_scale;

    unsigned watts;
    sample.has_power_draw = parse_unsigned(fields[8], &watts);
    if (sample.has_power_draw)
      sample.power_draw_mw = watts * 1000U;

    sample.has_soc_clock = parse_unsigned(fields[9], &sample.soc_clock_mhz);
    sample.has_soc_clock_max = parse_unsigned(fields[10], &sample.soc_clock_max_mhz);
    sample.has_pcie_gen_max = parse_unsigned(fields[11], &sample.pcie_gen_max);
    sample.has_pcie_gen_current = parse_unsigned(fields[12], &sample.pcie_gen_current);
    sample.has_pcie_width_max = parse_unsigned(fields[13], &sample.pcie_width_max);
    sample.has_pcie_width_current = parse_unsigned(fields[14], &sample.pcie_width_current);
    sample.has_ecc_uncorrected = parse_u64(fields[15], &sample.ecc_uncorrected);

    if (sample.has_utilization && sample.utilization_percent > 100)
      sample.utilization_percent = 100;

    samples[sample_count++] = sample;
  }
  return sample_count;
}

static struct hl_smi_device_sample *find_device(struct hl_smi_device_sample *devices, size_t count, unsigned index) {
  for (size_t i = 0; i < count; ++i) {
    if (devices[i].index == index)
      return &devices[i];
  }
  return NULL;
}

static bool first_table_uint(char *line, unsigned *value, char **after) {
  char *cursor = strchr(line, '|');
  if (!cursor)
    return false;
  cursor++;
  while (isspace((unsigned char)*cursor))
    ++cursor;
  if (!isdigit((unsigned char)*cursor))
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(cursor, &end, 10);
  if (errno || end == cursor || parsed > UINT32_MAX)
    return false;
  *value = (unsigned)parsed;
  if (after)
    *after = end;
  return true;
}

static void parse_power(char *line, struct hl_smi_device_sample *device) {
  char *separator = strstr(line, "W /");
  if (!separator || !device)
    return;

  char *draw_end = separator;
  char *draw_start = draw_end;
  while (draw_start > line && isspace((unsigned char)draw_start[-1]))
    --draw_start;
  while (draw_start > line && isdigit((unsigned char)draw_start[-1]))
    --draw_start;
  if (draw_start < draw_end) {
    errno = 0;
    char *parsed_end = NULL;
    unsigned long draw_watts = strtoul(draw_start, &parsed_end, 10);
    if (!errno && parsed_end == draw_end && draw_watts <= UINT32_MAX / 1000U) {
      device->power_draw_mw = (unsigned)draw_watts * 1000U;
      device->has_power_draw = true;
    }
  }

  char *cursor = separator + 3;
  while (isspace((unsigned char)*cursor))
    ++cursor;
  errno = 0;
  char *end = NULL;
  unsigned long watts = strtoul(cursor, &end, 10);
  if (!errno && end != cursor && watts <= UINT32_MAX / 1000U && *end == 'W') {
    device->power_limit_mw = (unsigned)watts * 1000U;
    device->has_power_limit = true;
  }
}

static bool parse_process_line(char *line, struct hl_smi_process_sample *process) {
  unsigned device_index;
  char *cursor;
  if (!first_table_uint(line, &device_index, &cursor))
    return false;

  while (isspace((unsigned char)*cursor))
    ++cursor;
  if (!isdigit((unsigned char)*cursor))
    return false;
  errno = 0;
  char *end = NULL;
  long pid = strtol(cursor, &end, 10);
  if (errno || end == cursor || pid <= 0 || pid > INT32_MAX)
    return false;

  cursor = end;
  while (isspace((unsigned char)*cursor))
    ++cursor;
  char *type_end = cursor;
  while (*type_end && !isspace((unsigned char)*type_end))
    ++type_end;
  if (type_end == cursor)
    return false;

  memset(process, 0, sizeof(*process));
  process->device_index = device_index;
  process->pid = (int)pid;
  size_t type_len = (size_t)(type_end - cursor);
  if (type_len >= sizeof(process->type))
    type_len = sizeof(process->type) - 1;
  memcpy(process->type, cursor, type_len);

  char *right_pipe = strrchr(type_end, '|');
  if (right_pipe)
    *right_pipe = '\0';
  char *tail = type_end + strlen(type_end);
  while (tail > type_end && isspace((unsigned char)tail[-1]))
    --tail;
  char *token = tail;
  while (token > type_end && !isspace((unsigned char)token[-1]))
    --token;
  if (token < tail && strcmp(token, "N/A") != 0) {
    errno = 0;
    char *memory_end = NULL;
    unsigned long long memory = strtoull(token, &memory_end, 10);
    if (!errno && memory_end != token) {
      if (strncmp(memory_end, "MiB", 3) == 0)
        memory *= MIB;
      else if (strncmp(memory_end, "GiB", 3) == 0)
        memory *= MIB * UINT64_C(1024);
      else if (strncmp(memory_end, "KiB", 3) == 0)
        memory *= UINT64_C(1024);
      process->memory_bytes = (uint64_t)memory;
      process->has_memory = true;
    }
  }
  return true;
}

size_t hl_smi_parse_table(char *text, struct hl_smi_device_sample *devices, size_t device_count,
                          struct hl_smi_process_sample *processes, size_t process_capacity) {
  if (!text)
    return 0;

  bool in_process_table = false;
  unsigned current_device = UINT32_MAX;
  size_t process_count = 0;
  char *save_line = NULL;
  for (char *line = strtok_r(text, "\r\n", &save_line); line; line = strtok_r(NULL, "\r\n", &save_line)) {
    if (strstr(line, "Compute Processes:")) {
      in_process_table = true;
      continue;
    }

    if (!in_process_table) {
      unsigned possible_index;
      if (first_table_uint(line, &possible_index, NULL) && strchr(line, ':')) {
        current_device = possible_index;
      } else if (current_device != UINT32_MAX && strstr(line, "W /")) {
        parse_power(line, find_device(devices, device_count, current_device));
        current_device = UINT32_MAX;
      }
      continue;
    }

    if (process_count < process_capacity && processes && parse_process_line(line, &processes[process_count]))
      ++process_count;
  }
  return process_count;
}
