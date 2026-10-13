#include "printing.h"
#include "logger.h"

#include <cups/cups.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value) {
  if (value == NULL) {
    return NULL;
  }

  size_t len = strlen(value) + 1;
  char *copy = malloc(len);

  if (copy != NULL) {
    memcpy(copy, value, len);
  }

  return copy;
}

void print_service_init(PrintService *service) {
  if (service == NULL) {
    return;
  }

  service->printers = NULL;
  service->count = 0;
  service->discovered_printers = NULL;
  service->discovered_count = 0;
}

void printer_free(Printer *printer) {
  if (printer == NULL) {
    return;
  }

  free(printer->name);
  free(printer->instance);
  free(printer->uri);
  free(printer->location);
  free(printer->description);
  free(printer->make_and_model);
  free(printer->state_message);

  for (size_t i = 0; i < printer->supported_media_count; i++) {
    free(printer->supported_media[i]);
  }

  free(printer->supported_media);

  memset(printer, 0, sizeof(*printer));
}

void free_printers(PrintService *service) {
  if (service == NULL) {
    return;
  }

  for (size_t i = 0; i < service->count; i++) {
    printer_free(&service->printers[i]);
  }

  free(service->printers);
  service->printers = NULL;
  service->count = 0;
}

static void free_discovered_printers(PrintService *service) {
  if (service == NULL) {
    return;
  }

  for (size_t i = 0; i < service->discovered_count; i++) {
    printer_free(&service->discovered_printers[i]);
  }

  free(service->discovered_printers);
  service->discovered_printers = NULL;
  service->discovered_count = 0;
}

void print_service_destroy(PrintService *service) {
  if (service == NULL) {
    return;
  }

  free_printers(service);
  free_discovered_printers(service);
}

const char *printer_state_to_string(PrinterState state) {
  switch (state) {
  case PRINTER_STATE_IDLE:
    return "idle";
  case PRINTER_STATE_PROCESSING:
    return "processing";
  case PRINTER_STATE_STOPPED:
    return "stopped";
  case PRINTER_STATE_UNKNOWN:
  default:
    return "unknown";
  }
}

static char *copy_option(const char *name, const cups_dest_t *dest) {
  return copy_string(cupsGetOption(name, dest->num_options, dest->options));
}

static char *copy_printer_uri(const cups_dest_t *dest) {
  const char *uri = cupsGetOption("printer-uri-supported", dest->num_options,
                                  dest->options);
  if (uri == NULL) {
    uri = cupsGetOption("device-uri", dest->num_options, dest->options);
  }
  return copy_string(uri);
}

