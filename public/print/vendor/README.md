# Browser print dependencies

These pinned, self-hosted browser libraries are used only by `/print/`:

- `pdf-lib.min.js` from pdf-lib 1.17.1 (MIT license)
- `qrcode-generator.js` from qrcode-generator 1.4.4 (MIT license)

They are served from this project, so browser PDF and QR processing works
without a CDN and stays within the site's same-origin Content Security Policy.
Their license texts are included beside the files.
