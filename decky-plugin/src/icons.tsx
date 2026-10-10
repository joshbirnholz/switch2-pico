// Button icons: Kenney's Input Prompts (CC0, kenney.nl; see src/kenney.ts),
// plus a few drawn here that the pack doesn't have (Capture, PS).
import { ReactNode } from "react";
import { KENNEY } from "./kenney";
import { deckyQam, MODE_DS, MODE_DS_EDGE, MODE_GC, MODE_SINPUT, MODE_SWITCH } from "./model";

const SIZE = 30;
const INK = "#e6e9ed";
const BODY = "#3a414c";

function K({ name, size = SIZE }: { name: string; size?: number }) {
  const svg = KENNEY[name];
  if (!svg) return <Text text="?" />;
  return <img src={"data:image/svg+xml;utf8," + encodeURIComponent(svg)} width={size} height={size} alt="" style={{ display: "block" }} />;
}

function Text({ text }: { text: string }) {
  return (
    <svg width={SIZE} height={SIZE} viewBox="0 0 30 30" aria-hidden="true">
      <circle cx="15" cy="15" r="11" fill={BODY} />
      <text x="15" y="15.5" fill={INK} fontSize={text.length > 1 ? 9 : 13} fontWeight="700" textAnchor="middle" dominantBaseline="central" fontFamily="sans-serif">{text}</text>
    </svg>
  );
}

function Capture() {
  return (
    <svg width={SIZE} height={SIZE} viewBox="0 0 30 30" aria-hidden="true">
      <rect x="5" y="5" width="20" height="20" rx="4" fill="#ffffff" />
      <circle cx="15" cy="15" r="5" fill="#14181f" />
    </svg>
  );
}

function None() {
  return (
    <svg width={SIZE} height={SIZE} viewBox="0 0 30 30" aria-hidden="true">
      <path d="M9 15h12" stroke="#a3adba" strokeWidth="2.2" strokeLinecap="round" />
    </svg>
  );
}

function Combo({ a, b }: { a: ReactNode; b: ReactNode }) {
  return (
    <span style={{ display: "inline-flex", alignItems: "center", gap: 1 }}>
      {a}
      <span style={{ fontWeight: 700, color: "#a3adba" }}>+</span>
      {b}
    </span>
  );
}

const DIRS = ["up", "down", "left", "right"];