static int option_is_true(const char *name, const cups_dest_t *dest) {
  const char *value = cupsGetOption(name, dest->num_options, dest->options);
  return value != NULL &&
         (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
}

static int option_is_false(const char *name, const cups_dest_t *dest) {
  const char *value = cupsGetOption(name, dest->num_options, dest->options);
  return value != NULL &&
         (strcmp(value, "false") == 0 || strcmp(value, "0") == 0);
}

static Printer printer_from_dest(const cups_dest_t *dest, size_t id,
                                 int discovered) {
  Printer printer = {0};

  printer.id = (int)id;
  printer.name = copy_string(dest->name);
  printer.instance = copy_string(dest->instance);
  printer.is_default = dest->is_default;
  printer.uri = copy_printer_uri(dest);
  printer.location = copy_option("printer-location", dest);
  printer.description = copy_option("printer-info", dest);
  printer.make_and_model = copy_option("printer-make-and-model", dest);
  printer.state_message = copy_option("printer-state-reasons", dest);
  printer.is_accepting_jobs =
      !option_is_false("printer-is-accepting-jobs", dest);
  printer.is_shared = option_is_true("printer-is-shared", dest);
  printer.is_discovered = discovered;
  printer.state = PRINTER_STATE_UNKNOWN;
  const char *state =
      cupsGetOption("printer-state", dest->num_options, dest->options);
  if (state != NULL) {
    if (strcmp(state, "3") == 0)
      printer.state = PRINTER_STATE_IDLE;
    else if (strcmp(state, "4") == 0)
      printer.state = PRINTER_STATE_PROCESSING;
    else if (strcmp(state, "5") == 0)
      printer.state = PRINTER_STATE_STOPPED;
  }
  printer.is_busy = printer.state == PRINTER_STATE_PROCESSING;
  return printer;
}

static int printer_matches_dest(const Printer *printer,
                                const cups_dest_t *dest) {
  return printer->name != NULL && dest->name != NULL &&
         strcmp(printer->name, dest->name) == 0 &&
         ((printer->instance == NULL && dest->instance == NULL) ||
          (printer->instance != NULL && dest->instance != NULL &&
           strcmp(printer->instance, dest->instance) == 0));
}

static void remove_discovered_dest(PrintService *service,
                                   const cups_dest_t *dest) {
  for (size_t i = 0; i < service->count; i++) {
    if (printer_matches_dest(&service->printers[i], dest)) {
      printer_free(&service->printers[i]);
      memmove(&service->printers[i], &service->printers[i + 1],
              (service->count - i - 1) * sizeof(*service->printers));
      service->count--;
      break;
    }
  }
}

static int store_discovered_dest(PrintService *service,
                                 const cups_dest_t *dest) {
  size_t index = service->count;
  for (size_t i = 0; i < service->count; i++) {
    if (printer_matches_dest(&service->printers[i], dest)) {
      index = i;
      break;
    }
  }

  Printer printer = printer_from_dest(dest, index + 1, 1);
  if (printer.name == NULL ||
      (dest->instance != NULL && printer.instance == NULL)) {
    printer_free(&printer);
    return -1;
  }

  if (index < service->count) {
    printer_free(&service->printers[index]);
    service->printers[index] = printer;
    return 0;
  }

  Printer *printers =
      realloc(service->printers, (service->count + 1) * sizeof(*printers));
  if (printers == NULL) {
    printer_free(&printer);
    return -1;
  }

  service->printers = printers;
  service->printers[service->count++] = printer;
  return 0;
}

typedef struct {
  PrintService discovered;
  int failed;
} DiscoveryContext;

static int discovery_callback(void *user_data, unsigned flags,
                              cups_dest_t *dest) {
  DiscoveryContext *context = user_data;
  if (context == NULL || dest == NULL || (flags & CUPS_DEST_FLAGS_ERROR)) {
    if (context != NULL) {
      context->failed = 1;
    }
    return 0;
  }

  if (flags & CUPS_DEST_FLAGS_REMOVED) {
    remove_discovered_dest(&context->discovered, dest);
  } else if (store_discovered_dest(&context->discovered, dest) != 0) {
    context->failed = 1;
    return 0;
  }

  return 1;
}

int discover_printers(PrintService *service, int timeout_ms) {
  if (service == NULL) {
    log_error("PrintService must not be NULL");
    return -1;
  }

  if (timeout_ms < 0) {
    timeout_ms = 0;
  } else if (timeout_ms > 5000) {
    timeout_ms = 5000;
  }

  DiscoveryContext context = {0};
  if (!cupsEnumDests(CUPS_DEST_FLAGS_NONE, timeout_ms, NULL,
                     CUPS_PRINTER_DISCOVERED, CUPS_PRINTER_DISCOVERED,
                     discovery_callback, &context) ||
      context.failed) {
    free_printers(&context.discovered);
    log_error("Failed to discover CUPS printers");
    return -1;
  }

  free_discovered_printers(service);
  service->discovered_printers = context.discovered.printers;
  service->discovered_count = context.discovered.count;

  char message[128];
  snprintf(message, sizeof(message), "Discovered %zu printer(s)",
           service->discovered_count);
  log_debug(message);
  return 0;
}

int get_printers(PrintService *service) {
  if (service == NULL) {
    log_error("PrintService must not be NULL");
    return -1;
  }

  cups_dest_t *dests = NULL;
  int num_dests = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);

  if (num_dests < 0) {
    log_error("Failed to retrieve printers");
    return -1;
  }

  Printer *printers = NULL;

  if (num_dests > 0) {
    printers = calloc((size_t)num_dests, sizeof(*printers));

    if (printers == NULL) {
      cupsFreeDests(num_dests, dests);
      log_error("Failed to allocate printer list");
      return -1;
    }
  }

  for (int i = 0; i < num_dests; i++) {
    printers[i] = printer_from_dest(&dests[i], (size_t)i + 1, 0);
    Printer *printer = &printers[i];

    if (printer->name == NULL ||
        (dests[i].instance != NULL && printer->instance == NULL)) {
      for (int j = 0; j <= i; j++) {
        printer_free(&printers[j]);
      }

      free(printers);
      cupsFreeDests(num_dests, dests);
      log_error("Failed to allocate printer information");
      return -1;
    }
  }

  cupsFreeDests(num_dests, dests);

  free_printers(service);
  service->printers = printers;
  service->count = (size_t)num_dests;

  char buffer[128];
  snprintf(buffer, sizeof(buffer), "Loaded %zu CUPS destination(s)",
           service->count);
  log_debug(buffer);

  return 0;
}

