#ifndef PRINT_DOCUMENT_H
#define PRINT_DOCUMENT_H

#include <stddef.h>

typedef struct {
  char *path;
  char *mime_type;
  size_t size_bytes;
} PrintDocument;

typedef enum {
  PRINT_JOB_UNKNOWN,
  PRINT_JOB_PENDING,
  PRINT_JOB_HELD,
  PRINT_JOB_PROCESSING,
  PRINT_JOB_COMPLETED,
  PRINT_JOB_CANCELED,
  PRINT_JOB_ABORTED
} PrintJobState;

typedef struct {
  int id;
  char *printer_name;
  char *title;
  char *owner;
  char *file_path;

  PrintJobState state;
  size_t size_bytes;
  int impressions_completed;
} PrintJob;

/* Select an available printer and submit a print job. */
int submit_print_job(PrintJob *job);

/* Check whether a printer has active jobs. */
int printer_has_active_jobs(const char *printer_name);

/* Retrieve print jobs, optionally filtered by printer. */
int get_print_jobs(const char *printer_name, PrintJob **jobs, size_t *count);

/* Cancel a print job by its CUPS job ID. */
int cancel_print_job(int job_id);

/* Release allocated job resources. */
void print_job_free(PrintJob *job);
void print_jobs_free(PrintJob *jobs, size_t count);

/* Document lifecycle and validation. */
int print_document_open(const char *path, PrintDocument *document);
int print_document_validate(const PrintDocument *document,
                            size_t max_size_bytes);
void print_document_free(PrintDocument *document);

#endif
