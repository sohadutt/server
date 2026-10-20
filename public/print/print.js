"use strict";

const form = document.querySelector("#print-form");
const tokenInput = document.querySelector("#token");
const documentInput = document.querySelector("#document");
const pagesInput = document.querySelector("#pages");
const fileInfo = document.querySelector("#file-info");
const statusMessage = document.querySelector("#status");
const submitButton = document.querySelector("#submit");

tokenInput.value = sessionStorage.getItem("print-token") || "";

function formatSize(bytes) {
  return bytes < 1024 * 1024
    ? `${Math.max(1, Math.round(bytes / 1024))} KiB`
    : `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function selectedFileChanged() {
  const file = documentInput.files[0];
  const isPdf = file && (file.type === "application/pdf" || file.name.toLowerCase().endsWith(".pdf"));
  pagesInput.disabled = !isPdf;
  if (!isPdf) pagesInput.value = "";
  fileInfo.textContent = file
    ? `${file.name} · ${formatSize(file.size)}`
    : "PDF, JPEG, or PNG · up to 64 MiB";
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

documentInput.addEventListener("change", selectedFileChanged);

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const file = documentInput.files[0];
  const token = tokenInput.value.trim();
  if (!file || !token) return;
  if (file.size === 0 || file.size > 64 * 1024 * 1024) {
    statusMessage.textContent = "Choose a non-empty file no larger than 64 MiB.";
    statusMessage.dataset.kind = "error";
    return;
  }
  if (pagesInput.value.trim() && !pagesInput.disabled &&
      !/^\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*$/.test(pagesInput.value.trim())) {
    statusMessage.textContent = "Enter pages like 1-3,5 or leave the field blank.";
    statusMessage.dataset.kind = "error";
    return;
  }

  sessionStorage.setItem("print-token", token);
  submitButton.disabled = true;
  statusMessage.dataset.kind = "";
  statusMessage.textContent = "Uploading and assigning a printer…";
  const headers = {
    "Authorization": `Bearer ${token}`,
    "Content-Type": contentTypeFor(file),
    "X-Print-Copies": document.querySelector("#copies").value,
    "X-Print-Sides": document.querySelector("#sides").value,
  };
  if (pagesInput.value.trim() && !pagesInput.disabled)
    headers["X-Print-Pages"] = pagesInput.value.trim();

  try {
    const response = await fetch("/api/print", { method: "POST", headers, body: file });
    const result = await response.json().catch(() => ({}));
    if (!response.ok)
      throw new Error(result.error || `Print submission failed (${response.status}).`);
    statusMessage.textContent = `Print job ${result.job_id} was sent to ${result.printer}.`;
    statusMessage.dataset.kind = "success";
    form.reset();
    tokenInput.value = token;
    selectedFileChanged();
  } catch (error) {
    statusMessage.textContent = error.message || "Print submission failed.";
    statusMessage.dataset.kind = "error";
  } finally {
    submitButton.disabled = false;
  }
});
