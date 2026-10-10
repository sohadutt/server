#include "resource_monitor.h"

#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

#include "logger.h"

static long long timeval_to_microseconds(const struct timeval *value) {
  return (long long)value->tv_sec * 1000000LL + value->tv_usec;
}

int resource_monitor_capture(ResourceUsageSnapshot *snapshot) {
  if (snapshot == NULL)
    return -1;

  struct timespec wall_time;
  struct rusage usage;
  if (clock_gettime(CLOCK_MONOTONIC, &wall_time) != 0 ||
      getrusage(RUSAGE_SELF, &usage) != 0) {
    return -1;
  }

  snapshot->wall_time_ns =
      (long long)wall_time.tv_sec * 1000000000LL + wall_time.tv_nsec;
  snapshot->user_cpu_us = timeval_to_microseconds(&usage.ru_utime);
  snapshot->system_cpu_us = timeval_to_microseconds(&usage.ru_stime);
#ifdef __APPLE__
  snapshot->peak_rss_kb = usage.ru_maxrss / 1024;
#else
  snapshot->peak_rss_kb = usage.ru_maxrss;
#endif
  snapshot->minor_page_faults = usage.ru_minflt;
  snapshot->major_page_faults = usage.ru_majflt;
  snapshot->voluntary_context_switches = usage.ru_nvcsw;
  snapshot->involuntary_context_switches = usage.ru_nivcsw;
  return 0;
}

static long long nonnegative_delta(long long end, long long start) {
  return end >= start ? end - start : 0;
}

static void copy_request_line(char *destination, size_t capacity,
                              const char *request_line) {
  if (destination == NULL || capacity == 0)
    return;
  static const char unknown[] = "unknown request";
  size_t output = 0;
  if (request_line == NULL || *request_line == '\0') {
    snprintf(destination, capacity, "%s", unknown);
    return;
  }

  for (size_t input = 0;
       request_line[input] != '\0' && request_line[input] != '\r' &&
       request_line[input] != '\n' && output < capacity - 1;
       input++) {
    unsigned char character = (unsigned char)request_line[input];
    if (character == '"' || character == '\\') {
      if (output + 1 >= capacity)
        break;
      destination[output++] = '\\';
    }
    destination[output++] =
        character >= 0x20 && character < 0x7f ? (char)character : '?';
  }
  destination[output] = '\0';
}

void resource_monitor_log_request(const ResourceUsageSnapshot *start,
                                  const char *request_line,
                                  size_t bytes_received, size_t bytes_sent) {
  if (start == NULL)
    return;

  ResourceUsageSnapshot end;
  if (resource_monitor_capture(&end) != 0) {
    log_warn("Could not capture resource usage for request");
    return;
  }

  char request[160];
  copy_request_line(request, sizeof(request), request_line);

  long long wall_ns = nonnegative_delta(end.wall_time_ns, start->wall_time_ns);
  long long user_us = nonnegative_delta(end.user_cpu_us, start->user_cpu_us);
  long long system_us =
      nonnegative_delta(end.system_cpu_us, start->system_cpu_us);
  long long wall_ms = wall_ns / 1000000LL;
  long long wall_sub_ms = (wall_ns % 1000000LL) / 1000LL;
  long long user_ms = user_us / 1000LL;
  long long user_sub_ms = user_us % 1000LL;
  long long system_ms = system_us / 1000LL;
  long long system_sub_ms = system_us % 1000LL;

  char message[768];
  snprintf(message, sizeof(message),
           "Request resources: request=\"%s\" wall=%lld.%03lldms "
           "user_cpu=%lld.%03lldms system_cpu=%lld.%03lldms "
           "process_peak_rss=%lldKB minor_faults=%lld "
           "major_faults=%lld voluntary_context_switches=%lld "
           "involuntary_context_switches=%lld bytes_in=%zu bytes_out=%zu",
           request, wall_ms, wall_sub_ms, user_ms, user_sub_ms, system_ms,
           system_sub_ms,
           end.peak_rss_kb,
           nonnegative_delta(end.minor_page_faults, start->minor_page_faults),
           nonnegative_delta(end.major_page_faults, start->major_page_faults),
           nonnegative_delta(end.voluntary_context_switches,
                             start->voluntary_context_switches),
           nonnegative_delta(end.involuntary_context_switches,
                             start->involuntary_context_switches),
           bytes_received, bytes_sent);
  log_debug(message);
}
