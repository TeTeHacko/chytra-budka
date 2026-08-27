import { useEffect, useState } from "preact/hooks";
import { api } from "../api";
import { devices } from "../store";
import type { PhotoRow } from "../types";
import { PhotoGrid, fmtDay } from "./device";

interface GalleryData {
  days: string[];
  triggers: Record<string, number>;
  photos: PhotoRow[];
}

export function GalleryView() {
  const [data, setData] = useState<GalleryData | null>(null);
  const [day, setDay] = useState("");
  const [device, setDevice] = useState("");
  const [trigger, setTrigger] = useState("");

  useEffect(() => {
    const q = new URLSearchParams();
    if (day) q.set("day", day);
    if (device) q.set("device", device);
    if (trigger) q.set("trigger", trigger);
    api.get<GalleryData>(`/api/gallery?${q}`).then(setData);
  }, [day, device, trigger]);

  if (!data) return <p class="muted center">Načítám…</p>;

  return (
    <div>
      <div class="filters">
        <select value={day} onChange={(e: Event) => setDay((e.target as HTMLSelectElement).value)}>
          <option value="">všechny dny</option>
          {data.days.map((d) => <option value={d}>{fmtDay(d)}</option>)}
        </select>
        <select value={device} onChange={(e: Event) => setDevice((e.target as HTMLSelectElement).value)}>
          <option value="">všechna zařízení</option>
          {devices.value.map((d) => (
            <option value={d.device_id}>{d.name || d.device_id}</option>
          ))}
        </select>
        <select value={trigger} onChange={(e: Event) => setTrigger((e.target as HTMLSelectElement).value)}>
          <option value="">všechny triggery</option>
          {Object.entries(data.triggers).map(([t, n]) => (
            <option value={t}>{t} ({n})</option>
          ))}
        </select>
      </div>
      <PhotoGrid photos={data.photos} />
    </div>
  );
}
