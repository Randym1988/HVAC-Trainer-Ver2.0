import React from "react";
import { createRoot } from "react-dom/client";

function App() {
  return (
    <main style={{ maxWidth: 760, margin: "2rem auto", fontFamily: "Segoe UI, sans-serif" }}>
      <h1>Student Lab Interface</h1>
      <p>Interactive exercises, checkpoints, and guided fault diagnostics.</p>
    </main>
  );
}

createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
