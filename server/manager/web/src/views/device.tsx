import { useEffect, useState } from "preact/hooks";
import { api, ApiError } from "../api";
import { photoBump, showToast } from "../store";
import type { ConfigItem, DeviceDetail, PhotoRow } from "../types";
import { StatusDot, rssiBars } from "./grid";

type Tab = "overview" | "controls" | "config" | "photos";

export function DeviceView({ id }: { id: string }) {
  const [dev, setDev] = useState<DeviceDetail | null>(null);
  const [tab, setTab] = useState<Tab>("overview");

  const load = () =>
    api.get<DeviceDetail>(`/api/devices/${id}`).then(setDev).catch(() => setDev(null));

  useEffect(() => {
    load();
    const t = setInterval(load, 5000);
    return () => clearInterval(t);
  }, [id]);

  if (!dev) return <p class="muted center">Načítám {id}…</p>;
  const bump = photoBump.value[id] ?? 0;

  return (
    <div class="device">
      <div class="device-head">
        <div class="thumb big">
          <img
            src={`/api/devices/${id}/photo/latest?b=${bump}`}
            onError={(e: Event) => ((e.target as HTMLImageElement).style.display = "none")}
            alt=""
          />
          {dev.latest_photo?.cap && <div class="cap">{dev.latest_photo.cap}</div>}
        </div>
        <div>
          <h2>
            <StatusDot status={dev.status} /> {dev.name || id}
            {dev.name && <span class="muted"> {id}</span>}
          </h2>
          <div class="statline">
            {dev.profile && <span>{dev.profile}</span>}
            {dev.soc && <span>🔋 {Math.round(parseFloat(dev.soc))} %</span>}
            {dev.rssi && <span>📶 {rssiBars(dev.rssi)} ({dev.rssi} dBm)</span>}
            {dev.fw_version && <span class="muted">fw {dev.fw_version}</span>}
          </div>
          <NameEditor id={id} name={dev.name} onSaved={load} />
        </div>
      </div>

      <nav class="tabs">
        {(["overview", "controls", "config", "photos"] as Tab[]).map((t) => (
          <button class={tab === t ? "active" : ""} onClick={() => setTab(t)}>
            {{ overview: "Přehled", controls: "Ovládání", config: "Konfigurace", photos: "Fotky" }[t]}
          </button>
        ))}
      </nav>

      {tab === "overview" && <Overview dev={dev} />}
      {tab === "controls" && <Controls id={id} />}
      {tab === "config" && <ConfigEditor id={id} config={dev.config} onChanged={load} />}
      {tab === "photos" && <DevicePhotos id={id} />}
    </div>
  );
}

function NameEditor({ id, name, onSaved }: { id: string; name: string | null; onSaved: () => void }) {
  const [editing, setEditing] = useState(false);
  const [value, setValue] = useState(name ?? "");
  if (!editing)
    return (
      <button class="link" onClick={() => { setValue(name ?? ""); setEditing(true); }}>
        {name ? "přejmenovat" : "pojmenovat"}
      </button>
    );
  return (
    <form
      class="inline"
      onSubmit={async (e: Event) => {
        e.preventDefault();
        await api.patch(`/api/devices/${id}`, { name: value });
        setEditing(false);
        onSaved();
      }}
    >
      <input value={value} onInput={(e: Event) => setValue((e.target as HTMLInputElement).value)} />
      <button type="submit">uložit</button>
      <button type="button" class="link" onClick={() => setEditing(false)}>zrušit</button>
    </form>
  );
}

