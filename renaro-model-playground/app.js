const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

const elements = {
  body: document.body,
  promptInput: $("#promptInput"),
  promptForm: $("#promptForm"),
  promptHelp: $("#promptHelp"),
  simulateRunButton: $("#simulateRun"),
  sendButton: $(".send-button"),
  conversationScroll: $("#conversationScroll"),
  commandDialog: $("#commandDialog"),
  commandSearch: $("#commandSearch"),
  commandResults: $("#commandResults"),
  toast: $("#toast"),
  temperature: $("#temperature"),
  temperatureValue: $("#temperatureValue"),
  contextLength: $("#contextLength"),
  modelFile: $("#modelFile"),
  loadModelLabel: $("#loadModelLabel"),
  loadModelMeta: $("#loadModelMeta"),
  workspaceTitle: $("#workspaceTitle"),
  connectionLabel: $("#connectionLabel"),
  telemetryState: $("#telemetryState"),
  runCommand: $("#runCommand"),
  newSession: $("#newSession")
};

const initialBarValues = [0.26, 0.42, 0.34, 0.58, 0.49, 0.72, 0.62, 0.84, 0.74, 0.94, 0.79, 0.68];
const sampleBarValues = [0.36, 0.48, 0.43, 0.68, 0.57, 0.78, 0.67, 0.9, 0.8, 0.96, 0.86, 0.76];
const defaultPrompt = "Compare the first three tokens and explain where uncertainty enters.";

let currentModel = "Qwen3 8B";
let sessionName = "Untitled study";
let simulationBusy = false;
let simulationToken = 0;
let toastTimer;
let activeCommandIndex = 0;

function modelSlug(modelName) {
  return modelName.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
}

function contextTokens() {
  return elements.contextLength.value.replace(/[^0-9]/g, "") || "8192";
}

function currentCommand() {
  const temperature = Number(elements.temperature.value).toFixed(2);
  return ["renaro run", "--model", modelSlug(currentModel), "--ctx", contextTokens(), "--temp", temperature].join(" ");
}

function updateCommand() {
  elements.runCommand.textContent = currentCommand();
}

function renderSessionTitle() {
  const separator = document.createElement("span");
  separator.textContent = "/";
  elements.workspaceTitle.replaceChildren(
    document.createTextNode(currentModel + " "),
    separator,
    document.createTextNode(" " + sessionName)
  );
}

function setConnection(label) {
  elements.connectionLabel.textContent = label;
}

function setTelemetryState(state) {
  elements.telemetryState.textContent = state;
  elements.telemetryState.classList.toggle("is-sampled", state === "sampled");
}

function setBarValues(values) {
  $$(".plot-bars span").forEach(function (bar, index) {
    bar.style.setProperty("--bar-scale", values[index] || 0.25);
  });
}

function resetTrace() {
  $$(".trace-row:not(.trace-row--heading)").forEach(function (row) {
    const duration = $("strong", row);
    const status = $(".trace-status", row);
    if (duration) duration.textContent = "—";
    if (status) {
      status.textContent = "pending";
      status.dataset.state = "pending";
    }
  });
}

function updateTrace() {
  const samples = [
    ["0.14 s", "sampled"],
    ["0.31 s", "sampled"],
    ["0.82 s", "sampled"],
    ["1.42 s", "sampled"]
  ];

  $$(".trace-row:not(.trace-row--heading)").forEach(function (row, index) {
    const sample = samples[index];
    const duration = $("strong", row);
    const status = $(".trace-status", row);
    if (!sample) return;
    if (duration) duration.textContent = sample[0];
    if (status) {
      status.textContent = sample[1];
      status.dataset.state = sample[1];
    }
  });
}

function resetTelemetry() {
  $("#promptSpeed").textContent = "—";
  $("#decodeSpeed").textContent = "—";
  $("#firstToken").textContent = "—";
  $("#memoryFootprint").textContent = "—";
  $("#promptSpeedNote").textContent = "run pending";
  $("#decodeSpeedNote").textContent = "run pending";
  $("#firstTokenNote").textContent = "not measured";
  $("#memoryNote").textContent = "adapter not connected";
  $("#plotNote").textContent = "awaiting run";
  setTelemetryState("waiting");
  setConnection("READY FOR A RUN");
  setBarValues(initialBarValues);
  resetTrace();
}

