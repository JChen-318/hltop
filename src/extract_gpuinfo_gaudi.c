/*
 * Intel Gaudi backend for hltop. Telemetry is read from the HL-SMI command
 * shipped with the Gaudi software stack, so no build-time Gaudi SDK is needed.
 *
 * This file is part of hltop, based on nvtop. It is distributed under the
 * terms of the GNU General Public License version 3 or later.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "list.h"
#include "nvtop/common.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/hl_smi_parser.h"

#define GAUDI_MAX_DEVICES 64
#define GAUDI_MAX_PROCESSES 4096
#define GAUDI_COMMAND_OUTPUT_LIMIT (4U * 1024U * 1024U)
#define GAUDI_REFRESH_GUARD_NS 100000000L

static const char gaudi_query_command[] =
    "hl-smi -Q "
    "index,name,bus_id,temperature.aip,utilization.aip,memory.total,memory.free,memory.used,power.draw,"
    "clocks.current.soc,clocks.max.soc,pcie.link.gen.max,pcie.link.gen.current,pcie.link.width.max,"
    "pcie.link.width.current,ecc.errors.uncorrected.volatile.total "
    "-f csv,noheader,nounits 2>/dev/null";

struct gpu_info_gaudi {
  struct gpu_info base;
  unsigned index;
};

static struct gpu_info_gaudi *gaudi_devices;
static unsigned gaudi_device_count;
static struct hl_smi_device_sample initial_samples[GAUDI_MAX_DEVICES];
static size_t initial_sample_count;
static struct timespec last_dynamic_refresh;
static struct timespec last_process_refresh;
static char gaudi_error[256] = "HL-SMI has not been initialized";

static bool gpuinfo_gaudi_init(void);
static void gpuinfo_gaudi_shutdown(void);
static const char *gpuinfo_gaudi_last_error_string(void);
static bool gpuinfo_gaudi_get_device_handles(struct list_head *devices, unsigned *count);
static void gpuinfo_gaudi_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_gaudi_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_gaudi_refresh_running_processes(struct gpu_info *_gpu_info);

static struct gpu_vendor gpu_vendor_gaudi = {
    .init = gpuinfo_gaudi_init,
    .shutdown = gpuinfo_gaudi_shutdown,
    .last_error_string = gpuinfo_gaudi_last_error_string,
    .get_device_handles = gpuinfo_gaudi_get_device_handles,
    .populate_static_info = gpuinfo_gaudi_populate_static_info,
    .refresh_dynamic_info = gpuinfo_gaudi_refresh_dynamic_info,
    .refresh_running_processes = gpuinfo_gaudi_refresh_running_processes,
    .name = "Intel Gaudi",
    .processing_unit = gpu_processing_unit_aip,
};

__attribute__((constructor)) static void init_extract_gpuinfo_gaudi(void) { register_gpu_vendor(&gpu_vendor_gaudi); }

static bool elapsed_guard(const struct timespec *last) {
  if (last->tv_sec == 0 && last->tv_nsec == 0)
    return true;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return true;
  time_t seconds = now.tv_sec - last->tv_sec;
  long nanoseconds = now.tv_nsec - last->tv_nsec;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  return seconds > 0 || (seconds == 0 && nanoseconds >= GAUDI_REFRESH_GUARD_NS);
}

static void mark_refreshed(struct timespec *last) {
  if (clock_gettime(CLOCK_MONOTONIC, last) != 0)
    memset(last, 0, sizeof(*last));
}

static char *run_command(const char *command) {
  FILE *pipe = popen(command, "r");
  if (!pipe) {
    snprintf(gaudi_error, sizeof(gaudi_error), "Cannot run hl-smi: %s", strerror(errno));
    return NULL;
  }

  size_t used = 0;
  size_t capacity = 4096;
  char *output = malloc(capacity);
  if (!output) {
    pclose(pipe);
    snprintf(gaudi_error, sizeof(gaudi_error), "Cannot allocate memory for hl-smi output");
    return NULL;
  }

  while (!feof(pipe)) {
    if (used + 2048 + 1 > capacity) {
      size_t next_capacity = capacity * 2;
      if (next_capacity > GAUDI_COMMAND_OUTPUT_LIMIT) {
        free(output);
        pclose(pipe);
        snprintf(gaudi_error, sizeof(gaudi_error), "hl-smi output exceeded %u bytes", GAUDI_COMMAND_OUTPUT_LIMIT);
        return NULL;
      }
      char *grown = realloc(output, next_capacity);
      if (!grown) {
        free(output);
        pclose(pipe);
        snprintf(gaudi_error, sizeof(gaudi_error), "Cannot allocate memory for hl-smi output");
        return NULL;
      }
      output = grown;
      capacity = next_capacity;
    }
    size_t received = fread(output + used, 1, capacity - used - 1, pipe);
    used += received;
    if (ferror(pipe)) {
      free(output);
      pclose(pipe);
      snprintf(gaudi_error, sizeof(gaudi_error), "Failed while reading hl-smi output");
      return NULL;
    }
  }
  output[used] = '\0';

  int status = pclose(pipe);
  if (status != 0) {
    free(output);
    snprintf(gaudi_error, sizeof(gaudi_error), "hl-smi exited with status %d", status);
    return NULL;
  }
  return output;
}

static size_t query_samples(struct hl_smi_device_sample *samples, size_t capacity) {
  char *output = run_command(gaudi_query_command);
  if (!output)
    return 0;
  size_t count = hl_smi_parse_query(output, samples, capacity);
  free(output);
  if (count == 0)
    snprintf(gaudi_error, sizeof(gaudi_error), "hl-smi returned no parseable Gaudi devices");
  return count;
}

static bool gpuinfo_gaudi_init(void) {
  initial_sample_count = query_samples(initial_samples, GAUDI_MAX_DEVICES);
  return initial_sample_count > 0;
}

static void gpuinfo_gaudi_shutdown(void) {
  free(gaudi_devices);
  gaudi_devices = NULL;
  gaudi_device_count = 0;
  initial_sample_count = 0;
  memset(&last_dynamic_refresh, 0, sizeof(last_dynamic_refresh));
  memset(&last_process_refresh, 0, sizeof(last_process_refresh));
}

static const char *gpuinfo_gaudi_last_error_string(void) { return gaudi_error; }

static bool gpuinfo_gaudi_get_device_handles(struct list_head *devices, unsigned *count) {
  if (initial_sample_count == 0)
    return false;

  gaudi_devices = calloc(initial_sample_count, sizeof(*gaudi_devices));
  if (!gaudi_devices) {
    snprintf(gaudi_error, sizeof(gaudi_error), "Cannot allocate Gaudi device information: %s", strerror(errno));
    return false;
  }

  gaudi_device_count = (unsigned)initial_sample_count;
  for (unsigned i = 0; i < gaudi_device_count; ++i) {
    gaudi_devices[i].base.vendor = &gpu_vendor_gaudi;
    gaudi_devices[i].index = initial_samples[i].index;
    strncpy(gaudi_devices[i].base.pdev, initial_samples[i].bus_id, sizeof(gaudi_devices[i].base.pdev) - 1);
    list_add_tail(&gaudi_devices[i].base.list, devices);
  }
  *count = gaudi_device_count;
  return true;
}

static struct gpu_info_gaudi *find_gaudi_device(unsigned index) {
  for (unsigned i = 0; i < gaudi_device_count; ++i) {
    if (gaudi_devices[i].index == index)
      return &gaudi_devices[i];
  }
  return NULL;
}

static const struct hl_smi_device_sample *find_initial_sample(unsigned index) {
  for (size_t i = 0; i < initial_sample_count; ++i) {
    if (initial_samples[i].index == index)
      return &initial_samples[i];
  }
  return NULL;
}

static bool gaudi_read_pcie_file(const char *pdev, const char *name, unsigned *value) {
  if (!pdev || !strchr(pdev, ':') || !name || !value)
    return false;
  char path[128];
  if (snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", pdev, name) >= (int)sizeof(path))
    return false;
  FILE *file = fopen(path, "r");
  if (!file)
    return false;
  bool valid = fscanf(file, "%u", value) == 1;
  fclose(file);
  return valid;
}

static void gpuinfo_gaudi_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_gaudi *gaudi = container_of(_gpu_info, struct gpu_info_gaudi, base);
  const struct hl_smi_device_sample *sample = find_initial_sample(gaudi->index);
  if (!sample)
    return;

  struct gpuinfo_static_info *info = &_gpu_info->static_info;
  if (sample->name[0]) {
    strncpy(info->device_name, sample->name, sizeof(info->device_name) - 1);
    SET_VALID(gpuinfo_device_name_valid, info->valid);
  }
  strncpy(info->device_architecture, "Gaudi2", sizeof(info->device_architecture) - 1);
  SET_VALID(gpuinfo_device_architecture_valid, info->valid);
  strncpy(info->memory_type, "HBM2E", sizeof(info->memory_type) - 1);
  SET_VALID(gpuinfo_memory_type_valid, info->valid);
  if (sample->has_pcie_gen_max)
    SET_GPUINFO_STATIC(info, max_pcie_gen, sample->pcie_gen_max);
  else {
    unsigned speed;
    if (gaudi_read_pcie_file(_gpu_info->pdev, "max_link_speed", &speed)) {
      unsigned generation = nvtop_pcie_gen_from_link_speed(speed);
      if (generation)
        SET_GPUINFO_STATIC(info, max_pcie_gen, generation);
    }
  }
  if (sample->has_pcie_width_max)
    SET_GPUINFO_STATIC(info, max_pcie_link_width, sample->pcie_width_max);
  else {
    unsigned width;
    if (gaudi_read_pcie_file(_gpu_info->pdev, "max_link_width", &width) && width)
      SET_GPUINFO_STATIC(info, max_pcie_link_width, width);
  }
}

static void apply_dynamic_sample(struct gpu_info_gaudi *gaudi, const struct hl_smi_device_sample *sample) {
  struct gpuinfo_dynamic_info *info = &gaudi->base.dynamic_info;
  RESET_ALL(info->valid);

  if (sample->has_utilization)
    SET_GPUINFO_DYNAMIC(info, gpu_util_rate, sample->utilization_percent);
  if (sample->has_temperature)
    SET_GPUINFO_DYNAMIC(info, gpu_temp, sample->temperature_c);
  if (sample->has_memory_total)
    SET_GPUINFO_DYNAMIC(info, total_memory, sample->memory_total_bytes);
  if (sample->has_memory_free)
    SET_GPUINFO_DYNAMIC(info, free_memory, sample->memory_free_bytes);
  if (sample->has_memory_used)
    SET_GPUINFO_DYNAMIC(info, used_memory, sample->memory_used_bytes);
  if (sample->has_memory_total && sample->has_memory_used && sample->memory_total_bytes > 0)
    SET_GPUINFO_DYNAMIC(info, mem_util_rate,
                        (unsigned)(sample->memory_used_bytes * UINT64_C(100) / sample->memory_total_bytes));
  if (sample->has_power_draw)
    SET_GPUINFO_DYNAMIC(info, power_draw, sample->power_draw_mw);
  if (sample->has_power_limit)
    SET_GPUINFO_DYNAMIC(info, power_draw_max, sample->power_limit_mw);
  if (sample->has_soc_clock)
    SET_GPUINFO_DYNAMIC(info, gpu_clock_speed, sample->soc_clock_mhz);
  if (sample->has_soc_clock_max)
    SET_GPUINFO_DYNAMIC(info, gpu_clock_speed_max, sample->soc_clock_max_mhz);
  if (sample->has_pcie_gen_current)
    SET_GPUINFO_DYNAMIC(info, pcie_link_gen, sample->pcie_gen_current);
  else {
    unsigned speed;
    if (gaudi_read_pcie_file(gaudi->base.pdev, "current_link_speed", &speed)) {
      unsigned generation = nvtop_pcie_gen_from_link_speed(speed);
      if (generation)
        SET_GPUINFO_DYNAMIC(info, pcie_link_gen, generation);
    }
  }
  if (sample->has_pcie_width_current)
    SET_GPUINFO_DYNAMIC(info, pcie_link_width, sample->pcie_width_current);
  else {
    unsigned width;
    if (gaudi_read_pcie_file(gaudi->base.pdev, "current_link_width", &width) && width)
      SET_GPUINFO_DYNAMIC(info, pcie_link_width, width);
  }
  if (sample->has_ecc_uncorrected)
    SET_GPUINFO_DYNAMIC(info, ecc_uncorrected, sample->ecc_uncorrected);
}

static void refresh_all_dynamic(void) {
  struct hl_smi_device_sample samples[GAUDI_MAX_DEVICES];
  size_t count = query_samples(samples, GAUDI_MAX_DEVICES);
  if (count == 0)
    return;
  for (size_t i = 0; i < count; ++i) {
    struct gpu_info_gaudi *gaudi = find_gaudi_device(samples[i].index);
    if (gaudi)
      apply_dynamic_sample(gaudi, &samples[i]);
  }
  mark_refreshed(&last_dynamic_refresh);
}

static void gpuinfo_gaudi_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  (void)_gpu_info;
  if (elapsed_guard(&last_dynamic_refresh))
    refresh_all_dynamic();
}

static void ensure_process_capacity(struct gpu_info *device, unsigned required) {
  if (required <= device->processes_array_size)
    return;
  unsigned capacity = required + COMMON_PROCESS_LINEAR_REALLOC_INC;
  struct gpu_process *grown = reallocarray(device->processes, capacity, sizeof(*grown));
  if (!grown)
    return;
  device->processes = grown;
  device->processes_array_size = capacity;
}

static void refresh_all_processes(void) {
  char *output = run_command("hl-smi 2>/dev/null");
  if (!output)
    return;

  struct hl_smi_device_sample devices[GAUDI_MAX_DEVICES];
  memset(devices, 0, sizeof(devices));
  for (unsigned i = 0; i < gaudi_device_count; ++i)
    devices[i].index = gaudi_devices[i].index;

  struct hl_smi_process_sample *processes = calloc(GAUDI_MAX_PROCESSES, sizeof(*processes));
  if (!processes) {
    free(output);
    return;
  }
  size_t process_count =
      hl_smi_parse_table(output, devices, gaudi_device_count, processes, GAUDI_MAX_PROCESSES);
  free(output);

  for (unsigned i = 0; i < gaudi_device_count; ++i) {
    gaudi_devices[i].base.processes_count = 0;
    if (devices[i].has_power_draw)
      SET_GPUINFO_DYNAMIC(&gaudi_devices[i].base.dynamic_info, power_draw, devices[i].power_draw_mw);
    if (devices[i].has_power_limit)
      SET_GPUINFO_DYNAMIC(&gaudi_devices[i].base.dynamic_info, power_draw_max, devices[i].power_limit_mw);
  }

  for (size_t i = 0; i < process_count; ++i) {
    struct gpu_info_gaudi *gaudi = find_gaudi_device(processes[i].device_index);
    if (!gaudi)
      continue;
    struct gpu_info *device = &gaudi->base;
    ensure_process_capacity(device, device->processes_count + 1);
    if (device->processes_count >= device->processes_array_size)
      continue;
    struct gpu_process *process = &device->processes[device->processes_count++];
    memset(process, 0, sizeof(*process));
    process->pid = (pid_t)processes[i].pid;
    process->type = gpu_process_compute;
    if (processes[i].has_memory)
      SET_GPUINFO_PROCESS(process, gpu_memory_usage, processes[i].memory_bytes);
  }

  free(processes);
  mark_refreshed(&last_process_refresh);
}

static void gpuinfo_gaudi_refresh_running_processes(struct gpu_info *_gpu_info) {
  (void)_gpu_info;
  if (elapsed_guard(&last_process_refresh))
    refresh_all_processes();
}
