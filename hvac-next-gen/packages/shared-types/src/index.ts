export interface TrainerTelemetry {
  trainerId: string;
  timestamp: string;
  mode: "idle" | "heating" | "cooling" | "fault";
  temperatureF: number;
  pressurePsi: number;
}

export interface FaultInjection {
  id: string;
  trainerId: string;
  code: string;
  enabled: boolean;
}