function updateTelemetry() {
  $("#promptSpeed").textContent = "42.6";
  $("#decodeSpeed").textContent = "18.4";
  $("#firstToken").textContent = "0.82 s";
  $("#memoryFootprint").textContent = "6.1 GB";
  $("#promptSpeedNote").textContent = "staged demo";
  $("#decodeSpeedNote").textContent = "staged demo";
  $("#firstTokenNote").textContent = "staged demo";
  $("#memoryNote").textContent = "staged demo";
  $("#plotNote").textContent = "sample only";
  setTelemetryState("sampled");
  setConnection("DEMO SAMPLE");
  setBarValues(sampleBarValues);
  updateTrace();
}

function addMessage(role, text, type, annotation) {
  const article = document.createElement("article");
  article.className = "message message--" + (type || "user");

  const meta = document.createElement("div");
  meta.className = "message__meta";

  const roleLabel = document.createElement("span");
  roleLabel.className = "message__role";
  roleLabel.textContent = role;

  const time = document.createElement("time");
  time.textContent = "now";

  const paragraph = document.createElement("p");
  paragraph.textContent = text;

  meta.append(roleLabel, time);
  article.append(meta, paragraph);

  if (annotation) {
    const annotationNode = document.createElement("div");
    annotationNode.className = "message__annotation";
    const line = document.createElement("span");
    line.className = "annotation-line";
    line.setAttribute("aria-hidden", "true");
    annotationNode.append(line, document.createTextNode(annotation));
    article.append(annotationNode);
  }

  elements.conversationScroll.appendChild(article);
  elements.conversationScroll.scrollTop = elements.conversationScroll.scrollHeight;
}

function setButtonLoading(button, isLoading, label) {
  if (!button) return;
  const labelNode = $(".button__label", button);
  button.disabled = isLoading;
  button.dataset.state = isLoading ? "loading" : "";
  if (labelNode) labelNode.textContent = isLoading ? "Working..." : label;
}

function cancelSimulation() {
  simulationToken += 1;
  simulationBusy = false;
  setButtonLoading(elements.simulateRunButton, false, "Simulate response");
  setButtonLoading(elements.sendButton, false, "Send prompt");
  elements.promptInput.disabled = false;
}

function simulateRun(promptText) {
  if (simulationBusy) return;

  simulationBusy = true;
  const runToken = ++simulationToken;
  const text = (promptText || "").trim() || defaultPrompt;
  const modelAtRun = currentModel;

  if ((promptText || "").trim()) addMessage("You", text, "user");

  switchPane("chat");
  setButtonLoading(elements.simulateRunButton, true, "Simulate response");
  setButtonLoading(elements.sendButton, true, "Send prompt");
  elements.promptInput.disabled = true;
  elements.promptHelp.textContent = "Sampling staged response...";
  setTelemetryState("sampling");
  setConnection("SIMULATING");

  window.setTimeout(function () {
    if (runToken !== simulationToken) return;

    addMessage(
      modelAtRun,
      "Staged response for \"" + text + "\". Connect the llama.cpp adapter here to replace this sample with a live stream.",
      "assistant",
      "staged sample · llama.cpp adapter pending"
    );
    updateTelemetry();
    setButtonLoading(elements.simulateRunButton, false, "Simulate response");
    setButtonLoading(elements.sendButton, false, "Send prompt");
    elements.promptInput.disabled = false;
    elements.promptInput.value = "";
    elements.promptForm.dataset.state = "";
    elements.promptHelp.textContent = "Enter sends · Shift+Enter adds a line";
    simulationBusy = false;
  }, 780);
}

function showToast(message) {
  window.clearTimeout(toastTimer);
  elements.toast.textContent = message;
  elements.toast.classList.add("is-visible");
  toastTimer = window.setTimeout(function () {
    elements.toast.classList.remove("is-visible");
  }, 3000);
}

async function copyText(value) {
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(value);
      return;
    }
    const helper = document.createElement("textarea");
    helper.value = value;
    helper.setAttribute("readonly", "");
    helper.style.position = "fixed";
    helper.style.opacity = "0";
    document.body.appendChild(helper);
    helper.select();
    document.execCommand("copy");
    helper.remove();
  } catch (error) {
    showToast("Copy failed in this browser");
  }
}

function copyCommand(button) {
  if (!button) return;
  void copyText(currentCommand());
  const label = $(".button__label", button);
  const original = label ? label.textContent : "";
  if (label) label.textContent = "Copied";
  button.dataset.state = "success";
  showToast("Run command copied");
  window.setTimeout(function () {
    if (label) label.textContent = original;
    button.dataset.state = "";
  }, 2500);
}