function Overview({ dev }: { dev: DeviceDetail }) {
  const rows: [string, string][] = [];
  for (const e of Object.values(dev.entities)) {
    if (e.entity_category === "config" || e.component === "camera") continue;
    if (e.value == null) continue;
    rows.push([e.name, `${e.value}${e.unit ? ` ${e.unit}` : ""}`]);
  }
  for (const [k, v] of Object.entries(dev.scalars)) rows.push([k, v.value]);
  rows.sort((a, b) => a[0].localeCompare(b[0], "cs"));
  return (
    <div class="cols">
      <table class="kv">
        <tbody>{rows.map(([k, v]) => <tr><td>{k}</td><td>{v}</td></tr>)}</tbody>
      </table>
      {dev.selftest && (
        <div>
          <h3>Selftest {String(dev.selftest["summary"] ?? "")}</h3>
          <table class="kv">
            <tbody>
              {Object.entries(dev.selftest)
                .filter(([, v]) => typeof v === "boolean")
                .map(([k, v]) => (
                  <tr><td>{k}</td><td>{v ? "✅" : "—"}</td></tr>
                ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}

function Controls({ id }: { id: string }) {
  const send = async (name: string, confirmMsg?: string) => {
    if (confirmMsg && !confirm(confirmMsg)) return;
    try {
      await api.post(`/api/devices/${id}/command/${name}`);
      showToast(`Příkaz ${name} odeslán`);
    } catch (e) {
      showToast(`Příkaz ${name} selhal: ${e instanceof ApiError ? e.status : e}`);
    }
  };
  return (
    <div class="controls">
      <button onClick={() => send("photo")}>📷 Vyfotit</button>
      <button onClick={() => send("snapshot")}>🔄 Aktualizovat stav</button>
      <button onClick={() => send("ota")}>⬆️ Zkontrolovat OTA</button>
      <button class="danger" onClick={() => send("reboot", `Opravdu restartovat ${id}?`)}>
        ♻️ Restart
      </button>
    </div>
  );
}

function ConfigEditor({ id, config, onChanged }:
  { id: string; config: Record<string, ConfigItem>; onChanged: () => void }) {
  const [pending, setPending] = useState<string | null>(null);

  const put = async (key: string, value: string) => {
    setPending(key);
    try {
      const r = await api.put<{ value: string; clamped: boolean }>(
        `/api/devices/${id}/config/${key}`, { value });
      showToast(r.clamped ? `${key}: zařízení hodnotu upravilo na ${r.value}` : `${key} = ${r.value}`);
      onChanged();
    } catch (e) {
      if (e instanceof ApiError && e.status === 504)
        showToast(`${key}: zařízení hodnotu tiše odmítlo nebo je offline`);
      else if (e instanceof ApiError && e.status === 422)
        showToast(`${key}: neplatná hodnota`);
      else showToast(`${key}: chyba`);
    } finally {
      setPending(null);
    }
  };

  const items = Object.values(config).sort((a, b) => a.key.localeCompare(b.key));
  if (!items.length) return <p class="muted">Žádné konfigurační entity (čekám na discovery).</p>;

  return (
    <table class="kv config">
      <tbody>
        {items.map((c) => (
          <tr key={c.key}>
            <td>
              {c.name}
              <div class="muted small">{c.key}</div>
            </td>
            <td>
              {c.type === "bool" && (
                <input
                  type="checkbox"
                  checked={c.value?.toUpperCase() === "ON"}
                  disabled={pending === c.key}
                  onChange={(e: Event) =>
                    put(c.key, (e.target as HTMLInputElement).checked ? "ON" : "OFF")}
                />
              )}
              {c.type === "select" && (
                <select
                  value={c.value ?? ""}
                  disabled={pending === c.key}
                  onChange={(e: Event) => put(c.key, (e.target as HTMLSelectElement).value)}
                >
                  {(c.options ?? []).map((o) => <option value={o}>{o}</option>)}
                </select>
              )}
              {c.type === "number" && (
                <NumberField item={c} disabled={pending === c.key}
                             onCommit={(v) => put(c.key, v)} />
              )}
              {pending === c.key && <span class="spinner">⏳</span>}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

function NumberField({ item, disabled, onCommit }:
  { item: ConfigItem; disabled: boolean; onCommit: (v: string) => void }) {
  const [v, setV] = useState(item.value ?? "");
  useEffect(() => setV(item.value ?? ""), [item.value]);
  return (
    <span class="numfield">
      <input
        type="number"
        value={v}
        min={item.min ?? undefined}
        max={item.max ?? undefined}
        step={item.step ?? undefined}
        disabled={disabled}
        onInput={(e: Event) => setV((e.target as HTMLInputElement).value)}
      />
      {item.min != null && item.max != null && (
        <span class="muted small">{item.min}–{item.max}</span>
      )}
      <button disabled={disabled || v === (item.value ?? "")} onClick={() => onCommit(v)}>
        ✓
      </button>
    </span>
  );
}

function DevicePhotos({ id }: { id: string }) {
  const [data, setData] = useState<{ days: string[]; photos: PhotoRow[] } | null>(null);
  const [day, setDay] = useState<string | null>(null);

  useEffect(() => {
    api.get<{ days: string[]; photos: PhotoRow[] }>(
      `/api/devices/${id}/photos${day ? `?day=${day}` : ""}`).then(setData);
  }, [id, day]);

  if (!data) return <p class="muted">Načítám…</p>;
  return (
    <div>
      <div class="filters">
        <select value={day ?? ""} onChange={(e: Event) =>
            setDay((e.target as HTMLSelectElement).value || null)}>
          <option value="">všechny dny</option>
          {data.days.map((d) => <option value={d}>{fmtDay(d)}</option>)}
        </select>
      </div>
      <PhotoGrid photos={data.photos} />
    </div>
  );
}

export function PhotoGrid({ photos }: { photos: PhotoRow[] }) {
  const [open, setOpen] = useState<PhotoRow | null>(null);
  if (!photos.length) return <p class="muted">Žádné fotky.</p>;
  return (
    <>
      <div class="photogrid">
        {photos.map((p) => (
          <button class="ph" onClick={() => setOpen(p)}>
            <img src={p.url} loading="lazy" alt="" />
            <span class="ph-label">{p.created_at.slice(11, 19)} · {p.trigger}</span>
          </button>
        ))}
      </div>
      {open && (
        <div class="lightbox" onClick={() => setOpen(null)}>
          <figure onClick={(e: Event) => e.stopPropagation()}>
            <img src={open.url} alt="" />
            <figcaption>
              {open.device_id} · {open.created_at.replace("T", " ")} · {open.trigger} ·
              seq {open.seq} · {(open.size / 1024).toFixed(0)} kB
              <button class="link" onClick={() => setOpen(null)}>zavřít</button>
            </figcaption>
          </figure>
        </div>
      )}
    </>
  );
}

export function fmtDay(d: string): string {
  return `${d.slice(6, 8)}.${d.slice(4, 6)}.${d.slice(0, 4)}`;
}
