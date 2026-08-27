import { signal } from "@preact/signals";
import { api } from "./api";
import type { DeviceSummary } from "./types";

export const devices = signal<DeviceSummary[]>([]);
export const wsState = signal<"connecting" | "open" | "closed">("connecting");
export const toast = signal<string | null>(null);

export interface AuthState {
  mode: "password" | "oidc" | "off";
  logged_in: boolean;
  user: string | null;
}
// null = still checking
export const auth = signal<AuthState | null>(null);

export async function refreshAuth(): Promise<AuthState> {
  const st = await api.get<AuthState>("/api/auth/state");
  auth.value = st;
  return st;
}

let toastTimer: number | undefined;
export function showToast(msg: string) {
  toast.value = msg;
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => (toast.value = null), 4000);
}

export async function refreshDevices() {
  devices.value = await api.get<DeviceSummary[]>("/api/devices");
}

// photo bumps let <img> tags cache-bust on new frames
export const photoBump = signal<Record<string, number>>({});

let refreshQueued = false;
function queueRefresh() {
  // WS patches carry deltas; simplest correct MVP is a debounced re-fetch.
  if (refreshQueued) return;
  refreshQueued = true;
  setTimeout(async () => {
    refreshQueued = false;
    try {
      await refreshDevices();
    } catch {
      /* transient */
    }
  }, 300);
}

export function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/api/ws`);
  wsState.value = "connecting";
  ws.onopen = () => (wsState.value = "open");
  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.t === "device" || msg.t === "hello") queueRefresh();
      if (msg.t === "photo") {
        photoBump.value = { ...photoBump.value, [msg.id]: Date.now() };
        queueRefresh();
      }
    } catch {
      /* ignore */
    }
  };
  ws.onclose = () => {
    wsState.value = "closed";
    setTimeout(connectWs, 2000);
  };
}