Printer *find_printer(PrintService *service, const char *name) {
  if (service == NULL || name == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < service->count; i++) {
    if (service->printers[i].name != NULL &&
        strcmp(service->printers[i].name, name) == 0) {
      return &service->printers[i];
    }
  }

  for (size_t i = 0; i < service->discovered_count; i++) {
    if (service->discovered_printers[i].name != NULL &&
        strcmp(service->discovered_printers[i].name, name) == 0) {
      return &service->discovered_printers[i];
    }
  }

  return NULL;
}

Printer *get_default_printer(PrintService *service) {
  if (service == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < service->count; i++) {
    if (service->printers[i].is_default) {
      return &service->printers[i];
    }
  }
  return NULL;
}

int refresh_printer(Printer *printer) {
  if (printer == NULL || printer->name == NULL)
    return -1;
  cups_dest_t *dest =
      cupsGetNamedDest(CUPS_HTTP_DEFAULT, printer->name, printer->instance);
  if (dest == NULL)
    return -1;
  const char *state =
      cupsGetOption("printer-state", dest->num_options, dest->options);
  printer->is_accepting_jobs =
      !option_is_false("printer-is-accepting-jobs", dest);
  printer->state = PRINTER_STATE_UNKNOWN;
  if (state != NULL) {
    if (strcmp(state, "3") == 0)
      printer->state = PRINTER_STATE_IDLE;
    else if (strcmp(state, "4") == 0)
      printer->state = PRINTER_STATE_PROCESSING;
    else if (strcmp(state, "5") == 0)
      printer->state = PRINTER_STATE_STOPPED;
  }
  printer->is_busy = printer->state == PRINTER_STATE_PROCESSING;
  cupsFreeDests(1, dest);
  return 0;
}

int refresh_printer_jobs(PrintService *service) {
  if (service == NULL)
    return -1;
  for (size_t i = 0; i < service->count; i++) {
    cups_job_t *jobs = NULL;
    int count =
        cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, service->printers[i].name, 0,
                     CUPS_WHICHJOBS_ACTIVE);
    if (count < 0) {
      return -1;
    }
    service->printers[i].queued_jobs = (size_t)count;
    cupsFreeJobs(count, jobs);
  }
  return 0;
}

static int printer_precedes(const Printer *candidate, const Printer *current) {
  if (current == NULL) {
    return 1;
  }

  int candidate_idle = candidate->state == PRINTER_STATE_IDLE;
  int current_idle = current->state == PRINTER_STATE_IDLE;
  if (candidate_idle != current_idle) {
    return candidate_idle;
  }

  if (candidate->is_default != current->is_default) {
    return candidate->is_default;
  }

  if (candidate->name == NULL) {
    return 0;
  }
  if (current->name == NULL) {
    return 1;
  }

  return strcmp(candidate->name, current->name) < 0;
}

Printer *select_printer(PrintService *service) {
  if (service == NULL || refresh_printer_jobs(service) != 0) {
    return NULL;
  }

  Printer *best_free = NULL;
  Printer *best_loaded = NULL;

  for (size_t i = 0; i < service->count; i++) {
    Printer *printer = &service->printers[i];
    if (refresh_printer(printer) != 0 || !printer->is_accepting_jobs ||
        printer->state == PRINTER_STATE_STOPPED) {
      continue;
    }

    if (printer->state == PRINTER_STATE_IDLE && printer->queued_jobs == 0) {
      if (printer_precedes(printer, best_free)) {
        best_free = printer;
      }
    } else if (best_loaded == NULL ||
               printer->queued_jobs < best_loaded->queued_jobs ||
               (printer->queued_jobs == best_loaded->queued_jobs &&
                printer_precedes(printer, best_loaded))) {
      best_loaded = printer;
    }
  }

  Printer *selected = best_free != NULL ? best_free : best_loaded;
  if (selected != NULL) {
    char message[256];
    snprintf(message, sizeof(message),
             "Selected printer '%s' (%zu active job(s), %s)",
             selected->name != NULL ? selected->name : "unknown",
             selected->queued_jobs, printer_state_to_string(selected->state));
    log_debug(message);
  }

  return selected;
}
