# User printing guide

## Sign in

Open `/print/` on the server over HTTPS. Enter the account's phone number in
international format (for example `+14155550123`) and password. New users can
choose **Create account** if signup is enabled for the deployment. The current
service does not verify phone ownership with SMS, so an administrator should
control who can access an account.

The browser keeps the session token in memory. Reloading the page or closing
the tab ends that browser session; sign in again to continue.

## Build a grouped print job

Choose one or more PDF, JPEG, or PNG files. A grouped job can contain up to 10
documents, with a maximum upload size of 64 MiB per document. Each document
becomes its own CUPS job, so you can set its options separately:

- **Copies:** 1 through 99.
- **Sides:** single-sided, double-sided along the long edge, or double-sided
  along the short edge.
- **Pages:** optional PDF ranges such as `1-3,5`. Leave blank to print all
  pages. Page ranges are only available for PDF files.

Select **Send print job** to submit the documents. They are sent one at a time
and share a group ID in job history. A printer is assigned by the server. The
browser cannot select or override the printer queue.

If one document fails after earlier documents were accepted, the accepted
jobs remain submitted. The page marks the failed document and lets you retry
it without re-submitting documents already marked **Sent**.

## Identity QR code

The default setting stamps a 12 mm QR code on the first printed page of the
first successfully submitted document in the group. The QR contains the
account's numeric user ID and phone number. A small printed user ID label is
placed beside it. Treat printed documents as containing this personal
information.

Open **Advanced print settings** to turn the QR off, move it to another corner,
or choose a 16 mm or 20 mm size. The mark is inset 10 mm from the page edge,
but the actual printable margins depend on the printer. The selected corner
may overlap page content or be clipped by a printer with large margins.

When a PDF page range is selected, the QR is placed on the first selected
page. Images are converted to PDF before stamping. If the browser cannot
prepare a document, it shows an error and does not submit that document.

## Follow or cancel jobs

The **Recent jobs** area groups documents submitted together and lists each
document's printer, copy count, duplex setting, CUPS job number, and status.
Select **Refresh** to query CUPS for current status. You can cancel a job while
it is pending or processing; a completed, failed, or canceled job cannot be
canceled again.

Job history refreshes when opened and when **Refresh** is selected. It is not
a live progress stream, so status can change between refreshes.

## Credits

The account bar shows the account's credit balance. Print submissions do not
currently deduct credits, and the application does not process payments.
Ask the administrator how credits are being managed for this deployment.
