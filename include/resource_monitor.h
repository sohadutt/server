#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include <stddef.h>

typedef struct ResourceUsageSnapshot {
  long long wall_time_ns;
  long long user_cpu_us;
  long long system_cpu_us;
  long long peak_rss_kb;
  long long minor_page_faults;
  long long major_page_faults;
  long long voluntary_context_switches;
  long long involuntary_context_switches;
} ResourceUsageSnapshot;

int resource_monitor_capture(ResourceUsageSnapshot *snapshot);
void resource_monitor_log_request(const ResourceUsageSnapshot *start,
                                  const char *request_line,
                                  size_t bytes_received, size_t bytes_sent);

#endif