function switchPane(paneName) {
  const pane = paneName || "chat";
  elements.body.dataset.activePane = pane;

  $$(".workspace-tab").forEach(function (tab) {
    const active = tab.dataset.pane === pane;
    tab.classList.toggle("is-active", active);
    tab.setAttribute("aria-selected", String(active));
  });

  $$(".nav-item").forEach(function (item) {
    const active = item.dataset.pane === pane;
    item.classList.toggle("is-active", active);
    if (active) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  });

  $$('[data-pane-view]').forEach(function (view) {
    view.hidden = view.dataset.paneView !== pane;
  });
}

function updateModelCards() {
  $$(".model-card").forEach(function (card) {
    const active = card.dataset.model === currentModel;
    card.setAttribute("aria-pressed", String(active));
    const state = $(".model-card__state", card);
    if (state) state.textContent = active ? "active" : "staged";
  });
}

function selectModel(card, announce) {
  if (!card) return;
  currentModel = card.dataset.model;
  updateModelCards();
  renderSessionTitle();
  updateCommand();
  resetTelemetry();
  if (announce) showToast(currentModel + " selected for this session");
}

function updateRecentSessionMeta() {
  const activeSession = $(".session-item:first-of-type");
  const meta = $("small", activeSession);
  if (meta) meta.textContent = currentModel + " · just now";
}

function resetConversation() {
  elements.conversationScroll.replaceChildren();
  addMessage("System", "You are a local research assistant. State assumptions, show uncertainty, and keep the answer close to the evidence.", "system");
}

function startNewSession(showMessage) {
  cancelSimulation();
  sessionName = "Untitled study";
  resetConversation();
  renderSessionTitle();
  updateRecentSessionMeta();
  $$(".session-item").forEach(function (item, index) {
    item.classList.toggle("is-active", index === 0);
  });
  elements.promptInput.value = "";
  elements.promptForm.dataset.state = "";
  elements.promptHelp.textContent = "Enter sends · Shift+Enter adds a line";
  resetTelemetry();
  switchPane("chat");
  if (showMessage) {
    showToast("New session ready");
    elements.promptInput.focus({ preventScroll: true });
  }
}

function activateSavedSession(item) {
  const name = $("strong", item);
  const meta = $("small", item);
  const modelName = meta ? meta.textContent.split(" · ")[0] : "";
  const matchingCard = $$(".model-card").find(function (card) {
    return card.dataset.model === modelName;
  });

  sessionName = name ? name.textContent : "Untitled study";
  if (matchingCard) {
    currentModel = matchingCard.dataset.model;
    updateModelCards();
  }
  renderSessionTitle();
  updateCommand();
  resetTelemetry();
  $$(".session-item").forEach(function (session) {
    session.classList.toggle("is-active", session === item);
  });
  switchPane("chat");
  showToast("Opened " + sessionName);
}

function openCommandPalette() {
  if (!elements.commandDialog) return;
  if (!elements.commandDialog.open) elements.commandDialog.showModal();
  elements.commandSearch.value = "";
  filterCommands("");
  window.setTimeout(function () {
    elements.commandSearch.focus({ preventScroll: true });
  }, 0);
}

function closeCommandPalette() {
  if (elements.commandDialog && elements.commandDialog.open) elements.commandDialog.close();
}

function visibleCommandItems() {
  return $$(".command-item", elements.commandResults).filter(function (item) {
    return !item.hidden;
  });
}

function updateCommandHighlight() {
  const items = visibleCommandItems();
  if (!items.length) {
    activeCommandIndex = 0;
    return;
  }
  activeCommandIndex = Math.max(0, Math.min(activeCommandIndex, items.length - 1));
  items.forEach(function (item, index) {
    item.classList.toggle("is-active", index === activeCommandIndex);
  });
}

function filterCommands(query) {
  const normalized = query.toLowerCase().trim();
  $$(".command-item", elements.commandResults).forEach(function (item) {
    item.hidden = normalized.length > 0 && !item.textContent.toLowerCase().includes(normalized);
    item.classList.remove("is-active");
  });
  activeCommandIndex = 0;
  updateCommandHighlight();
}

function runCommandAction(action) {
  closeCommandPalette();
  if (action === "focus-prompt") {
    switchPane("chat");
    elements.promptInput.focus({ preventScroll: true });
  }
  if (action === "open-diagnostics") switchPane("diagnostics");
  if (action === "simulate") simulateRun("");
  if (action === "copy-command") copyCommand($("[data-copy-command]"));
  if (action === "new-session") startNewSession(true);
}

