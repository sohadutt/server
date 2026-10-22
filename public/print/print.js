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
const documentInput = document.querySelector("#document");
const pagesInput = document.querySelector("#pages");
const fileInfo = document.querySelector("#file-info");
const statusMessage = document.querySelector("#status");
const submitButton = document.querySelector("#submit");

let sessionToken = "";
let signupMode = false;

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
  passwordInput.value = "";
  confirmPassword.value = "";
  loginStatus.textContent = "";
}

function endSession(message = "") {
  sessionToken = "";
  form.hidden = true;
  loginForm.hidden = false;
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
  const token = sessionToken;
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
    if (response.status === 401)
      endSession("Session expired. Please sign in again.");
    if (!response.ok)
      throw new Error(result.error || `Print submission failed (${response.status}).`);
    statusMessage.textContent = `Print job ${result.job_id} was sent to ${result.printer}.`;
    statusMessage.dataset.kind = "success";
    form.reset();
    selectedFileChanged();
    try {
      const account = await fetch("/api/me", {
        headers: { Authorization: `Bearer ${sessionToken}` },
      });
      if (account.ok) {
        const user = await account.json();
        accountBalance.textContent = `${user.credits} credits`;
      }
    } catch (_) {}
  } catch (error) {
    statusMessage.textContent = error.message || "Print submission failed.";
    statusMessage.dataset.kind = "error";
  } finally {
    submitButton.disabled = false;
  }
});

loadSession();
