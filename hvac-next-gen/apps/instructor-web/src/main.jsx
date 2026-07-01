import React from "react";
import { createRoot } from "react-dom/client";
import "./styles.css";

function App() {
	return (
		<main className="page">
			<h1>Instructor Console</h1>
			<p>Scenario control and live telemetry surface for HVAC training labs.</p>
		</main>
	);
}

createRoot(document.getElementById("root")).render(
	<React.StrictMode>
		<App />
	</React.StrictMode>
);
