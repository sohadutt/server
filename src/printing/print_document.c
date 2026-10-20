#include "print_document.h"

#include <cups/cups.h>
#include <stdio.h>
#include <stdlib.h>

void print_document_free(PrintDocument *document) {
  if (document == NULL) {
    return;
  }
  free(document->path);
  free(document->mime_type);
  *document = (PrintDocument){0};
}

void print_job_free(PrintJob *job) {
  if (job == NULL) {
    return;
  }
  free(job->printer_name);
  free(job->title);
  free(job->owner);
  free(job->file_path);
  *job = (PrintJob){0};
}

void print_jobs_free(PrintJob *jobs, size_t count) {
  if (jobs == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    print_job_free(&jobs[i]);
  }
  free(jobs);
}

int printer_has_active_jobs(const char *printer_name) {
  cups_job_t *jobs = NULL;

  int count = cupsGetJobs(&jobs, printer_name, 0, CUPS_WHICHJOBS_ACTIVE);
  cupsFreeJobs(count, jobs);
  return count > 0;
}

int submit_print_job(PrintJob *job) {
  if (job == NULL || job->file_path == NULL) {
    return -1;
  }
  cups_dest_t *dests = NULL;
  int num_dests = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);
  if (num_dests <= 0) {
    fprintf(stderr, "No printers found\n");
    return -1;
  }
  cups_dest_t *printer = NULL;

  for (int i = 0; i < num_dests; i++) {
    if (!printer_has_active_jobs(dests[i].name)) {
      printer = &dests[i];
      break;
    }
  }
  if (printer == NULL) {
    fprintf(stderr, "No idle printer available\n");
    cupsFreeDests(num_dests, dests);
    return -1;
  }
  cups_dinfo_t *info = cupsCopyDestInfo(CUPS_HTTP_DEFAULT, printer);
  if (info == NULL) {
    fprintf(stderr, "Unable to get printer information\n");
    cupsFreeDests(num_dests, dests);
    return -1;
  }
  const char *title = job->title ? job->title : "Print Job";
  ipp_status_t status = cupsCreateDestJob(CUPS_HTTP_DEFAULT, printer, info,
                                          &job->id, title, 0, NULL);
  if (status != IPP_STATUS_OK) {
    fprintf(stderr, "Unable to create job: %s\n", cupsLastErrorString());
    job->id = 0;
    cupsFreeDestInfo(info);
    cupsFreeDests(num_dests, dests);
    return -1;
  }
  printf("Created job %d on printer %s\n", job->id, printer->name);
  cupsFreeDestInfo(info);
  cupsFreeDests(num_dests, dests);

  return 0;
}
