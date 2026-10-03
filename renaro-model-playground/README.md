# Renaro Model Playground

This is a no-build HTML/CSS/JS prototype for Renaro's local model playground. It uses the blue Terminal Pulse visual language as a single, focused research workbench.

Open index.html directly in a browser, or serve the repository from D:\imgui:

    python -m http.server 8080

Then visit http://localhost:8080/renaro-model-playground/.

The interaction layer is intentionally staged. Model cards, local model selection, prompt submission, command copying, diagnostics, trace tabs, temperature and context controls, the file picker, session switching, and the command palette are ready to demonstrate the playground UX. The telemetry values only appear after Simulate response and are labelled as staged demo samples.

The logo is referenced from ../renaro/assets/logo/white-transparent.png, so the prototype stays linked to the existing Renaro asset.
