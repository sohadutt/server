"use strict";

const form = document.querySelector("#print-form");
const loginForm = document.querySelector("#login-form");
const loginStatus = document.querySelector("#login-status");
const authTitle = document.querySelector("#auth-title");
const authHint = document.querySelector("#auth-hint");
const authToggle = document.querySelector("#auth-toggle");
const switchPrompt = document.querySelector("#switch-prompt");
const passwordInput = document.querySelector("#password");
const confirmPassword = document.querySelector("#confirm-password");
const confirmLabel = document.querySelector("#confirm-label");
const signupNote = document.querySelector("#signup-note");
const accountPhone = document.querySelector("#account-phone");
const accountBalance = document.querySelector("#account-balance");
const documentInput = document.querySelector("#documents");
const documentList = document.querySelector("#document-list");
const statusMessage = document.querySelector("#status");
const submitButton = document.querySelector("#submit");
const jobsList = document.querySelector("#jobs-list");
const jobsEmpty = document.querySelector("#jobs-empty");

let sessionToken = "";
let signupMode = false;
let documents = [];
let batchId = "";
let qrApplied = false;
let signedInUser = null;

authToggle.addEventListener("click", () => {
  signupMode = !signupMode;
  authTitle.textContent = signupMode ? "Create your account" : "Sign in";
  authHint.textContent = signupMode
    ? "Create an account with your phone number and a password."
    : "Use the phone number and password provided for your account.";
  document.querySelector("#login-submit").textContent = signupMode
    ? "Create account"
    : "Sign in";
  switchPrompt.textContent = signupMode ? "Already registered?" : "New here?";
  authToggle.textContent = signupMode ? "Sign in" : "Create account";
  confirmPassword.hidden = !signupMode;
  confirmLabel.hidden = !signupMode;
  confirmPassword.required = signupMode;
  signupNote.hidden = !signupMode;
  passwordInput.autocomplete = signupMode ? "new-password" : "current-password";
  loginStatus.textContent = "";
  loginStatus.dataset.kind = "";
});

function showPrintForm(user) {
  loginForm.hidden = true;
  form.hidden = false;
  accountPhone.textContent = user.phone;
  accountBalance.textContent = `${user.credits} credits`;
  signedInUser = { id: user.user_id, phone: user.phone };
  passwordInput.value = "";
  confirmPassword.value = "";
  loginStatus.textContent = "";
  refreshJobs();
}

function endSession(message = "") {
  sessionToken = "";
  form.hidden = true;
  loginForm.hidden = false;
  documents = [];
  batchId = "";
  qrApplied = false;
  signedInUser = null;
  documentList.replaceChildren();
  documentInput.value = "";
  submitButton.disabled = true;
  jobsList.replaceChildren();
  if (message) {
    loginStatus.textContent = message;
    loginStatus.dataset.kind = "error";
  }
  passwordInput.value = "";
  confirmPassword.value = "";
}

async function loadSession() {
  if (!sessionToken) return;
  try {
    const response = await fetch("/api/me", {
      headers: { Authorization: `Bearer ${sessionToken}` },
    });
    if (!response.ok) throw new Error("Please sign in again.");
    showPrintForm(await response.json());
  } catch (error) {
    endSession(error.message);
  }
}

loginForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const button = document.querySelector("#login-submit");
  const phone = document.querySelector("#phone").value.trim();
  const password = passwordInput.value;
  if (signupMode && password !== confirmPassword.value) {
    loginStatus.textContent = "The passwords do not match.";
    loginStatus.dataset.kind = "error";
    return;
  }
  if (!/^\+[1-9]\d{7,14}$/.test(phone)) {
    loginStatus.textContent = "Enter a phone number in international format, such as +14155550123.";
    loginStatus.dataset.kind = "error";
    return;
  }
  button.disabled = true;
  authToggle.disabled = true;
  loginStatus.dataset.kind = "";
  loginStatus.textContent = signupMode ? "Creating your account…" : "Signing in…";
  try {
    const response = await fetch(signupMode ? "/api/signup" : "/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        phone,
        password,
      }),
    });
    const result = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(result.error || "Sign in failed.");
    sessionToken = result.token;
    showPrintForm(result);
  } catch (error) {
    passwordInput.value = "";
    confirmPassword.value = "";
    loginStatus.textContent = error.message || "Sign in failed.";
    loginStatus.dataset.kind = "error";
  } finally {
    button.disabled = false;
    authToggle.disabled = false;
  }
});

