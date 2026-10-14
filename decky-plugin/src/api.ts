// Backend calls (main.py).
import { callable } from "@decky/api";
import { Settings, Status } from "./model";

export interface Result {
  ok: boolean;
  error?: string;
}
export interface UpdateInfo extends Result {
  latest?: string;
  file?: string | null;
  available?: boolean;
  installed?: string;
  platform?: string;
}
export interface Prefs {
  check_updates: boolean;
  dsu_enabled: boolean;
  dsu_port: number;
}
export interface DsuStatus {
  running: boolean;
  port: number;
  clients: number;
  mode: string | null;      // what it reads motion from ("sinput", "dualsense", "switch_pro")
  error: string;
}

export const getStatus = callable<[], Result & { status?: Status }>("get_status");
export const getSettings = callable<[], Result & { settings?: Settings }>("get_settings");
export const setSettings = callable<[form: string], Result & { settings?: Settings }>("set_settings");
export const action = callable<[what: string], Result>("action");
export const getPrefs = callable<[], Prefs>("get_prefs");
export const setPref = callable<[key: string, value: boolean | number], Prefs>("set_pref");
export const getDsuStatus = callable<[], DsuStatus>("get_dsu_status");
export const checkUpdate = callable<[force: boolean], UpdateInfo>("check_update");
export const installUpdate = callable<[], Result & { version?: string }>("install_update");
export interface PluginUpdate extends Result {
  installed?: string;
  latest?: string;
  available?: boolean;
  url?: string;
  sha256?: string;
}
export const checkPluginUpdate = callable<[force: boolean], PluginUpdate>("check_plugin_update");
export const installPluginUpdate = callable<[], Result & { version?: string }>("install_plugin_update");
