export interface DeviceSummary {
  device_id: string;
  name: string | null;
  status: "online" | "offline" | "sleeping" | "stale" | "unknown";
  profile: string | null;
  fw_version: string | null;
  soc: string | null;
  v_bat: string | null;
  rssi: string | null;
  temp: string | null;
  next_wake_s: number | null;
  latest_photo: { seq?: number; trigger?: string; cap?: string; url?: string } | null;
  last_seen: number | null;
}

export interface EntityPub {
  component: string;
  object_id: string;
  name: string;
  unit: string | null;
  device_class: string | null;
  entity_category: string | null;
  value: string | null;
  attributes: Record<string, unknown> | null;
  last_seen: number | null;
}

export interface ConfigItem {
  key: string;
  type: "bool" | "number" | "select";
  name: string;
  value: string | null;
  min: number | null;
  max: number | null;
  step: number | null;
  options: string[] | null;
  category: string | null;
}

export interface DeviceDetail extends DeviceSummary {
  availability: string;
  reset_reason: string | null;
  fw: Record<string, string> | null;
  ds: Record<string, unknown> | null;
  selftest: Record<string, unknown> | null;
  entities: Record<string, EntityPub>;
  scalars: Record<string, { value: string; ts: number }>;
  config: Record<string, ConfigItem>;
}

export interface PhotoRow {
  id: number;
  device_id: string;
  seq: number | null;
  day: string;
  trigger: string;
  size: number;
  url: string;
  created_at: string;
}