// The icon of output `idx` (out_button_t in Switch mode, gp_out_t otherwise)
// in USB mode `mode`.
export function OutputIcon({ mode, idx }: { mode: number; idx: number }) {
  if (idx === 0) return <None />;
  if (idx === deckyQam(mode)) return <K name="steamdeck_button_quickaccess" />;
  if (mode === MODE_SWITCH) {
    const names: Record<number, string> = {
      1: "switch_button_a", 2: "switch_button_b", 3: "switch_button_x", 4: "switch_button_y",
      5: "switch_button_l", 6: "switch_button_r", 7: "switch_button_zl", 8: "switch_button_zr",
      9: "switch_button_minus", 10: "switch_button_plus", 11: "switch_stick_l_press", 12: "switch_stick_r_press",
      13: "switch_button_home",
    };
    if (names[idx]) return <K name={names[idx]} />;
    if (idx === 14) return <Capture />;
    if (idx >= 15 && idx <= 18) return <K name={"switch_dpad_" + DIRS[idx - 15]} />;
    if (idx === 19) return <Combo a={<K name="switch_button_home" />} b={<K name="switch_button_a" />} />;
    if (idx === 20) return <Combo a={<K name="switch_button_home" />} b={<K name="switch_button_r" />} />;
    return <Text text="?" />;
  }
  if (mode === MODE_SINPUT) {
    // Nintendo labels by position (South B, East A, West Y, North X); Steam
    // shows the second paddle pair and the extra button as GL, GR and C.
    const names: Record<number, string> = {
      1: "switch_button_b", 2: "switch_button_a", 3: "switch_button_y", 4: "switch_button_x",
      5: "switch_button_l", 6: "switch_button_r", 7: "switch_button_zl", 8: "switch_button_zr",
      9: "switch_button_minus", 10: "switch_button_plus", 11: "switch_stick_l_press", 12: "switch_stick_r_press",
      13: "switch_button_home",
      22: "steamdeck_button_l4", 23: "steamdeck_button_r4", 24: "switch_button_gl", 25: "switch_button_gr",
      29: "switch_button_c",
    };
    if (names[idx]) return <K name={names[idx]} />;
    if (idx >= 14 && idx <= 17) return <K name={"switch_dpad_" + DIRS[idx - 14]} />;
    if (idx === 21) return <Capture />;
    return <Text text="?" />;
  }
  if (mode === MODE_GC) {
    const names: Record<number, string> = {
      1: "gamecube_button_color_a", 3: "gamecube_button_color_b", 2: "gamecube_button_x", 4: "gamecube_button_y",
      6: "gamecube_button_z", 7: "gamecube_trigger_l", 8: "gamecube_trigger_r", 10: "gamecube_button_start",
    };
    if (names[idx]) return <K name={names[idx]} />;
    if (idx >= 14 && idx <= 17) return <K name={"gamecube_dpad_" + DIRS[idx - 14]} />;
    return <Text text="?" />;
  }
  if (mode === MODE_DS || mode === MODE_DS_EDGE) {
    const names: Record<number, string> = {
      1: "playstation_button_color_cross", 2: "playstation_button_color_circle",
      3: "playstation_button_color_square", 4: "playstation_button_color_triangle",
      5: "playstation_trigger_l1", 6: "playstation_trigger_r1", 7: "playstation_trigger_l2", 8: "playstation_trigger_r2",
      9: "playstation5_button_create", 10: "playstation5_button_options", 11: "playstation_button_l3", 12: "playstation_button_r3",
      18: "playstation5_touchpad_press_center", 19: "playstation5_touchpad_press_left", 20: "playstation5_touchpad_press_right",
      21: "playstation5_button_mute", 22: "playstation5_elite_lb", 23: "playstation5_elite_rb",
      24: "playstation5_elite_fn_l", 25: "playstation5_elite_fn_r",
    };
    if (names[idx]) return <K name={names[idx]} />;
    if (idx === 13) return <Text text="PS" />;
    if (idx >= 14 && idx <= 17) return <K name={"playstation_dpad_" + DIRS[idx - 14]} />;
    if (idx === 26) return <Combo a={<Text text="PS" />} b={<K name="playstation_button_color_cross" />} />;
    if (idx === 27) return <Combo a={<Text text="PS" />} b={<K name="playstation_trigger_r1" />} />;
    return <Text text="?" />;
  }
  // Xbox 360
  const names: Record<number, string> = {
    1: "xbox_button_color_a", 2: "xbox_button_color_b", 3: "xbox_button_color_x", 4: "xbox_button_color_y",
    5: "xbox_lb", 6: "xbox_rb", 7: "xbox_lt", 8: "xbox_rt", 9: "xbox_button_back", 10: "xbox_button_start",
    11: "xbox_ls", 12: "xbox_rs", 13: "xbox_guide",
  };
  if (names[idx]) return <K name={names[idx]} />;
  if (idx >= 14 && idx <= 17) return <K name={"xbox_dpad_" + DIRS[idx - 14]} />;
  if (idx === 26) return <Combo a={<K name="xbox_guide" />} b={<K name="xbox_button_color_a" />} />;
  if (idx === 27) return <Combo a={<K name="xbox_guide" />} b={<K name="xbox_rb" />} />;
  return <Text text="?" />;
}

// The controller's own extra buttons (C, Capture, GL / GR, SL / SR).
export function InputIcon({ label }: { label: string }) {
  if (label === "Capture") return <Capture />;
  return <K name={"switch_button_" + label.toLowerCase()} />;
}

// Profile buttons: a face button or D-pad direction (GameCube ones on a
// GameCube controller), or a number for numbered profiles.
export function SlotIcon({ label, gamecube = false }: { label: string; gamecube?: boolean }) {
  const dir = ["Up", "Down", "Left", "Right"].indexOf(label);
  if (dir >= 0) return <K name={(gamecube ? "gamecube_dpad_" : "switch_dpad_") + DIRS[dir]} />;
  if (/^[ABXY]$/.test(label)) {
    if (!gamecube) return <K name={"switch_button_" + label.toLowerCase()} />;
    return <K name={label === "A" ? "gamecube_button_color_a" : label === "B" ? "gamecube_button_color_b" : "gamecube_button_" + label.toLowerCase()} />;
  }
  return <Text text={label} />;
}