document.querySelector("#logout").addEventListener("click", async () => {
  if (sessionToken) {
    try {
      await fetch("/api/logout", {
        method: "POST",
        headers: { Authorization: `Bearer ${sessionToken}` },
      });
    } catch (_) {}
  }
  endSession();
});

function formatSize(bytes) {
  return bytes < 1024 * 1024
    ? `${Math.max(1, Math.round(bytes / 1024))} KiB`
    : `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function contentTypeFor(file) {
  if (file.type === "application/pdf" || file.type === "image/jpeg" || file.type === "image/png")
    return file.type;
  const name = file.name.toLowerCase();
  if (name.endsWith(".pdf")) return "application/pdf";
  if (name.endsWith(".jpg") || name.endsWith(".jpeg")) return "image/jpeg";
  if (name.endsWith(".png")) return "image/png";
  return file.type;
}

function newRequestId() {
  if (crypto.randomUUID) return crypto.randomUUID();
  return Array.from(crypto.getRandomValues(new Uint8Array(16)))
    .map((value) => value.toString(16).padStart(2, "0")).join("");
}

function isPdf(file) {
  return file.type === "application/pdf" || file.name.toLowerCase().endsWith(".pdf");
}

function pagesToPrint(pageRange, pageCount) {
  if (!pageRange.trim()) return Array.from({ length: pageCount }, (_, index) => index);
  const selected = [];
  for (const part of pageRange.split(",")) {
    const [firstText, lastText = firstText] = part.split("-");
    const first = Number(firstText);
    const last = Number(lastText);
    if (!Number.isInteger(first) || !Number.isInteger(last) || first < 1 ||
        last < first || last > pageCount)
      throw new Error(`Page range for this PDF must be between 1 and ${pageCount}.`);
    for (let page = first; page <= last; page++) selected.push(page - 1);
  }
  return selected;
}

async function prepareStampedDocument(item, user, settings) {
  const { PDFDocument, StandardFonts, rgb } = PDFLib;
  const sourceBytes = new Uint8Array(await item.file.arrayBuffer());
  const pdf = isPdf(item.file) ? await PDFDocument.load(sourceBytes) : await PDFDocument.create();

  if (isPdf(item.file)) {
    const originalPages = pdf.getPageCount();
    const selectedIndexes = pagesToPrint(item.pages, originalPages);
    if (selectedIndexes.length === 0)
      throw new Error("Select at least one page to print.");
    if (selectedIndexes.length !== originalPages ||
        selectedIndexes.some((page, index) => page !== index)) {
      const selectedPages = await pdf.copyPages(pdf, selectedIndexes);
      for (let index = pdf.getPageCount() - 1; index >= 0; index--)
        pdf.removePage(index);
      for (const page of selectedPages) pdf.addPage(page);
    }
  } else {
    const image = contentTypeFor(item.file) === "image/png"
      ? await pdf.embedPng(sourceBytes)
      : await pdf.embedJpg(sourceBytes);
    const pointsPerPixel = 72 / 150;
    const page = pdf.addPage([
      Math.max(72, image.width * pointsPerPixel),
      Math.max(72, image.height * pointsPerPixel),
    ]);
    page.drawImage(image, {
      x: 0, y: 0,
      width: image.width * pointsPerPixel,
      height: image.height * pointsPerPixel,
    });
  }

  // Printed pages can be shared or discarded. Keep phone numbers out of the
  // QR payload; this identifier is only for operational lookup, never auth.
  const payload = JSON.stringify({ user_id: user.id });
  const code = qrcode(0, "L");
  code.addData(payload, "Byte");
  code.make();
  const font = await pdf.embedFont(StandardFonts.Helvetica);
  const page = pdf.getPage(0);
  const size = Number(settings.size) * 72 / 25.4;
  const margin = 10 * 72 / 25.4;
  const { width, height } = page.getSize();
  const left = settings.position.endsWith("left");
  const top = settings.position.startsWith("top");
  const x = left ? margin : width - margin - size;
  const y = top ? height - margin - size : margin;
  if (x < 0 || y < 0 || x + size > width || y + size > height)
    throw new Error("This page is too small for the selected QR size and margin.");
  const moduleCount = code.getModuleCount();
  const moduleSize = size / (moduleCount + 8);
  page.drawRectangle({ x, y, width: size, height: size, color: rgb(1, 1, 1) });
  for (let row = 0; row < moduleCount; row++) {
    for (let column = 0; column < moduleCount; column++) {
      if (code.isDark(row, column)) {
        page.drawRectangle({
          x: x + (column + 4) * moduleSize,
          y: y + size - (row + 5) * moduleSize,
          width: moduleSize,
          height: moduleSize,
          color: rgb(0, 0, 0),
        });
      }
    }
  }
  const labelY = top ? y - 8 : y + size + 2;
  const label = `ID: ${user.id}`;
  const labelWidth = font.widthOfTextAtSize(label, 6);
  if (labelY < 0 || labelY + 6 > height || x + labelWidth > width)
    throw new Error("This page does not have enough room for the QR label.");
  page.drawText(label, {
    x, y: labelY, size: 6, font,
    maxWidth: Math.max(24, width - x - margin),
  });

  const result = await pdf.save({ useObjectStreams: true });
  return new Blob([result], { type: "application/pdf" });
}

function renderDocuments() {
  documentList.replaceChildren();
  for (const item of documents) {
    const card = document.createElement("article");
    card.className = "document-card";
    const heading = document.createElement("div");
    heading.className = "document-heading";
    const headingText = document.createElement("div");
    const title = document.createElement("p");
    title.className = "document-title";
    title.textContent = item.file.name;
    const meta = document.createElement("p");
    meta.className = "document-meta";
    meta.textContent = `${formatSize(item.file.size)} · ${item.status || "Ready"}`;
    headingText.append(title, meta);
    const remove = document.createElement("button");
    remove.className = "icon-button";
    remove.type = "button";
    remove.textContent = "Remove";
    remove.disabled = item.status === "Sending…" || item.status === "Sent";
    remove.addEventListener("click", () => {
      documents = documents.filter((entry) => entry !== item);
      if (documents.length === 0 || documents.every((entry) => entry.status === "Sent"))
        batchId = "";
      renderDocuments();
    });
    heading.append(headingText, remove);
    card.append(heading);

    const options = document.createElement("div");
    options.className = "options";
    const copiesField = document.createElement("div");
    copiesField.className = "option";
    const copiesLabel = document.createElement("label");
    copiesLabel.className = "field-label";
    copiesLabel.textContent = "Copies";
    const copies = document.createElement("input");
    copies.type = "number";
    copies.min = "1";
    copies.max = "99";
    copies.value = item.copies;
    copies.disabled = item.status === "Sent";
    copiesLabel.htmlFor = `copies-${item.requestId}`;
    copies.id = copiesLabel.htmlFor;
    copies.addEventListener("input", () => { item.copies = copies.value; });
    copiesField.append(copiesLabel, copies);

    const sidesField = document.createElement("div");
    sidesField.className = "option";
    const sidesLabel = document.createElement("label");
    sidesLabel.className = "field-label";
    sidesLabel.textContent = "Sides";
    const sides = document.createElement("select");
    sides.id = `sides-${item.requestId}`;
    sidesLabel.htmlFor = sides.id;
    for (const [value, label] of [
      ["one-sided", "Single-sided"],
      ["two-sided-long-edge", "Double-sided · long edge"],
      ["two-sided-short-edge", "Double-sided · short edge"],
    ]) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = label;
      sides.append(option);
    }
    sides.value = item.sides;
    sides.disabled = item.status === "Sent";
    sides.addEventListener("change", () => { item.sides = sides.value; });
    sidesField.append(sidesLabel, sides);
    options.append(copiesField, sidesField);
    card.append(options);

    const pagesLabel = document.createElement("label");
    pagesLabel.className = "field-label";
    pagesLabel.textContent = "Pages";
    pagesLabel.htmlFor = `pages-${item.requestId}`;
    const pages = document.createElement("input");
    pages.id = pagesLabel.htmlFor;
    pages.className = "pages-field";
    pages.type = "text";
    pages.inputMode = "numeric";
    pages.placeholder = isPdf(item.file) ? "All pages" : "PDF only";
    pages.disabled = !isPdf(item.file) || item.status === "Sent";
    pages.value = item.pages;
    pages.addEventListener("input", () => { item.pages = pages.value; });
    card.append(pagesLabel, pages);
    if (item.error) {
      const error = document.createElement("p");
      error.className = "document-meta status-error";
      error.textContent = item.error;
      card.append(error);
    }
    documentList.append(card);
  }
  submitButton.disabled = !sessionToken || documents.length === 0 ||
    documents.every((item) => item.status === "Sent");
  submitButton.textContent = documents.some((item) => item.status === "Sent")
    ? "Send remaining documents"
    : "Send print job";
}

documentInput.addEventListener("change", () => {
  const incoming = Array.from(documentInput.files || []);
  if (documents.length === 0 || documents.every((entry) => entry.status === "Sent")) {
    batchId = "";
    qrApplied = false;
  }
  const available = Math.max(0, 10 - documents.length);
  const added = incoming.slice(0, available);
  for (const file of added) {
    documents.push({
      file,
      copies: "1",
      sides: "one-sided",
      pages: "",
      requestId: newRequestId(),
      status: "Ready",
      error: "",
    });
  }
  if (incoming.length > available) {
    statusMessage.textContent = "A grouped print job can contain up to 10 documents.";
    statusMessage.dataset.kind = "error";
  }
  documentInput.value = "";
  renderDocuments();
});

function renderJobHistory(jobs) {
  jobsList.replaceChildren();
  jobsEmpty.hidden = jobs.length > 0;
  const groups = new Map();
  for (const job of jobs) {
    const id = job.batch_id || `single-${job.id}`;
    if (!groups.has(id)) groups.set(id, []);
    groups.get(id).push(job);
  }
  for (const [id, entries] of groups) {
    const batch = document.createElement("article");
    batch.className = "job-batch";
    const heading = document.createElement("p");
    heading.className = "job-batch-title";
    const date = new Date(entries[0].created_at * 1000);
    heading.textContent = `Print job · ${date.toLocaleString()} · ${entries.length} document${entries.length === 1 ? "" : "s"}`;
    heading.dataset.batchId = id;
    batch.append(heading);
    for (const job of entries) {
      const row = document.createElement("div");
      row.className = "job-entry";
      const name = document.createElement("span");
      name.className = "job-entry-name";
      name.textContent = job.document_name || job.format;
      const detail = document.createElement("span");
      detail.className = "job-entry-detail";
      detail.textContent = `${job.printer} · ${job.copies} cop${job.copies === 1 ? "y" : "ies"} · ${job.sides} · CUPS #${job.cups_job_id}`;
      const state = document.createElement("span");
      state.className = "job-status";
      state.dataset.status = job.status;
      state.textContent = job.status;
      row.append(name, detail, state);
      if (job.status === "submitted" || job.status === "processing") {
        const cancel = document.createElement("button");
        cancel.className = "icon-button job-cancel";
        cancel.type = "button";
        cancel.textContent = "Cancel";
        cancel.addEventListener("click", async () => {
          cancel.disabled = true;
          try {
            const response = await fetch(`/api/jobs/${encodeURIComponent(job.id)}/cancel`, {
              method: "POST",
              headers: { Authorization: `Bearer ${sessionToken}` },
            });
            const result = await response.json().catch(() => ({}));
            if (!response.ok) throw new Error(result.error || "Could not cancel this print job.");
            await refreshJobs();
          } catch (error) {
            cancel.disabled = false;
            statusMessage.textContent = error.message || "Could not cancel this print job.";
            statusMessage.dataset.kind = "error";
          }
        });
        row.append(cancel);
      }
      batch.append(row);
    }
    jobsList.append(batch);
  }
}

