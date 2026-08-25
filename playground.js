(() => {
  "use strict";

  const editor = document.querySelector("#source-editor");
  const preview = document.querySelector("#canvas-preview");
  const runButton = document.querySelector("#run-button");
  const resetButton = document.querySelector("#reset-button");
  const status = document.querySelector("#preview-status");
  const lineCount = document.querySelector("#line-count");

  const defaultSource = String.raw`<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  * { box-sizing: border-box; }
  html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; }
  body { background: #303b49; font-family: ui-monospace, Consolas, monospace; }
  canvas { display: block; width: 100%; height: 100%; }
  .readout {
    position: fixed; left: 18px; bottom: 16px; padding: 8px 10px;
    border: 1px solid rgba(222,235,243,.28); color: #c8d6df;
    background: rgba(18,26,32,.55); font-size: 10px; letter-spacing: .08em;
    text-transform: uppercase; pointer-events: none;
  }
  .readout b { color: #53cdb2; font-weight: 700; }
</style>
</head>
<body>
<canvas id="canvas"></canvas>
<div class="readout">Canvas 2D · <b>requestAnimationFrame</b></div>
<script>
const canvas = document.querySelector("#canvas");
const ctx = canvas.getContext("2d");
const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
const colors = ["#53cdb2", "#e85d68", "#f2b544", "#4b86e8"];
const blocks = Array.from({ length: 12 }, (_, index) => ({
  orbit: 74 + (index % 4) * 72,
  angle: index * 0.78,
  speed: 0.00012 + (index % 5) * 0.000018,
  size: 8 + (index % 3) * 6,
  color: colors[index % colors.length]
}));
let width = 0;
let height = 0;
let ratio = 1;
let pointer = { x: 0.48, y: 0.44 };

function resize() {
  const rect = canvas.getBoundingClientRect();
  ratio = Math.min(devicePixelRatio || 1, 2);
  width = Math.max(1, Math.floor(rect.width));
  height = Math.max(1, Math.floor(rect.height));
  canvas.width = Math.floor(width * ratio);
  canvas.height = Math.floor(height * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

function draw(timestamp) {
  const light = {
    x: width * pointer.x + Math.sin(timestamp * 0.00031) * width * 0.05,
    y: height * pointer.y + Math.cos(timestamp * 0.00027) * height * 0.06
  };

  ctx.fillStyle = "#303b49";
  ctx.fillRect(0, 0, width, height);

  const glow = ctx.createRadialGradient(light.x, light.y, 0, light.x, light.y, Math.max(width, height) * 0.7);
  glow.addColorStop(0, "rgba(255,255,255,.12)");
  glow.addColorStop(0.18, "rgba(125,160,184,.055)");
  glow.addColorStop(1, "rgba(10,18,25,0)");
  ctx.fillStyle = glow;
  ctx.fillRect(0, 0, width, height);

  const centerX = width * 0.52;
  const centerY = height * 0.5;

  for (let index = 0; index < blocks.length; index += 1) {
    const block = blocks[index];
    const angle = block.angle + timestamp * block.speed;
    const x = centerX + Math.cos(angle) * block.orbit * Math.min(width / 780, 1.25);
    const y = centerY + Math.sin(angle * 1.17) * block.orbit * 0.68;
    const dx = x - light.x;
    const dy = y - light.y;
    const distance = Math.hypot(dx, dy) || 1;
    const shadowLength = Math.min(520, 100 + distance * 0.72);
    const shadowX = x + dx / distance * shadowLength;
    const shadowY = y + dy / distance * shadowLength;

    ctx.beginPath();
    ctx.moveTo(x - block.size * 0.45, y - block.size * 0.45);
    ctx.lineTo(x + block.size * 0.45, y + block.size * 0.45);
    ctx.lineTo(shadowX + block.size * 1.5, shadowY + block.size * 1.5);
    ctx.lineTo(shadowX - block.size * 1.5, shadowY - block.size * 1.5);
    ctx.closePath();
    ctx.fillStyle = "rgba(15,24,33,.34)";
    ctx.fill();

    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(angle * 0.72);
    ctx.fillStyle = block.color;
    ctx.fillRect(-block.size / 2, -block.size / 2, block.size, block.size);
    ctx.restore();
  }

  ctx.beginPath();
  ctx.arc(light.x, light.y, 4.5, 0, Math.PI * 2);
  ctx.fillStyle = "#f6fbff";
  ctx.shadowColor = "#ffffff";
  ctx.shadowBlur = 14;
  ctx.fill();
  ctx.shadowBlur = 0;

  if (!reducedMotion) requestAnimationFrame(draw);
}

addEventListener("resize", resize);
addEventListener("pointermove", event => {
  pointer.x = event.clientX / Math.max(innerWidth, 1);
  pointer.y = event.clientY / Math.max(innerHeight, 1);
});
resize();
requestAnimationFrame(draw);
</script>
</body>
</html>`;

  function updateLineCount() {
    const count = editor.value.split("\n").length;
    lineCount.textContent = `${count} ${count === 1 ? "line" : "lines"}`;
  }

  function setStatus(message, state = "idle") {
    status.textContent = message;
    status.dataset.state = state;
  }

  function runSource(source) {
    if (!source.trim()) {
      setStatus("Source is empty", "error");
      return;
    }

    setStatus("Loading document…", "idle");

    try {
      preview.srcdoc = source;
    } catch (error) {
      setStatus(error instanceof Error ? error.message : "Unable to load source", "error");
    }
  }

  function resetSource() {
    editor.value = defaultSource;
    updateLineCount();
    runSource(defaultSource);
    editor.focus();
  }

  preview.addEventListener("load", () => {
    setStatus("Canvas running", "running");
  });

  runButton.addEventListener("click", () => {
    runSource(editor.value);
  });

  resetButton.addEventListener("click", resetSource);

  editor.addEventListener("input", updateLineCount);

  editor.addEventListener("keydown", event => {
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
      event.preventDefault();
      runSource(editor.value);
      return;
    }

    if (event.key === "Tab") {
      event.preventDefault();
      const start = editor.selectionStart;
      const end = editor.selectionEnd;
      editor.setRangeText("  ", start, end, "end");
      updateLineCount();
    }
  });

  window.runSource = runSource;
  window.resetSource = resetSource;
  window.setStatus = setStatus;

  editor.value = defaultSource;
  updateLineCount();
  runSource(defaultSource);
})();
