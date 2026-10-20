# Printing and CUPS

The print service uses `cupsGetDests2` to read configured queues and
`cupsEnumDests` to discover network printers. Discovery is bounded by a caller
supplied timeout (up to five seconds), handles repeated updates and removal
notifications, and keeps discovered devices separate from configured queues.
The `uri` field retains a discovered device URI when CUPS supplies one. The
browser print page is at `/print/`; normal URLs continue to serve the
configured static HTML, CSS, JavaScript, and other files.

Before assigning a future print job, `select_printer()` refreshes the active
job count for each configured queue across all users. It skips queues that
reject jobs or are stopped, prefers an idle queue with no active jobs, and
otherwise chooses the eligible queue with the fewest active jobs. Ties prefer
an idle queue, then the CUPS default queue, then the alphabetically first name.
It returns `NULL` if no eligible queue is available or CUPS cannot refresh the
counts. Printer assignment belongs to the service; clients should not choose
a printer in the job manifest.

## Configure CUPS and add a printer

Use the helper to start and inspect the local CUPS scheduler:

```sh
./scripts/setup_cups.sh
./scripts/setup_cups.sh --status
./scripts/setup_cups.sh --discover
./scripts/setup_cups.sh --add-printer
```

The setup helper supports systemd on Linux and launchd on macOS. It asks for
administrator privileges when the service manager needs them, but does not
install packages or enable remote CUPS administration. `--discover` lists IPP
destinations and `--add-printer` runs the interactive driverless queue setup.
Adding a queue uses `lpadmin` and requires CUPS administrator privileges.

`cupsAddDest` only creates a saved options entry for a queue that already
exists, so it is not used to create a printer queue. See the
[CUPS Programming Manual](https://openprinting.github.io/cups/doc/cupspm.html)
for destination enumeration callbacks and their update/removal flags.

Once a queue exists, `cups_server` and `cups_encryption_required` in the
server config control how this application connects to the scheduler. These
settings do not configure printer hardware or CUPS itself.