async function refreshJobs() {
  if (!sessionToken) return;
  try {
    const response = await fetch("/api/jobs", {
      headers: { Authorization: `Bearer ${sessionToken}` },
    });
    if (response.status === 401) return endSession("Session expired. Please sign in again.");
    if (!response.ok) throw new Error("Could not load print history.");
    const result = await response.json();
    renderJobHistory(Array.isArray(result.jobs) ? result.jobs : []);
  } catch (error) {
    jobsEmpty.textContent = error.message || "Could not load print history.";
    jobsEmpty.hidden = false;
  }
}

document.querySelector("#refresh-jobs").addEventListener("click", refreshJobs);

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const token = sessionToken;
  if (documents.length === 0 || !token) return;
  for (const item of documents.filter((entry) => entry.status !== "Sent")) {
    if (item.file.size === 0 || item.file.size > 64 * 1024 * 1024) {
      statusMessage.textContent = `${item.file.name} must be non-empty and no larger than 64 MiB.`;
      statusMessage.dataset.kind = "error";
      return;
    }
    if (!/^[1-9]\d{0,1}$/.test(item.copies) || Number(item.copies) > 99) {
      statusMessage.textContent = `Copies for ${item.file.name} must be between 1 and 99.`;
      statusMessage.dataset.kind = "error";
      return;
    }
    if (item.pages.trim() && (!isPdf(item.file) ||
        !/^\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*$/.test(item.pages.trim()))) {
      statusMessage.textContent = `Check the page range for ${item.file.name}. Use a PDF range like 1-3,5.`;
      statusMessage.dataset.kind = "error";
      return;
    }
  }

  submitButton.disabled = true;
  statusMessage.dataset.kind = "";
  if (!batchId) batchId = newRequestId();
  const pending = documents.filter((item) => item.status !== "Sent");
  let sent = 0;

  try {
    for (const item of pending) {
      item.status = "Sending…";
      item.error = "";
      renderDocuments();
      statusMessage.textContent = `Sending ${sent + 1} of ${pending.length}: ${item.file.name}`;
      try {
        const qrForThisDocument = document.querySelector("#qr-enabled").checked && !qrApplied;
        let upload = item.file;
        let uploadType = contentTypeFor(item.file);
        if (qrForThisDocument) {
          upload = await prepareStampedDocument(item, signedInUser, {
            position: document.querySelector("#qr-position").value,
            size: document.querySelector("#qr-size").value,
          });
          uploadType = "application/pdf";
        }
        if (upload.size > 64 * 1024 * 1024)
          throw new Error(`${item.file.name} exceeds 64 MiB after browser-side preparation.`);
        const headers = {
          Authorization: `Bearer ${token}`,
          "Content-Type": uploadType,
          "X-Print-Copies": item.copies,
          "X-Print-Sides": item.sides,
          "X-Print-Batch-ID": batchId,
          "X-Print-Name": item.file.name,
          "X-Print-Request-ID": item.requestId,
        };
        if (item.pages.trim()) {
          headers["X-Print-Pages"] = item.pages.trim();
          if (qrForThisDocument) headers["X-Print-Pages-Applied"] = "true";
        }
        const response = await fetch("/api/print", {
          method: "POST", headers, body: upload,
        });
        const result = await response.json().catch(() => ({}));
        if (response.status === 401) {
          endSession("Session expired. Please sign in again.");
          throw new Error("Session expired. Please sign in again.");
        }
        if (!response.ok)
          throw new Error(result.error || `Print submission failed (${response.status}).`);
        item.status = "Sent";
        item.result = result;
        if (qrForThisDocument) qrApplied = true;
        sent++;
      } catch (error) {
        item.status = "Failed";
        item.error = error.message || "Print submission failed.";
      }
      renderDocuments();
    }
    const failed = documents.filter((item) => item.status === "Failed").length;
    statusMessage.textContent = failed
      ? `${sent} document${sent === 1 ? "" : "s"} sent; ${failed} failed. You can retry failed documents.`
      : `${sent} document${sent === 1 ? "" : "s"} sent as grouped print job ${batchId.slice(0, 8)}.`;
    statusMessage.dataset.kind = failed ? "error" : "success";
    await refreshJobs();
  } finally {
    renderDocuments();
  }
});

loadSession();