function formatBytes(bytes) {
  if (!bytes) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  return (bytes / Math.pow(1024, index)).toFixed(index ? 1 : 0) + " " + units[index];
}

function isTypingTarget(target) {
  return target && ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName);
}

$$('.nav-item').forEach(function (item) {
  item.addEventListener("click", function () {
    switchPane(item.dataset.pane);
  });
});

$$('.workspace-tab').forEach(function (tab) {
  tab.addEventListener("click", function () {
    switchPane(tab.dataset.pane);
  });
});

$$('[data-switch-pane]').forEach(function (button) {
  button.addEventListener("click", function () {
    switchPane(button.dataset.switchPane);
  });
});

$$('.model-card').forEach(function (card) {
  card.addEventListener("click", function () {
    selectModel(card, true);
  });
});

$$('.session-item').forEach(function (item) {
  item.addEventListener("click", function () {
    activateSavedSession(item);
  });
});

$$('[data-open-command]').forEach(function (button) {
  button.addEventListener("click", openCommandPalette);
});

$$('[data-close-command]').forEach(function (button) {
  button.addEventListener("click", closeCommandPalette);
});

$$('[data-copy-command]').forEach(function (button) {
  button.addEventListener("click", function () {
    copyCommand(button);
  });
});

$$('.command-item').forEach(function (item) {
  item.addEventListener("click", function () {
    runCommandAction(item.dataset.command);
  });
});

elements.commandSearch.addEventListener("input", function () {
  filterCommands(elements.commandSearch.value);
});

elements.commandSearch.addEventListener("keydown", function (event) {
  const items = visibleCommandItems();
  if (event.key === "ArrowDown") {
    event.preventDefault();
    activeCommandIndex += 1;
    updateCommandHighlight();
  }
  if (event.key === "ArrowUp") {
    event.preventDefault();
    activeCommandIndex -= 1;
    updateCommandHighlight();
  }
  if (event.key === "Enter" && items[activeCommandIndex]) {
    event.preventDefault();
    runCommandAction(items[activeCommandIndex].dataset.command);
  }
});

elements.commandDialog.addEventListener("click", function (event) {
  if (event.target === elements.commandDialog) closeCommandPalette();
});

elements.simulateRunButton.addEventListener("click", function () {
  simulateRun("");
});

elements.newSession.addEventListener("click", function () {
  startNewSession(true);
});

$(".sidebar__sessions .icon-button").addEventListener("click", function () {
  startNewSession(true);
});

$(".model-shelf .panel-heading .icon-button").addEventListener("click", function () {
  showToast("Model library options are staged for the adapter");
});

elements.promptForm.addEventListener("submit", function (event) {
  event.preventDefault();
  const text = elements.promptInput.value.trim();
  if (!text) {
    elements.promptForm.dataset.state = "error";
    elements.promptHelp.textContent = "Write a prompt first, or use Simulate response for the staged sample.";
    elements.promptInput.focus();
    return;
  }
  simulateRun(text);
});

elements.promptInput.addEventListener("keydown", function (event) {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    elements.promptForm.requestSubmit();
  }
});

elements.temperature.addEventListener("input", function () {
  elements.temperatureValue.textContent = Number(elements.temperature.value).toFixed(2);
  updateCommand();
});

elements.contextLength.addEventListener("change", updateCommand);

elements.modelFile.addEventListener("change", function () {
  const file = elements.modelFile.files[0];
  if (!file) return;
  elements.loadModelLabel.textContent = "Ready to import";
  elements.loadModelMeta.textContent = file.name + " · " + formatBytes(file.size);
  showToast("Selected " + file.name + " for the next import flow");
});

document.addEventListener("keydown", function (event) {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
    event.preventDefault();
    openCommandPalette();
    return;
  }

  if (isTypingTarget(event.target)) return;

  if (event.key === "/") {
    event.preventDefault();
    openCommandPalette();
    return;
  }

  const panes = { "1": "chat", "2": "diagnostics", "3": "trace", "4": "settings" };
  if (panes[event.key]) switchPane(panes[event.key]);
});

document.title = "Renaro Playground";
elements.temperatureValue.textContent = Number(elements.temperature.value).toFixed(2);
updateModelCards();
renderSessionTitle();
updateCommand();
resetTelemetry();
switchPane("chat");
