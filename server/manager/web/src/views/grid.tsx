import { devices, photoBump } from "../store";
import type { DeviceSummary } from "../types";

const STATUS_LABEL: Record<string, string> = {
  online: "online",
  offline: "offline",
  sleeping: "spí",
  stale: "bez odezvy",
  unknown: "?",
};

export function StatusDot({ status }: { status: string }) {
  return <span class={`dot dot-${status}`} title={STATUS_LABEL[status] ?? status} />;
}

export function rssiBars(rssi: string | null): string {
  const v = rssi ? parseInt(rssi, 10) : NaN;
  if (Number.isNaN(v)) return "";
  if (v >= -55) return "▂▄▆█";
  if (v >= -65) return "▂▄▆_";
  if (v >= -75) return "▂▄__";
  return "▂___";
}

function Card({ d }: { d: DeviceSummary }) {
  const bump = photoBump.value[d.device_id] ?? 0;
  return (
    <a class="card" href={`#/device/${d.device_id}`}>
      <div class="thumb">
        <img
          src={`/api/devices/${d.device_id}/photo/latest?b=${bump}`}
          onError={(e) => ((e.target as HTMLImageElement).style.display = "none")}
          alt=""
        />
        {d.latest_photo?.cap && <div class="cap">{d.latest_photo.cap}</div>}
      </div>
      <div class="card-body">
        <div class="card-title">
          <StatusDot status={d.status} />
          <b>{d.name || d.device_id}</b>
          {d.name && <span class="muted"> {d.device_id}</span>}
        </div>
        <div class="statline">
          {d.profile && <span>{d.profile}</span>}
          {d.soc && <span>🔋 {Math.round(parseFloat(d.soc))} %</span>}
          {d.rssi && <span>📶 {rssiBars(d.rssi)}</span>}
          {d.temp && <span>🌡 {d.temp} °C</span>}
          {d.fw_version && <span class="muted">{d.fw_version}</span>}
        </div>
      </div>
    </a>
  );
}

export function DeviceGrid() {
  const list = devices.value;
  if (!list.length) return <p class="muted center">Zatím žádná zařízení — čekám na MQTT…</p>;
  return <div class="grid">{list.map((d) => <Card key={d.device_id} d={d} />)}</div>;
}
