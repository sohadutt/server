#ifndef PRINTING_H
#define PRINTING_H

#include <stddef.h>

typedef enum {
  PRINTER_STATE_UNKNOWN = 0,
  PRINTER_STATE_IDLE,
  PRINTER_STATE_PROCESSING,
  PRINTER_STATE_STOPPED
} PrinterState;

typedef struct {
  int id;
  char *name;
  char *instance;
  char *uri;
  char *location;
  char *description;
  char *make_and_model;

  PrinterState state;
  char *state_message;

  int is_default;
  int is_accepting_jobs;
  int is_shared;
  int is_busy;

  size_t queued_jobs;

  int supports_color;
  int supports_duplex;
  int is_discovered;

  char **supported_media;
  size_t supported_media_count;

  int resolution_x;
  int resolution_y;
} Printer;

typedef struct {
  Printer *printers;
  size_t count;
  Printer *discovered_printers;
  size_t discovered_count;
} PrintService;

void print_service_init(PrintService *service);
/* Reads configured queues from the current CUPS server. */
int get_printers(PrintService *service);
/* Enumerates network destinations for up to timeout_ms milliseconds. */
int discover_printers(PrintService *service, int timeout_ms);
int refresh_printer(Printer *printer);
/* Returns an internal service-owned printer, including discovered entries. */
Printer *find_printer(PrintService *service, const char *name);
Printer *get_default_printer(PrintService *service);
int refresh_printer_jobs(PrintService *service);
/* Chooses a service-owned queue after refreshing all active-job counts. */
Printer *select_printer(PrintService *service);
const char *printer_state_to_string(PrinterState state);
void printer_free(Printer *printer);
void free_printers(PrintService *service);
void print_service_destroy(PrintService *service);

#endif
